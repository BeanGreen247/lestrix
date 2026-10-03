"""SFTP file browser panel that can follow the terminal's working directory."""

from __future__ import annotations

import subprocess
import time
from pathlib import Path, PurePosixPath

from PyQt6.QtCore import QObject, QRunnable, QStandardPaths, Qt, QThreadPool, QTimer, pyqtSignal
from PyQt6.QtGui import QDragEnterEvent, QDropEvent
from PyQt6.QtWidgets import (
    QApplication, QCheckBox, QFileDialog, QHBoxLayout, QInputDialog, QLabel, QLineEdit, QMenu,
    QMessageBox, QStyle, QToolButton, QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget,
)

from .sftp import SftpError, SftpSession, SftpUnavailable, join


class _Signals(QObject):
    done = pyqtSignal(object)
    failed = pyqtSignal(object)


class _Job(QRunnable):
    def __init__(self, fn):
        super().__init__()
        self.fn = fn
        self.signals = _Signals()

    def run(self) -> None:
        try:
            result = self.fn()
        except subprocess.TimeoutExpired:
            self.signals.failed.emit(SftpError("timed out"))
        except Exception as exc:  # reported to the user in the panel status line
            self.signals.failed.emit(exc)
        else:
            self.signals.done.emit(result)


def fmt_size(n: int) -> str:
    size = float(n)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if size < 1024 or unit == "TB":
            return f"{int(size)} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return str(n)


class _DropTree(QTreeWidget):
    files_dropped = pyqtSignal(list)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if event.mimeData().hasUrls():
            event.acceptProposedAction()

    dragMoveEvent = dragEnterEvent

    def dropEvent(self, event: QDropEvent) -> None:
        paths = [Path(u.toLocalFile()) for u in event.mimeData().urls() if u.isLocalFile()]
        if paths:
            self.files_dropped.emit(paths)
            event.acceptProposedAction()


class FilePanel(QWidget):
    """Remote file browser. Needs the terminal's ssh to be the ControlMaster."""

    def __init__(self, session: SftpSession, follow: bool = True, parent=None):
        super().__init__(parent)
        self.session = session
        self.path = ""
        self._ready = False
        self._busy = 0
        self._pool = QThreadPool.globalInstance()
        self._jobs: set[_Job] = set()
        self._osc7_seen = False
        self._active = True  # terminal output since the last cwd poll
        self._last_poll = 0.0
        self._pending_cwd: str | None = None
        style = self.style()

        def tool(icon, tip, slot) -> QToolButton:
            b = QToolButton(autoRaise=True, toolTip=tip)
            b.setIcon(style.standardIcon(icon))
            b.clicked.connect(slot)
            return b

        bar = QHBoxLayout()
        bar.setSpacing(2)
        S = QStyle.StandardPixmap
        for icon, tip, slot in (
            (S.SP_ArrowUp, "Parent folder", self.go_up),
            (S.SP_BrowserReload, "Refresh", lambda: self.navigate(self.path)),
            (S.SP_DirHomeIcon, "Home folder", lambda: self.navigate(None)),
        ):
            bar.addWidget(tool(icon, tip, slot))
        actions = QToolButton(text="Actions", popupMode=QToolButton.ToolButtonPopupMode.InstantPopup)
        menu = QMenu(actions)
        menu.addAction("New folder…", self.new_folder)
        menu.addAction("Upload files…", self.upload_dialog)
        menu.addAction("Download selected…", self.download_selected)
        menu.addSeparator()
        menu.addAction("Rename…", self.rename_selected)
        menu.addAction("Delete…", self.delete_selected)
        actions.setMenu(menu)
        bar.addWidget(actions)
        bar.addStretch(1)

        self.pathbox = QLineEdit(placeholderText="/")
        self.pathbox.returnPressed.connect(lambda: self.navigate(self.pathbox.text().strip() or "/"))
        self.follow = QCheckBox("Follow terminal path")
        self.follow.setChecked(follow)
        self.follow.setToolTip("Browse to the directory your shell is in, as you cd around")
        self.follow.toggled.connect(self._follow_toggled)

        self.tree = _DropTree()
        self.tree.setColumnCount(3)
        self.tree.setHeaderLabels(["Name", "Size", "Modified"])
        self.tree.setRootIsDecorated(False)
        self.tree.setSelectionMode(QTreeWidget.SelectionMode.ExtendedSelection)
        self.tree.setAcceptDrops(True)
        self.tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._menu)
        self.tree.itemActivated.connect(self._activate)
        self.tree.files_dropped.connect(self.upload)
        self.tree.header().resizeSection(0, 180)

        self.status = QLabel("Waiting for the connection…")
        self.status.setObjectName("statusLine")
        self.status.setWordWrap(True)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(6, 6, 4, 6)
        lay.addLayout(bar)
        lay.addWidget(self.pathbox)
        lay.addWidget(self.follow)
        lay.addWidget(self.tree, 1)
        lay.addWidget(self.status)

        self._wait = QTimer(self, interval=1000)
        self._wait.timeout.connect(self._check_master)
        self._wait.start()
        self._poll = QTimer(self, interval=1500)
        self._poll.timeout.connect(self._poll_cwd)
        self._polling = False
        self._follow_toggled(follow)

    # -- async plumbing --------------------------------------------------------

    def _run(self, fn, ok=None, fail=None) -> None:
        job = _Job(fn)
        self._jobs.add(job)

        def finish(cb, value):
            self._jobs.discard(job)
            if cb:
                cb(value)

        job.signals.done.connect(lambda r: finish(ok, r))
        job.signals.failed.connect(lambda e: finish(fail or self._error, e))
        self._pool.start(job)

    def _error(self, exc: Exception) -> None:
        self.status.setText(f"⚠ {exc}")

    # -- connection / listing --------------------------------------------------

    def _check_master(self) -> None:
        if self._ready:
            return
        self._run(self.session.master_ready, self._master_result, lambda e: None)

    def _master_result(self, ok: bool) -> None:
        if not ok or self._ready:
            return
        self._ready = True
        self._wait.stop()
        self.status.setText("Connected")
        self.navigate(None)

    def navigate(self, path: str | None) -> None:
        if not self._ready:
            return

        def work():
            target = path if path else self.session.home()
            return target, self.session.listdir(target)

        self._run(work, self._listed)

    def _listed(self, result) -> None:
        path, entries = result
        self.path = path
        self.pathbox.setText(path)
        self.tree.clear()
        for e in entries:
            item = QTreeWidgetItem([e.name, "" if e.is_dir else fmt_size(e.size), e.modified])
            icon = QStyle.StandardPixmap.SP_DirIcon if e.is_dir or e.is_link else QStyle.StandardPixmap.SP_FileIcon
            item.setIcon(0, self.style().standardIcon(icon))
            item.setData(0, Qt.ItemDataRole.UserRole, e)
            item.setTextAlignment(1, Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            self.tree.addTopLevelItem(item)
        self.status.setText(f"{len(entries)} item(s) · {self.session.label}")
        if self._pending_cwd and self._pending_cwd != self.path and self.follow.isChecked():
            nxt, self._pending_cwd = self._pending_cwd, None
            self.navigate(nxt)

    def go_up(self) -> None:
        if self.path and self.path != "/":
            self.navigate(str(PurePosixPath(self.path).parent))

    def _activate(self, item: QTreeWidgetItem) -> None:
        e = item.data(0, Qt.ItemDataRole.UserRole)
        target = join(self.path, e.name)
        if e.is_dir:
            self.navigate(target)
        elif e.is_link:
            # a symlink may point at a directory: try it, else treat as a file
            self._run(lambda: self.session.is_dir(target),
                      lambda is_dir: self.navigate(target) if is_dir else self._download([target], self._downloads(), [False]))
        else:
            self._download([target], self._downloads(), [False])

    # -- follow terminal path ---------------------------------------------------

    def _follow_toggled(self, on: bool) -> None:
        if on:
            self._poll.start()
            self._poll_cwd(force=True)
        else:
            self._poll.stop()

    def terminal_cwd(self, path: str) -> None:
        """Called with an OSC 7 report from the shell; pushes instead of polling."""
        self._osc7_seen = True
        self._poll.stop()
        if self.follow.isChecked() and path != self.path:
            self._pending_cwd = path
            if self._ready:
                self.navigate(path)

    def note_activity(self) -> None:
        self._active = True

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._poll_cwd(force=True)

    def _poll_cwd(self, force: bool = False) -> None:
        if not (self._ready and self.follow.isChecked()) or self._polling or self._osc7_seen:
            return
        if not force:
            # each poll is an ssh round trip: skip it while hidden, and while the shell is quiet
            if not self.isVisible():
                return
            if not self._active and time.monotonic() - self._last_poll < 10:
                return
        self._active = False
        self._last_poll = time.monotonic()
        self._polling = True

        def done(cwd):
            self._polling = False
            if cwd and cwd != self.path:
                self.navigate(cwd)

        self._run(self.session.terminal_cwd, done, lambda e: setattr(self, "_polling", False))

    # -- file operations --------------------------------------------------------

    @staticmethod
    def _downloads() -> Path:
        d = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DownloadLocation)
        return Path(d or Path.home())

    def _selected(self) -> list:
        return [i.data(0, Qt.ItemDataRole.UserRole) for i in self.tree.selectedItems()]

    def _download(self, remotes: list[str], dest: Path, is_dirs: list[bool]) -> None:
        self.status.setText(f"Downloading {len(remotes)} item(s)…")

        def work():
            for r, d in zip(remotes, is_dirs):
                self.session.download(r, dest, d)
            return dest

        self._run(work, lambda d: self.status.setText(f"Downloaded to {d}"))

    def download_selected(self) -> None:
        sel = self._selected()
        if not sel:
            self.status.setText("Select something to download first")
            return
        dest = QFileDialog.getExistingDirectory(self, "Download to", str(self._downloads()))
        if dest:
            self._download([join(self.path, e.name) for e in sel], Path(dest), [e.is_dir for e in sel])

    def upload_dialog(self) -> None:
        files, _ = QFileDialog.getOpenFileNames(self, "Upload files", str(Path.home()))
        if files:
            self.upload([Path(f) for f in files])

    def upload(self, paths: list[Path]) -> None:
        if not self._ready:
            return
        self.status.setText(f"Uploading {len(paths)} item(s)…")
        target = self.path

        def work():
            for p in paths:
                self.session.upload(p, target)

        self._run(work, lambda _: (self.status.setText("Upload complete"), self.navigate(target)))

    def new_folder(self) -> None:
        name, ok = QInputDialog.getText(self, "New folder", "Folder name:")
        if ok and name.strip():
            target = join(self.path, name.strip())
            self._run(lambda: self.session.mkdir(target), lambda _: self.navigate(self.path))

    def rename_selected(self) -> None:
        sel = self._selected()
        if len(sel) != 1:
            return
        name, ok = QInputDialog.getText(self, "Rename", "New name:", text=sel[0].name)
        if ok and name.strip() and name != sel[0].name:
            old, new = join(self.path, sel[0].name), join(self.path, name.strip())
            self._run(lambda: self.session.rename(old, new), lambda _: self.navigate(self.path))

    def delete_selected(self) -> None:
        sel = self._selected()
        if not sel:
            return
        names = ", ".join(e.name for e in sel[:5]) + ("…" if len(sel) > 5 else "")
        if QMessageBox.question(self, "Delete", f"Delete {len(sel)} item(s) on the server?\n{names}") \
                != QMessageBox.StandardButton.Yes:
            return
        items = [(join(self.path, e.name), e.is_dir) for e in sel]

        def work():
            for path, is_dir in items:
                self.session.remove(path, is_dir)

        self._run(work, lambda _: self.navigate(self.path))

    def _menu(self, pos) -> None:
        sel = self._selected()
        menu = QMenu(self)
        if sel:
            menu.addAction("Download…", self.download_selected)
            if len(sel) == 1:
                menu.addAction("Rename…", self.rename_selected)
                menu.addAction("Copy path", lambda: QApplication.clipboard().setText(join(self.path, sel[0].name)))
            menu.addAction("Delete…", self.delete_selected)
            menu.addSeparator()
        menu.addAction("Upload files…", self.upload_dialog)
        menu.addAction("New folder…", self.new_folder)
        menu.addAction("Refresh", lambda: self.navigate(self.path))
        menu.exec(self.tree.viewport().mapToGlobal(pos))
