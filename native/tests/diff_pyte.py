"""Differential test: the C terminal core must agree with pyte on random escape-sequence streams.

Run from native/: ../.venv/bin/python tests/diff_pyte.py [seeds]
"""
import ctypes
import os
import random
import sys
import unicodedata

import pyte
from pyte.graphics import FG_BG_256

LIB = os.path.join(os.path.dirname(__file__), "..", "build", "libvt.so")
vt = ctypes.CDLL(LIB)


class Cell(ctypes.Structure):
    _fields_ = [("cp", ctypes.c_uint32), ("sf", ctypes.c_uint32)]


class Style(ctypes.Structure):
    _fields_ = [("fg", ctypes.c_uint32), ("bg", ctypes.c_uint32), ("attrs", ctypes.c_uint16)]


vt.vt_new.restype = ctypes.c_void_p
vt.vt_new.argtypes = [ctypes.c_int] * 3
vt.vt_feed.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
vt.vt_line.restype = ctypes.POINTER(Cell)
vt.vt_line.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
vt.vt_style.restype = ctypes.POINTER(Style)
vt.vt_style.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
vt.vt_comb_char.restype = ctypes.c_uint32
vt.vt_comb_char.argtypes = [ctypes.c_void_p, ctypes.c_uint]
for fn in ("vt_cursor_x", "vt_cursor_y"):
    getattr(vt, fn).argtypes = [ctypes.c_void_p]
vt.vt_modes.argtypes = [ctypes.c_void_p]
vt.vt_modes.restype = ctypes.c_uint32
vt.vt_free.argtypes = [ctypes.c_void_p]

NAMES = ["black", "red", "green", "brown", "blue", "magenta", "cyan", "white"]
BOLD, ITALIC, UNDER, REVERSE, STRIKE = 1, 4, 8, 32, 128
CURSOR_VISIBLE = 1 << 5


def color(c: int) -> str:
    kind = c >> 24
    if kind == 0:
        return "default"
    if kind == 1:
        i = c & 0xFF
        if i < 8:
            return NAMES[i]
        if i < 16:
            return "bright" + NAMES[i - 8]
        return FG_BG_256[i]
    return "%02x%02x%02x" % ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def c_cell(t, cell):
    flags = cell.sf & 0xFF
    if flags & 2:            # tail of a wide char
        return ("", "", "", 0)
    ch = cell.cp & 0x1FFFFF
    if ch == 0 or ch == 32:
        return (" ", "", "", 0)  # blank: colours are not compared (erase colouring differs from pyte)
    s = vt.vt_style(t, cell.sf >> 8).contents
    text = chr(ch)
    comb = vt.vt_comb_char(t, cell.cp >> 21)
    if comb:
        text = unicodedata.normalize("NFC", text + chr(comb))
    attr = (s.attrs & BOLD) | (s.attrs & ITALIC) | (s.attrs & UNDER) | (s.attrs & REVERSE) | (s.attrs & STRIKE)
    return (text, color(s.fg), color(s.bg), attr)


def p_cell(c):
    if c.data == "":
        return ("", "", "", 0)
    if c.data == " ":
        return (" ", "", "", 0)
    flags = (BOLD if c.bold else 0) | (ITALIC if c.italics else 0) | (UNDER if c.underscore else 0) \
        | (REVERSE if c.reverse else 0) | (STRIKE if c.strikethrough else 0)
    return (c.data, c.fg, c.bg, flags)


def snapshot_c(t, cols, rows):
    grid = []
    for y in range(rows):
        line = vt.vt_line(t, y, None)
        grid.append([c_cell(t, line[x]) for x in range(cols)])
    return grid, min(vt.vt_cursor_x(t), cols), vt.vt_cursor_y(t), bool(vt.vt_modes(t) & CURSOR_VISIBLE)


def snapshot_p(s, cols, rows):
    grid = [[p_cell(s.buffer[y][x]) for x in range(cols)] for y in range(rows)]
    return grid, min(s.cursor.x, cols), s.cursor.y, not s.cursor.hidden


PIECES = [
    "hello world", "x" * 100, "\r\n", "\n", "\r", "\t", "\b", "\x1b[31m", "\x1b[0m", "\x1b[1;4;38;5;200m",
    "\x1b[48;2;10;20;30m", "\x1b[m", "\x1b[H", "\x1b[5;7H", "\x1b[3A", "\x1b[2B", "\x1b[4C", "\x1b[2D",
    "\x1b[1K", "\x1b[2K", "\x1b[1J", "\x1b[2J", "\x1b[?25l", "\x1b[?25h", "\x1b]0;title\x07",
"\x1bM", "\x1bD", "漢字かな", "é", "\x1b[4h", "\x1b[4l", "ünïcode",
    "\x07", "\x1b[7m", "\x1b[27m", "\x1b[9m", "\x1b[3m", "\x1b[10;20H", "\x1b[G", "\x1b[7G", "\x1b[3d", "\x1bH",
    "\x1b[g", "\x1b[3g", "\x1b[1;31;42m", "\x1b[91;104m", "A" * 37,
]


def run(seed: int, cols=40, rows=12) -> bool:
    rnd = random.Random(seed)
    data = ("".join(rnd.choice(PIECES) for _ in range(rnd.randint(5, 150)))).encode()
    ref = pyte.Screen(cols, rows)
    pyte.ByteStream(ref).feed(data)
    t = vt.vt_new(cols, rows, 100)
    pos = 0
    while pos < len(data):  # arbitrary chunk boundaries, including mid-sequence and mid-UTF-8
        n = rnd.randint(1, 41)
        vt.vt_feed(t, data[pos:pos + n], len(data[pos:pos + n]))
        pos += n
    a, b = snapshot_c(t, cols, rows), snapshot_p(ref, cols, rows)
    vt.vt_free(t)
    if a == b:
        return True
    ga, gb = a[0], b[0]
    print(f"seed {seed}: MISMATCH cursor {a[1:]} vs {b[1:]}")
    shown = 0
    for y in range(rows):
        for x in range(cols):
            if ga[y][x] != gb[y][x] and shown < 4:
                print(f"  cell ({x},{y}): C={ga[y][x]} pyte={gb[y][x]}")
                shown += 1
    print("  data:", data[:300])
    return False


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    bad = sum(not run(s) for s in range(n))
    print(f"{n - bad}/{n} streams identical")
    sys.exit(1 if bad else 0)
