# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
"""Runs lxcat in a pty the way the terminal does (LESTRIX_FASTCAT set), answers its OSC 7777 requests by reading and
unlinking the files, and checks the bytes are identical to the input: from a pipe (splice path) and from a file
redirect (read/write fallback), for empty, tiny, multi-chunk and 40 MB inputs."""
import hashlib, os, pty, re, select, sys, tempfile, urllib.parse

lx = sys.argv[1]
fails = 0


def run(data_path, mode):
    cmd = {"pipe": "/bin/cat '%s' | '%s' -" % (data_path, lx), "redir": "'%s' - < '%s'" % (lx, data_path)}[mode]
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["LESTRIX_FASTCAT"] = "tok"
        os.execvp("/bin/sh", ["sh", "-c", cmd])
    buf, got, total = b"", hashlib.sha256(), 0
    pat = re.compile(rb"\x1b\]7777;cat;tok;([^;]*);0;0;(\d+)\x07")
    while True:
        r, _, _ = select.select([fd], [], [], 60)
        if not r:
            break
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
        while True:
            m = pat.search(buf)
            if not m:
                break
            buf = buf[m.end():]
            path = urllib.parse.unquote(m.group(1).decode())
            with open(path, "rb") as f:
                b = f.read()
            got.update(b)
            total += len(b)
            if int(m.group(2)) & 1 and path.startswith("/dev/shm/lxcat-"):
                os.unlink(path)
    os.waitpid(pid, 0)
    return got.hexdigest(), total


with tempfile.TemporaryDirectory() as d:
    cases = {"empty": b"", "tiny": b"hello\n", "chunky": os.urandom(9 << 20), "big": os.urandom(40 << 20)}
    for name, data in cases.items():
        p = os.path.join(d, name)
        with open(p, "wb") as f:
            f.write(data)
        want = hashlib.sha256(data).hexdigest()
        for mode in ("pipe", "redir"):
            h, n = run(p, mode)
            ok = h == want and n == len(data)
            fails += not ok
            print("  lxcat %-6s %-5s %9d bytes %s" % (name, mode, n, "ok" if ok else "MISMATCH"))
leftover = [f for f in os.listdir("/dev/shm") if f.startswith("lxcat-")]
if leftover:
    fails += 1
    print("  leftover files in /dev/shm:", leftover)
sys.exit(1 if fails else 0)
