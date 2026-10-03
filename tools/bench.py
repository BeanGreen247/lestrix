"""Micro-benchmarks for the terminal hot paths. Run: QT_QPA_PLATFORM=offscreen python tools/bench.py"""

import os
import random
import sys
import time

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PyQt6.QtGui import QImage, QPainter
from PyQt6.QtWidgets import QApplication

from lestrix.terminal import TerminalWidgetSW


def make_data(mb: float) -> bytes:
    rnd = random.Random(1)
    words = ["alpha", "beta", "gamma", "delta", "/usr/lib/x86_64-linux-gnu", "error", "WARN", "0x7ffd1234", "ok"]
    lines = []
    size = 0
    while size < mb * 1e6:
        parts = []
        for _ in range(rnd.randint(4, 14)):
            w = rnd.choice(words)
            parts.append(f"\x1b[{rnd.choice([0, 1, 31, 32, 33, 34, 36, 90])}m{w}\x1b[0m" if rnd.random() < 0.4 else w)
        line = " ".join(parts) + "\r\n"
        lines.append(line)
        size += len(line)
    return "".join(lines).encode()


def main() -> None:
    app = QApplication([])
    term = TerminalWidgetSW(["/bin/cat"])
    term.resize(1600, 1000)
    term.show()
    app.processEvents()
    print(f"grid {term.cols}x{term.rows}")

    plain = ("".join(f"2026-10-03 12:00:{i % 60:02d} INFO worker-{i % 8} processed request id={i} in {i % 97} ms\r\n"
                     for i in range(100_000))).encode()
    for label, data in (("plain log", plain[:8_000_000]), ("colour-heavy", make_data(8))):
        t = time.perf_counter()
        for i in range(0, len(data), 65536):
            term._feed(data[i:i + 65536])
        dt = time.perf_counter() - t
        print(f"feed ({label}): {len(data) / 1e6 / dt:6.2f} MB/s  ({dt:.2f}s for {len(data) / 1e6:.1f} MB)")

    img = QImage(term.size(), QImage.Format.Format_ARGB32)
    frames = 40
    for label, rows in (("full repaint", range(term.rows)), ("one row dirty", [term.rows - 1]), ("nothing dirty", [])):
        t = time.perf_counter()
        for _ in range(frames):
            term.screen.dirty.update(rows)
            term.render(img)
        print(f"paint: {(time.perf_counter() - t) / frames * 1000:6.1f} ms/frame ({label})")
    term.close_session()


if __name__ == "__main__":
    main()
