"""pictoagents: start, stop and look at the PictoChat AI agents. Meant for people, other Claude sessions and UIs.

    ./pictoagents start            start in the background (radio host, agents, status screen); returns at once
    ./pictoagents stop             stop everything
    ./pictoagents restart          stop, then start
    ./pictoagents status [--json]  running? radio, people in the room, agents and what each one is doing
    ./pictoagents logs [host|manager|run] [-n 40]
    ./pictoagents say TEXT         write TEXT into the room as the host
    ./pictoagents agents           the agents and their settings

Nothing is installed: `start` runs run.py as a normal background process (logs in logs/), which keeps the host and
the manager running and restarts them, until `stop`, logout or a reboot. Settings: pictoagents.json.
"""
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOGS = ROOT / "logs"
PIDFILE = LOGS / "run.pid"
STATUS_FILE = LOGS / "status.json"           # written by the manager every 2 s
REGISTRY_FILE = ROOT / "agents" / "state" / "registry.json"
PYTHON = str(ROOT / ".venv" / "bin" / "python")
GUI_PORT = 7777                              # the host's JSON-lines port (run.py's default --gui-port)


def running_pid():
    """The pid of run.py from logs/run.pid, or None if it is not running."""
    try:
        pid = int(PIDFILE.read_text().strip())
        command_line = subprocess.run(["ps", "-o", "command=", "-p", str(pid)], capture_output=True,
                                      text=True).stdout
        return pid if "run.py" in command_line else None
    except (OSError, ValueError):
        return None


def start():
    pid = running_pid()
    if pid:
        print("already running (pid %d)" % pid)
        return 0
    LOGS.mkdir(exist_ok=True)
    log_file = open(LOGS / "run.log", "a")
    process = subprocess.Popen([PYTHON, str(ROOT / "run.py")], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=log_file,
                               stderr=subprocess.STDOUT, start_new_session=True,    # survives the terminal / chat
                               env=dict(os.environ, PYTHONUNBUFFERED="1"))
    PIDFILE.write_text("%d\n" % process.pid)
    time.sleep(1.5)
    if process.poll() is not None:
        print("run.py exited at once (code %s); see logs/run.log" % process.returncode)
        return 1
    print("started (pid %d). `./pictoagents status` shows how it is going; logs are in logs/" % process.pid)
    return 0


def stop():
    """SIGTERM to run.py (it stops the host and the manager), SIGKILL if it is still there after 20 s
    (run.py can need about 10 s to stop the manager and the host)."""
    pid = running_pid()
    if not pid:
        print("not running")
        return 0
    os.kill(pid, signal.SIGTERM)
    for _ in range(200):
        if not running_pid():
            break
        time.sleep(0.1)
    else:
        os.kill(pid, signal.SIGKILL)
    PIDFILE.unlink(missing_ok=True)
    print("stopped")
    return 0


def status(as_json=False):
    """What the manager last wrote to logs/status.json, if run.py is running and the file is less than 10 s old."""
    pid = running_pid()
    report = {}
    try:
        report = json.loads(STATUS_FILE.read_text())
    except (OSError, ValueError):
        pass
    fresh = pid and report and time.time() - report.get("time", 0) < 10
    if as_json:
        print(json.dumps({"running": bool(pid), "pid": pid, "status": report if fresh else None}, indent=1))
        return 0
    if not pid:
        print("not running (./pictoagents start)")
        return 0
    print("running (pid %d)" % pid)
    if not fresh:
        print("  waiting for the radio and the agents (./pictoagents logs run)")
        return 0
    print_report(report)
    return 0


def print_report(report):
    """The status report as text: radio, people in the room, then one line per agent."""
    radio = report["radio"]
    signal_text = ", %s dBm" % radio["rssi"] if radio.get("rssi") is not None else ""
    answered_text = ", %d%% polls answered" % round(radio["answered"] * 100) if radio.get("answered") else ""
    print("  radio: %s%s%s" % (radio.get("radio") or "?", signal_text, answered_text))
    people = report.get("people", [])
    print("  in the room: %s" % (", ".join("%s%s" % (person["name"], " (trusted)" if person.get("trusted") else "")
                                           for person in people) or "nobody yet"))
    for agent in report.get("agents", []):
        print("  agent %-10s %-4s %-8s %s" % (agent["name"], "PC" if agent.get("kind") == "pc" else "",
                                              agent["state"], agent.get("detail") or ""))


def logs(which="manager", line_count=40):
    """The last `line_count` lines of logs/<which>.log."""
    path = LOGS / (which + ".log")
    if not path.exists():
        print("no %s" % path)
        return 1
    lines = path.read_text(errors="replace").splitlines()
    print("\n".join(lines[-line_count:]))
    return 0


def say(text):
    """Send TEXT to the room as the host's chat pictures, through the host's JSON-lines port."""
    sys.path.insert(0, str(ROOT))
    from agents.claude_agent import Room, text_pictures
    room = Room(GUI_PORT)
    for rows in text_pictures(text):
        room.send({"cmd": "chat", "pixels": rows})
    print("sent")
    return 0


def agents():
    """The agents in agents/state/registry.json, and the trusted DSes."""
    if not REGISTRY_FILE.exists():
        print("no agents yet (they are created on the first start)")
        return 0
    registry = json.loads(REGISTRY_FILE.read_text())
    for name, agent in registry["agents"].items():
        print("%-10s %-4s %s%s" % (name, agent.get("kind", "chat"), agent.get("about", ""),
                                   "  [%s]" % agent["folder"] if agent.get("folder") else ""))
    trusted = registry.get("trusted", {})          # DS MAC -> nickname
    print("trusted DSes: %s" % (", ".join("%s (%s)" % (nickname, ds_mac) for ds_mac, nickname in trusted.items())
                                or "none"))
    return 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0
    command, rest = argv[0], argv[1:]
    if command == "start":
        return start()
    if command == "stop":
        return stop()
    if command == "restart":
        stop()
        return start()
    if command == "status":
        return status("--json" in rest)
    if command == "logs":
        try:
            line_count = int(rest[rest.index("-n") + 1]) if "-n" in rest else 40
        except (IndexError, ValueError):
            print("usage: ./pictoagents logs [host|manager|run] [-n LINES]")
            return 2
        which = next((word for word in rest if word in ("host", "manager", "run")), "manager")
        return logs(which, line_count)
    if command == "say" and not rest:
        print("usage: ./pictoagents say TEXT")
        return 2
    if command == "say":
        return say(" ".join(rest))
    if command == "agents":
        return agents()
    print("unknown command; ./pictoagents help")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
