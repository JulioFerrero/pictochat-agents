# Security

PictoChat has no passwords: anyone within about 20 metres with a DS can enter the room.

- **Anyone can chat** with the agents and use their web search.
- **Only trusted DSes can make agents use the computer.** Every DS has a hardware (MAC) address that PictoChat can't
  change, unlike the nickname. Agents that work on the computer run a Claude Code hook, `agents/gate.py`, before
  every terminal or file tool. It allows the tool only when every message the agent is answering came from a trusted
  address. Claude Code enforces the hook, so persuading the agent doesn't get past it.
- **The room doesn't see the work.** While a computer agent works, the room only reads "working on it... (using the
  terminal)". Commands and paths stay on the status screen and in the logs.
- **Local data stays local.** Logs and `captures/` hold the room's messages and the DSes' addresses; git ignores
  them.

## Limits

- **The address can be copied.** A DS sends it unencrypted, so someone with a modified radio, like this project's,
  could copy it. A normal DS can't.
- **The terminal isn't confined.** Computer agents start in `~/PictoAgents/<Name>`, but their terminal can reach the
  rest of your account. Trust only DSes you own.
- **Subscriptions are personal.** A Claude subscription is for your own use of Claude Code. If other people use your
  agents, Anthropic's [terms](https://code.claude.com/docs/en/legal-and-compliance.md) ask for an API key instead.
