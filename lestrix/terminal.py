"""Embedded terminal: a pty backend feeding a pyte screen, drawn by a QWidget."""

from __future__ import annotations

import os
import re
import select
import shutil
import subprocess
import sys
import time
from collections import deque
from itertools import groupby, repeat
from operator import itemgetter
from urllib.parse import unquote

import pyte
from PyQt6.QtCore import QObject, QPoint, QSocketNotifier, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QFontDatabase, QFontMetrics, QGuiApplication, QPainter, QPen, QPixmap
from PyQt6.QtOpenGLWidgets import QOpenGLWidget
from PyQt6.QtWidgets import QApplication, QMenu, QWidget

from .screen import FastScreen, Feeder

IS_WIN = sys.platform == "win32"
IS_MAC = sys.platform == "darwin"
if not IS_WIN:
    import fcntl
    import pty
    import signal
    import struct
    import termios

_STYLE = itemgetter(1, 2, 3, 5, 7)  # Char fields: fg, bg, bold, underscore, reverse

# DECCKM (application cursor keys) as stored by pyte: private mode 1, shifted by 5
DECCKM = 1 << 5

ANSI: dict[str, str] = {}
DEFAULT_FG = QColor("#d7dae0")
DEFAULT_BG = QColor("#16181d")
SELECTION = QColor(80, 120, 160, 120)


_QCOLORS: dict[str, QColor | None] = {}


def qcolor(name: str) -> QColor | None:
    """Cached QColor for a pyte colour name ('red', 'brightblue', 'ff8800'); None for 'default'."""
    try:
        return _QCOLORS[name]
    except KeyError:
        if name in ANSI:
            c = QColor(ANSI[name])
        elif len(name) == 6:
            c = QColor("#" + name)
        else:
            c = None
        _QCOLORS[name] = c
        return c


def apply_theme(theme) -> None:
    """Point the terminal palette at a theme (call update() on live terminals afterwards)."""
    global DEFAULT_FG, DEFAULT_BG, SELECTION
    from .theme import ansi_map

    ANSI.clear()
    ANSI.update(ansi_map(theme))
    _QCOLORS.clear()
    TerminalMixinBase.theme_epoch += 1
    DEFAULT_FG, DEFAULT_BG = QColor(theme.term_fg), QColor(theme.term_bg)
    SELECTION = QColor(theme.selection)
    SELECTION.setAlpha(150)


# ── Backends ─────────────────────────────────────────────────────────────────


class PosixPty(QObject):
    data = pyqtSignal(bytes)
    exited = pyqtSignal(int)

    def __init__(self, argv: list[str], cols: int, rows: int, env: dict[str, str], cwd: str | None = None):
        super().__init__()
        self.master, slave = pty.openpty()
        self._set_size(self.master, cols, rows)
        self.proc = subprocess.Popen(
            argv, stdin=slave, stdout=slave, stderr=slave, env=env, close_fds=True, cwd=cwd,
            start_new_session=True,
            # make the pty the controlling terminal so ssh can prompt for passwords
            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0),
        )
        os.close(slave)
        self._notifier = QSocketNotifier(self.master, QSocketNotifier.Type.Read, self)
        self._notifier.activated.connect(self._readable)
        self._alive = True

    @staticmethod
    def _set_size(fd: int, cols: int, rows: int) -> None:
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def _readable(self) -> None:
        chunk = b""
        try:
            chunk = os.read(self.master, 65536)
            # a flood arrives in small pty-sized reads: drain what is already waiting and
            # hand it over in one piece, so per-call overhead is paid once
            while len(chunk) < 262144 and select.select([self.master], [], [], 0)[0]:
                more = os.read(self.master, 65536)
                if not more:
                    break
                chunk += more
        except OSError:
            pass  # EIO once the child exits: deliver what was read, the next call sees EOF
        if chunk:
            self.data.emit(chunk)
        else:
            self._finish()

    def _finish(self) -> None:
        if not self._alive:
            return
        self._alive = False
        self._notifier.setEnabled(False)
        try:
            code = self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            code = self.proc.wait()
        os.close(self.master)
        self.exited.emit(code)

    def write(self, payload: bytes) -> None:
        if self._alive:
            try:
                os.write(self.master, payload)
            except OSError:
                pass

    def resize(self, cols: int, rows: int) -> None:
        if self._alive:
            self._set_size(self.master, cols, rows)

    @property
    def alive(self) -> bool:
        return self._alive

    def close(self) -> None:
        if not self._alive:
            return
        self._alive = False
        self._notifier.setEnabled(False)
        try:
            os.killpg(self.proc.pid, signal.SIGHUP)
        except OSError:
            pass
        try:
            self.proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        os.close(self.master)


class WinPty(QObject):
    """ConPTY via pywinpty. Reader runs in a thread; Qt queues the signal."""

    data = pyqtSignal(bytes)
    exited = pyqtSignal(int)

    def __init__(self, argv: list[str], cols: int, rows: int, env: dict[str, str], cwd: str | None = None):
        super().__init__()
        import threading

        from winpty import PtyProcess

        self.proc = PtyProcess.spawn(argv, dimensions=(rows, cols), env=env, cwd=cwd)
        self._alive = True
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self) -> None:
        while True:
            try:
                text = self.proc.read(65536)
            except EOFError:
                break
            if text:
                self.data.emit(text.encode("utf-8", "replace"))
        self._alive = False
        self.exited.emit(self.proc.exitstatus or 0)

    def write(self, payload: bytes) -> None:
        if self._alive:
            self.proc.write(payload.decode("utf-8", "replace"))

    def resize(self, cols: int, rows: int) -> None:
        if self._alive:
            self.proc.setwinsize(rows, cols)

    @property
    def alive(self) -> bool:
        return self._alive

    def close(self) -> None:
        if self._alive:
            self._alive = False
            self.proc.terminate(force=True)


_SHELLS = ("bash", "zsh", "fish", "sh", "dash", "ksh", "tcsh", "csh", "nu", "xonsh", "elvish", "pwsh")
_WIN_SHELLS = ("pwsh", "powershell", "cmd", "wsl", "bash")


def _shell_argv(path: str) -> list[str]:
    # macOS terminals start login shells; elsewhere interactive non-login is the norm
    return [path, "-l"] if IS_MAC and os.path.basename(path) in ("bash", "zsh", "fish", "sh", "ksh") else [path]


def detect_shells() -> list[tuple[str, list[str]]]:
    """Installed local shells as (label, argv); the user's default comes first."""
    found: dict[str, str] = {}
    if IS_WIN:
        for name in _WIN_SHELLS:
            if path := shutil.which(name):
                found[name] = path
        return [(n, [p]) for n, p in found.items()]
    default = os.environ.get("SHELL")
    if default and os.path.exists(default):
        found[os.path.basename(default)] = default
    for name in _SHELLS:
        if name not in found and (path := shutil.which(name)):
            found[name] = path
    try:
        for line in open("/etc/shells"):
            line = line.strip()
            if line.startswith("/") and os.access(line, os.X_OK):
                found.setdefault(os.path.basename(line), line)
    except OSError:
        pass
    return [(n, _shell_argv(p)) for n, p in found.items()]


def default_shell() -> list[str]:
    shells = detect_shells()
    return shells[0][1] if shells else (["cmd.exe"] if IS_WIN else ["/bin/sh"])


_MODE_RE = re.compile(rb"\x1b\[\?([\d;]+)([hl])")
_PARTIAL_RE = re.compile(rb"\x1b\[\?[\d;]*$|\x1b\]7;[^\x07\x1b]*$")
_OSC7_RE = re.compile(rb"\x1b\]7;file://[^/\x07\x1b]*(/[^\x07\x1b]*)(?:\x07|\x1b\\)")
_ALT_MODES = {47, 1047, 1049}
_MOUSE_MODES = {1000, 1002, 1003}


# ── Screen ───────────────────────────────────────────────────────────────────


_Screen = FastScreen


# ── Widget ───────────────────────────────────────────────────────────────────

_KEYS = {
    Qt.Key.Key_Return: b"\r", Qt.Key.Key_Enter: b"\r", Qt.Key.Key_Backspace: b"\x7f",
    Qt.Key.Key_Tab: b"\t", Qt.Key.Key_Escape: b"\x1b",
    Qt.Key.Key_Home: b"\x1b[H", Qt.Key.Key_End: b"\x1b[F",
    Qt.Key.Key_Insert: b"\x1b[2~", Qt.Key.Key_Delete: b"\x1b[3~",
    Qt.Key.Key_PageUp: b"\x1b[5~", Qt.Key.Key_PageDown: b"\x1b[6~",
    Qt.Key.Key_F1: b"\x1bOP", Qt.Key.Key_F2: b"\x1bOQ", Qt.Key.Key_F3: b"\x1bOR",
    Qt.Key.Key_F4: b"\x1bOS", Qt.Key.Key_F5: b"\x1b[15~", Qt.Key.Key_F6: b"\x1b[17~",
    Qt.Key.Key_F7: b"\x1b[18~", Qt.Key.Key_F8: b"\x1b[19~", Qt.Key.Key_F9: b"\x1b[20~",
    Qt.Key.Key_F10: b"\x1b[21~", Qt.Key.Key_F11: b"\x1b[23~", Qt.Key.Key_F12: b"\x1b[24~",
}
_CTRL_SYMBOLS = {
    Qt.Key.Key_BracketLeft: b"\x1b", Qt.Key.Key_Backslash: b"\x1c", Qt.Key.Key_BracketRight: b"\x1d",
    Qt.Key.Key_AsciiCircum: b"\x1e", Qt.Key.Key_Underscore: b"\x1f", Qt.Key.Key_Slash: b"\x1f",
    Qt.Key.Key_2: b"\x00",
}
_ARROWS = {Qt.Key.Key_Up: b"A", Qt.Key.Key_Down: b"B", Qt.Key.Key_Right: b"C", Qt.Key.Key_Left: b"D"}


def mono_font(size: int) -> QFont:
    font = QFontDatabase.systemFont(QFontDatabase.SystemFont.FixedFont)
    for family in ("JetBrains Mono", "DejaVu Sans Mono", "Cascadia Mono", "Menlo", "Consolas"):
        if family in QFontDatabase.families():
            font.setFamily(family)
            break
    font.setPointSize(size)
    font.setStyleHint(QFont.StyleHint.Monospace)
    return font


class TerminalMixinBase:
    theme_epoch = 0  # bumped on theme change so every terminal drops its cached rows


class _TerminalMixin(TerminalMixinBase):
    """Terminal behaviour shared by the OpenGL and software-painted widgets."""

    SCROLLBACK = 5000
    PAD = 8  # breathing room between the widget edge and the text grid

    def __init__(self, argv: list[str], font_size: int = 11, parent=None, cwd: str | None = None):
        super().__init__(parent)
        self.argv = argv
        self.cwd = cwd
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setCursor(Qt.CursorShape.IBeamCursor)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)
        self._font = mono_font(font_size)
        self._bold = QFont(self._font)
        self._bold.setBold(True)
        self._measure()
        self.cols, self.rows = 80, 24
        self.offset = 0  # lines scrolled back
        self._sel: tuple[tuple[int, int], tuple[int, int]] | None = None
        self._dragging = False
        self.backend: PosixPty | WinPty | None = None
        self._exited = False
        self.alt = False
        self.bracketed = False
        self.mouse = self.mouse_motion = self.mouse_sgr = False
        self._held = b""
        self._mouse_btn = None
        self._row_cache: dict[int, QPixmap] = {}
        self._cache_key = None

        self._new_screen()
        self._repaint_timer = QTimer(self, singleShot=True, interval=12)
        self._repaint_timer.timeout.connect(self._flush_paint)
        self._pending_bytes = 0
        self._last_flush = 0.0
        self._spawn()

    # -- setup ---------------------------------------------------------------

    def _measure(self) -> None:
        fm = QFontMetrics(self._font)
        self.cw = max(1, fm.horizontalAdvance("M"))
        self.ch = fm.height()
        self.ascent = fm.ascent()

    def _make_screen(self, history: int) -> _Screen:
        screen = _Screen(self.cols, self.rows, history=history)
        screen.on_title = self.title_changed.emit
        screen.on_bell = self.bell.emit
        screen.write_process_input = lambda s: self.send(s.encode())
        return screen

    def _new_screen(self) -> None:
        self.alt = False
        self.bracketed = self.mouse = self.mouse_motion = self.mouse_sgr = False
        self._held = b""
        self.screen = self._main = self._make_screen(self.SCROLLBACK)
        self.feeder = Feeder(self.screen)

    def _set_alt(self, on: bool) -> None:
        self.alt = on
        self.screen = self._make_screen(1) if on else self._main
        self.feeder.attach(self.screen)
        self.offset = 0
        self._sel = None

    def _feed(self, data: bytes) -> None:
        """Feed pyte, tracking the private modes it ignores (alt screen, mouse, bracketed paste)."""
        data = self._held + data
        self._held = b""
        if m := _PARTIAL_RE.search(data):
            self._held, data = data[m.start():], data[:m.start()]
        for m in _OSC7_RE.finditer(data):
            self.cwd_changed.emit(unquote(m.group(1).decode("utf-8", "replace")))
        pos = 0
        for m in _MODE_RE.finditer(data):
            on = m.group(2) == b"h"
            params = {int(x) for x in m.group(1).split(b";") if x}
            if 2004 in params:
                self.bracketed = on
            if params & _MOUSE_MODES:
                self.mouse = on
                self.mouse_motion = on and bool(params & {1002, 1003})
            if 1006 in params:
                self.mouse_sgr = on
            if params & _ALT_MODES and on != self.alt:
                self.feeder.feed(data[pos:m.end()])
                self._set_alt(on)
                pos = m.end()
        self.feeder.feed(data[pos:])

    def _spawn(self) -> None:
        env = dict(os.environ, TERM="xterm-256color", COLORTERM="truecolor")
        if "UTF-8" not in env.get("LC_ALL", env.get("LANG", "")).upper().replace("UTF8", "UTF-8"):
            env.setdefault("LANG", "C.UTF-8")
        cls = WinPty if IS_WIN else PosixPty
        self.backend = cls(self.argv, self.cols, self.rows, env, self.cwd)
        self.backend.data.connect(self._on_data)
        self.backend.exited.connect(self._on_exit)
        self._exited = False

    def set_font_size(self, size: int) -> None:
        self._font.setPointSize(size)
        self._bold.setPointSize(size)
        self._measure()
        self._fit()
        self.update()

    # -- backend events --------------------------------------------------------

    def _on_data(self, chunk: bytes) -> None:
        self._feed(chunk)
        self.activity.emit()
        self.offset = 0
        self._sel = None
        self._pending_bytes += len(chunk)
        if self._repaint_timer.isActive():
            return
        # an echoed keystroke after idle repaints at once; a flood only needs ~20 fps, and
        # skipping the frames nobody can read is where most of the CPU goes
        interval = 50 if self._pending_bytes > 24_000 else 12
        wait = interval - (time.monotonic() - self._last_flush) * 1000
        if wait <= 0:
            self._flush_paint()
        else:
            self._repaint_timer.start(int(wait) + 1)

    def _flush_paint(self) -> None:
        self._pending_bytes = 0
        self._last_flush = time.monotonic()
        self.update()

    def _on_exit(self, code: int) -> None:
        self._exited = True
        if self.alt:
            self._set_alt(False)
        self._feed(
            f"\r\n\x1b[2m[session ended, exit code {code} - press Enter to reconnect]\x1b[0m\r\n".encode()
        )
        self.update()
        self.finished.emit(code)

    def send(self, payload: bytes) -> None:
        if self.backend:
            self.backend.write(payload)

    def is_running(self) -> bool:
        return bool(self.backend and self.backend.alive)

    def close_session(self) -> None:
        if self.backend:
            self.backend.close()

    def reconnect(self) -> None:
        self._new_screen()
        self.offset = 0
        self._spawn()
        self._fit()
        self.update()
        self.restarted.emit()

    # -- geometry ------------------------------------------------------------

    def _fit(self) -> None:
        cols = max(2, (self.width() - 2 * self.PAD) // self.cw)
        rows = max(2, (self.height() - 2 * self.PAD) // self.ch)
        if (cols, rows) != (self.cols, self.rows):
            self.cols, self.rows = cols, rows
            self._main.resize(rows, cols)
            if self.alt:
                self.screen.resize(rows, cols)
            if self.backend:
                self.backend.resize(cols, rows)

    def resizeEvent(self, event) -> None:
        self._fit()
        super().resizeEvent(event)

    # -- view composition ----------------------------------------------------------

    def _lines(self) -> list:
        """Visible rows (each a dict col->Char), honoring scrollback offset."""
        screen_rows = [self.screen.buffer[y] for y in range(self.rows)]
        if not self.offset:
            return screen_rows
        hist = list(self.screen.history.top)
        full = hist + screen_rows
        end = len(full) - self.offset
        return full[end - self.rows:end]

    def _max_offset(self) -> int:
        return 0 if self.alt else len(self.screen.history.top)

    # -- painting ------------------------------------------------------------

    def _runs(self, line, blank) -> list:
        """Merge a row into runs of equal style: [x, text, fg, bg, bold, underline]."""
        if not line:
            return []  # untouched row: the pixmap background already shows it
        row = list(map(line.get, range(self.cols), repeat(blank)))
        runs: list[list] = []
        x = 0
        for (fg_name, bg_name, bold, under, reverse), group in groupby(row, _STYLE):
            cells = list(group)
            fg = qcolor(fg_name) or DEFAULT_FG
            bg = qcolor(bg_name) or DEFAULT_BG
            if reverse:
                fg, bg = bg, fg
            text = "".join([c.data or " " for c in cells])
            if text.isascii():
                if not text.isspace() or bg is not DEFAULT_BG or under:
                    runs.append([x, text, fg, bg, bold, under])
            else:  # glyph widths may not match the cell grid: place each character itself
                runs.extend([x + i, c.data or " ", fg, bg, bold, under] for i, c in enumerate(cells))
            x += len(cells)
        return runs

    def _draw_runs(self, p: QPainter, runs: list, ty: int) -> None:
        cw, ch = self.cw, self.ch
        for x, text, fg, bg, bold, under in runs:
            tx, width = x * cw, len(text) * cw
            if bg is not DEFAULT_BG:
                p.fillRect(tx, ty, width, ch, bg)
            if not text.isspace():
                p.setFont(self._bold if bold else self._font)
                p.setPen(fg)
                p.drawText(tx, ty + self.ascent, text)
            if under:
                p.setPen(fg)
                p.drawLine(tx, ty + ch - 1, tx + width, ty + ch - 1)

    def _row_pixmap(self, y: int, line, blank, dpr: float) -> QPixmap:
        """A rendered row, cached until pyte marks the row dirty. Typing or a spinner then
        re-renders one row; every other row is a single blit."""
        pm = self._row_cache.get(y)
        if pm is None:
            pm = QPixmap(round(self.cols * self.cw * dpr), round(self.ch * dpr))
            pm.setDevicePixelRatio(dpr)
            pm.fill(DEFAULT_BG)
            pp = QPainter(pm)
            self._draw_runs(pp, self._runs(line, blank), 0)
            pp.end()
            self._row_cache[y] = pm
        return pm

    def _paint(self) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), DEFAULT_BG)
        p.translate(self.PAD, self.PAD)
        screen = self.screen
        dpr = self.devicePixelRatioF()
        key = (id(screen), self.cols, self.rows, self.cw, self.ch, dpr, TerminalMixinBase.theme_epoch)
        if self._cache_key != key:
            self._cache_key = key
            self._row_cache.clear()
            screen.dirty.clear()
        elif screen.dirty:
            for y in screen.dirty:
                self._row_cache.pop(y, None)
            screen.dirty.clear()
        blank = screen.default_char
        ch = self.ch
        for y, line in enumerate(self._lines()):
            ty = y * ch
            if self.offset:  # scrolled back: rows are history, draw them directly
                self._draw_runs(p, self._runs(line, blank), ty)
            else:
                p.drawPixmap(0, ty, self._row_pixmap(y, line, blank, dpr))
            if self._sel:
                self._paint_selection(p, y, ty)
        self._paint_cursor(p)
        p.end()

    def _paint_selection(self, p: QPainter, y: int, ty: int) -> None:
        a, b = sorted(self._sel)
        row = self._abs_row(y)
        if not a[0] <= row <= b[0]:
            return
        lo = a[1] if row == a[0] else 0
        hi = b[1] if row == b[0] else self.cols - 1
        p.fillRect(lo * self.cw, ty, (hi - lo + 1) * self.cw, self.ch, SELECTION)

    def _paint_cursor(self, p: QPainter) -> None:
        cur = self.screen.cursor
        if self.offset or cur.hidden:
            return
        x, y = cur.x * self.cw, cur.y * self.ch
        if self.hasFocus():
            cursor = QColor(DEFAULT_FG)
            cursor.setAlpha(190)
            p.fillRect(x, y, self.cw, self.ch, cursor)
            ch = self.screen.buffer[cur.y][cur.x].data
            if ch.strip():
                p.setPen(DEFAULT_BG)
                p.drawText(x, y + self.ascent, ch)
        else:
            p.setPen(QPen(DEFAULT_FG, 1))
            p.drawRect(x, y, self.cw - 1, self.ch - 1)

    # -- selection / clipboard ---------------------------------------------------

    def _cell_at(self, pos: QPoint) -> tuple[int, int]:
        return (
            min(max((pos.x() - self.PAD) // self.cw, 0), self.cols - 1),
            min(max((pos.y() - self.PAD) // self.ch, 0), self.rows - 1),
        )

    def _abs_row(self, y: int) -> int:
        return self._max_offset() - self.offset + y

    def selected_text(self) -> str:
        if not self._sel:
            return ""
        (r1, c1), (r2, c2) = sorted(self._sel)
        full = list(self.screen.history.top) + [self.screen.buffer[y] for y in range(self.rows)]
        out = []
        for r in range(r1, min(r2, len(full) - 1) + 1):
            line = full[r]
            lo = c1 if r == r1 else 0
            hi = c2 if r == r2 else self.cols - 1
            out.append("".join(line[x].data if x in line else " " for x in range(lo, hi + 1)).rstrip())
        return "\n".join(out)

    def copy(self) -> None:
        text = self.selected_text()
        if text:
            QApplication.clipboard().setText(text)

    def paste(self) -> None:
        self._paste_text(QApplication.clipboard().text())

    def _paste_text(self, text: str) -> None:
        if not text:
            return
        text = text.replace("\r\n", "\n").replace("\n", "\r")
        if self.bracketed:
            text = text.replace("\x1b[201~", "")
            text = f"\x1b[200~{text}\x1b[201~"
        self.send(text.encode())

    def _report_mouse(self, button: int, pos: QPoint, press: bool, motion: bool = False, mods=None) -> None:
        x, y = self._cell_at(pos)
        code = button + (32 if motion else 0)
        if mods is not None:
            code += (4 if mods & Qt.KeyboardModifier.ShiftModifier else 0)
            code += (8 if mods & Qt.KeyboardModifier.AltModifier else 0)
            code += (16 if mods & Qt.KeyboardModifier.ControlModifier else 0)
        if self.mouse_sgr:
            self.send(b"\x1b[<%d;%d;%d%s" % (code, x + 1, y + 1, b"M" if press else b"m"))
        elif x < 223 and y < 223:
            code = 3 if not press and button < 64 else code
            self.send(b"\x1b[M" + bytes([32 + code, 33 + x, 33 + y]))

    def _mouse_forwarded(self, event) -> bool:
        """App mouse reporting is on and Shift (the bypass modifier) is not held."""
        return self.mouse and not (event.modifiers() & Qt.KeyboardModifier.ShiftModifier)

    _BUTTONS = {Qt.MouseButton.LeftButton: 0, Qt.MouseButton.MiddleButton: 1, Qt.MouseButton.RightButton: 2}

    def mousePressEvent(self, event) -> None:
        self.setFocus()
        pos = event.position().toPoint()
        if self._mouse_forwarded(event) and event.button() in self._BUTTONS:
            self._report_mouse(self._BUTTONS[event.button()], pos, True, mods=event.modifiers())
            self._mouse_btn = self._BUTTONS[event.button()]
            return
        if event.button() == Qt.MouseButton.LeftButton:
            x, y = self._cell_at(pos)
            self._anchor = (self._abs_row(y), x)
            self._sel = None
            self._dragging = True
            self.update()
        elif event.button() == Qt.MouseButton.MiddleButton:
            clip = QGuiApplication.clipboard()
            self._paste_text(clip.text(clip.Mode.Selection) or clip.text())

    def mouseMoveEvent(self, event) -> None:
        pos = event.position().toPoint()
        if self._mouse_forwarded(event):
            if self.mouse_motion and getattr(self, "_mouse_btn", None) is not None:
                self._report_mouse(self._mouse_btn, pos, True, motion=True, mods=event.modifiers())
            return
        if self._dragging:
            x, y = self._cell_at(pos)
            self._sel = (self._anchor, (self._abs_row(y), x))
            self.update()

    def mouseReleaseEvent(self, event) -> None:
        if self._mouse_forwarded(event) and getattr(self, "_mouse_btn", None) is not None:
            self._report_mouse(self._mouse_btn, event.position().toPoint(), False, mods=event.modifiers())
            self._mouse_btn = None
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self._dragging = False
            text = self.selected_text()
            if text:
                clip = QGuiApplication.clipboard()
                if clip.supportsSelection():
                    clip.setText(text, clip.Mode.Selection)

    def contextMenuEvent(self, event) -> None:
        menu = QMenu(self)
        menu.addAction("Copy", self.copy).setEnabled(bool(self._sel))
        menu.addAction("Paste", self.paste)
        menu.addAction("Clear scrollback", lambda: (self.screen.history.top.clear(), self.update()))
        menu.exec(event.globalPos())

    def wheelEvent(self, event) -> None:
        up = event.angleDelta().y() > 0
        if self._mouse_forwarded(event):
            self._report_mouse(64 if up else 65, event.position().toPoint(), True, mods=event.modifiers())
            return
        if self.alt:  # full-screen app without mouse mode: wheel acts as arrow keys
            self.send((b"\x1bOA" if DECCKM in self.screen.mode else b"\x1b[A") * 3 if up
                      else (b"\x1bOB" if DECCKM in self.screen.mode else b"\x1b[B") * 3)
            return
        step = 3 if up else -3
        self.offset = min(max(self.offset + step, 0), self._max_offset())
        self.update()

    # -- keyboard --------------------------------------------------------------

    def focusNextPrevChild(self, nxt: bool) -> bool:
        return False  # Tab belongs to the shell

    def focusInEvent(self, event) -> None:
        self.update()

    def focusOutEvent(self, event) -> None:
        self.update()

    def keyPressEvent(self, event) -> None:
        key, mods, text = event.key(), event.modifiers(), event.text()
        ctrl = bool(mods & (Qt.KeyboardModifier.MetaModifier if IS_MAC else Qt.KeyboardModifier.ControlModifier))
        shift = bool(mods & Qt.KeyboardModifier.ShiftModifier)
        alt = bool(mods & Qt.KeyboardModifier.AltModifier)
        copy_mod = bool(mods & Qt.KeyboardModifier.MetaModifier) if IS_MAC else (
            bool(mods & Qt.KeyboardModifier.ControlModifier) and shift
        )

        if self._exited and key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self.reconnect()
            return
        if copy_mod and key == Qt.Key.Key_C:
            self.copy()
            return
        if (copy_mod and key == Qt.Key.Key_V) or (shift and key == Qt.Key.Key_Insert):
            self.paste()
            return
        if shift and key in (Qt.Key.Key_PageUp, Qt.Key.Key_PageDown):
            delta = self.rows - 1 if key == Qt.Key.Key_PageUp else -(self.rows - 1)
            self.offset = min(max(self.offset + delta, 0), self._max_offset())
            self.update()
            return

        data = b""
        if key in _ARROWS:
            mod = 1 + shift + 2 * alt + 4 * ctrl
            if mod > 1:
                data = b"\x1b[1;%d%s" % (mod, _ARROWS[key])
            else:
                app = DECCKM in self.screen.mode
                data = (b"\x1bO" if app else b"\x1b[") + _ARROWS[key]
        elif key in (Qt.Key.Key_Home, Qt.Key.Key_End) and DECCKM in self.screen.mode:
            data = b"\x1bOH" if key == Qt.Key.Key_Home else b"\x1bOF"
        elif key == Qt.Key.Key_Backtab:
            data = b"\x1b[Z"
        elif key in _KEYS:
            data = _KEYS[key]
            if alt:
                data = b"\x1b" + data
        elif ctrl and Qt.Key.Key_A <= key <= Qt.Key.Key_Z:
            data = bytes([key - Qt.Key.Key_A + 1])
        elif ctrl and key == Qt.Key.Key_Space:
            data = b"\x00"
        elif ctrl and key in _CTRL_SYMBOLS:
            data = _CTRL_SYMBOLS[key]
        elif text:
            data = text.encode()
            if alt:
                data = b"\x1b" + data
        if data:
            if self.offset:
                self.offset = 0
                self.update()
            self.send(data)
        else:
            super().keyPressEvent(event)


class TerminalWidgetGL(_TerminalMixin, QOpenGLWidget):
    """GPU-composited terminal: QPainter runs on Qt's OpenGL paint engine."""

    finished = pyqtSignal(int)
    title_changed = pyqtSignal(str)
    activity = pyqtSignal()
    bell = pyqtSignal()
    restarted = pyqtSignal()
    cwd_changed = pyqtSignal(str)

    def paintGL(self) -> None:
        self._paint()


class TerminalWidgetSW(_TerminalMixin, QWidget):
    """Software-painted fallback for platforms/drivers without usable OpenGL."""

    finished = pyqtSignal(int)
    title_changed = pyqtSignal(str)
    activity = pyqtSignal()
    bell = pyqtSignal()
    restarted = pyqtSignal()
    cwd_changed = pyqtSignal(str)

    def paintEvent(self, event) -> None:
        self._paint()


def gpu_available() -> bool:
    if os.environ.get("LESTRIX_RENDERER", "auto").lower() == "software":
        return False
    return QGuiApplication.platformName() not in ("offscreen", "minimal", "vnc")


def create_terminal(argv: list[str], font_size: int = 11, parent=None, cwd: str | None = None):
    cls = TerminalWidgetGL if gpu_available() else TerminalWidgetSW
    return cls(argv, font_size, parent, cwd)


from . import theme as _theme  # noqa: E402

apply_theme(_theme.CURRENT)
