#!/usr/bin/env python3
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

"""Writes the everyday benchmark files used by `benchmark`: a log, colour-heavy text, Unicode text, `seq` output and
full-screen redraws. Deterministic: the same bytes every time, so every terminal is shown identical output.
usage: gen_workloads.py DIR [NAME...]   (NAME: log colour unicode seq redraw; default all)"""
import os, random, sys

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
rnd = random.Random(42)

want = set(sys.argv[2:]) or {"log", "colour", "unicode", "seq", "redraw"}

def write(name, chunks):
    if name[:-4] not in want:
        return
    p = os.path.join(d, name)
    with open(p, "wb") as f:
        for c in chunks:
            f.write(c)

def log(total=107_000_000):
    n = i = 0
    while n < total:
        line = b"2026-10-03 12:00:%02d INFO worker-%d processed request id=%d in %d ms\n" % (i % 60, i % 8, i, i % 97)
        yield line; n += len(line); i += 1

def colour(total=40_000_000):
    words = ["alpha", "beta", "gamma", "delta", "error"]
    n = 0
    while n < total:
        out = []
        for k in range(8):
            out.append("\x1b[%dm%s\x1b[0m \x1b[1;%dm/usr/lib/x86_64-linux-gnu\x1b[0m " % (30 + k % 8, words[k % 5], 90 + k % 8))
        line = ("".join(out) + "0\r\n").encode()
        yield line; n += len(line)

def unicode_text(total=30_000_000):
    pool = "abcdefghijklmnopqrstuvwxyz àéîõüßñøåæ αβγδεζηθ жзийкл ─│┌┐└┘═║ 日本語漢字한국어 ".replace(" ", "  ")
    n = 0
    while n < total:
        line = ("".join(rnd.choice(pool) for _ in range(120)) + "\r\n").encode()
        yield line; n += len(line)

def seq():
    for i in range(1, 4_000_001):
        yield b"%d\n" % i

def redraw(frames=1500, rows=50, cols=200):
    for fr in range(frames):
        out = [b"\x1b[H"]
        for r in range(1, rows + 1):
            out.append(("\x1b[%d;1H" % r).encode())
            for k in range(10):
                out.append(("\x1b[%d;%dm" % (31 + (r + k + fr) % 7, 40 + (r + k) % 8)).encode())
                out.append(bytes(33 + (r + k * 3 + fr + j) % 90 for j in range(10)))
            out.append(b"\x1b[0m")
        yield b"".join(out)

write("log.txt", log())
write("colour.txt", colour())
write("unicode.txt", unicode_text())
write("seq.txt", seq())
write("redraw.txt", redraw())
