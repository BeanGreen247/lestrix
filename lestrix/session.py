"""One tab: a terminal. SSH tabs also own an SFTP FilePanel, which the main window
shows in the shared sidebar while the tab is current."""

from __future__ import annotations

import shutil
from pathlib import Path

from PyQt6.QtCore import QPointF, QRectF, QSize, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QIcon, QPainter, QPen, QPixmap
from PyQt6.QtWidgets import QVBoxLayout, QWidget

from .files import FilePanel
from .sftp import SUPPORTED, SftpSession, make_control_path
from . import theme
from .store import Connection
from .terminal import create_terminal

TAB_COLORS = [
    ("Red", "#e06c75"), ("Orange", "#e59a5a"), ("Yellow", "#e5c07b"), ("Green", "#98c379"),
    ("Teal", "#4fb39a"), ("Blue", "#61afef"), ("Purple", "#c678dd"), ("Pink", "#e88ab8"),
]


def tab_icon(kind: str, color: str = "", dot: str = "") -> QIcon:
    """Terminal glyph for local shells, server glyph for ssh; optional status dot."""
    size, ratio = 18, 2
    pm = QPixmap(size * ratio, size * ratio)
    pm.setDevicePixelRatio(ratio)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    tint = QColor(color or theme.CURRENT.text2)
    pen = QPen(tint, 1.6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
    p.setPen(pen)
    p.setBrush(Qt.BrushStyle.NoBrush)
    if kind == "ssh":  # two stacked server units
        for y in (2.5, 9.5):
            p.drawRoundedRect(QRectF(2, y, 14, 5.5), 1.5, 1.5)
            p.drawPoint(QPointF(5, y + 2.75))
    else:  # terminal window with prompt
        p.drawRoundedRect(QRectF(1.5, 3, 15, 12), 2, 2)
        p.drawPolyline([QPointF(5, 6.8), QPointF(7.6, 9), QPointF(5, 11.2)])
        p.drawLine(QPointF(9.3, 11.4), QPointF(12.6, 11.4))
    if dot:
        p.setPen(QPen(QColor(theme.CURRENT.term_bg), 1.4))
        p.setBrush(QColor(dot))
        p.drawEllipse(QPointF(14, 14), 3.2, 3.2)
    p.end()
    return QIcon(pm)


class SessionTab(QWidget):
    changed = pyqtSignal()  # icon/state needs a refresh

    def __init__(self, argv: list[str], title: str, font_size: int, conn: Connection | None = None,
                 follow: bool = True, cwd: str | None = None):
        super().__init__()
        self.title = title
        self.conn = conn
        self.local_argv = argv
        self.color = conn.color if conn else ""
        self.state = "idle"  # idle | activity | bell | ended
        self.foreground = False
        self._ctrl_dir: Path | None = None
        self.panel: FilePanel | None = None

        control_path = None
        if conn and SUPPORTED:
            control_path = make_control_path()
            self._ctrl_dir = Path(control_path).parent
            argv = conn.ssh_argv(control_path)
        elif conn:
            argv = conn.ssh_argv()

        self.term = create_terminal(argv, font_size, cwd=cwd)
        self.term.activity.connect(self._on_activity)
        self.term.bell.connect(self._on_bell)
        self.term.finished.connect(lambda _c: self._set_state("ended"))
        self.term.restarted.connect(lambda: self._set_state("idle"))

        if control_path:
            self.panel = FilePanel(SftpSession(conn, control_path), follow)
            self.term.cwd_changed.connect(self.panel.terminal_cwd)
            self.term.activity.connect(self.panel.note_activity)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self.term)

    # -- state dot -----------------------------------------------------------

    @property
    def dot(self) -> str:
        t = theme.CURRENT
        return {"activity": t.blue, "bell": t.red, "ended": t.red}.get(self.state, "")

    def icon(self) -> QIcon:
        return tab_icon("ssh" if self.conn else "local", self.color, self.dot)

    def _set_state(self, state: str) -> None:
        if state != self.state:
            self.state = state
            self.changed.emit()

    def _on_activity(self) -> None:
        if not self.foreground and self.state == "idle":
            self._set_state("activity")

    def _on_bell(self) -> None:
        if not self.foreground and self.state in ("idle", "activity"):
            self._set_state("bell")

    def set_foreground(self, on: bool) -> None:
        self.foreground = on
        if on and self.state in ("activity", "bell"):
            self._set_state("idle")

    # -- delegation ------------------------------------------------------------

    def focus_terminal(self) -> None:
        self.term.setFocus()

    def is_running(self) -> bool:
        return self.term.is_running()

    def set_font_size(self, size: int) -> None:
        self.term.set_font_size(size)

    def close_session(self) -> None:
        self.term.close_session()
        if self._ctrl_dir:
            shutil.rmtree(self._ctrl_dir, ignore_errors=True)
            self._ctrl_dir = None
