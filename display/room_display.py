#!/usr/bin/env python3
"""The PictoChat room on a small screen, drawn in the style of the DS's PictoChat.

It joins the host's JSON-lines port like any bot (agents/claude_agent.py's Room), keeps track of who is in the room
and of the last messages, draws a 320x240 screen with Pillow and sends only the bands that changed to one of:

  - the CYD (ESP32-2432S028 running cyd_display/), with --serial: frames over USB serial at 921600 baud,
        AA 55 | type u8 | length u16 LE | payload | xor8(type, length bytes, payload)
        PC -> CYD  'P' ping, 'O' orientation (swap_xy, mirror_x, mirror_y, invert: u8 each),
                   'B' blit (below), 'L' RGB LED (red, green, blue: u8 each, 0 = off)
        CYD -> PC  'S' status text, 'A' blit drawn, 'E' error text
  - a screen on the radio modem board (esp32c3_modem/main/lcd.c), with --modem-screen: the same 'B' blits, and 'D'
    to set the screen up, sent through the host as {"cmd": "display", "type": "D" or "B", "hex": payload}.

A 'B' blit is left u16, top u16, width u16, height u16 (LE), then runs of (count u8 1-255, RGB565 colour u16 with the
high byte first) covering width x height pixels.

dashboard.py draws the status screen of the agents with the same helpers; agents/manager.py shows it with ModemScreen.

    .venv/bin/python display/room_display.py --serial /dev/cu.usbserial-XXXX
    .venv/bin/python display/room_display.py --preview room.png      # draw once into a PNG, no board needed
"""
import argparse
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import pictochat_gui                           # noqa: E402
from agents.claude_agent import Room           # noqa: E402
from PIL import Image, ImageDraw, ImageFont    # noqa: E402

WIDTH, HEIGHT = 320, 240
HEADER_HEIGHT, FOOTER_HEIGHT = 20, 18
MESSAGES_X, MESSAGES_WIDTH = 4, 238            # message column: a 230-px picture plus border and padding
MEMBERS_X = MESSAGES_X + MESSAGES_WIDTH + 4    # member column
CYD_BAND_ROWS = 16                             # rows per blit (MAX_BLIT in cyd_display is 320 x 16)

# The DS's 16 favourite colours (user settings), used for name tabs as PictoChat does.
FAVOURITE_COLOURS = [(97, 130, 154), (186, 73, 0), (251, 0, 24), (251, 138, 251), (251, 146, 0), (243, 227, 0),
                     (170, 251, 0), (0, 251, 0), (0, 162, 56), (73, 219, 138), (48, 186, 243), (0, 89, 243),
                     (0, 0, 146), (138, 0, 211), (211, 0, 235), (251, 0, 146)]
BACKGROUND, BACKGROUND_LINE = (226, 228, 232), (214, 216, 222)
BAR_COLOUR, BAR_TEXT_COLOUR = (44, 48, 60), (236, 238, 242)
WHITE = (255, 255, 255)
FONT_PATHS = ["/System/Library/Fonts/Geneva.ttf", "DejaVuSans.ttf"]


def load_font(size):
    for path in FONT_PATHS:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


SMALL_FONT, TAB_FONT, HEADER_FONT = load_font(10), load_font(11), load_font(12)


def draw_text(draw, position, text, colour, font=SMALL_FONT):
    draw.fontmode = "1"                        # crisp pixels, like the DS
    draw.text(position, text, fill=colour, font=font)


def readable_on(rgb):
    """Black or white text, whichever reads better on this colour."""
    return (0, 0, 0) if (rgb[0] * 299 + rgb[1] * 587 + rgb[2] * 114) / 1000 > 150 else (255, 255, 255)


def blank_screen():
    """A new screen image and its drawing context: background with the DS's ruled lines between header and footer."""
    image = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND)
    draw = ImageDraw.Draw(image)
    for y in range(HEADER_HEIGHT, HEIGHT - FOOTER_HEIGHT, 4):
        draw.line((0, y, WIDTH, y), fill=BACKGROUND_LINE)
    return image, draw


# ---------------------------------------------------------------- room state
class RoomState:
    """What the room looks like, updated from the host's events (on_event); read under `lock`."""

    def __init__(self):
        self.lock = threading.Lock()
        self.members = {}          # id (0 = host) -> {"name", "colour"} (+ "agent" when the host reports it)
        self.messages = []         # [{"id", "rows"}], oldest first
        self.last_received = self.last_sent = 0.0   # time of the last drawing from a DS / from the host
        self.aids = {}             # DS MAC -> AID, from state events
        self.rssi = None
        self.answered = None       # share of polls a DS answered, from the last two state events
        self.poll_counts = None    # (polls sent, replies heard) from the last state event
        self.radio = ""
        self.changed = threading.Event()

    def on_event(self, event, live):
        """Takes one event from the host's JSON port: "message" (member info or a drawing) or "state"."""
        kind = event.get("kind")
        with self.lock:
            if kind == "message":
                self._on_message(event)
            elif kind == "state":
                self._on_state(event)
            else:
                return
        self.changed.set()

    def _on_message(self, event):
        message = bytes.fromhex(event.get("hex", ""))
        sender = event.get("id", 0) if event.get("dir") == "rx" else 0
        if message[:1] == b"\x03" and message[1:2] in (b"\x00", b"\x01") and len(message) >= 84:
            # member info (joining / ready): UTF-16 name at 8-27, colour u16 at 80
            name = message[8:28].decode("utf-16-le", "replace").split("\x00")[0] or "?"
            colour = struct.unpack_from("<H", message, 80)[0] & 15
            self.members[sender] = {"name": name, "colour": colour}
            return
        rows = pictochat_gui.decode_image(message)
        if rows is None:
            return
        self.messages = (self.messages + [{"id": sender, "rows": rows}])[-12:]
        if sender:
            self.last_received = time.time()
        else:
            self.last_sent = time.time()

    def _on_state(self, event):
        self.aids = event.get("aids", {})
        if "members" in event:                         # hosts from this repo report names and colours
            self.members = {int(member_id): {"name": fields[0], "colour": int(fields[1]) & 15,
                                             "agent": len(fields) > 2 and fields[2] == "agent"}
                            for member_id, fields in event["members"].items()}
        live_ids = set(self.aids.values())
        for member_id in [member_id for member_id, member in self.members.items()
                          if member_id and not member.get("agent") and member_id not in live_ids]:
            del self.members[member_id]
        levels = [reading[1] for mac, reading in event.get("rssi", {}).items() if reading and mac in self.aids]
        self.rssi = round(sum(levels) / len(levels)) if levels else None
        polls, replies = event.get("cmd", 0), event.get("replies", 0)
        if self.poll_counts and polls > self.poll_counts[0]:
            self.answered = min(1.0, (replies - self.poll_counts[1]) / (polls - self.poll_counts[0]))
        self.poll_counts = (polls, replies)
        self.radio = event.get("radio", "")

    def writing(self):
        """The host's agent is answering: a DS message came after the host's last message (and not too long ago)."""
        return self.last_received > self.last_sent and time.time() - self.last_received < 60


# ---------------------------------------------------------------- drawing
def trimmed(rows):
    """Visible columns of a picture, cut below its last line with ink (as the DS does)."""
    left, right = pictochat_gui.VIS_X0, pictochat_gui.VIS_X1 + 1
    last_inked = max([y for y, row in enumerate(rows) if any(row[left:right])] or [15])
    return [row[left:right] for row in rows[:min(80, last_inked + 3)]]


def picture_image(rows):
    """The visible part of a drawing (rows of palette indices) as an RGB image."""
    visible = trimmed(rows)
    image = Image.new("RGB", (len(visible[0]), len(visible)))
    image.putdata([pictochat_gui.PALETTE.get(index, (128, 128, 128)) if index else WHITE
                   for row in visible for index in row])
    return image


def name_tab(draw, x, y, name, colour, width=58):
    draw.rectangle((x, y, x + width - 1, y + 14), fill=colour)
    draw_text(draw, (x + 3, y + 1), name[:10], readable_on(colour), TAB_FONT)


def bubble(image, draw, bottom, picture, name, colour):
    """A message bubble whose bottom edge is at `bottom`; returns its top edge."""
    top = bottom - (picture.height + 4)
    draw.rectangle((MESSAGES_X, top, MESSAGES_X + MESSAGES_WIDTH - 1, bottom - 1), fill=WHITE, outline=colour)
    image.paste(picture, (MESSAGES_X + 4, top + 2), None)
    name_tab(draw, MESSAGES_X + 1, top + 1, name, colour)
    return top


def signal_bars(draw, x, y, share):
    """Three bars for the share of polls the DSes answered (all grey when unknown)."""
    if share is None:
        lit_bars = 0
    else:
        lit_bars = 3 if share > 0.85 else 2 if share > 0.6 else 1 if share > 0.2 else 0
    for i in range(3):
        bar_height = 4 + 3 * i
        draw.rectangle((x + i * 5, y + 10 - bar_height, x + i * 5 + 3, y + 10),
                       fill=BAR_TEXT_COLOUR if i < lit_bars else (90, 96, 112))


def draw_writing_bubble(draw, bottom, host, now):
    """The "<host> is writing..." bubble; returns its top edge."""
    top = bottom - 18
    colour = FAVOURITE_COLOURS[host["colour"]]
    draw.rectangle((MESSAGES_X, top, MESSAGES_X + MESSAGES_WIDTH - 1, bottom - 1), fill=WHITE, outline=colour)
    name_tab(draw, MESSAGES_X + 1, top + 1, host["name"], colour)
    dots = "." * (1 + int(now * 2) % 3)
    draw_text(draw, (MESSAGES_X + 66, top + 3), "is writing" + dots, (90, 90, 90), TAB_FONT)
    return top


def draw_messages(image, draw, members, messages, writing, now):
    """The message column, newest at the bottom, as many as fit."""
    bottom = HEIGHT - FOOTER_HEIGHT - 4
    host = members.get(0, {"name": "host", "colour": 2})
    if writing:
        bottom = draw_writing_bubble(draw, bottom, host, now) - 4
    for message in reversed(messages):
        if bottom < HEADER_HEIGHT + 8:
            break
        sender = members.get(message["id"], {"name": "?" if message["id"] else host["name"], "colour": 0})
        top = bubble(image, draw, bottom, picture_image(message["rows"]), sender["name"],
                     FAVOURITE_COLOURS[sender["colour"]])
        bottom = top - 4


def draw_room_header(draw, room_letter, member_count, answered):
    """Room badge, title, member count and signal."""
    draw.rectangle((0, 0, WIDTH, HEADER_HEIGHT - 1), fill=BAR_COLOUR)
    draw.rounded_rectangle((4, 3, 17, 16), radius=3, fill=(250, 250, 250))
    draw_text(draw, (7, 3), room_letter, BAR_COLOUR, HEADER_FONT)
    draw_text(draw, (23, 4), "CHAT ROOM " + room_letter, BAR_TEXT_COLOUR, HEADER_FONT)
    draw_text(draw, (WIDTH - 72, 5), "%d/16" % max(1, member_count), BAR_TEXT_COLOUR)
    signal_bars(draw, WIDTH - 22, 4, answered)


def draw_member_list(draw, members):
    """The member column, host first."""
    draw.rectangle((MEMBERS_X - 2, HEADER_HEIGHT, WIDTH, HEIGHT - FOOTER_HEIGHT - 1), fill=(236, 238, 242))
    draw_text(draw, (MEMBERS_X + 2, HEADER_HEIGHT + 3), "IN THE ROOM", (110, 116, 130))
    y = HEADER_HEIGHT + 17
    for member_id in sorted(members):
        member = members[member_id]
        name_tab(draw, MEMBERS_X + 1, y, member["name"], FAVOURITE_COLOURS[member["colour"]],
                 width=WIDTH - MEMBERS_X - 3)
        y += 17


def draw_room_footer(draw, radio, rssi, answered, now):
    """Radio, channel, signal level, share of polls answered and the clock."""
    draw.rectangle((0, HEIGHT - FOOTER_HEIGHT, WIDTH, HEIGHT), fill=BAR_COLOUR)
    radio = "ESP32 modem" if "ESP32" in radio else (radio or "radio")
    parts = [radio, "ch 1"]
    if rssi is not None:
        parts.append("%d dBm" % rssi)
    if answered is not None:
        parts.append("%d%% polls" % round(answered * 100))
    draw_text(draw, (5, HEIGHT - FOOTER_HEIGHT + 3), "   ".join(parts), BAR_TEXT_COLOUR)
    draw_text(draw, (WIDTH - 34, HEIGHT - FOOTER_HEIGHT + 3), time.strftime("%H:%M", time.localtime(now)),
              BAR_TEXT_COLOUR)


def render(state, room_letter="A", now=None):
    """The room screen (320x240) for a RoomState."""
    now = now or time.time()
    image, draw = blank_screen()
    with state.lock:
        members = dict(state.members)
        messages = list(state.messages)
        writing = state.writing()
        rssi, answered, radio = state.rssi, state.answered, state.radio
    draw_messages(image, draw, members, messages, writing, now)
    draw_room_header(draw, room_letter, len(members), answered)   # after the messages: cut-off bubbles go under it
    draw_member_list(draw, members)
    draw_room_footer(draw, radio, rssi, answered, now)
    return image


# ---------------------------------------------------------------- pixels for the panels
def rgb565(image):
    """Big-endian RGB565 bytes, the order the panel takes them."""
    raw = image.convert("RGB").tobytes()
    packed = bytearray(len(raw) // 3 * 2)
    for i in range(len(raw) // 3):
        red, green, blue = raw[3 * i], raw[3 * i + 1], raw[3 * i + 2]
        value = (red >> 3) << 11 | (green >> 2) << 5 | blue >> 3
        packed[2 * i], packed[2 * i + 1] = value >> 8, value & 0xFF
    return bytes(packed)


def rle(pixels):
    """Runs of (count 1-255, RGB565 as 2 bytes) for the firmware's 'B' blit."""
    runs = bytearray()
    i, pixel_count = 0, len(pixels) // 2
    while i < pixel_count:
        colour = pixels[2 * i:2 * i + 2]
        j = i + 1
        while j < pixel_count and j - i < 255 and pixels[2 * j:2 * j + 2] == colour:
            j += 1
        runs += bytes((j - i,)) + colour                   # high byte first, as the panel takes it
        i = j
    return bytes(runs)


def changed_columns(old_band, new_band, band_rows):
    """(left, right + 1): the columns that differ between two bands of RGB565 rows that are not equal."""
    row_bytes = WIDTH * 2
    columns = [x for x in range(WIDTH)
               if any(new_band[row * row_bytes + 2 * x:row * row_bytes + 2 * x + 2] !=
                      old_band[row * row_bytes + 2 * x:row * row_bytes + 2 * x + 2] for row in range(band_rows))]
    return columns[0], columns[-1] + 1


def changed_bands(shown, pixels, band_rows):
    """(left, top, width, height, rle payload) for each band of rows that differs from what the panel shows.

    shown and pixels are full screens of RGB565 bytes (rgb565()); shown is None when the panel content is unknown.
    Only the columns that changed in a band are sent."""
    row_bytes = WIDTH * 2
    bands = []
    for top in range(0, HEIGHT, band_rows):
        new_band = pixels[top * row_bytes:(top + band_rows) * row_bytes]
        if shown is None:
            left, right = 0, WIDTH
        else:
            old_band = shown[top * row_bytes:(top + band_rows) * row_bytes]
            if old_band == new_band:
                continue
            left, right = changed_columns(old_band, new_band, band_rows)
        band_pixels = b"".join(new_band[row * row_bytes + 2 * left:row * row_bytes + 2 * right]
                               for row in range(band_rows))
        bands.append((left, top, right - left, band_rows, rle(band_pixels)))
    return bands


# ---------------------------------------------------------------- the screen on the radio modem board
# Screens on the radio modem board itself, driven through the host ("display" command, esp32c3_modem/main/lcd.c):
# pins sclk, mosi, miso, cs, dc, rst, bl, bl_on, then swap_xy, mirror_x, mirror_y, invert, bgr, spi_mode, MHz.
NOT_CONNECTED = 255
MODEM_SCREENS = {
    # Waveshare ESP32-S3-LCD-2: ST7789 240x320 turned to landscape; pins, RGB order and inversion as in Waveshare's
    # ESP-IDF demo (06_lvgl_example). SPI mode 3 at 40 MHz is what was checked on the board (the demo uses mode 0).
    "waveshare-s3-lcd2": dict(pins=(39, 38, 40, 45, 42, NOT_CONNECTED, 1, 1), panel=(1, 1, 0, 1, 0, 3, 40)),
}


class ModemScreen:
    """The screen on the radio modem board, through the host's JSON port. No acks: the USB link has flow control.

    room_ref() returns the current Room (it changes when the host connection is made again)."""
    BAND_ROWS = 8                              # LCD_MAX_BLIT in lcd.c is 320 x 8

    def __init__(self, room_ref, profile, orientation=None):
        self.room_ref, self.profile = room_ref, MODEM_SCREENS[profile]
        self.orientation = orientation         # swap_xy, mirror_x, mirror_y, invert: replaces the profile's
        self.shown = None                      # what the panel shows, as RGB565 bytes

    def _send(self, frame_type, payload):
        self.room_ref().send({"cmd": "display", "type": frame_type, "hex": payload.hex()})

    def init(self):
        """Sets the screen up ('D': pins, size, panel settings) and redraws everything on the next show()."""
        panel = list(self.profile["panel"])
        if self.orientation:
            panel[0:4] = self.orientation
        self._send("D", bytes(self.profile["pins"]) + struct.pack("<HH", WIDTH, HEIGHT) + bytes(panel))
        self.shown = None

    def led(self, *rgb):
        """No LED on the modem board."""

    def show(self, image):
        """Send the bands that changed, plus one more each time in turn: blits are not acknowledged, so a band
        lost on the way would otherwise stay wrong until that part of the screen changes."""
        pixels = rgb565(image)
        bands = changed_bands(self.shown, pixels, self.BAND_ROWS)
        if self.shown is not None:
            self.refresh_top = (getattr(self, "refresh_top", -self.BAND_ROWS) + self.BAND_ROWS) % HEIGHT
            if all(top != self.refresh_top for _, top, _, _, _ in bands):
                bands += changed_bands(None, pixels, self.BAND_ROWS)[self.refresh_top // self.BAND_ROWS:][:1]
        for left, top, width, height, runs in bands:
            self._send("B", struct.pack("<HHHH", left, top, width, height) + runs)
        self.shown = pixels


# ---------------------------------------------------------------- the CYD over USB serial
class CydScreen:
    """The CYD (cyd_display/ firmware) on a serial port: framed commands, each blit acknowledged with 'A'."""

    def __init__(self, port, orientation):
        import serial
        self.serial = serial.Serial(None, 921600, timeout=0.05)
        self.serial.port, self.serial.dtr, self.serial.rts = port, False, False     # no auto-reset
        self.serial.open()
        self.received = bytearray()
        deadline = time.time() + 6
        while time.time() < deadline:
            self._send(b"P", b"")
            if self._wait(b"S", 1.0):
                break
        else:
            raise OSError("no answer from the CYD on %s (flashed with cyd_display/?)" % port)
        self._send(b"O", bytes(orientation))
        self._wait(b"S", 1.0)
        self.shown = None                                  # what the panel shows, as RGB565 bytes

    def _send(self, frame_type, payload):
        length = len(payload)
        checksum = frame_type[0] ^ (length & 0xFF) ^ (length >> 8)
        for byte in payload:
            checksum ^= byte
        self.serial.write(b"\xaa\x55" + frame_type + struct.pack("<H", length) + payload + bytes((checksum,)))

    def _wait(self, frame_type, timeout):
        """Reads frames until one of `frame_type` arrives and returns its payload (None on timeout); prints status
        and error frames. The checksum is not checked."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.received += self.serial.read(4096)
            while True:
                start = self.received.find(b"\xaa\x55")
                if start < 0 or len(self.received) < start + 6:
                    break
                length = self.received[start + 3] | self.received[start + 4] << 8
                if len(self.received) < start + 6 + length:
                    break
                received_type = self.received[start + 2:start + 3]
                payload = bytes(self.received[start + 5:start + 5 + length])
                del self.received[:start + 6 + length]
                if received_type in (b"S", b"E"):
                    print("cyd:", payload.decode("latin1"), flush=True)
                if received_type == frame_type:
                    return payload
        return None

    def led(self, red, green, blue):
        self._send(b"L", bytes((red, green, blue)))

    def show(self, image):
        pixels = rgb565(image)
        for left, top, width, height, runs in changed_bands(self.shown, pixels, CYD_BAND_ROWS):
            self._send(b"B", struct.pack("<HHHH", left, top, width, height) + runs)
            if self._wait(b"A", 1.0) is None:
                print("cyd: no ack for band %d" % top, flush=True)
        self.shown = pixels


# ---------------------------------------------------------------- main
class RoomLink:
    """The connection to the host's JSON port: feeds a RoomState and connects again when the host goes away."""

    def __init__(self, port, state):
        self.port, self.state = port, state
        self.room = Room(port)
        self.reconnected = threading.Event()   # the modem may have restarted with the host: set its screen up again
        self.ds_message = threading.Event()    # a live message from a DS: blink the LED

    def start(self):
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            try:
                for event, live in self.room.events():
                    self.state.on_event(event, live)
                    if live and event.get("kind") == "message" and event.get("dir") == "rx":
                        self.ds_message.set()
            except OSError as error:
                print("room: %s; reconnecting" % error, flush=True)
            while True:
                time.sleep(2)
                try:
                    self.room = Room(self.port)
                    self.reconnected.set()
                    break
                except OSError:
                    continue


def write_preview(state, path):
    """Waits for the host's replay and one state event (for the stats), then draws the room into a PNG at 2x."""
    started = time.time()
    while time.time() - started < 6 and state.poll_counts is None:
        time.sleep(0.2)
    render(state).resize((WIDTH * 2, HEIGHT * 2), Image.NEAREST).save(path)
    print("wrote", path)


def run_screen(screen, state, link):
    """Redraws whenever the room changes (and every 0.5 s); blinks the LED green when a DS sends something."""
    led_off_at = 0
    while True:
        state.changed.wait(timeout=0.5)
        state.changed.clear()
        if link.reconnected.is_set() and isinstance(screen, ModemScreen):
            link.reconnected.clear()
            time.sleep(1)
            screen.init()
        if link.ds_message.is_set():
            link.ds_message.clear()
            screen.led(0, 1, 0)
            led_off_at = time.time() + 0.4
        if led_off_at and time.time() > led_off_at:
            screen.led(0, 0, 0)
            led_off_at = 0
        try:
            screen.show(render(state))
        except OSError as error:               # the host went away; the link connects again
            print("display: %s" % error, flush=True)
            time.sleep(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=7777, help="host GUI port (pictochat.py host --gui-port)")
    parser.add_argument("--serial", help="the CYD's serial port, e.g. /dev/cu.usbserial-110")
    parser.add_argument("--modem-screen", choices=sorted(MODEM_SCREENS),
                        help="draw on the screen of the radio modem board itself, through the host")
    parser.add_argument("--preview", help="draw the room once into this PNG (2x) and exit")
    parser.add_argument("--orientation",
                        help="swap_xy,mirror_x,mirror_y,invert for the panel (CYD default 1,1,0,0)")
    args = parser.parse_args()
    if not (args.preview or args.modem_screen or args.serial):
        parser.error("say where to draw: --modem-screen PROFILE, --serial PORT (a CYD) or --preview FILE.png")
    state = RoomState()
    link = RoomLink(args.port, state)
    if args.preview:
        link.start()
        write_preview(state, args.preview)
        return
    orientation = [int(value) for value in args.orientation.split(",")] if args.orientation else None
    if args.modem_screen:
        screen = ModemScreen(lambda: link.room, args.modem_screen, orientation)
        screen.init()
    else:
        screen = CydScreen(args.serial, orientation or [1, 1, 0, 0])
    link.start()
    print("room display running on %s" % (args.modem_screen or args.serial), flush=True)
    run_screen(screen, state, link)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
