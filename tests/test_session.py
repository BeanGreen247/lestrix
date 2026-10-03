import os
import time

import pytest
from PyQt6.QtWidgets import QApplication

from lestrix.session import SessionTab
from lestrix.store import Connection
from tests.test_sftp import OPTS, _localhost_ok

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


@pytest.fixture(scope="module")
def app():
    return QApplication.instance() or QApplication([])


def wait(app, cond, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        app.processEvents()
        if cond():
            return True
        time.sleep(0.05)
    return False


def test_tab_state_dots(app):
    tab = SessionTab(["/bin/sh"], "t", 11)
    tab.set_foreground(False)
    tab.term.send(b"echo hi\r")
    assert wait(app, lambda: tab.state == "activity")
    tab.set_foreground(True)
    assert tab.state == "idle"
    tab.term.send(b"exit\r")
    assert wait(app, lambda: tab.state == "ended")
    assert tab.dot
    tab.close_session()


@pytest.mark.skipif(not _localhost_ok(), reason="needs passwordless ssh to localhost")
def test_file_panel_follows_terminal_cwd(app):
    conn = Connection(name="l", host="localhost", options=" ".join(OPTS))
    tab = SessionTab([], "l", 11, conn, follow=True)
    tab.resize(1000, 600)
    tab.show()
    tab.panel.resize(300, 400)
    tab.panel.show()  # polling only runs while the panel is on screen
    try:
        assert wait(app, lambda: tab.panel._ready), tab.panel.status.text()
        assert wait(app, lambda: tab.panel.path != "")
        tab.term.send(b"cd /etc\r")
        assert wait(app, lambda: tab.panel.path == "/etc"), tab.panel.path
        assert tab.panel.tree.topLevelItemCount() > 5
        tab.panel.follow.setChecked(False)
        tab.term.send(b"cd /usr\r")
        time.sleep(2.5)
        app.processEvents()
        assert tab.panel.path == "/etc"
    finally:
        tab.close_session()


def test_rename_and_color_tab(app):
    from lestrix.app import MainWindow
    from lestrix.store import Store
    import tempfile, pathlib

    store = Store(pathlib.Path(tempfile.mkdtemp()) / "c.json")
    w = MainWindow(store)
    tab = w._add_tab("local", argv=["/bin/sh"])
    tab.title = "renamed"
    w.set_tab_color(tab, "#e06c75")
    assert w.tabs.tabText(0) == "renamed" and tab.color == "#e06c75"
    assert w.tabs.tabBar().tabTextColor(0).name() == "#e06c75"
    tab.close_session()
