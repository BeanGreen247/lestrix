#!/usr/bin/env python3
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
"""Run inside a Lestrix window by `make test-gui`: turns on the kitty keyboard protocol (flag 1), records the bytes the keys scripted
into the window produce until 1.2 s of silence, and writes repr(bytes) to the file named in argv[1]."""
import os, select, sys, termios, tty
fd = 0
old = termios.tcgetattr(fd)
tty.setraw(fd)
os.write(1, b"\x1b[>1u")
buf = b""
while True:
    r, _, _ = select.select([fd], [], [], 1.2)
    if not r:
        break
    buf += os.read(fd, 64)
os.write(1, b"\x1b[<u")
termios.tcsetattr(fd, termios.TCSADRAIN, old)
open(sys.argv[1], "w").write(repr(buf))
