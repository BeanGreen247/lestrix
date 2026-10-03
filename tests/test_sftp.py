import shutil
import subprocess

import pytest

from lestrix.sftp import SftpSession, make_control_path, parse_ls, q
from lestrix.store import Connection

OPTS = ["-o", "UserKnownHostsFile=/dev/null", "-o", "StrictHostKeyChecking=no", "-o", "LogLevel=ERROR"]
LS = """drwxr-xr-x    2 bob      bob          4096 Oct  3 12:01 My Docs
-rw-r--r--    1 bob      bob           120 Jan  5  2025 notes.txt
lrwxrwxrwx    1 bob      bob             4 Jan  5 10:00 link -> docs
drwxr-xr-x    2 bob      bob          4096 Oct  3 12:01 .
drwxr-xr-x    9 bob      bob          4096 Oct  3 12:01 ..
"""


def test_parse_ls_handles_spaces_links_and_sorting():
    es = parse_ls(LS)
    assert [e.name for e in es] == ["link", "My Docs", "notes.txt"]
    assert es[1].is_dir and es[0].is_link and es[2].size == 120


def test_quote():
    assert q('a "b"\\c') == '"a \\"b\\"\\\\c"'


def _localhost_ok() -> bool:
    if not shutil.which("ssh"):
        return False
    r = subprocess.run(["ssh", *OPTS, "-o", "BatchMode=yes", "-o", "ConnectTimeout=3", "localhost", "true"],
                       capture_output=True)
    return r.returncode == 0


@pytest.mark.skipif(not _localhost_ok(), reason="needs passwordless ssh to localhost")
@pytest.mark.parametrize("mode,xfer", [("sftp", None), ("ssh", ["scp"]), ("ssh", ["ssh"])],
                         ids=["sftp", "ssh-exec+scp", "ssh-exec+stream"])
def test_roundtrip_over_multiplexed_connection(tmp_path, mode, xfer):
    cp = make_control_path()
    conn = Connection(name="l", host="localhost", user="")
    master = subprocess.Popen(
        ["ssh", *OPTS, "-N", "-o", "ControlMaster=yes", "-o", f"ControlPath={cp}", "localhost"])
    try:
        import time
        s = SftpSession(conn, cp)
        for _ in range(50):
            if s.master_ready():
                break
            time.sleep(0.1)
        assert s.master_ready()
        s.mode = mode
        if xfer:
            s.xfer_order = xfer
        home = s.home()
        work = f"{home}/.lestrix-test dir"
        s.mkdir(work)
        f = tmp_path / "up file.txt"
        f.write_text("hi")
        d = tmp_path / "updir"
        d.mkdir()
        (d / "inner.txt").write_text("deep")
        s.upload(f, work)
        s.upload(d, work)
        names = [e.name for e in s.listdir(work)]
        assert names == ["updir", "up file.txt"], names
        out = tmp_path / "dl"
        out.mkdir()
        s.download(f"{work}/up file.txt", out)
        s.download(f"{work}/updir", out, is_dir=True)
        assert (out / "up file.txt").read_text() == "hi"
        assert (out / "updir" / "inner.txt").read_text() == "deep"
        s.rename(f"{work}/up file.txt", f"{work}/renamed.txt")
        assert s.is_dir(work) and not s.is_dir(f"{work}/renamed.txt")
        s.remove(f"{work}/renamed.txt", is_dir=False)
        s.remove(work, is_dir=True)
        assert not s.is_dir(work)
    finally:
        master.terminate()
        master.wait()


def test_missing_sftp_binary_falls_back_to_ssh(monkeypatch):
    import lestrix.sftp as m

    real = subprocess.run

    def fake(cmd, *a, **k):
        if cmd[0] == "sftp":
            raise FileNotFoundError("sftp")
        return real(cmd, *a, **k)

    monkeypatch.setattr(m.subprocess, "run", fake)
    s = SftpSession(Connection(name="x", host="h"), "/nonexistent")
    assert s._try_sftp(["pwd"]) is None and s.mode == "ssh"
