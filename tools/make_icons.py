"""Render the app icon to PNG, ICO and ICNS (run: python tools/make_icons.py)."""

import struct
import sys
from pathlib import Path

from PyQt6.QtCore import QBuffer, QByteArray, QIODevice, QPointF, QRectF, Qt
from PyQt6.QtGui import QColor, QGuiApplication, QImage, QPainter, QPen

OUT = Path(__file__).resolve().parent.parent / "lestrix" / "assets"


def render(size: int) -> QImage:
    img = QImage(size, size, QImage.Format.Format_ARGB32)
    img.fill(Qt.GlobalColor.transparent)
    p = QPainter(img)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    s = size
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(QColor("#16181d"))
    p.drawRoundedRect(QRectF(s * .04, s * .04, s * .92, s * .92), s * .2, s * .2)
    p.setBrush(QColor("#20242b"))
    p.drawRoundedRect(QRectF(s * .04, s * .04, s * .92, s * .17), s * .2, s * .2)
    p.drawRect(QRectF(s * .04, s * .13, s * .92, s * .08))
    for i, c in enumerate(("#e06c75", "#e5c07b", "#4fb39a")):
        p.setBrush(QColor(c))
        p.drawEllipse(QPointF(s * (.17 + i * .09), s * .125), s * .027, s * .027)
    pen = QPen(QColor("#4fb39a"), s * .075, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
    p.setPen(pen)
    p.drawPolyline([QPointF(s * .24, s * .40), QPointF(s * .42, s * .56), QPointF(s * .24, s * .72)])
    p.drawLine(QPointF(s * .50, s * .73), QPointF(s * .74, s * .73))
    p.end()
    return img


def png_bytes(size: int) -> bytes:
    ba = QByteArray()
    buf = QBuffer(ba)
    buf.open(QIODevice.OpenModeFlag.WriteOnly)
    render(size).save(buf, "PNG")
    return bytes(ba)


def main() -> None:
    QGuiApplication(sys.argv[:1] + ["-platform", "offscreen"])
    OUT.mkdir(parents=True, exist_ok=True)
    big = png_bytes(256)
    (OUT / "lestrix.png").write_bytes(big)
    # ICO with an embedded 256px PNG (supported since Windows Vista)
    (OUT / "lestrix.ico").write_bytes(
        struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(big), 22) + big
    )
    # ICNS with a single 256px PNG entry (ic08)
    entry = b"ic08" + struct.pack(">I", len(big) + 8) + big
    (OUT / "lestrix.icns").write_bytes(b"icns" + struct.pack(">I", len(entry) + 8) + entry)
    print("icons written to", OUT)


if __name__ == "__main__":
    main()
