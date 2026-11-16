"""The status screen for the radio board (or the CYD): rooms, agents and what each one is doing, people, radio.

render(room, agents, trusted) draws 320x240 in the same DS style as the room view in room_display.py, which also
turns it into blits for the screen (agents/manager.py sends it through room_display.ModemScreen).
`room` is a room_display.RoomState. `agents` is a list of dicts: name, colour (DS favourite colour index),
kind ("chat"/"pc"), state ("idle", "thinking", "tool", "writing"), detail (text), since (time.time() of the last
state change), msgs (count). `trusted` holds the MACs of the DSes that may use the Mac (they get a key).
"""
import time

from room_display import (BAR_COLOUR, BAR_TEXT_COLOUR, FAVOURITE_COLOURS, FOOTER_HEIGHT, HEADER_FONT, HEADER_HEIGHT,
                          HEIGHT, SMALL_FONT, TAB_FONT, WHITE, WIDTH, blank_screen, draw_text, readable_on,
                          signal_bars)

AGENT_ROW_FILL, AGENT_ROW_OUTLINE = (248, 249, 251), (214, 218, 226)
MUTED = (120, 126, 140)
DARK = (40, 44, 56)
OUR_ROOM_COLOUR, OTHER_ROOM_COLOUR = (52, 120, 246), (200, 204, 212)
AGENT_ROW_HEIGHT = 24
MAX_AGENT_ROWS = 6


def ago(timestamp, now):
    """How long ago, shortly: 42s, 5m, 1h05."""
    seconds = max(0, int(now - timestamp))
    if seconds < 60:
        return "%ds" % seconds
    if seconds < 3600:
        return "%dm" % (seconds // 60)
    return "%dh%02d" % (seconds // 3600, seconds // 60 % 60)


def state_icon(draw, x, y, state, now, colour):
    """A 12x12 icon: zz idle, three dots thinking (animated), a cog-like square for tools, a pencil writing."""
    if state == "idle":
        draw_text(draw, (x, y - 1), "z", MUTED, SMALL_FONT)
        draw_text(draw, (x + 5, y - 4), "z", MUTED, SMALL_FONT)
    elif state == "thinking":
        lit_dot = int(now * 3) % 3
        for i in range(3):
            draw.ellipse((x + i * 4, y + 4, x + i * 4 + 2, y + 6), fill=colour if i == lit_dot else MUTED)
    elif state == "tool":
        corner = int(now * 4) % 4                 # a small spinning square
        draw.rectangle((x + 2, y + 2, x + 9, y + 9), outline=DARK)
        corner_x, corner_y = [(x + 2, y + 2), (x + 9, y + 2), (x + 9, y + 9), (x + 2, y + 9)][corner]
        draw.rectangle((corner_x - 1, corner_y - 1, corner_x + 1, corner_y + 1), fill=colour)
    else:                                         # writing: a pencil
        draw.line((x + 1, y + 10, x + 9, y + 2), fill=DARK, width=2)
        draw.point((x + 1, y + 11), fill=DARK)


def draw_header(draw, answered, now):
    draw.rectangle((0, 0, WIDTH, HEADER_HEIGHT - 1), fill=BAR_COLOUR)
    draw_text(draw, (6, 4), "PICTOCHAT  AI", BAR_TEXT_COLOUR, HEADER_FONT)
    draw_text(draw, (WIDTH - 66, 5), time.strftime("%H:%M", time.localtime(now)), BAR_TEXT_COLOUR, SMALL_FONT)
    signal_bars(draw, WIDTH - 22, 4, answered)


def draw_rooms(draw, room_letter, people_count, agent_count):
    """Rooms A-D (ours is room_letter, with its member count), then how many people and AIs."""
    y = 25
    draw_text(draw, (6, y + 2), "ROOMS", MUTED, SMALL_FONT)
    for i, letter in enumerate("ABCD"):
        x = 52 + i * 34
        ours = letter == room_letter
        draw.rounded_rectangle((x, y, x + 28, y + 14), radius=3, fill=OUR_ROOM_COLOUR if ours else OTHER_ROOM_COLOUR)
        draw_text(draw, (x + 4, y + 1), letter, WHITE, TAB_FONT)
        if ours:
            draw_text(draw, (x + 14, y + 2), str(1 + people_count), WHITE, SMALL_FONT)
    draw_text(draw, (196, y + 2), "%d people  %d AIs" % (people_count, agent_count), DARK, SMALL_FONT)


def fit(draw, text, max_width, font):
    """The text, cut so that it fits in max_width pixels (long agent names in their 62-px tab)."""
    while text and draw.textlength(text, font=font) > max_width:
        text = text[:-1]
    return text


def draw_agent_row(draw, y, agent, now):
    """One agent: name tab, PC badge, state icon, what it is doing and since when."""
    colour = FAVOURITE_COLOURS[agent["colour"] & 15]
    draw.rectangle((4, y, WIDTH - 5, y + AGENT_ROW_HEIGHT - 3), fill=AGENT_ROW_FILL, outline=AGENT_ROW_OUTLINE)
    draw.rectangle((4, y, 66, y + AGENT_ROW_HEIGHT - 3), fill=colour)
    draw_text(draw, (8, y + 4), fit(draw, agent["name"], 56, TAB_FONT), readable_on(colour), TAB_FONT)
    on_pc = agent.get("kind") == "pc"
    if on_pc:
        draw.rounded_rectangle((70, y + 5, 86, y + 15), radius=2, fill=DARK)
        draw_text(draw, (72, y + 5), "PC", WHITE, SMALL_FONT)
    x = 92 if on_pc else 72
    state_icon(draw, x, y + 4, agent["state"], now, colour)
    detail = agent.get("detail") or agent["state"]
    draw_text(draw, (x + 18, y + 4), detail[:30], DARK if agent["state"] != "idle" else MUTED, SMALL_FONT)
    draw_text(draw, (WIDTH - 34, y + 4), ago(agent["since"], now), MUTED, SMALL_FONT)


def draw_agents(draw, agents, now):
    y = 46
    draw_text(draw, (6, y), "AGENTS", MUTED, SMALL_FONT)
    y += 13
    for agent in agents[:MAX_AGENT_ROWS]:
        draw_agent_row(draw, y, agent, now)
        y += AGENT_ROW_HEIGHT
    if not agents:
        draw_text(draw, (8, y + 2), "no agents yet: write /new Name on the DS", MUTED, SMALL_FONT)


def draw_people(draw, people, aids, trusted):
    """The people in the room as name tabs, as many as fit; trusted ones get a small key."""
    y = HEIGHT - FOOTER_HEIGHT - 20
    draw_text(draw, (6, y + 3), "IN ROOM", MUTED, SMALL_FONT)
    x = 56
    for member_id in sorted(people):
        person = people[member_id]
        mac = next((ds_mac for ds_mac, aid in aids.items() if aid == member_id), "")
        colour = FAVOURITE_COLOURS[person["colour"] & 15]
        tab_width = 8 + 7 * len(person["name"][:10]) + (8 if mac in trusted else 0)
        if x + tab_width > WIDTH - 4:
            break
        draw.rectangle((x, y + 1, x + tab_width, y + 15), fill=colour)
        draw_text(draw, (x + 4, y + 2), person["name"][:10], readable_on(colour), TAB_FONT)
        if mac in trusted:                        # a small key: this DS may use the Mac
            key_x = x + tab_width - 8
            draw.ellipse((key_x, y + 5, key_x + 4, y + 9), outline=readable_on(colour))
            draw.line((key_x + 2, y + 9, key_x + 2, y + 13), fill=readable_on(colour))
        x += tab_width + 4


def draw_footer(draw, agents, radio, rssi, answered):
    """Radio, channel, signal level, share of polls answered and the agents' message count."""
    draw.rectangle((0, HEIGHT - FOOTER_HEIGHT, WIDTH, HEIGHT), fill=BAR_COLOUR)
    parts = ["ESP32 radio" if "ESP32" in (radio or "") else (radio or "radio"), "ch 1"]
    if rssi is not None:
        parts.append("%d dBm" % rssi)
    if answered is not None:
        parts.append("%d%% polls" % round(answered * 100))
    parts.append("%d msgs" % sum(agent.get("msgs", 0) for agent in agents))
    draw_text(draw, (5, HEIGHT - FOOTER_HEIGHT + 3), "   ".join(parts), BAR_TEXT_COLOUR, SMALL_FONT)


def render(room, agents, trusted=(), now=None, room_letter="A"):
    now = now or time.time()
    image, draw = blank_screen()
    with room.lock:
        members = dict(room.members)
        aids = dict(room.aids)
        rssi, answered, radio = room.rssi, room.answered, room.radio
    people = {member_id: member for member_id, member in members.items() if member_id and not member.get("agent")}
    draw_header(draw, answered, now)
    draw_rooms(draw, room_letter, len(people), len(agents))
    draw_agents(draw, agents, now)
    draw_people(draw, people, aids, trusted)
    draw_footer(draw, agents, radio, rssi, answered)
    return image
