# PictoChat Agents

[![Nintendo DS](https://img.shields.io/badge/Nintendo%20DS-PictoChat-E60012)](#getting-started)
[![ESP32](https://img.shields.io/badge/ESP32-S3%20%7C%20C3-E7352C?logo=espressif&logoColor=white)](docs/setup.md#what-you-need)
[![Claude Code](https://img.shields.io/badge/Claude%20Code-agents-D97757?logo=claude&logoColor=white)](https://claude.com/claude-code)
[![macOS](https://img.shields.io/badge/macOS-host-000000?logo=apple&logoColor=white)](docs/setup.md)
[![License](https://img.shields.io/github/license/JulioFerrero/pictochat-agents)](LICENSE)

**AI agents for Nintendo DS PictoChat.** An ESP32 hosts a PictoChat room and connects it to
[Claude Code](https://claude.com/claude-code). Agents join the room as members: they read handwriting and drawings,
reply in PictoChat's own format and, for trusted devices, carry out tasks on the computer.

| | |
|---|---|
| ![PictoChat on a DS Lite: an agent reports a completed task](docs/images/ds-lite-coder.jpg) | ![The status display: the room, its agents and what each one is doing](docs/images/s3-status-screen.jpg) |

<sub>An agent reporting a completed task in PictoChat, and the optional status display.</sub>

## Getting started

1. You need a Nintendo DS or DSi, an ESP32-S3 or ESP32-C3 board with native USB, a Mac and
   [Claude Code](https://claude.com/claude-code). [Setup](docs/setup.md#what-you-need) lists the tested hardware.
2. Install:

   ```sh
   git clone https://github.com/JulioFerrero/pictochat-agents.git && cd pictochat-agents
   python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
   ```

3. Flash the ESP32 with `tools/flash_modem.sh`. It requires
   [ESP-IDF 5.4](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32s3/get-started/index.html).
4. Start with `./pictoagents start`.
5. On the DS, open PictoChat and join **Chat Room A**.

Agents use the computer only for trusted devices. `./pictoagents status` shows your DS's address; add it to
`pictoagents.json` as `{"trust": ["00:09:bf:12:34:56"]}`.

## Commands

Typed or handwritten in PictoChat:

| Command | |
|---|---|
| `@Name message` | Address an agent. Messages without a name go to the agent you last addressed. |
| `/new Name description` | Add an agent to the room. |
| `/new Name pc description` | Add an agent that works on the computer (trusted devices only). |
| `/agents` | List the agents and what each one is doing. |
| `/stop Name` | Remove an agent. |
| `/trust Nickname` | Trust another DS in the room (trusted devices only). |

Agents can also draw, on a blank message or on top of the one they received.

## Documentation

- [Setup](docs/setup.md): hardware, settings, the `pictoagents` command, commit signatures, testing without a DS
- [How it works](docs/how-it-works.md): the radio, the host and the agents, the layout, what is untested
- [Security](docs/security.md): what each device can do, and the limits

## Licence

GPL-3.0. Built on [LNYmachines/pictochat-host](https://github.com/LNYmachines/pictochat-host), which provides the
PictoChat host, the ESP32 radio firmware and the protocol research.

PictoChat and Nintendo DS are trademarks of Nintendo, and Claude is a trademark of Anthropic. This project isn't
affiliated with either.
