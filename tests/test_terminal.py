import os
import shutil
import time

import pytest
from PyQt6.QtWidgets import QApplication

from lestrix.terminal import create_terminal, detect_shells

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


@pytest.fixture(scope="module")
def app():
    return QApplication.instance() or QApplication([])


def text(term) -> str:
    return "\n".join(
        "".join(term.screen.buffer[y][x].data for x in range(term.cols)).rstrip() for y in range(term.rows)
    )


def pump(app, term, needle, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        app.processEvents()
        if needle in text(term):
            return True
        time.sleep(0.02)
    return False


def test_detect_shells_has_default():
    assert detect_shells()


@pytest.mark.parametrize("shell", [s for s in ("bash", "zsh", "fish", "sh") if shutil.which(s)])
def test_shell_runs_command(app, shell):
    term = create_terminal([shutil.which(shell)])
    term.resize(800, 400)
    term.show()
    app.processEvents()
    time.sleep(0.5)
    term.send(b"echo SD_$((20+22))_OK\r" if shell != "fish" else b"echo SD_(math 20+22)_OK\r")
    assert pump(app, term, "SD_42_OK"), text(term)
    term.close_session()


def test_alt_screen_restores_main(app):
    term = create_terminal(["/bin/sh"])
    term.resize(800, 400)
    app.processEvents()
    term.send(b"echo MA$((1))N; printf '\\033[?1049hXY%d' 7; sleep 0.3; printf '\\033[?1049l'; echo BA$((2))CK\r")
    assert pump(app, term, "BA2CK"), text(term)
    assert "MA1N" in text(term) and "XY7" not in text(term)
    term.close_session()


def test_bracketed_paste_and_mouse_tracking(app):
    term = create_terminal(["/bin/sh"])
    term._feed(b"\x1b[?2004h\x1b[?1000h\x1b[?1006h")
    assert term.bracketed and term.mouse and term.mouse_sgr
    sent = []
    term.send = sent.append
    term._paste_text("a\nb")
    assert sent == [b"\x1b[200~a\rb\x1b[201~"]
    term.close_session()


def test_split_mode_sequence_across_chunks(app):
    term = create_terminal(["/bin/sh"])
    term._feed(b"\x1b[?10")
    term._feed(b"49hX")
    assert term.alt
    term.close_session()
