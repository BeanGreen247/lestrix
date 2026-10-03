import random

import pyte

from lestrix.screen import FastScreen, Feeder

PIECES = [
    "hello world", "x" * 100, "\r\n", "\n", "\r", "\t", "\b", "\x1b[31m", "\x1b[0m", "\x1b[1;4;38;5;200m",
    "\x1b[48;2;10;20;30m", "\x1b[m", "\x1b[2J", "\x1b[H", "\x1b[5;7H", "\x1b[3A", "\x1b[K", "\x1b[?25l", "\x1b[?25h",
    "\x1b]0;title\x07", "\x1b(0qq\x1b(B", "\x1b7", "\x1b8", "\x1bM", "漢字かな", "é", "\x1b[2;10r", "\x1b[L",
    "\x1b[M", "\x1b[3@", "\x1b[2P", "\x1bD", "\x1b[?7l", "\x1b[?7h", "\x1b[4h", "\x1b[4l", "ünïcode", "\x07",
]


def snapshot(screen):
    return (
        [[tuple(screen.buffer[y][x]) for x in range(screen.columns)] for y in range(screen.lines)],
        screen.cursor.x, screen.cursor.y, screen.cursor.hidden, sorted(screen.mode), screen.title,
    )


def test_fast_path_matches_pyte_on_random_streams():
    rnd = random.Random(7)
    for _ in range(60):
        data = "".join(rnd.choice(PIECES) for _ in range(rnd.randint(5, 120))).encode()
        ref = pyte.Screen(40, 12)
        pyte.ByteStream(ref).feed(data)
        fast = FastScreen(40, 12)
        feeder = Feeder(fast)
        pos = 0
        while pos < len(data):  # arbitrary chunk boundaries, including mid-sequence and mid-UTF-8
            n = rnd.randint(1, 37)
            feeder.feed(data[pos:pos + n])
            pos += n
        assert snapshot(fast) == snapshot(ref), data


def test_scrollback_keeps_lines_that_scroll_off():
    s = FastScreen(10, 3, history=100)
    f = Feeder(s)
    f.feed(b"".join(b"line%d\r\n" % i for i in range(8)))
    top = ["".join(c.data for c in (line[x] for x in range(10))).rstrip() for line in s.history.top]
    assert top[:3] == ["line0", "line1", "line2"] and len(top) == 6
