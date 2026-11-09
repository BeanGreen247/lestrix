#!/usr/bin/env python3
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
"""Inline-image window test helper.
  gfx_demo.py gen DIR     writes DIR/six.txt (a 60x30 pure-red Sixel) and DIR/kitty.txt (a 32x32 pure-blue kitty image, f=24, plus a green
                          one placed 2 cells wide with z=-1) for `cat` to print inside a Lestrix window
  gfx_demo.py magenta PPM N  asserts at least N pure magenta pixels (the 40x40 PNG `kitten icat` draws)
  gfx_demo.py check PPM   asserts the window screenshot shows them: exact red, blue and green pixels in the sizes the images have"""
import base64
import sys


def gen(d):
    rows = 5  # 30 px = 5 sixel bands
    six = "\x1bPq\"1;1;60;30#1;2;100;0;0#1" + "-".join(["!60~"] * rows) + "\x1b\\\n"
    open(d + "/six.txt", "w").write(six)
    blue = base64.b64encode(bytes([0, 0, 255]) * (32 * 32)).decode()
    green = base64.b64encode(bytes([0, 255, 0]) * (8 * 8)).decode()
    k = "\x1b_Ga=T,f=24,s=32,v=32,i=1;" + blue + "\x1b\\\n"
    k += "\x1b_Ga=T,f=24,s=8,v=8,i=2,c=4,r=2,z=-1;" + green + "\x1b\\\n"
    open(d + "/kitty.txt", "w").write(k)


def count(path):
    f = open(path, "rb")
    assert f.readline().strip() == b"P6"
    line = f.readline()
    while line.startswith(b"#"):
        line = f.readline()
    w, h = map(int, line.split())
    f.readline()
    data = f.read()
    n = {"red": 0, "blue": 0, "green": 0}
    for i in range(0, w * h * 3, 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if (r, g, b) == (255, 0, 0): n["red"] += 1
        elif (r, g, b) == (0, 0, 255): n["blue"] += 1
        elif (r, g, b) == (0, 255, 0): n["green"] += 1
    return n


def check(path):
    n = count(path)
    print("gfx pixels:", n)
    ok = n["red"] >= 1500 and n["blue"] >= 900 and n["green"] >= 1000   # 1800, 1024 and 4x2 cells of the stretched green
    if not ok:
        print("gfx_demo: image pixels missing from the screenshot", path)
        sys.exit(1)


def magenta(path, want):
    f = open(path, "rb")
    f.readline()
    line = f.readline()
    while line.startswith(b"#"):
        line = f.readline()
    w, h = map(int, line.split())
    f.readline()
    d = f.read()
    n = sum(1 for i in range(0, w * h * 3, 3) if (d[i], d[i + 1], d[i + 2]) == (255, 0, 255))
    print("magenta pixels:", n)
    if n < want:
        print("gfx_demo: expected at least", want, "magenta pixels in", path)
        sys.exit(1)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "gen": gen(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == "check": check(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "magenta": magenta(sys.argv[2], int(sys.argv[3]))
    else: sys.exit("usage: gfx_demo.py gen DIR | check PPM | magenta PPM MINPIXELS")
