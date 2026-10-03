"""A faster pyte screen.

pyte.HistoryScreen intercepts every attribute access to wrap event handlers, which
costs more than the parsing itself. This screen keeps scrollback with a plain
override of index() and adds a fast path to draw() for ordinary ASCII text.
"""

from __future__ import annotations

import codecs
import re
from collections import deque
from types import SimpleNamespace

import pyte
from pyte import modes as mo
from pyte.screens import Margins


class _Cells(dict):
    """char -> Char for one set of attributes. Char tuples are immutable, so cells are shared."""

    def __init__(self, attrs):
        super().__init__()
        self.attrs = attrs

    def __missing__(self, ch: str):
        cell = self[ch] = self.attrs._replace(data=ch)
        return cell


class FastScreen(pyte.Screen):
    on_title = None
    on_bell = None

    def __init__(self, columns: int, lines: int, history: int = 5000):
        self.history = SimpleNamespace(top=deque(maxlen=history))
        self._cells: dict = {}  # attrs -> _Cells
        self._all_rows = frozenset(range(lines))
        super().__init__(columns, lines)

    def set_title(self, param: str) -> None:
        super().set_title(param)
        if self.on_title:
            self.on_title(param)

    def bell(self, *args) -> None:
        if self.on_bell:
            self.on_bell()

    def resize(self, lines=None, columns=None) -> None:
        super().resize(lines, columns)
        self._all_rows = frozenset(range(self.lines))

    def index(self) -> None:
        top, bottom = self.margins or Margins(0, self.lines - 1)
        if self.cursor.y != bottom:
            return self.cursor_down()
        buf = self.buffer
        if top == 0:  # a full-screen scroll: the top line goes to scrollback
            self.history.top.append(buf[0])
        self.dirty.update(self._all_rows)
        # same result as pyte's per-row loop, but the copying runs in C
        buf.update(zip(range(top, bottom), map(buf.__getitem__, range(top + 1, bottom + 1))))
        buf.pop(bottom, None)

    def draw(self, data: str) -> None:
        if not (data.isascii() and data.isprintable()) or mo.IRM in self.mode or mo.DECAWM not in self.mode \
                or self.charset or self.g0_charset is not pyte.charsets.LAT1_MAP:
            return super().draw(data)
        cur = self.cursor
        cols = self.columns
        buf = self.buffer
        attrs = cur.attrs
        cells = self._cells.get(attrs)
        if cells is None:
            if len(self._cells) > 256:
                self._cells.clear()
            cells = self._cells[attrs] = _Cells(attrs)
        get_cell = cells.__getitem__
        x = cur.x
        line = buf[cur.y]
        while data:
            if x == cols:  # autowrap
                self.dirty.add(cur.y)
                cur.x = x
                self.carriage_return()
                self.linefeed()
                line = buf[cur.y]
                x = cur.x
            n = min(len(data), cols - x)
            line.update(zip(range(x, x + n), map(get_cell, data[:n])))  # C-speed loop over the run
            x += n
            data = data[n:]
        cur.x = x
        self.dirty.add(cur.y)


_TOKEN = re.compile(
    r"(?P<text>[^\x00-\x1f\x7f-\x9f]+)"
    r"|(?P<sgr>\x1b\[[0-9;]*m)"
    r"|(?P<crlf>\r\n)"
    r"|(?P<nl>\n)"
    r"|(?P<cr>\r)"
    r"|(?P<esc>\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\)|[()*+%#][ -~]|(?![\[\]()*+%#])[ -~]))"
    r"|(?P<partial>\x1b(?:\[[0-?]*[ -/]*|\][^\x07\x1b]*\x1b?|[()*+%#])?\Z)"
    r"|(?P<ctl>[\x00-\x1f\x7f-\x9f])"
)
_MAX_HELD = 4096
_SGR: dict[str, tuple] = {}


class Feeder:
    """Feeds terminal output to a screen, handling plain text, newlines and colour changes
    directly and leaving every other sequence to pyte's parser. Sequences are always passed
    to pyte whole, so its state machine stays in the ground state between calls."""

    def __init__(self, screen: FastScreen):
        self.screen = screen
        self.stream = pyte.Stream(screen)
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._held = ""

    def attach(self, screen: FastScreen) -> None:
        self.stream.detach(self.screen)
        self.screen = screen
        self.stream.attach(screen)

    def feed(self, data: bytes) -> None:
        text = self._held + self._decoder.decode(data)
        self._held = ""
        screen = self.screen
        stream_feed = self.stream.feed
        for m in _TOKEN.finditer(text):
            kind = m.lastgroup
            if kind == "text":
                screen.draw(m[0])
            elif kind == "sgr":
                tok = m[0]
                params = _SGR.get(tok)
                if params is None:
                    body = tok[2:-1]
                    params = tuple(int(p) if p else 0 for p in body.split(";")) if body else (0,)
                    if len(_SGR) < 4096:
                        _SGR[tok] = params
                screen.select_graphic_rendition(*params)
            elif kind == "crlf":
                screen.carriage_return()
                screen.linefeed()
            elif kind == "nl":
                screen.linefeed()
            elif kind == "cr":
                screen.carriage_return()
            elif kind == "esc":
                stream_feed(m[0])
            elif kind == "partial":
                tail = m[0]
                if len(tail) <= _MAX_HELD:
                    self._held = tail
                # an over-long unterminated sequence is dropped rather than buffered forever
            else:
                ch = m[0]
                if ch not in "\x1b\x18\x1a":  # stray ESC / CAN / SUB just cancel
                    stream_feed(ch)
