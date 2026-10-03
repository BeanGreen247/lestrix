"""Main window: saved connections on the left, terminal tabs on the right."""

from __future__ import annotations

import argparse
import dataclasses
import socket
import sys
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PyQt6.QtCore import QObject, QSettings, QSize, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import (
    QAction, QActionGroup, QColor, QFont, QFontDatabase, QIcon, QKeySequence, QPainter, QPixmap, QShortcut,
)
from PyQt6.QtWidgets import (
    QApplication, QColorDialog, QComboBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout, QHBoxLayout,
    QInputDialog, QLabel, QLineEdit, QMainWindow, QMenu, QMessageBox, QPushButton, QSpinBox, QSplitter,
    QStackedWidget, QTabWidget, QToolButton, QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget,
)

from . import __version__, terminal, theme
from .session import TAB_COLORS, SessionTab
from .store import DEFAULT_GROUP, Connection, Store, parse_ansible_ini, parse_ssh_config
from .terminal import default_shell, detect_shells
from .theme import DEFAULT_THEME, THEMES

X11_LABELS = [("off", "Off"), ("untrusted", "X11 forwarding (-X)"), ("trusted", "Trusted X11 forwarding (-Y)")]
ASSETS = Path(__file__).resolve().parent / "assets"


def dot_icon(state: str) -> QIcon:
    t = theme.CURRENT
    color = {"unknown": t.muted, "online": t.up, "offline": t.down}[state]
    pm = QPixmap(28, 28)
    pm.setDevicePixelRatio(2)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setBrush(QColor(color))
    p.setPen(Qt.PenStyle.NoPen)
    p.drawEllipse(3, 3, 8, 8)
    p.end()
    return QIcon(pm)


def age(ts: float) -> str:
    secs = max(0, time.time() - ts)
    if secs < 90:
        return "just now"
    for limit, unit, div in ((5400, "m", 60), (129600, "h", 3600)):
        if secs < limit:
            return f"{max(1, int(secs / div))}{unit} ago"
    return f"{int(secs / 86400)}d ago"


class Prober(QObject):
    """Checks TCP reachability of each connection's SSH port off the UI thread."""

    result = pyqtSignal(str, bool)

    def __init__(self):
        super().__init__()
        self._pool = ThreadPoolExecutor(max_workers=16)

    def check(self, conn: Connection) -> None:
        self._pool.submit(self._run, conn.id, conn.host, conn.port)

    def _run(self, conn_id: str, host: str, port: int) -> None:
        try:
            with socket.create_connection((host, port), timeout=2.5):
                ok = True
        except OSError:
            ok = False
        self.result.emit(conn_id, ok)


class WelcomePage(QWidget):
    """Start page: the wordmark and the nine most recent connections, numbered 1-9."""

    open_requested = pyqtSignal(str)
    local_requested = pyqtSignal()

    def __init__(self):
        super().__init__()
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._ids: list[str] = []

        title = QLabel("Lestrix")
        title.setObjectName("wordmark")
        self.meta = QLabel()
        self.meta.setObjectName("welcomeMeta")
        label = QLabel("RECENT")
        label.setObjectName("sectionLabel")
        self.rows = QVBoxLayout()
        self.rows.setSpacing(0)

        col = QVBoxLayout()
        col.setSpacing(2)
        col.addWidget(title)
        col.addWidget(self.meta)
        col.addSpacing(34)
        col.addWidget(label)
        col.addSpacing(6)
        col.addLayout(self.rows)
        col.addStretch(1)
        holder = QWidget()
        holder.setLayout(col)
        holder.setMaximumWidth(560)
        outer = QHBoxLayout(self)
        outer.setContentsMargins(64, 56, 32, 32)
        outer.addWidget(holder, 1)
        outer.addStretch(1)

        for i in range(10):
            sc = QShortcut(QKeySequence(str(i)), self)
            sc.setContext(Qt.ShortcutContext.WidgetWithChildrenShortcut)
            sc.activated.connect(lambda n=i: self._hotkey(n))

    def _hotkey(self, n: int) -> None:
        if n == 0:
            self.local_requested.emit()
        elif n <= len(self._ids):
            self.open_requested.emit(self._ids[n - 1])

    def _row(self, key: str, name: str, host: str, when: str, slot) -> QPushButton:
        btn = QPushButton()
        btn.setObjectName("recentRow")
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        lay = QHBoxLayout(btn)
        lay.setContentsMargins(4, 0, 4, 0)
        lay.setSpacing(12)
        for text, obj, stretch in ((key, "rowKey", 0), (name, "rowName", 0), (host, "rowHost", 1), (when, "rowAge", 0)):
            lbl = QLabel(text)
            lbl.setObjectName(obj)
            lbl.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
            if obj == "rowKey":
                lbl.setFixedSize(22, 20)
                lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            elif obj == "rowName":
                lbl.setMinimumWidth(132)
            lay.addWidget(lbl, stretch, Qt.AlignmentFlag.AlignVCenter)
        btn.setMinimumHeight(40)
        btn.clicked.connect(slot)
        return btn

    def set_recent(self, conns: list[Connection], total: int = 0) -> None:
        while self.rows.count():
            w = self.rows.takeAt(0).widget()
            if w:
                w.deleteLater()
        self._ids = [c.id for c in conns]
        self.meta.setText(f"{total} connection{'s' * (total != 1)} saved" if total else "No connections yet")
        for i, c in enumerate(conns, 1):
            self.rows.addWidget(self._row(str(i), c.name, c.dest, age(c.last_used),
                                          lambda _=False, cid=c.id: self.open_requested.emit(cid)))
        self.rows.addWidget(self._row("0", "Local shell", "this machine", "", self.local_requested.emit))
        if not conns:
            hint = QLabel("Double-click a host on the left, or press + to add one.")
            hint.setObjectName("welcomeMeta")
            hint.setContentsMargins(4, 14, 0, 0)
            self.rows.addWidget(hint)


class ConnectionDialog(QDialog):
    def __init__(self, parent, conn: Connection | None, groups: list[str]):
        super().__init__(parent)
        self.setWindowTitle("Edit connection" if conn else "New connection")
        self.setMinimumWidth(460)
        c = conn or Connection(name="", host="")
        self._id = c.id
        self._color = c.color

        self.name = QLineEdit(c.name)
        self.group = QComboBox(editable=True)
        self.group.addItems(groups or [DEFAULT_GROUP])
        self.group.setCurrentText(c.group)
        self.host = QLineEdit(c.host)
        self.port = QSpinBox(minimum=1, maximum=65535, value=c.port)
        self.user = QLineEdit(c.user)
        self.key = QLineEdit(c.key)
        self.key.setPlaceholderText("optional - defaults to ssh-agent and ~/.ssh keys")
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse)
        key_row = QHBoxLayout()
        key_row.addWidget(self.key)
        key_row.addWidget(browse)
        self.x11 = QComboBox()
        for value, label in X11_LABELS:
            self.x11.addItem(label, value)
        self.x11.setCurrentIndex(max(0, [v for v, _ in X11_LABELS].index(c.x11)))
        self.remote = QComboBox(editable=True)
        self.remote.addItems(["", "bash -l", "zsh -l", "fish -l", "tmux new-session -A -s main"])
        self.remote.setCurrentText(c.remote_command)
        self.remote.lineEdit().setPlaceholderText("empty = the account's login shell")
        self.options = QLineEdit(c.options)
        self.options.setPlaceholderText("e.g. -J jumphost -L 8080:localhost:80")

        form = QFormLayout()
        form.setVerticalSpacing(9)
        for label, widget in (
            ("Name", self.name), ("Group", self.group), ("Host", self.host), ("Port", self.port),
            ("User", self.user), ("Private key", key_row), ("X11", self.x11),
            ("Remote shell", self.remote), ("Extra ssh args", self.options),
        ):
            form.addRow(label, widget)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Save | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.setSpacing(14)
        layout.addLayout(form)
        layout.addWidget(buttons)

    def _browse(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Private key", str(Path.home() / ".ssh"))
        if path:
            self.key.setText(path)

    def _accept(self) -> None:
        if not self.name.text().strip() or not self.host.text().strip():
            QMessageBox.warning(self, "Missing fields", "Name and host are required.")
            return
        self.accept()

    def result_connection(self) -> Connection:
        return Connection(
            id=self._id,
            name=self.name.text().strip(),
            host=self.host.text().strip(),
            group=self.group.currentText().strip() or DEFAULT_GROUP,
            port=self.port.value(),
            user=self.user.text().strip(),
            key=self.key.text().strip(),
            x11=self.x11.currentData(),
            options=self.options.text().strip(),
            remote_command=self.remote.currentText().strip(),
            color=self._color,
        )


class MainWindow(QMainWindow):
    def __init__(self, store: Store):
        super().__init__()
        self.store = store
        self.settings = QSettings("Lestrix", "Lestrix")
        self.font_size = int(self.settings.value("font_size", 11))
        self.follow_default = self.settings.value("follow_path", True, type=bool)
        self.show_ssh_config = self.settings.value("show_ssh_config", True, type=bool)
        self.theme_name = str(self.settings.value("theme", DEFAULT_THEME))
        self.accent = str(self.settings.value("accent", ""))
        self.setWindowTitle("Lestrix")
        self.resize(1200, 720)
        self._items: dict[str, QTreeWidgetItem] = {}
        self._status: dict[str, bool] = {}

        self.prober = Prober()
        self.prober.result.connect(self._on_probe)

        self._build_sidebar()
        self._build_tabs()
        self._build_menu()

        self.splitter = QSplitter()
        self.splitter.addWidget(self.sidebar)
        self.splitter.addWidget(self.stack)
        self.splitter.setStretchFactor(1, 1)
        self.splitter.setSizes([300, 900])
        self.splitter.setChildrenCollapsible(False)
        self.splitter.setHandleWidth(1)
        self.setCentralWidget(self.splitter)

        geometry = self.settings.value("geometry")
        if geometry:
            self.restoreGeometry(geometry)
        sizes = self.settings.value("splitter")
        if sizes:
            self.splitter.setSizes([int(s) for s in sizes])

        self.apply_theme()
        self._probe_timer = QTimer(self, interval=30_000)
        self._probe_timer.timeout.connect(self.probe_all)
        self._probe_timer.start()

    # -- theming -----------------------------------------------------------------

    def apply_theme(self, name: str | None = None) -> None:
        if name:
            self.theme_name = name
            self.settings.setValue("theme", name)
        base = THEMES.get(self.theme_name, THEMES[DEFAULT_THEME])
        t = theme.set_current(theme.with_accent(base, self.accent or None))
        app = QApplication.instance()
        app.setPalette(theme.palette(t))
        app.setStyleSheet(theme.stylesheet(t))
        terminal.apply_theme(t)
        for i in range(self.tabs.count()):
            tab = self.tabs.widget(i)
            tab.term.update()
            self._refresh_tab(tab)
        self.refresh()

    def pick_accent(self) -> None:
        color = QColorDialog.getColor(QColor(theme.CURRENT.accent), self, "Accent color")
        if color.isValid():
            self.accent = color.name()
            self.settings.setValue("accent", self.accent)
            self.apply_theme()

    def reset_accent(self) -> None:
        self.accent = ""
        self.settings.setValue("accent", "")
        self.apply_theme()

    # -- UI construction ---------------------------------------------------------

    def _build_sidebar(self) -> None:
        self.sidebar = QWidget()
        self.sidebar.setMinimumWidth(210)
        self.sidebar.setMaximumWidth(520)
        self.sidebar.setAttribute(Qt.WidgetAttribute.WA_StyledBackground)
        self.search = QLineEdit(placeholderText="Search connections", clearButtonEnabled=True)
        self.search.textChanged.connect(self._apply_filter)
        add = QToolButton(text="+")
        add.setToolTip("New connection (Ctrl+N)")
        add.clicked.connect(self.new_connection)
        top = QHBoxLayout()
        top.setSpacing(6)
        top.addWidget(self.search)
        top.addWidget(add)

        self.tree = QTreeWidget()
        self.tree.setHeaderHidden(True)
        self.tree.setIndentation(10)
        self.tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._tree_menu)
        self.tree.itemActivated.connect(self._activate)
        self.tree.itemDoubleClicked.connect(self._activate)

        sessions = QWidget()
        sl = QVBoxLayout(sessions)
        sl.setContentsMargins(10, 10, 6, 6)
        sl.setSpacing(8)
        sl.addLayout(top)
        sl.addWidget(self.tree)

        # files of the current SSH tab; one panel per tab, swapped as tabs change
        self.files_box = QWidget()
        fl = QVBoxLayout(self.files_box)
        fl.setContentsMargins(10, 6, 6, 4)
        fl.setSpacing(4)
        self.files_label = QLabel("NO SSH SESSION")
        self.files_label.setObjectName("sectionLabel")
        self.files_stack = QStackedWidget()
        self.files_hint = QLabel("The file browser appears\nwhen an SSH tab is open.")
        self.files_hint.setObjectName("statusLine")
        self.files_hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.files_stack.addWidget(self.files_hint)
        fl.addWidget(self.files_label)
        fl.addWidget(self.files_stack, 1)

        # one sidebar, two tabs: saved sessions and the file browser
        self.side_tabs = QTabWidget(documentMode=True)
        self.side_tabs.setObjectName("sideTabs")
        self.side_tabs.addTab(sessions, "Sessions")
        self.side_tabs.addTab(self.files_box, "Files")
        self.side_tabs.tabBar().setExpanding(True)
        self.side_tabs.tabBar().setDrawBase(False)
        layout = QVBoxLayout(self.sidebar)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.side_tabs)

    def _build_tabs(self) -> None:
        self.tabs = QTabWidget(movable=True, documentMode=True)
        bar = self.tabs.tabBar()
        bar.setTabsClosable(False)
        bar.setIconSize(QSize(18, 18))
        bar.setExpanding(False)
        bar.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        bar.customContextMenuRequested.connect(self._tab_menu)
        bar.tabBarDoubleClicked.connect(self._rename_at)
        self.tabs.currentChanged.connect(self._on_tab_changed)

        self.welcome = WelcomePage()
        self.welcome.open_requested.connect(self._open_by_id)
        self.welcome.local_requested.connect(self.open_local)
        self.stack = QStackedWidget()
        self.stack.addWidget(self.welcome)
        self.stack.addWidget(self.tabs)

    def _build_menu(self) -> None:
        def act(menu: QMenu, text: str, slot, shortcut: str = "") -> QAction:
            a = menu.addAction(text, slot)
            if shortcut:
                a.setShortcut(QKeySequence(shortcut))
            return a

        file_menu = self.menuBar().addMenu("&File")
        act(file_menu, "New connection…", self.new_connection, "Ctrl+N")
        act(file_menu, "New local shell", self.open_local, "Ctrl+Shift+T")
        shells = file_menu.addMenu("New local shell as…")
        for label, argv in detect_shells():
            shells.addAction(label, lambda a=argv, n=label: self._add_tab(n, argv=a))
        file_menu.addSeparator()
        act(file_menu, "Import Ansible inventory…", self.import_ansible)
        act(file_menu, "Copy ~/.ssh/config hosts into saved connections", self.import_ssh_config)
        file_menu.addSeparator()
        act(file_menu, "Quit", self.close, "Ctrl+Q")

        view = self.menuBar().addMenu("&View")
        act(view, "Toggle sidebar", lambda: self.sidebar.setVisible(not self.sidebar.isVisible()), "Ctrl+B")
        act(view, "Bigger font", lambda: self.change_font(1), "Ctrl+=")
        act(view, "Smaller font", lambda: self.change_font(-1), "Ctrl+-")
        act(view, "Re-check hosts", self.probe_all, "F5")
        view.addSeparator()
        themes = view.addMenu("Theme")
        group = QActionGroup(self)
        for name in THEMES:
            a = themes.addAction(name)
            a.setCheckable(True)
            a.setChecked(name == self.theme_name)
            group.addAction(a)
            a.triggered.connect(lambda _=False, n=name: self.apply_theme(n))
        themes.addSeparator()
        themes.addAction("Accent color…", self.pick_accent)
        themes.addAction("Reset accent", self.reset_accent)
        view.addSeparator()
        act(view, "Sessions / Files sidebar", lambda: self._toggle_files(self.side_tabs.currentIndex() == 0),
            "Ctrl+Shift+B")
        hosts = act(view, "Show hosts from ~/.ssh/config", lambda: None)
        hosts.setCheckable(True)
        hosts.setChecked(self.show_ssh_config)
        hosts.toggled.connect(self._toggle_ssh_config)
        boot = act(view, "Open a local shell on startup", lambda: None)
        boot.setCheckable(True)
        boot.setChecked(self.settings.value("startup_shell", False, type=bool))
        boot.toggled.connect(lambda on: self.settings.setValue("startup_shell", on))

        help_menu = self.menuBar().addMenu("&Help")
        act(help_menu, "About", self.about)

        QShortcut(QKeySequence("Ctrl+Shift+W"), self, activated=lambda: self.close_tab(self.tabs.currentIndex()))
        QShortcut(QKeySequence("Ctrl+PgDown"), self, activated=lambda: self._cycle(1))
        QShortcut(QKeySequence("Ctrl+PgUp"), self, activated=lambda: self._cycle(-1))

    # -- connection list -----------------------------------------------------------

    def refresh(self) -> None:
        self.store.reload_ssh_config(self.show_ssh_config)
        t = theme.CURRENT
        self.tree.clear()
        self._items.clear()
        group_font = QFont(self.font())
        group_font.setPointSizeF(8.5)
        group_font.setBold(True)
        group_font.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, 1.2)
        groups: dict[str, QTreeWidgetItem] = {}
        for conn in sorted(self.store.all(), key=lambda c: (c.group.lower(), c.name.lower())):
            parent = groups.get(conn.group)
            if parent is None:
                parent = QTreeWidgetItem(self.tree, [conn.group.upper()])
                parent.setFlags(Qt.ItemFlag.ItemIsEnabled)
                parent.setForeground(0, QColor(t.muted))
                parent.setFont(0, group_font)
                groups[conn.group] = parent
            item = QTreeWidgetItem(parent, [conn.name])
            state = {True: "online", False: "offline"}.get(self._status.get(conn.id), "unknown")
            item.setIcon(0, dot_icon(state))
            item.setData(0, Qt.ItemDataRole.UserRole, conn.id)
            item.setToolTip(0, f"ssh {conn.alias}  (from ~/.ssh/config)" if conn.is_live else f"{conn.dest}:{conn.port}")
            if conn.color:
                item.setForeground(0, QColor(conn.color))
            self._items[conn.id] = item
        self.tree.expandAll()
        self.welcome.set_recent(self.store.recent(), len(self.store.all()))
        self._apply_filter(self.search.text())
        self.probe_all()

    def _apply_filter(self, text: str) -> None:
        needle = text.lower().strip()
        for i in range(self.tree.topLevelItemCount()):
            group = self.tree.topLevelItem(i)
            visible = 0
            for j in range(group.childCount()):
                child = group.child(j)
                conn = self.store.get(child.data(0, Qt.ItemDataRole.UserRole))
                hit = not needle or (conn and needle in f"{conn.name} {conn.host} {conn.group}".lower())
                child.setHidden(not hit)
                visible += bool(hit)
            group.setHidden(visible == 0)

    def probe_all(self) -> None:
        if self.isMinimized():  # nobody is looking at the dots
            return
        for conn in self.store.all():
            self.prober.check(conn)

    def _on_probe(self, conn_id: str, ok: bool) -> None:
        self._status[conn_id] = ok
        item = self._items.get(conn_id)
        if item:
            item.setIcon(0, dot_icon("online" if ok else "offline"))

    def _selected_connection(self) -> Connection | None:
        item = self.tree.currentItem()
        return self.store.get(item.data(0, Qt.ItemDataRole.UserRole) or "") if item else None

    def _tree_menu(self, pos) -> None:
        conn = self._selected_connection()
        menu = QMenu(self)
        if conn:
            menu.addAction("Connect", lambda: self.open_connection(conn))
            if conn.is_live:
                menu.addAction("Save a copy to edit…", lambda: self.edit_connection(conn))
            else:
                menu.addAction("Edit…", lambda: self.edit_connection(conn))
                menu.addAction("Duplicate", lambda: self.duplicate_connection(conn))
                menu.addAction("Delete", lambda: self.delete_connection(conn))
            menu.addSeparator()
        menu.addAction("New connection…", self.new_connection)
        menu.exec(self.tree.viewport().mapToGlobal(pos))

    def _activate(self, item: QTreeWidgetItem) -> None:
        self._open_by_id(item.data(0, Qt.ItemDataRole.UserRole) or "")

    # -- connection CRUD -------------------------------------------------------------

    def new_connection(self) -> None:
        self._edit(None)

    def edit_connection(self, conn: Connection) -> None:
        if conn.is_live:  # live hosts are read-only: edit a saved copy instead
            conn = dataclasses.replace(conn, id=uuid.uuid4().hex, alias="", group="Saved")
        self._edit(conn)

    def _edit(self, conn: Connection | None) -> None:
        dlg = ConnectionDialog(self, conn, self.store.groups())
        if dlg.exec():
            self.store.upsert(dlg.result_connection())
            self.refresh()

    def duplicate_connection(self, conn: Connection) -> None:
        self.store.upsert(dataclasses.replace(conn, id=uuid.uuid4().hex, name=conn.name + " copy"))
        self.refresh()

    def delete_connection(self, conn: Connection) -> None:
        if QMessageBox.question(self, "Delete", f"Delete “{conn.name}”?") == QMessageBox.StandardButton.Yes:
            self.store.remove(conn.id)
            self.refresh()

    # -- importers --------------------------------------------------------------------

    def import_ansible(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Ansible inventory (INI)", str(Path.home()))
        if path:
            self._import(lambda: parse_ansible_ini(Path(path)))

    def import_ssh_config(self) -> None:
        self._import(lambda: parse_ssh_config(Path.home() / ".ssh" / "config"))

    def _import(self, loader) -> None:
        try:
            added = self.store.add_imported(loader())
        except (OSError, ValueError) as exc:
            QMessageBox.warning(self, "Import failed", str(exc))
            return
        self.refresh()
        self.statusBar().showMessage(f"Imported {added} connection(s)", 5000)

    # -- tabs ----------------------------------------------------------------------------

    def _open_by_id(self, conn_id: str) -> None:
        conn = self.store.get(conn_id)
        if conn:
            self.open_connection(conn)

    def open_connection(self, conn: Connection) -> None:
        self.store.touch(conn.id)
        self.welcome.set_recent(self.store.recent(), len(self.store.all()))
        self._add_tab(conn.name, conn=conn)

    def open_local(self) -> None:
        self._add_tab("local", argv=default_shell())

    def _add_tab(self, title: str, argv: list[str] | None = None, conn: Connection | None = None,
                 cwd: str | None = None) -> SessionTab:
        tab = SessionTab(argv or [], title, self.font_size, conn, follow=self.follow_default, cwd=cwd)
        if tab.panel:
            tab.panel.follow.toggled.connect(self._remember_follow)
            self.files_stack.addWidget(tab.panel)
        tab.changed.connect(lambda t=tab: self._refresh_tab(t))
        idx = self.tabs.addTab(tab, tab.icon(), title)
        bar = self.tabs.tabBar()
        close = QToolButton(text="×", autoRaise=True, toolTip="Close tab")
        close.setObjectName("tabClose")
        close.clicked.connect(lambda _=False, t=tab: self.close_tab(self.tabs.indexOf(t)))
        bar.setTabButton(idx, bar.ButtonPosition.RightSide, close)
        self._refresh_tab(tab)
        self.tabs.setCurrentIndex(idx)
        self.stack.setCurrentWidget(self.tabs)
        tab.focus_terminal()
        return tab

    def _refresh_tab(self, tab: SessionTab) -> None:
        idx = self.tabs.indexOf(tab)
        if idx < 0:
            return
        self.tabs.setTabIcon(idx, tab.icon())
        self.tabs.setTabText(idx, tab.title)
        self.tabs.tabBar().setTabTextColor(idx, QColor(tab.color or theme.CURRENT.text))
        tips = {"activity": "New output", "bell": "Bell", "ended": "Session ended"}
        self.tabs.setTabToolTip(idx, tips.get(tab.state, ""))

    def _on_tab_changed(self, index: int) -> None:
        for i in range(self.tabs.count()):
            self.tabs.widget(i).set_foreground(i == index)
        tab = self.tabs.widget(index)
        if tab:
            tab.focus_terminal()
            self.files_stack.setCurrentWidget(tab.panel or self.files_hint)
            self.files_label.setText(tab.title.upper() if tab.panel else "NO SSH SESSION")
        else:
            self.files_stack.setCurrentWidget(self.files_hint)
            self.files_label.setText("NO SSH SESSION")

    def _tab_menu(self, pos) -> None:
        idx = self.tabs.tabBar().tabAt(pos)
        if idx < 0:
            return
        tab = self.tabs.widget(idx)
        menu = QMenu(self)
        menu.addAction("Rename…", lambda: self.rename_tab(tab))
        colors = menu.addMenu("Color")
        for name, hex_ in TAB_COLORS:
            pm = QPixmap(14, 14)
            pm.fill(QColor(hex_))
            colors.addAction(QIcon(pm), name, lambda h=hex_: self.set_tab_color(tab, h))
        colors.addAction("Custom…", lambda: self._custom_color(tab))
        colors.addSeparator()
        colors.addAction("Default", lambda: self.set_tab_color(tab, ""))
        menu.addSeparator()
        menu.addAction("Duplicate tab", lambda: self.duplicate_tab(tab))
        menu.addSeparator()
        menu.addAction("Close", lambda: self.close_tab(self.tabs.indexOf(tab)))
        menu.addAction("Close other tabs", lambda: self.close_others(tab))
        menu.exec(self.tabs.tabBar().mapToGlobal(pos))

    def _rename_at(self, index: int) -> None:
        tab = self.tabs.widget(index)
        if tab:
            self.rename_tab(tab)

    def rename_tab(self, tab: SessionTab) -> None:
        name, ok = QInputDialog.getText(self, "Rename tab", "Tab name:", text=tab.title)
        if ok and name.strip():
            tab.title = name.strip()
            self._refresh_tab(tab)
            self._on_tab_changed(self.tabs.currentIndex())

    def _custom_color(self, tab: SessionTab) -> None:
        color = QColorDialog.getColor(QColor(tab.color or theme.CURRENT.accent), self, "Tab color")
        if color.isValid():
            self.set_tab_color(tab, color.name())

    def set_tab_color(self, tab: SessionTab, color: str) -> None:
        tab.color = color
        if tab.conn:  # remembered per connection
            self.store.update_meta(tab.conn, color=color)
            self.refresh()
        self._refresh_tab(tab)

    def duplicate_tab(self, tab: SessionTab) -> None:
        if tab.conn:
            self.open_connection(tab.conn)
        else:
            self._add_tab(tab.title, argv=tab.local_argv)

    def close_others(self, keep: SessionTab) -> None:
        for i in reversed(range(self.tabs.count())):
            if self.tabs.widget(i) is not keep:
                self.close_tab(i)

    def close_tab(self, index: int) -> None:
        tab = self.tabs.widget(index)
        if tab is None:
            return
        if tab.is_running():
            ask = QMessageBox.question(self, "Close tab", "This session is still running. Close it?")
            if ask != QMessageBox.StandardButton.Yes:
                return
        tab.close_session()
        self.tabs.removeTab(index)
        if tab.panel:
            self.files_stack.removeWidget(tab.panel)
            tab.panel.deleteLater()
        tab.deleteLater()
        if not self.tabs.count():
            self.stack.setCurrentIndex(0)
            self.welcome.setFocus()

    def _cycle(self, step: int) -> None:
        if self.tabs.count():
            self.tabs.setCurrentIndex((self.tabs.currentIndex() + step) % self.tabs.count())

    def change_font(self, delta: int) -> None:
        self.font_size = min(28, max(7, self.font_size + delta))
        for i in range(self.tabs.count()):
            self.tabs.widget(i).set_font_size(self.font_size)

    def _toggle_files(self, on: bool) -> None:
        self.side_tabs.setCurrentIndex(1 if on else 0)

    def _toggle_ssh_config(self, on: bool) -> None:
        self.show_ssh_config = on
        self.settings.setValue("show_ssh_config", on)
        self.refresh()

    def _remember_follow(self, on: bool) -> None:
        self.follow_default = on
        self.settings.setValue("follow_path", on)

    def about(self) -> None:
        QMessageBox.about(
            self, "About Lestrix",
            f"<b>Lestrix {__version__}</b><br>Saved SSH connections and terminal tabs in one window.",
        )

    def closeEvent(self, event) -> None:
        running = sum(self.tabs.widget(i).is_running() for i in range(self.tabs.count()))
        if running and QMessageBox.question(
            self, "Quit", f"{running} session(s) still running. Quit anyway?"
        ) != QMessageBox.StandardButton.Yes:
            event.ignore()
            return
        for i in range(self.tabs.count()):
            self.tabs.widget(i).close_session()
        self.settings.setValue("geometry", self.saveGeometry())
        self.settings.setValue("splitter", self.splitter.sizes())
        self.settings.setValue("font_size", self.font_size)
        event.accept()


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    ap = argparse.ArgumentParser(prog="lestrix", description="SSH sessions and terminals in one window.")
    ap.add_argument("--version", action="version", version=f"Lestrix {__version__}")
    ap.add_argument("--local", action="store_true", help="open a local shell tab on startup")
    ap.add_argument("--working-directory", metavar="DIR", help="start the local shell in DIR")
    ap.add_argument("-e", "--execute", nargs=argparse.REMAINDER, metavar="CMD",
                    help="run CMD in a new tab (lets Lestrix act as x-terminal-emulator)")
    return ap.parse_known_args(argv)


def main(argv: list[str] | None = None) -> int:
    from PyQt6.QtGui import QSurfaceFormat

    args, qt_args = parse_args(sys.argv[1:] if argv is None else argv)
    fmt = QSurfaceFormat()
    fmt.setSwapInterval(1)  # vsync: no redraws faster than the display
    QSurfaceFormat.setDefaultFormat(fmt)
    QApplication.setAttribute(Qt.ApplicationAttribute.AA_ShareOpenGLContexts)
    app = QApplication([sys.argv[0], *qt_args])
    app.setApplicationName("Lestrix")
    app.setDesktopFileName("lestrix")
    icon = ASSETS / "lestrix.png"
    if icon.exists():
        app.setWindowIcon(QIcon(str(icon)))
    families = QFontDatabase.families()
    for family in ("Roboto", "Noto Sans", "Segoe UI", "Cantarell", "Ubuntu"):
        if family in families:
            app.setFont(QFont(family, 10))
            break

    win = MainWindow(Store())
    win.show()
    if args.execute:
        win._add_tab(Path(args.execute[0]).name, argv=args.execute, cwd=args.working_directory)
    elif args.local or win.settings.value("startup_shell", False, type=bool):
        win._add_tab("local", argv=default_shell(), cwd=args.working_directory)
    else:
        win.welcome.setFocus()
    return app.exec()
