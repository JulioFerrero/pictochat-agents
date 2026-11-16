#!/usr/bin/env python3
"""Start everything and keep it running: the PictoChat host on the ESP32 radio and the agent manager (with the
status screen). If the radio disappears (USB unplugged, board reset) the host is started again when it comes back;
if anything crashes, it is restarted.

    .venv/bin/python run.py                       # settings from pictoagents.json, see below
    .venv/bin/python run.py --trust 00:09:bf:12:34:56 --screen waveshare-s3-lcd2

pictoagents.json (optional, next to this file; command-line options win):
    {"serial": "auto", "trust": ["00:09:bf:12:34:56"], "screen": "waveshare-s3-lcd2", "name": "Claude", "color": 4,
     "members": true}
"name" and "color" are the host's nickname and colour in the room; each agent is a member of the room unless "members"
is false.

Logs go to logs/host.log and logs/manager.log. Usually started in the background by `./pictoagents start` (and
stopped by `./pictoagents stop`); nothing is installed to run at login.
"""
import argparse
import glob
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PYTHON = sys.executable
LOGS = ROOT / "logs"
SETTINGS_FILE = ROOT / "pictoagents.json"


def log(*parts):
    print(time.strftime("%H:%M:%S"), "run:", *parts, flush=True)


def find_radio(setting):
    """The radio's serial port: `setting` if it is a path that exists, or with "auto" the first USB modem port.
    None when there is none (yet)."""
    if setting and setting != "auto":
        return setting if os.path.exists(setting) else None
    ports = sorted(glob.glob("/dev/cu.usbmodem*")) + sorted(glob.glob("/dev/ttyACM*"))
    return ports[0] if ports else None


def port_open(port):
    """True when something listens on 127.0.0.1:port (the host's JSON-lines port, once the host is up)."""
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
        return True
    except OSError:
        return False


class Child:
    """A child process (the host or the manager), its output appended to logs/<name>.log."""

    def __init__(self, name, command):
        self.name, self.command, self.process, self.started = name, command, None, 0

    def start(self):
        LOGS.mkdir(exist_ok=True)
        log_file = open(LOGS / (self.name + ".log"), "a")
        log_file.write("\n==== %s start: %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), " ".join(self.command)))
        log_file.flush()
        self.process = subprocess.Popen(self.command, cwd=ROOT, stdout=log_file, stderr=subprocess.STDOUT,
                                        env=dict(os.environ, PYTHONUNBUFFERED="1"))
        self.started = time.time()
        log("%s started (pid %d)" % (self.name, self.process.pid))

    def running(self):
        return self.process is not None and self.process.poll() is None

    def exit_code(self):
        return self.process.returncode if self.process is not None else None

    def stop(self):
        """Ctrl-C (SIGINT) so it cleans up, then kill after 5 s."""
        if self.running():
            self.process.send_signal(signal.SIGINT)
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process = None


def read_settings():
    """The command-line options, with what they leave out taken from pictoagents.json (or the defaults)."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--serial", help="radio port, or auto (the first /dev/cu.usbmodem*)")
    parser.add_argument("--trust", action="append", help="MAC of a DS that may make agents use the Mac")
    parser.add_argument("--screen", help="status screen on the radio board, e.g. waveshare-s3-lcd2")
    parser.add_argument("--gui-port", type=int, default=7777)
    options = parser.parse_args()
    config = {}
    if SETTINGS_FILE.exists():
        config = json.loads(SETTINGS_FILE.read_text())
    options.serial = options.serial or config.get("serial", "auto")
    options.trust = options.trust or config.get("trust", [])
    options.screen = options.screen or config.get("screen")
    options.name = config.get("name", "Claude")
    options.color = str(config.get("color", 4))
    options.members = bool(config.get("members", True))      # agents are room members unless "members": false
    return options


def host_command(radio, options):
    return [PYTHON, "pictochat.py", "host", "--serial", radio, "--offload", "--channel", "1",
            "--gui-port", str(options.gui_port), "--name", options.name, "--color", options.color,
            "--out", str(LOGS / "host_capture")]


def manager_command(options):
    command = [PYTHON, "agents/manager.py", "--port", str(options.gui_port)]
    for ds_mac in options.trust:
        command += ["--trust", ds_mac]
    if options.screen:
        command += ["--screen", options.screen]
    if options.members:                     # each agent a member of the room, with its own name tab
        command += ["--members"]
    return command


class Supervisor:
    """Keeps the host running while its radio is plugged in, and the manager running while the host's JSON-lines
    port answers. Checks once a second until SIGTERM or Ctrl-C, then stops both."""

    def __init__(self, options):
        self.options = options
        self.host = None
        self.radio = None                   # the serial port the current host was started on
        self.manager = Child("manager", manager_command(options))
        self.stop_requested = False

    def run(self):
        signal.signal(signal.SIGTERM, self.request_stop)
        try:
            while not self.stop_requested:
                if not self.host_up():
                    continue
                self.keep_manager_running()
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            self.manager.stop()
            if self.host:
                self.host.stop()
            log("stopped")

    def request_stop(self, *_signal_args):
        self.stop_requested = True

    def host_up(self):
        """Start the host if it is not running and the radio is there; stop it if its radio is gone. False when
        there is no host to work with this round (the caller starts the round over)."""
        if self.host is None or not self.host.running():
            if self.host is not None:
                log("host stopped (code %s)" % self.host.exit_code())
                self.host = None
                time.sleep(2)
            radio = find_radio(self.options.serial)
            if not radio:
                log("waiting for the radio (plug in the ESP32)")
                time.sleep(3)
                return False
            self.host = Child("host", host_command(radio, self.options))
            self.radio = radio
            self.host.start()
        # a host that lost its radio keeps running but writes errors: restart it when the port is gone
        if self.host.running() and not os.path.exists(self.radio):
            log("radio unplugged; restarting the host when it is back")
            self.host.stop()
            self.host = None                     # stopped on purpose: no exit code to report next round
            return False
        return True

    def keep_manager_running(self):
        """(Re)start the manager once the host's JSON-lines port answers."""
        if not self.manager.running() and port_open(self.options.gui_port):
            if self.manager.process is not None:
                log("manager stopped (code %s)" % self.manager.exit_code())
                time.sleep(2)
            self.manager.start()


def main():
    Supervisor(read_settings()).run()


if __name__ == "__main__":
    main()
