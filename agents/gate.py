#!/usr/bin/env python3
"""The permission gate of PC agents: a Claude Code PreToolUse hook. A tool that reads or changes the Mac runs only
when every message the agent is answering came from a trusted DS, recognised by its hardware (MAC) address, not its
nickname.

agents/manager.py writes the agent's current turn to the file in $PICTO_TURN before each turn:
    {"trusted": true|false, "who": ["Ana"], "trusted_names": ["Ana"]}
and passes this script to Claude Code as a hook with --settings, so Claude Code itself enforces it: Claude can't
talk its way around a deny. Claude Code sends the tool call as JSON on stdin; this prints a deny decision as JSON,
or nothing to let the call go on. Web search and web fetch are not gated (anyone in the room may use them)."""
import json
import os
import sys

GATED_TOOLS = {"Bash", "Read", "Edit", "Write", "MultiEdit", "NotebookEdit", "Glob", "Grep", "LS"}


def current_turn():
    """The turn file manager.py wrote, or {} (untrusted) if it is missing or unreadable."""
    try:
        with open(os.environ["PICTO_TURN"]) as turn_file:
            return json.load(turn_file)
    except (KeyError, OSError, ValueError):
        return {}


def main():
    tool_call = json.load(sys.stdin)
    if tool_call.get("tool_name") not in GATED_TOOLS:
        return
    turn = current_turn()
    if turn.get("trusted"):
        return                                   # no decision: the agent's own --allowedTools rules apply
    senders = " and ".join(turn.get("who") or ["someone"])
    owners = ", ".join(turn.get("trusted_names") or []) or "its owner"
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "deny",
        "permissionDecisionReason": "Blocked: %s asked, and only %s's DS can make me use the Mac. Tell them "
                                    "politely; chatting and web search are still fine." % (senders, owners)}}))


if __name__ == "__main__":
    main()
