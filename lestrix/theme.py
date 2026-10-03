"""Themes. Surfaces step up in three levels (bg0 < bg1 < bg2); text and borders are
alpha mixes of one ink colour, so every theme keeps the same hierarchy."""

from __future__ import annotations

from dataclasses import dataclass, replace

from PyQt6.QtGui import QColor, QPalette


def mix(a: str, b: str, t: float) -> str:
    """Blend colour a towards b by t (0..1)."""
    ca, cb = QColor(a), QColor(b)
    return QColor(
        round(ca.red() + (cb.red() - ca.red()) * t),
        round(ca.green() + (cb.green() - ca.green()) * t),
        round(ca.blue() + (cb.blue() - ca.blue()) * t),
    ).name()


@dataclass(frozen=True)
class Theme:
    name: str
    dark: bool
    bg0: str          # window chrome, terminal surround
    bg1: str          # sidebar / panels
    bg2: str          # raised: inputs, buttons, selected rows
    ink: str          # primary text
    accent: str
    term_bg: str
    term_fg: str
    ansi: tuple[str, ...]  # black red green yellow blue magenta cyan white + 8 bright
    up: str = "#3fb68b"
    down: str = "#e5534b"
    blue: str = "#4aa3ff"
    red: str = "#ef4b5b"

    @property
    def text(self) -> str:
        return self.ink

    @property
    def text2(self) -> str:
        return mix(self.bg1, self.ink, 0.70)

    @property
    def muted(self) -> str:
        return mix(self.bg1, self.ink, 0.48)

    @property
    def border(self) -> str:
        return mix(self.bg1, self.ink, 0.14)

    @property
    def border_strong(self) -> str:
        return mix(self.bg1, self.ink, 0.28)

    @property
    def hover(self) -> str:
        return mix(self.bg1, self.ink, 0.06)

    @property
    def on_accent(self) -> str:
        return "#101214" if QColor(self.accent).lightness() > 120 else "#ffffff"

    @property
    def selection(self) -> str:
        return mix(self.term_bg, self.accent, 0.38)


_ANSI_NAMES = ("black", "red", "green", "brown", "blue", "magenta", "cyan", "white")


def ansi_map(theme: Theme) -> dict[str, str]:
    names = _ANSI_NAMES + tuple("bright" + n for n in _ANSI_NAMES)
    return dict(zip(names, theme.ansi))


THEMES: dict[str, Theme] = {t.name: t for t in (
    Theme(  # surface ladder and cyan accent follow the Xylonic player
        "Xylonic Dark", True, "#121212", "#1e1e1e", "#2a2a2a", "#ffffff", "#00bcd4",
        "#121212", "#e6e6e6",
        ("#2a2a2a", "#ef5350", "#66bb6a", "#ffca28", "#42a5f5", "#ba68c8", "#26c6da", "#cfd8dc",
         "#616161", "#ff7b78", "#8bd18d", "#ffd95a", "#7cc0ff", "#d28be0", "#5ce1ef", "#ffffff"),
    ),
    Theme(
        "Xylonic Light", False, "#f7f7f8", "#efeff1", "#ffffff", "#1b1b1f", "#0097a7",
        "#fbfbfc", "#24272b",
        ("#24272b", "#c62828", "#2e7d32", "#9a6a00", "#1565c0", "#8e24aa", "#00838f", "#b0b5ba",
         "#6b7076", "#e53935", "#43a047", "#b8860b", "#1e88e5", "#ab47bc", "#00acc1", "#d4d7da"),
        up="#2e9e6b", down="#d0453d", blue="#1e78d6", red="#d93a4a",
    ),
    Theme(
        "Graphite", True, "#171615", "#201f1d", "#2c2a27", "#ece8e1", "#e0a43a",
        "#171615", "#dcd7cf",
        ("#2c2a27", "#e0605a", "#9bbd6a", "#e0a43a", "#6fa3d6", "#c28ab8", "#6cbfb0", "#c9c3b8",
         "#6b665e", "#f08a84", "#b5d68a", "#f0c060", "#92bde8", "#d8a6cf", "#8fd6c8", "#f2eee7"),
    ),
    Theme(
        "Nord", True, "#2b303b", "#323845", "#3b4252", "#eceff4", "#88c0d0",
        "#2e3440", "#d8dee9",
        ("#3b4252", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
         "#4c566a", "#d08770", "#b5d19c", "#f0d9a0", "#94b3d3", "#c4a0be", "#9fd0de", "#eceff4"),
    ),
    Theme(
        "Gruvbox", True, "#1d2021", "#282828", "#3c3836", "#ebdbb2", "#d79921",
        "#282828", "#ebdbb2",
        ("#3c3836", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#a89984",
         "#928374", "#fb4934", "#b8bb26", "#fabd2f", "#83a598", "#d3869b", "#8ec07c", "#ebdbb2"),
    ),
    Theme(
        "Solarized Dark", True, "#00212b", "#002b36", "#073642", "#eee8d5", "#2aa198",
        "#002b36", "#93a1a1",
        ("#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
         "#586e75", "#cb4b16", "#93a1a1", "#a58a2a", "#839496", "#6c71c4", "#35b8ae", "#fdf6e3"),
    ),
)}
DEFAULT_THEME = "Xylonic Dark"
CURRENT: Theme = THEMES[DEFAULT_THEME]


def set_current(theme: Theme) -> Theme:
    global CURRENT
    CURRENT = theme
    return theme


def with_accent(theme: Theme, accent: str | None) -> Theme:
    return replace(theme, accent=accent) if accent else theme


def palette(t: Theme) -> QPalette:
    pal = QPalette()
    for role, color in (
        (QPalette.ColorRole.Window, t.bg0), (QPalette.ColorRole.WindowText, t.text),
        (QPalette.ColorRole.Base, t.bg1), (QPalette.ColorRole.AlternateBase, t.bg2),
        (QPalette.ColorRole.Text, t.text), (QPalette.ColorRole.Button, t.bg2),
        (QPalette.ColorRole.ButtonText, t.text), (QPalette.ColorRole.Highlight, t.accent),
        (QPalette.ColorRole.HighlightedText, t.on_accent), (QPalette.ColorRole.ToolTipBase, t.bg2),
        (QPalette.ColorRole.ToolTipText, t.text), (QPalette.ColorRole.PlaceholderText, t.muted),
    ):
        pal.setColor(role, QColor(color))
    return pal


def stylesheet(t: Theme) -> str:
    return f"""
QMainWindow, QDialog {{ background: {t.bg0}; }}
QWidget {{ color: {t.text}; font-size: 13px; }}
QToolTip {{ background: {t.bg2}; color: {t.text}; border: 1px solid {t.border_strong}; padding: 4px 6px; }}

QMenuBar {{ background: {t.bg0}; }}
QMenuBar::item {{ padding: 5px 10px; background: transparent; }}
QMenuBar::item:selected {{ background: {t.hover}; border-radius: 4px; }}
QMenu {{ background: {t.bg2}; border: 1px solid {t.border_strong}; padding: 4px; }}
QMenu::item {{ padding: 6px 22px 6px 12px; border-radius: 4px; }}
QMenu::item:selected {{ background: {t.accent}; color: {t.on_accent}; }}
QMenu::separator {{ height: 1px; background: {t.border}; margin: 4px 6px; }}

QTreeWidget {{ background: {t.bg1}; border: none; outline: 0; padding: 2px 0; }}
QTreeWidget::item {{ padding: 5px 6px; border-radius: 6px; margin: 0 4px; }}
QTreeWidget::item:hover {{ background: {t.hover}; }}
QTreeWidget::item:selected {{ background: {t.bg2}; color: {t.text}; }}
QHeaderView::section {{ background: {t.bg1}; color: {t.muted}; border: none; padding: 4px 6px;
    font-size: 11px; }}

QLineEdit, QSpinBox, QComboBox {{ background: {t.bg2}; border: 1px solid {t.border}; border-radius: 6px;
    padding: 5px 8px; selection-background-color: {t.accent}; selection-color: {t.on_accent}; }}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus {{ border-color: {t.accent}; }}
QComboBox QAbstractItemView {{ background: {t.bg2}; border: 1px solid {t.border_strong}; outline: 0; }}

QPushButton, QToolButton {{ background: {t.bg2}; border: 1px solid {t.border}; border-radius: 6px; padding: 5px 12px; }}
QPushButton:hover, QToolButton:hover {{ background: {t.hover}; border-color: {t.border_strong}; }}
QPushButton:pressed, QToolButton:pressed {{ background: {t.border}; }}
QPushButton:default {{ background: {t.accent}; color: {t.on_accent}; border-color: {t.accent}; }}
QToolButton[flat="true"], QToolButton:!hover[autoRaise="true"] {{ border-color: transparent; background: transparent; }}
QToolButton#tabClose {{ border: none; background: transparent; font-size: 15px; padding: 0 5px; color: {t.muted}; }}
QToolButton#tabClose:hover {{ background: {t.hover}; color: {t.down}; border-radius: 4px; }}

QCheckBox {{ spacing: 7px; color: {t.text2}; }}
QCheckBox::indicator {{ width: 14px; height: 14px; border: 1px solid {t.border_strong}; border-radius: 4px; background: {t.bg2}; }}
QCheckBox::indicator:checked {{ background: {t.accent}; border-color: {t.accent}; }}

QTabWidget::pane {{ border: none; }}
QTabBar {{ background: {t.bg0}; }}
QTabBar::tab {{ background: transparent; padding: 7px 12px 7px 10px; margin-right: 1px; border-bottom: 2px solid transparent;
    color: {t.text2}; }}
QTabBar::tab:selected {{ background: {t.term_bg}; color: {t.text}; border-bottom: 2px solid {t.accent}; }}
QTabBar::tab:hover:!selected {{ background: {t.hover}; }}

QSplitter::handle {{ background: {t.border}; }}
QTabWidget#sideTabs > QTabBar::tab {{ padding: 8px 0; margin: 0; min-width: 60px; font-size: 11px; font-weight: 600;
    letter-spacing: 1.2px; color: {t.muted}; border-bottom: 2px solid transparent; background: {t.bg1}; }}
QTabWidget#sideTabs > QTabBar::tab:selected {{ color: {t.text}; border-bottom: 2px solid {t.accent}; background: {t.bg1}; }}
QTabWidget#sideTabs > QTabBar::tab:hover:!selected {{ color: {t.text2}; }}
QTabWidget#sideTabs > QWidget {{ background: {t.bg1}; }}
QSplitter::handle:horizontal {{ width: 1px; }}
QScrollBar:vertical {{ background: transparent; width: 10px; margin: 0; }}
QScrollBar::handle:vertical {{ background: {t.border_strong}; border-radius: 4px; min-height: 28px; margin: 2px; }}
QScrollBar::handle:vertical:hover {{ background: {t.muted}; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; width: 0; }}
QScrollBar:horizontal {{ background: transparent; height: 10px; }}
QScrollBar::handle:horizontal {{ background: {t.border_strong}; border-radius: 4px; min-width: 28px; margin: 2px; }}
QStatusBar {{ background: {t.bg0}; color: {t.muted}; }}

QLabel#sideLabel, QLabel#statusLine {{ color: {t.muted}; font-size: 11px; }}

WelcomePage {{ background: {t.term_bg}; }}
QLabel#wordmark {{ font-size: 30px; font-weight: 700; letter-spacing: -1px; color: {t.text}; }}
QLabel#wordmarkDot {{ font-size: 30px; font-weight: 700; color: {t.accent}; }}
QLabel#welcomeMeta {{ color: {t.muted}; font-size: 12px; }}
QLabel#sectionLabel {{ color: {t.muted}; font-size: 11px; font-weight: 600; letter-spacing: 1.4px; }}
QPushButton#recentRow {{ background: transparent; border: none; border-bottom: 1px solid {t.border};
    border-radius: 0; text-align: left; padding: 9px 4px; }}
QPushButton#recentRow:hover {{ background: {t.hover}; }}
QLabel#rowKey {{ color: {t.muted}; font-size: 11px; border: 1px solid {t.border_strong}; border-radius: 4px;
    padding: 1px 6px; }}
QLabel#rowName {{ font-size: 14px; font-weight: 600; }}
QLabel#rowHost {{ color: {t.text2}; font-size: 12px; }}
QLabel#rowAge {{ color: {t.muted}; font-size: 11px; }}
"""
