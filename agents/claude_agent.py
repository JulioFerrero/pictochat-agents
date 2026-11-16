#!/usr/bin/env python3
"""A Claude agent in the PictoChat room, powered by headless Claude Code on your Claude subscription.

This module holds what every agent is made of; agents/manager.py uses it to run several agents at once:
- ClaudeSession: one long-lived `claude -p` process (stream-json in and out), i.e. one Claude Code session. Its id
  is kept in agents/state/<name>.json and resumed, so the session survives restarts.
- Pictures: picture_png shows Claude a PictoChat picture as a PNG with rulers; reply_pictures turns Claude's reply
  (plain text and ```draw blocks) back into PictoChat pictures (text_pictures, draw_picture).
- Room: the connection to the host's JSON-lines port (`pictochat.py host ... --gui-port 7777`).

Run on its own, it is a single agent that answers every picture a DS sends:

    .venv/bin/python agents/claude_agent.py --name Claude

Claude Code runs with an empty context: no settings, plugins, skills, MCP servers or memory of the person running it
(see CLAUDE_ARGS), and with web search and web fetch as its only tools.
"""
import argparse
import base64
import functools
import io
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
# the PictoChat picture format: palette, visible columns, clip() to what the DS shows, decode_image() of a message
from pictochat_gui import PALETTE, VIS_X0, VIS_X1, clip, decode_image   # noqa: E402
from PIL import Image, ImageDraw, ImageFont                              # noqa: E402

SYSTEM = """You are {name}, an AI who lives inside a Nintendo DS PictoChat room (Chat Room A). The room is hosted by \
a Mac through an ESP32 radio; people join from their DS.

Every message reaches you as a picture of the PictoChat message, preceded by the sender's nickname and the local \
date and time. People type with the DS keyboard, write by hand with the stylus, or draw. Read the picture and answer \
like a friendly member of the room. If it is a drawing, react to what you see. If you can't read it, say so briefly.

The light grid and the rulers around each picture are added for you, they are not part of the message. Picture \
coordinates: x 0-229 from the left, y 0-79 from the top, as the rulers show. The sender's name tab covers x 0-57, \
y 0-15.

Your text replies are drawn into PictoChat messages, so:
- plain text only: no markdown, no emoji, no URLs unless asked
- short: usually 1 to 3 short lines, never more than 8
- answer in the language the person used

You can also draw. Put one or more blocks like this in your reply, each becomes its own message, in order:
```draw
{{"on": "last", "shapes": [["color", "red"], ["X", 160, 30, 9], ["O", 60, 30, 9], ["line", 10, 70, 90, 70], \
["rect", 5, 20, 40, 50], ["fill", 50, 20, 70, 40], ["text", 100, 60, "hi!"]]}}
```
- "on": "last" draws on top of the last picture you received and sends it back (for games on a drawn board, \
corrections, adding to a drawing); "blank" starts an empty picture.
- X and O take a centre and a half-size; line and rect take two corners; fill is a filled rect; text takes the \
top-left corner. ["width", 1-4] sets the pen, ["color", name] the ink: black, grey, red, orange, yellow, green, \
cyan, blue, violet, pink, magenta, white (erases).
- In games like tic-tac-toe, put your mark in the middle of the right cell of THEIR board and send it back with \
"on": "last", then add a short line of text if useful. Check the cell is empty first.

You can search the web when someone asks about current things (weather, news, scores)."""

# Claude Code with nothing of the user's own setup loaded (~600 tokens of context instead of ~48 000) and only the
# web tools, which run on Anthropic's side. `--bare` would be simpler but ignores the subscription login.
CLAUDE_ARGS = ["--disable-slash-commands", "--strict-mcp-config", "--no-chrome", "--setting-sources", "project",
               "--permission-mode", "dontAsk",
               # a resumed session takes the current system prompt, not the one it started with
               "--system-prompt-snapshot", "off"]
WEB_TOOLS = ["WebSearch", "WebFetch"]
# where sessions and agent folders live; tests point PICTO_STATE_DIR elsewhere so they don't touch the real ones
STATE_DIR = Path(os.environ.get("PICTO_STATE_DIR") or ROOT / "agents" / "state")
WORK_DIR = STATE_DIR / "workdir" if os.environ.get("PICTO_STATE_DIR") else ROOT / "agents" / "workdir"
CAPTURES_DIR = ROOT / "captures" / "agent"     # every picture a DS sent, as the PNG Claude saw
# how a message's local date and time is written for Claude
MESSAGE_TIME_FORMAT = "%a %d %b %Y %H:%M %Z"

# typographic quotes, dashes, ellipsis, non-breaking space and bullet, as characters the DS font has
PLAIN_REPLACEMENTS = {"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": " - ",
                      "…": "...", " ": " ", "•": "-"}
# colour names a ```draw block may use -> PictoChat palette index
INK_COLOURS = {"white": 0, "black": 1, "grey": 2, "gray": 2, "magenta": 3, "pink": 4, "red": 5, "orange": 7,
               "yellow": 8, "lime": 9, "green": 10, "cyan": 11, "sky": 12, "blue": 13, "navy": 14, "violet": 15,
               "purple": 15}

PICTURE_WIDTH, PICTURE_HEIGHT = 256, 80        # a PictoChat picture: 80 rows of 256 palette indices
# PictoChat's own text: ~9 px letters on 16-px lines, the first line starting right after the name tab
# (measured on a DS Lite message: "hello" at x 84-106, y 3-11). Geneva 11 matches it within a pixel.
FIRST_LINE_X, OTHER_LINES_X, RIGHT_EDGE_X = 84, 26, 250
LINE_HEIGHT, BASELINE = 16, 12
LINES_PER_PICTURE = 5


def log(*parts):
    """One line of the log (stdout), with the time."""
    print(time.strftime("%H:%M:%S"), *parts, flush=True)


# ---------------------------------------------------------------- text in the DS style
@functools.cache
def text_font():
    """The font closest to the DS's own text: Geneva 11 on a Mac, else DejaVu Sans 10, else Pillow's default."""
    for path, size in (("/System/Library/Fonts/Geneva.ttf", 11), ("DejaVuSans.ttf", 10)):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def char_advance(char):
    """How far the pen moves after one character: whole pixels, at least 1 (letters are drawn one by one)."""
    return max(1, round(text_font().getlength(char)))


def text_width(text):
    return sum(char_advance(char) for char in text)


def draw_text(draw, x, baseline, text, ink):
    """Draw text letter by letter, each on a whole pixel, so it is exactly as wide as text_width says."""
    font = text_font()
    for char in text:
        draw.text((x, baseline), char, fill=ink, font=font, anchor="ls")
        x += char_advance(char)


def plain(text):
    """What the DS font can show: no markdown, no emoji, Latin script only."""
    text = unicodedata.normalize("NFC", text)
    text = re.sub(r"\*\*|__|`|^#+\s*", "", text, flags=re.M)
    text = "".join(PLAIN_REPLACEMENTS.get(char, char) for char in text)
    text = "".join(char for char in text if char in "\n€°" or (32 <= ord(char) < 0x250))
    return re.sub(r"\n{3,}", "\n\n", text).strip()


def wrap(text):
    """Lines as PictoChat lays them out: the first line of each message is shorter (it starts after the name
    tab). Returns a list of messages, each a list of up to 5 lines."""
    messages, current = [], []          # finished messages, lines of the message being filled

    def next_line_width():
        return RIGHT_EDGE_X - (OTHER_LINES_X if current else FIRST_LINE_X)

    def add_line(line):
        nonlocal current
        current.append(line)
        if len(current) == LINES_PER_PICTURE:
            messages.append(current)
            current = []

    for paragraph in text.split("\n"):
        line = ""
        for word in paragraph.split(" "):
            candidate = (line + " " + word).strip() if line else word
            if text_width(candidate) <= next_line_width():
                line = candidate
                continue
            if line:
                add_line(line)
            line = word
            while text_width(line) > next_line_width():      # a word longer than a whole line: cut it
                cut = len(line)
                while cut > 1 and text_width(line[:cut]) > next_line_width():
                    cut -= 1
                add_line(line[:cut])
                line = line[cut:]
        add_line(line)
    if current:
        messages.append(current)
    for message in messages:
        while message and not message[-1].strip():          # trailing blank lines
            message.pop()
    return [message for message in messages if message]


# ---------------------------------------------------------------- pictures
def picture_png(rows, scale=3):
    """The part of a PictoChat picture the DS shows, scaled up for the vision model, with a light 10-px grid under
    the ink and rulers in picture coordinates (x 0-229, y 0-79) so Claude can say where to draw."""
    image = _with_rulers(_scaled_picture(rows, scale), scale)
    buffer = io.BytesIO()
    image.save(buffer, "PNG")
    return buffer.getvalue()


def _scaled_picture(rows, scale):
    """The visible columns of the picture, `scale` times bigger: the ink over a light 10-px grid (darker every 50)."""
    width, height = VIS_X1 + 1 - VIS_X0, len(rows)
    picture = Image.new("RGB", (width * scale, height * scale), (255, 255, 255))
    draw = ImageDraw.Draw(picture)
    for x in range(0, width, 10):
        draw.line((x * scale, 0, x * scale, height * scale), fill=(222, 232, 246) if x % 50 else (196, 212, 236))
    for y in range(0, height, 10):
        draw.line((0, y * scale, width * scale, y * scale), fill=(222, 232, 246) if y % 50 else (196, 212, 236))
    for y, row in enumerate(rows):
        for x, colour in enumerate(row[VIS_X0:VIS_X1 + 1]):
            if colour:
                draw.rectangle((x * scale, y * scale, x * scale + scale - 1, y * scale + scale - 1),
                               fill=PALETTE.get(colour, (128, 128, 128)))
    return picture


def _with_rulers(picture, scale):
    """The scaled picture in a frame with rulers: a tick every 10 px (longer every 50), x labelled every 20 and y
    every 10, in picture coordinates."""
    width, height = picture.width // scale, picture.height // scale
    margin_left, margin_top = 30, 18
    framed = Image.new("RGB", (picture.width + margin_left + 4, picture.height + margin_top + 4), (245, 245, 245))
    framed.paste(picture, (margin_left, margin_top))
    draw = ImageDraw.Draw(framed)
    draw.rectangle((margin_left - 1, margin_top - 1, margin_left + picture.width, margin_top + picture.height),
                   outline=(150, 150, 150))
    for x in range(0, width + 1, 10):
        tick_x = margin_left + x * scale
        draw.line((tick_x, margin_top - (6 if x % 50 == 0 else 3), tick_x, margin_top - 1), fill=(90, 90, 90))
        if x % 20 == 0:
            draw.text((tick_x - 6, 1), str(x), fill=(60, 60, 60))
    for y in range(0, height + 1, 10):
        tick_y = margin_top + y * scale
        draw.line((margin_left - 5, tick_y, margin_left - 1, tick_y), fill=(90, 90, 90))
        draw.text((3, tick_y - 6), str(y), fill=(60, 60, 60))
    return framed


def _canvas(rows=None):
    """A picture-sized image of palette indices to draw on (a copy of `rows`, or blank), and its pen, which draws
    without anti-aliasing."""
    image = Image.new("L", (PICTURE_WIDTH, PICTURE_HEIGHT), 0)
    if rows:
        image.putdata([colour for row in rows for colour in row])
    draw = ImageDraw.Draw(image)
    draw.fontmode = "1"
    return image, draw


def _picture_rows(image):
    """A canvas as PictoChat picture rows, blanked where the DS shows nothing (outside columns 23-252, under the
    name tab)."""
    pixels = image.load()
    return clip([[pixels[x, y] for x in range(PICTURE_WIDTH)] for y in range(PICTURE_HEIGHT)])


def text_pictures(text):
    """A reply's text as PictoChat pictures (80 rows of 256 palette indices each)."""
    pictures = []
    for lines in wrap(plain(text) or "..."):
        image, draw = _canvas()
        for i, line in enumerate(lines):
            draw_text(draw, FIRST_LINE_X if i == 0 else OTHER_LINES_X, i * LINE_HEIGHT + BASELINE, line, 1)
        pictures.append(_picture_rows(image))
    return pictures


def draw_picture(spec, last_rows):
    """Apply a ```draw block (JSON) to the last received picture or to a blank one. Coordinates are the ones
    Claude sees on the rulers: x 0-229 (picture column 23 + x), y 0-79."""
    if isinstance(spec, list):                    # just the shapes, without {"on": ..., "shapes": ...}
        spec = {"shapes": spec}
    if not isinstance(spec, dict):
        spec = {}
    on_last = spec.get("on", "blank") == "last" and last_rows
    image, draw = _canvas(last_rows if on_last else None)
    ink, pen_width = 1, 2

    def picture_x(ruler_x):
        return VIS_X0 + int(round(float(ruler_x)))

    def picture_y(ruler_y):
        return int(round(float(ruler_y)))

    shapes = spec.get("shapes")
    for shape in shapes if isinstance(shapes, list) else []:
        if not isinstance(shape, list) or not shape:
            continue
        kind, values = str(shape[0]).lower(), shape[1:]
        try:
            if kind == "color":
                ink = INK_COLOURS.get(str(values[0]).lower(), 1)
            elif kind == "width":
                pen_width = max(1, min(4, int(values[0])))
            elif kind == "x":
                centre_x, centre_y, half_size = picture_x(values[0]), picture_y(values[1]), int(values[2])
                draw.line((centre_x - half_size, centre_y - half_size, centre_x + half_size, centre_y + half_size),
                          fill=ink, width=pen_width)
                draw.line((centre_x - half_size, centre_y + half_size, centre_x + half_size, centre_y - half_size),
                          fill=ink, width=pen_width)
            elif kind == "o":
                centre_x, centre_y, half_size = picture_x(values[0]), picture_y(values[1]), int(values[2])
                draw.ellipse((centre_x - half_size, centre_y - half_size, centre_x + half_size, centre_y + half_size),
                             outline=ink, width=pen_width)
            elif kind == "line":
                draw.line((picture_x(values[0]), picture_y(values[1]), picture_x(values[2]), picture_y(values[3])),
                          fill=ink, width=pen_width)
            elif kind in ("rect", "fill"):
                box = (picture_x(min(values[0], values[2])), picture_y(min(values[1], values[3])),
                       picture_x(max(values[0], values[2])), picture_y(max(values[1], values[3])))
                if kind == "rect":
                    draw.rectangle(box, outline=ink, width=pen_width)
                else:
                    draw.rectangle(box, fill=ink)
            elif kind == "text":
                draw_text(draw, picture_x(values[0]), picture_y(values[1]) + BASELINE - 3, plain(str(values[2])), ink)
        except (IndexError, TypeError, ValueError):
            log("draw: skipped %r" % (shape,))
    return _picture_rows(image)


DRAW_BLOCK = re.compile(r"```draw\s*(.*?)```", re.S)


def reply_pictures(reply, last_rows):
    """Text and ```draw blocks of a reply, in order, as PictoChat pictures."""
    pictures, position = [], 0
    for block in DRAW_BLOCK.finditer(reply):
        text_before = reply[position:block.start()]
        if text_before.strip():
            pictures += text_pictures(text_before)
        try:
            pictures.append(draw_picture(json.loads(block.group(1)), last_rows))
        except (ValueError, TypeError, AttributeError) as error:     # Claude wrote a block we can't draw
            log("draw: bad block (%s)" % error)
        position = block.end()
    text_after = reply[position:]
    if text_after.strip() or not pictures:
        pictures += text_pictures(text_after)
    return pictures


# ---------------------------------------------------------------- messages from the room
def member_nickname(message):
    """The nickname in a DS's member message (`03 00` joining, `03 01` ready), or None for any other message."""
    if message[:1] == b"\x03" and message[1:2] in (b"\x00", b"\x01") and len(message) >= 28:
        return message[8:28].decode("utf-16-le", "replace").split("\x00")[0] or "?"
    return None


def save_capture(png, nickname):
    """Keep a picture a DS sent (as Claude sees it) in captures/agent, named by time and sender."""
    CAPTURES_DIR.mkdir(parents=True, exist_ok=True)
    filename = "%s-%s.png" % (time.strftime("%Y%m%d-%H%M%S"), re.sub(r"\W", "_", nickname))
    (CAPTURES_DIR / filename).write_bytes(png)


def image_block(png_base64):
    """A base64 PNG as an image content block of a Claude user turn."""
    return {"type": "image", "source": {"type": "base64", "media_type": "image/png", "data": png_base64}}


# ---------------------------------------------------------------- the room (host JSON-lines port)
class Room:
    """The connection to the host: send() a JSON command, events() for everything that happens in the room."""

    def __init__(self, port):
        self.connection = socket.create_connection(("127.0.0.1", port))
        self.lock = threading.Lock()            # several threads send

    def send(self, command):
        with self.lock:
            self.connection.sendall((json.dumps(command) + "\n").encode())

    def events(self):
        """Yields (event, live). On connect the host replays its recent events in one burst; the first quiet
        moment after it marks the start of live traffic."""
        buffer, live = b"", False
        self.connection.settimeout(0.5)
        while True:
            try:
                received = self.connection.recv(65536)
            except socket.timeout:
                if not live:
                    live = True
                    self.connection.settimeout(None)
                    log("room: replay done, listening")
                continue
            if not received:
                raise ConnectionError("the host closed the connection")
            buffer += received
            *lines, buffer = buffer.split(b"\n")
            for line in lines:
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                yield event, live


# ---------------------------------------------------------------- Claude Code
class ClaudeSession:
    """One long-lived `claude -p` stream-json process: a Claude Code session that survives restarts (its id is kept
    in agents/state/<name>.json and resumed).

    tools: the Claude Code tools it may use (default: web only). cwd: where it works (default: an empty private
    folder). add_dirs: more folders it may touch. settings: extra Claude Code settings (e.g. the gate.py hook).
    env: extra environment. on_event: called with every stream-json event (for live status).
    `system` may be changed between turns; Claude Code gets it when it (re)starts."""

    def __init__(self, name, model, effort, system=None, tools=None, cwd=None, add_dirs=(), settings=None, env=None,
                 on_event=None):
        self.name, self.model, self.effort = name, model, effort
        self.system = system or SYSTEM.format(name=name)
        self.tools = WEB_TOOLS if tools is None else list(tools)
        self.workdir = WORK_DIR / name
        self.workdir.mkdir(parents=True, exist_ok=True)
        self.cwd = Path(cwd) if cwd else self.workdir
        self.cwd.mkdir(parents=True, exist_ok=True)
        self.add_dirs = list(add_dirs)
        self.settings = settings
        self.env = env or {}
        self.on_event = on_event
        self.mcp_config = self.workdir / "empty-mcp.json"
        self.mcp_config.write_text('{"mcpServers": {}}\n')
        self.stderr_log = self.workdir / "claude-stderr.log"
        self.state_file = STATE_DIR / (name + ".json")
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        self.session_id = None
        if self.state_file.exists():
            self.session_id = json.loads(self.state_file.read_text()).get("session_id")
        self.process = None

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
        self.process = None

    def _command_line(self):
        tools = ",".join(self.tools)
        command = ["claude", "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
                   "--model", self.model, "--effort", self.effort, "--system-prompt", self.system,
                   "--mcp-config", str(self.mcp_config), "--tools", tools] + CLAUDE_ARGS
        if self.tools:
            command += ["--allowedTools", tools]
        for folder in self.add_dirs:
            command += ["--add-dir", str(folder)]
        if self.settings:
            command += ["--settings", json.dumps(self.settings)]
        if self.session_id:
            command += ["--resume", self.session_id]
        return command

    def _environment(self):
        # an API key in the environment would take precedence over the subscription login
        environment = {name: value for name, value in os.environ.items()
                       if name not in ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN")}
        environment.update(self.env)
        return environment

    def _start(self):
        with open(self.stderr_log, "a") as stderr_log:          # the child keeps its own copy of the handle
            self.process = subprocess.Popen(self._command_line(), cwd=self.cwd, env=self._environment(),
                                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr_log,
                                            text=True, bufsize=1)
        log("claude %s: started (%s, effort %s, tools %s%s)" % (
            self.name, self.model, self.effort, ",".join(self.tools) or "none",
            ", resuming " + self.session_id[:8] if self.session_id else ""))

    def _remember(self, session_id):
        if session_id and session_id != self.session_id:
            self.session_id = session_id
            self.state_file.write_text(json.dumps({"session_id": session_id}) + "\n")

    def _report(self, event):
        if self.on_event:
            try:
                self.on_event(event)
            except Exception as error:      # status display must never break a turn
                log("claude %s: status callback failed: %s" % (self.name, error))

    def _read_reply(self):
        """Claude Code's events up to the end of the turn: returns the reply text, or None if the process ended
        first."""
        for line in self.process.stdout:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            self._report(event)
            if event.get("type") == "system" and event.get("subtype") == "init":
                self._remember(event.get("session_id"))
            elif event.get("type") == "result":
                self._remember(event.get("session_id"))
                if event.get("is_error"):
                    raise RuntimeError(event.get("result") or event.get("subtype"))
                return event.get("result") or ""
        return None

    def ask(self, content):
        """Send one user turn (a list of content blocks); returns Claude's reply text. If Claude Code has stopped
        it is started again, resuming the session; a second stop in a row raises RuntimeError."""
        for attempt in (1, 2):
            if self.process is None or self.process.poll() is not None:
                self._start()
            try:
                turn = {"type": "user", "message": {"role": "user", "content": content}}
                self.process.stdin.write(json.dumps(turn) + "\n")
                self.process.stdin.flush()
                reply = self._read_reply()
                if reply is not None:
                    return reply
            except BrokenPipeError:
                pass
            log("claude: process ended (code %s), restarting" % self.process.poll())
            self.process = None
            if attempt == 2:
                raise RuntimeError("Claude Code stopped twice in a row; see %s" % self.stderr_log)


# ---------------------------------------------------------------- the single agent
class Agent:
    """This file run on its own: every picture a DS sends goes to one Claude session, and the reply to the room."""

    def __init__(self, room, claude, name):
        self.room, self.claude, self.name = room, claude, name
        self.nicknames = {}             # sender id (AID) -> nickname, from the DSes' member messages
        self.inbox = []                 # [(nickname, png bytes, local time)] waiting for Claude
        self.inbox_ready = threading.Condition()
        self.last_rows = None           # the last picture a DS sent, for "on": "last" drawings

    def on_event(self, event, live):
        if event.get("kind") != "message" or event.get("dir") != "rx":
            return
        message = bytes.fromhex(event.get("hex", ""))
        nickname = member_nickname(message)
        if nickname is not None:
            self.nicknames[event.get("id")] = nickname
            return
        rows = decode_image(message)
        if rows is None or not live:
            return
        nickname = self.nicknames.get(event.get("id"), "someone")
        png = picture_png(rows)
        self.last_rows = rows
        save_capture(png, nickname)
        log("room: picture from %s" % nickname)
        with self.inbox_ready:
            self.inbox.append((nickname, png, time.localtime()))
            self.inbox_ready.notify()

    def worker(self):
        """Answers the inbox: what arrived while Claude was busy goes to it in one turn."""
        while True:
            with self.inbox_ready:
                while not self.inbox:
                    self.inbox_ready.wait()
                batch, self.inbox = self.inbox, []
            content = []
            for nickname, png, received in batch:
                # the local time, so Claude can answer "what time is it" and knows how long ago the last message was
                content.append({"type": "text",
                                "text": "%s, %s:" % (nickname, time.strftime(MESSAGE_TIME_FORMAT, received))})
                content.append(image_block(base64.b64encode(png).decode()))
            started = time.time()
            try:
                reply = self.claude.ask(content)
            except Exception as error:      # tell the room instead of going silent
                log("claude: error:", error)
                reply = "(error: %s)" % str(error)[:80]
            pictures = reply_pictures(reply, self.last_rows)
            log("claude: %.1fs -> %r (%d message%s)" % (time.time() - started, reply[:120], len(pictures),
                                                       "" if len(pictures) == 1 else "s"))
            for rows in pictures:
                try:
                    self.room.send({"cmd": "chat", "pixels": rows})
                except OSError as error:
                    log("room: reply lost (%s)" % error)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=7777, help="host GUI port (pictochat.py host --gui-port)")
    parser.add_argument("--name", default="Claude", help="nickname in the room (max 10 characters)")
    parser.add_argument("--model", default="claude-opus-5-5", help="Claude Code --model (e.g. claude-sonnet-5-5)")
    parser.add_argument("--effort", default="low", choices=["low", "medium", "high", "xhigh", "max"])
    args = parser.parse_args()
    agent = Agent(None, ClaudeSession(args.name, args.model, args.effort), args.name)
    threading.Thread(target=agent.worker, daemon=True).start()
    while True:                 # the host may restart: keep reconnecting, Claude's session stays the same
        try:
            agent.room = Room(args.port)
            agent.room.send({"cmd": "set_name", "name": args.name[:10]})
            log("agent %s: connected to the room on port %d" % (args.name, args.port))
            for event, live in agent.room.events():
                agent.on_event(event, live)
        except OSError as error:
            log("room: %s; reconnecting in 2 s" % error)
            time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
