"""Small pixel-art icons in PictoChat's palette, so agents can use emoji.

PictoChat messages are pictures and its font has no emoji, so each supported emoji is drawn as an 11x11 icon instead:
inline in text (claude_agent.text_pictures) and with ["icon", name, x, y] in drawings (claude_agent.draw_picture).

Each icon is 11 rows of 11 characters; every character is one pixel:
    .  transparent      W  paper (white)    K  black        g  light grey
    M  magenta          P  pink             R  red          O  red-orange
    o  orange           Y  yellow           L  yellow-green G  green
    C  cyan-green       S  sky blue         B  blue         N  navy         V  violet
"""

ICON_SIZE = 11

PALETTE_LETTERS = {"W": 0, "K": 1, "g": 2, "M": 3, "P": 4, "R": 5, "O": 6, "o": 7, "Y": 8, "L": 9, "G": 10,
                   "C": 11, "S": 12, "B": 13, "N": 14, "V": 15}


DOT = [
    "...........",
    "...XXXXX...",
    "..XXXXXXX..",
    ".XXXXXXXXX.",
    ".XXXXXXXXX.",
    ".XXXXXXXXX.",
    ".XXXXXXXXX.",
    ".XXXXXXXXX.",
    "..XXXXXXX..",
    "...XXXXX...",
    "...........",
]


def _dot(letter):
    """A filled circle in one colour: the coloured status dots."""
    return [row.replace("X", letter) for row in DOT]


ICONS = {
    "check": [
        ".GGGGGGGGG.",
        "GGGGGGGGGGG",
        "GGGGGGGGWWG",
        "GGGGGGGWWWG",
        "GWWGGGWWWGG",
        "GWWWGWWWGGG",
        "GGWWWWWGGGG",
        "GGGWWWGGGGG",
        "GGGGWGGGGGG",
        "GGGGGGGGGGG",
        ".GGGGGGGGG.",
    ],
    "cross": [
        "RR.......RR",
        "RRR.....RRR",
        ".RRR...RRR.",
        "..RRR.RRR..",
        "...RRRRR...",
        "....RRR....",
        "...RRRRR...",
        "..RRR.RRR..",
        ".RRR...RRR.",
        "RRR.....RRR",
        "RR.......RR",
    ],
    "warning": [
        ".....o.....",
        "....oYo....",
        "....oKo....",
        "...oYKYo...",
        "...oYKYo...",
        "..oYYKYYo..",
        "..oYYKYYo..",
        ".oYYYYYYYo.",
        ".oYYYKYYYo.",
        "oYYYYYYYYYo",
        "ooooooooooo",
    ],
    "hourglass": [
        "KKKKKKKKKKK",
        ".K.......K.",
        ".KoooooooK.",
        "..KoooooK..",
        "...KoooK...",
        "....KoK....",
        "...K.o.K...",
        "..K..o..K..",
        ".K..ooo..K.",
        ".KoooooooK.",
        "KKKKKKKKKKK",
    ],
    "star": [
        ".....o.....",
        "....oYo....",
        "....oYo....",
        "oooooYooooo",
        "oYYYYYYYYYo",
        ".oYYYYYYYo.",
        "..oYYYYYo..",
        "..oYYoYYo..",
        ".oYYo.oYYo.",
        ".oYo...oYo.",
        ".oo.....oo.",
    ],
    "heart": [
        "...........",
        ".RRR...RRR.",
        "RRWRR.RRRRR",
        "RWRRRRRRRRR",
        "RRRRRRRRRRR",
        "RRRRRRRRRRR",
        ".RRRRRRRRR.",
        "..RRRRRRR..",
        "...RRRRR...",
        "....RRR....",
        ".....R.....",
    ],
    "sun": [
        ".....Y.....",
        ".Y...Y...Y.",
        "..Y.ooo.Y..",
        "...oYYYo...",
        "..oYYYYYo..",
        "YYoYYYYYoYY",
        "..oYYYYYo..",
        "...oYYYo...",
        "..Y.ooo.Y..",
        ".Y...Y...Y.",
        ".....Y.....",
    ],
    "cloud": [
        "...........",
        "...........",
        "....SSS....",
        "...SWWWS...",
        ".SSSWWWWS..",
        "SWWWWWWWWS.",
        "SWWWWWWWWWS",
        "SWWWWWWWWWS",
        ".SSSSSSSSS.",
        "...........",
        "...........",
    ],
    "rain": [
        "....SSS....",
        "...SWWWS...",
        ".SSSWWWWS..",
        "SWWWWWWWWS.",
        "SWWWWWWWWWS",
        ".SSSSSSSSS.",
        "...........",
        ".B..B..B...",
        "B..B..B....",
        "...........",
        "...........",
    ],
    "bug": [
        "....K.K....",
        ".....K.....",
        "...KKKKK...",
        "..RRRKRRR..",
        ".RRKRKRKRR.",
        ".RRRRKRRRR.",
        ".RKRRKRRKR.",
        ".RRRRKRRRR.",
        "..RRRKRRR..",
        "...RRKRR...",
        "...........",
    ],
    "rocket": [
        ".....S.....",
        "....SSS....",
        "....gWg....",
        "...gWBWg...",
        "...gWWWg...",
        "...gWWWg...",
        "...gWWWg...",
        "..RgWWWgR..",
        ".RRgWWWgRR.",
        "...oOOOo...",
        "....oYo....",
    ],
    "folder": [
        "...........",
        "ooooo......",
        "oYYYYo.....",
        "oooooooooo.",
        "oYYYYYYYYYo",
        "oYYYYYYYYYo",
        "oYYYYYYYYYo",
        "oYYYYYYYYYo",
        "oYYYYYYYYYo",
        "ooooooooooo",
        "...........",
    ],
    "gear": [
        "....KKK....",
        ".K..KKK..K.",
        ".KKKKKKKKK.",
        "..KKK.KKK..",
        "KKKK...KKKK",
        "KKK.....KKK",
        "KKKK...KKKK",
        "..KKK.KKK..",
        ".KKKKKKKKK.",
        ".K..KKK..K.",
        "....KKK....",
    ],
    "bulb": [
        "....ooo....",
        "...oYYYo...",
        "..oYWYYYo..",
        "..oWYYYYo..",
        "..oYYYYYo..",
        "...oYYYo...",
        "....oYo....",
        "....KKK....",
        "....KKK....",
        ".....K.....",
        "...........",
    ],
    "fire": [
        ".....R.....",
        "....RR.....",
        "....RRR..R.",
        "...RRORR.R.",
        "..RRROORRR.",
        ".RRROOoORR.",
        ".RROOoYoORR",
        ".RROoYYYoRR",
        ".RROoYYYoOR",
        "..ROOoYoOR.",
        "...RRRRRR..",
    ],
    "smile": [
        "...ooooo...",
        "..oYYYYYo..",
        ".oYYYYYYYo.",
        "oYYKYYYKYYo",
        "oYYKYYYKYYo",
        "oYYYYYYYYYo",
        "oYKYYYYYKYo",
        "oYYKKKKKYYo",
        ".oYYYYYYYo.",
        "..oYYYYYo..",
        "...ooooo...",
    ],
    "pencil": [
        ".........PP",
        "........PPP",
        ".......oPP.",
        "......ooo..",
        ".....ooo...",
        "....ooo....",
        "...ooo.....",
        "..ooo......",
        ".Wgo.......",
        ".KW........",
        "K..........",
    ],
    "gamepad": [
        "...........",
        "...........",
        ".KKKKKKKKK.",
        "KKKKKKKKKKK",
        "KWKKKKKKRKK",
        "WWWKKKKRKBK",
        "KWKKKKKKGKK",
        "KKKKKKKKKKK",
        "KKK.....KKK",
        ".K.......K.",
        "...........",
    ],
    "green": _dot("G"),
    "yellow": _dot("Y"),
    "orange": _dot("o"),
    "red": _dot("R"),
    "blue": _dot("B"),
}

# Emoji (and a few plain symbols) that agents may write, and the icon each one becomes.
EMOJI = {
    "✅": "check", "✔": "check", "✓": "check", "☑": "check",
    "❌": "cross", "✖": "cross", "✗": "cross", "❎": "cross",
    "⚠": "warning", "❗": "warning",
    "⏳": "hourglass", "⌛": "hourglass", "\U0001f552": "hourglass",
    "⭐": "star", "★": "star", "\U0001f31f": "star",
    "❤": "heart", "♥": "heart",
    "☀": "sun", "\U0001f31e": "sun",
    "☁": "cloud", "⛅": "cloud",
    "\U0001f327": "rain", "\U0001f326": "rain", "☔": "rain",
    "\U0001f41e": "bug", "\U0001f41b": "bug",
    "\U0001f680": "rocket",
    "\U0001f4c1": "folder", "\U0001f4c2": "folder",
    "⚙": "gear", "\U0001f527": "gear",
    "\U0001f4a1": "bulb",
    "\U0001f525": "fire",
    "\U0001f642": "smile", "\U0001f60a": "smile", "\U0001f600": "smile", "\U0001f604": "smile",
    "✏": "pencil", "\U0001f4dd": "pencil",
    "\U0001f3ae": "gamepad",
    "\U0001f7e2": "green", "\U0001f7e1": "yellow", "\U0001f7e0": "orange", "\U0001f534": "red", "\U0001f535": "blue",
}

# In text, each icon travels as one character from Unicode's private use area until it is drawn.
PLACEHOLDER_BASE = 0xE000
ICON_NAMES = sorted(ICONS)


def placeholder(name):
    return chr(PLACEHOLDER_BASE + ICON_NAMES.index(name))


def icon_for_placeholder(char):
    """The icon name a placeholder character stands for, or None for ordinary characters."""
    index = ord(char) - PLACEHOLDER_BASE
    return ICON_NAMES[index] if 0 <= index < len(ICON_NAMES) else None


def emoji_to_placeholders(text):
    """Supported emoji -> placeholders; the variation selector and joiner that often follow emoji are dropped."""
    text = text.replace("️", "").replace("‍", "")
    return "".join(placeholder(EMOJI[char]) if char in EMOJI else char for char in text)


def draw_icon(draw, left, top, name):
    """Draw an icon with its top-left corner at (left, top) on a palette-index image."""
    for row_number, row in enumerate(ICONS[name]):
        for column, letter in enumerate(row):
            if letter in PALETTE_LETTERS:
                draw.point((left + column, top + row_number), fill=PALETTE_LETTERS[letter])
