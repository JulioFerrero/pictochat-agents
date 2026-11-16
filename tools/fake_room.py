#!/usr/bin/env python3
"""A stand-in for the host's JSON-lines port, to test bots without a radio or a DS.

It replays a DS joining as NICK, then for each TEXT sends one chat picture with TEXT written on it, prints what the
bot sends back and saves the bot's pictures as PNGs (bot-reply-1.png, ...). The next TEXT goes out once the bot has
been quiet for 6 s after its last picture (or after 150 s).

    tools/fake_room.py PORT NICK TEXT [TEXT ...]

FAKE_OUT=dir puts the PNGs there (default: the current folder). A TEXT ending in .png is a capture from
captures/agent, sent as drawn; "\\n" in a TEXT starts a new line.
"""
import json
import os
import socket
import sys
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import pictochat
import pictochat_gui

DS_MAC = bytes.fromhex("0009bf123456")         # the pretend DS
DS_MAC_TEXT = "00:09:bf:12:34:56"
QUIET_AFTER_REPLY = 6                          # s without a new picture: the bot's reply is complete
REPLY_TIMEOUT = 150                            # s to wait for a reply at most


def nearest_palette_index(color):
    """The PictoChat palette index closest to an RGB colour; 0 (blank) for anything near white."""
    if min(color) > 200:
        return 0
    palette = list(pictochat_gui.PALETTE.items())
    return min(palette, key=lambda entry: sum((have - want) ** 2 for have, want in zip(entry[1], color)))[0]


def capture_to_rows(path):
    """A capture PNG (the visible part of a picture at 3x, maybe with rulers around it) back to 80 rows of 256
    palette indices."""
    image = Image.open(path).convert("RGB")
    if image.width > 700:                      # with rulers: cut them off
        image = image.crop((30, 18, 30 + 690, 18 + 240))
    image = image.resize((image.width // 3, image.height // 3), Image.NEAREST)
    rows = [[0] * 256 for _ in range(80)]
    for y in range(min(80, image.height)):
        for x in range(min(230, image.width)):
            rows[y][pictochat_gui.VIS_X0 + x] = nearest_palette_index(image.getpixel((x, y)))
    return rows


def picture(text):
    """80 rows of 256 palette indices: TEXT written on a picture, or the picture in a capture PNG."""
    if not text.endswith(".png"):
        return pictochat_gui.clip(pictochat_gui.text_to_pixels(text))
    return capture_to_rows(text)


def save_picture(pixels, path):
    """A chat picture (80 rows of 256 palette indices, 0 = blank) as a PNG at 3x."""
    image = Image.new("RGB", (256, 80))
    image.putdata([pictochat_gui.PALETTE[index] if index else (255, 255, 255) for row in pixels for index in row])
    image.resize((768, 240), Image.NEAREST).save(path)


class FakeRoom:
    """The host's side of one bot connection."""

    def __init__(self, bot, out_dir):
        self.bot, self.out_dir = bot, out_dir
        self.received = b""                    # bytes from the bot not yet split into lines
        self.picture_count = 0                 # pictures the bot has sent so far

    def send_event(self, **fields):
        """One event line, as the host sends it ("t": seconds, mod 1000)."""
        fields.setdefault("t", round(time.time() % 1000, 3))
        self.bot.sendall((json.dumps(fields) + "\n").encode())

    def replay_join(self, nick):
        """The DS's 'joining' and 'ready' member messages, as if it had joined before the bot connected."""
        for kind in (0, 1):
            self.send_event(kind="message", id=1, dir="rx",
                            hex=pictochat.member_info_message(kind, DS_MAC, nick, 5).hex())

    def send_chat(self, text):
        """The DS sends a chat picture: TEXT written on it, or a capture PNG."""
        message = pictochat.chat_message(DS_MAC, pictochat.bitmap_to_image(picture(text)))
        self.send_event(kind="message", id=1, dir="rx", mac=DS_MAC_TEXT, hex=message.hex())

    def collect_replies(self, sent_at):
        """Print the bot's commands and save its pictures until it has been quiet for QUIET_AFTER_REPLY s after a
        picture, it hangs up, or REPLY_TIMEOUT s have passed since `sent_at`."""
        last_picture_at = None
        while (time.time() - sent_at < REPLY_TIMEOUT
               and not (last_picture_at and time.time() - last_picture_at > QUIET_AFTER_REPLY)):
            try:
                chunk = self.bot.recv(1 << 20)
            except socket.timeout:
                continue
            if not chunk:
                break
            self.received += chunk
            *lines, self.received = self.received.split(b"\n")
            for line in lines:
                command = json.loads(line)
                if command.get("cmd") == "chat":
                    self.picture_count += 1
                    last_picture_at = time.time()
                    path = self.out_dir / ("bot-reply-%d.png" % self.picture_count)
                    save_picture(command["pixels"], path)
                    print("  %.1fs: picture %d -> %s" % (time.time() - sent_at, self.picture_count, path.name),
                          flush=True)
                elif command.get("cmd") != "display":
                    print("  bot command:", command, flush=True)


def main():
    port, nick = int(sys.argv[1]), sys.argv[2]
    texts = [text.replace("\\n", "\n") for text in sys.argv[3:]]
    out_dir = Path(os.environ.get("FAKE_OUT", "."))
    server = socket.socket()
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", port))
    server.listen(1)
    print("fake room on %d, waiting for the bot" % port, flush=True)
    bot, _address = server.accept()
    room = FakeRoom(bot, out_dir)
    room.replay_join(nick)
    time.sleep(1.5)                            # quiet gap: the bot treats what follows as live
    bot.settimeout(1.0)
    for text in texts:
        room.send_chat(text)
        sent_at = time.time()
        print("sent: %r" % text, flush=True)
        room.collect_replies(sent_at)
    print("done: %d picture(s)" % room.picture_count)


if __name__ == "__main__":
    main()
