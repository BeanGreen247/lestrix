import random, sys
sys.path.insert(0, "tests")
import diff_pyte as d

def mismatch(pieces, cols=40, rows=12):
    data = "".join(pieces).encode()
    ref = d.pyte.Screen(cols, rows); d.pyte.ByteStream(ref).feed(data)
    t = d.vt.vt_new(cols, rows, 100); d.vt.vt_feed(t, data, len(data))
    r = d.snapshot_c(t, cols, rows) != d.snapshot_p(ref, cols, rows)
    d.vt.vt_free(t); return r

seed = int(sys.argv[1])
rnd = random.Random(seed)
pieces = [rnd.choice(d.PIECES) for _ in range(rnd.randint(5, 150))]
if not mismatch(pieces):
    print("whole-feed does not mismatch (chunking issue?)"); sys.exit()
changed = True
while changed:
    changed = False
    for i in range(len(pieces)):
        t = pieces[:i] + pieces[i+1:]
        if t and mismatch(t): pieces = t; changed = True; break
print(pieces)
