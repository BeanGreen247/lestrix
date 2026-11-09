#!/usr/bin/env python3
# usage: lig_check.py on.ppm off.ppm -- the prompt line typed with `a -> b => c` must look different with ligatures on than off.
import sys
from PIL import Image, ImageChops
box = (660, 60, 1330, 125)
a = Image.open(sys.argv[1]).convert("RGB").crop(box)
b = Image.open(sys.argv[2]).convert("RGB").crop(box)
d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 40 else 0)
n = sum(1 for v in d.getdata() if v)
print("ligature pixels changed:", n)
sys.exit(0 if n >= 150 else 1)
