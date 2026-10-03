"""File operations that ride on the terminal's own ssh connection (OpenSSH ControlMaster).

The terminal's `ssh` is the master; every operation here is a short-lived client that
multiplexes over its socket, so there is no second login and no second password prompt.

Backends, tried in this order:
  1. sftp          - browsing and transfers over the server's sftp subsystem
  2. ssh exec      - if sftp is missing locally or disabled on the server: ls/mv/rm/mkdir over ssh
  3. scp, then ssh - transfers when sftp is unavailable: legacy scp, else cat/tar streamed over ssh
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from .store import Connection

SUPPORTED = sys.platform != "win32"  # Windows OpenSSH has no ControlMaster


class SftpError(Exception):
    pass


class SftpUnavailable(SftpError):
    """The server has no sftp subsystem (or it is disabled in sshd_config)."""


@dataclass
class Entry:
    name: str
    is_dir: bool
    is_link: bool
    size: int
    modified: str
    perms: str


_LS_RE = re.compile(
    r"^(?P<perms>[-dlbcps][rwxsStT-]{9}[.+@]?)\s+(?:\d+|\?)\s+\S+\s+\S+\s+(?P<size>\d+)\s+"
    r"(?P<mtime>\w{3}\s+\d+\s+[\d:]+)\s+(?P<name>.+)$"
)


def parse_ls(output: str) -> list[Entry]:
    """Parse `ls -la` from sftp (OpenSSH long-name format)."""
    entries = []
    for line in output.splitlines():
        m = _LS_RE.match(line.strip())
        if not m:
            continue
        name = m["name"]
        is_link = m["perms"][0] == "l"
        if is_link and " -> " in name:
            name = name.split(" -> ", 1)[0]
        if name in (".", ".."):
            continue
        entries.append(Entry(name, m["perms"][0] == "d", is_link, int(m["size"]), m["mtime"], m["perms"]))
    entries.sort(key=lambda e: (not (e.is_dir or e.is_link), e.name.lower()))
    return entries


def q(path: str) -> str:
    """Quote a path for the sftp batch language."""
    return '"' + path.replace("\\", "\\\\").replace('"', '\\"') + '"'


def scp_quote(path: str) -> str:
    """Escape a remote path for legacy scp. Backslashes rather than quotes: the scp client
    glob-matches what the server sends back against this string, and quotes would never match."""
    return re.sub(r"([^A-Za-z0-9_./~+:@%-])", r"\\\1", path)


def make_control_path() -> str:
    # unix socket paths are limited to ~100 chars, so keep this short and under /tmp
    base = "/tmp" if os.path.isdir("/tmp") else tempfile.gettempdir()
    d = tempfile.mkdtemp(prefix="sd-", dir=base)
    os.chmod(d, 0o700)
    return str(Path(d) / "m")


class SftpSession:
    def __init__(self, conn: Connection, control_path: str):
        self.conn = conn
        self.control_path = control_path
        self.mode = "sftp"  # browsing backend: "sftp" or "ssh"
        self.xfer_order = ["scp", "ssh"]  # transfer fallbacks once sftp is unavailable
        self.dest = conn.dest
        self._port = [] if conn.alias else ["-p", str(conn.port)]

    def _mux(self) -> list[str]:
        return ["-o", f"ControlPath={self.control_path}", "-o", "ControlMaster=no", "-o", "BatchMode=yes"]

    @property
    def label(self) -> str:
        return "SFTP" if self.mode == "sftp" else "ssh (no SFTP on this server)"

    def master_ready(self) -> bool:
        if not os.path.exists(self.control_path):
            return False
        r = subprocess.run(
            ["ssh", "-S", self.control_path, "-O", "check", self.dest], capture_output=True, timeout=5
        )
        return r.returncode == 0

    def run(self, command: str, timeout: float = 10) -> str:
        r = subprocess.run(
            ["ssh", *self._mux(), *self._port, self.dest, command],
            capture_output=True, text=True, timeout=timeout,
        )
        if r.returncode != 0:
            raise SftpError(r.stderr.strip() or f"command failed ({r.returncode})")
        return r.stdout

    def batch(self, commands: list[str], timeout: float = 60) -> str:
        try:
            r = subprocess.run(
                ["sftp", *self._mux(), *([] if self.conn.alias else ["-P", str(self.conn.port)]), "-b", "-", self.dest],
                input="\n".join(commands) + "\n", capture_output=True, text=True, timeout=timeout,
            )
        except FileNotFoundError:
            raise SftpUnavailable("the sftp command is not installed") from None
        err = r.stderr.strip()
        if r.returncode != 0:
            if "subsystem request failed" in err.lower() or "subsystem" in err.lower():
                raise SftpUnavailable("SFTP is not enabled on this server (no sftp Subsystem in sshd_config)")
            raise SftpError(err or f"sftp failed ({r.returncode})")
        return r.stdout

    # -- operations ----------------------------------------------------------

    def _try_sftp(self, commands: list[str], timeout: float = 60) -> str | None:
        """Run an sftp batch. None means sftp is unavailable and the session fell back to ssh."""
        if self.mode != "sftp":
            return None
        try:
            return self.batch(commands, timeout)
        except SftpUnavailable:
            self.mode = "ssh"
            return None

    def home(self) -> str:
        out = self._try_sftp(["pwd"])
        if out is None:
            return self.run('printf %s "$HOME"').strip() or "/"
        m = re.search(r"Remote working directory:\s*(.+)", out)
        if not m:
            raise SftpError("could not read the remote home directory")
        return m.group(1).strip()

    def listdir(self, path: str) -> list[Entry]:
        out = self._try_sftp([f"cd {q(path)}", "ls -la"])
        if out is None:
            out = self.run(f"cd -- {shlex.quote(path)} && LC_ALL=C ls -la")
        return parse_ls(out)

    def is_dir(self, path: str) -> bool:
        try:
            if self._try_sftp([f"cd {q(path)}"]) is None:
                self.run(f"test -d {shlex.quote(path)}")
            return True
        except SftpError:
            return False

    def mkdir(self, path: str) -> None:
        if self._try_sftp([f"mkdir {q(path)}"]) is None:
            self.run(f"mkdir -- {shlex.quote(path)}")

    def rename(self, old: str, new: str) -> None:
        if self._try_sftp([f"rename {q(old)} {q(new)}"]) is None:
            self.run(f"mv -- {shlex.quote(old)} {shlex.quote(new)}")

    def remove(self, path: str, is_dir: bool) -> None:
        if is_dir:  # sftp has no recursive delete
            self.run(f"rm -rf -- {shlex.quote(path)}")
        elif self._try_sftp([f"rm {q(path)}"]) is None:
            self.run(f"rm -- {shlex.quote(path)}")

    # -- transfers ---------------------------------------------------------------

    def download(self, remote: str, local_dir: Path, is_dir: bool = False) -> None:
        if self._try_sftp([f"get {'-r ' if is_dir else ''}{q(remote)} {q(str(local_dir))}"], 3600) is not None:
            return
        self._fallback_transfer(
            scp=lambda: self._scp([*(["-r"] if is_dir else []), f"{self.dest}:{scp_quote(remote)}", f"{local_dir}/"]),
            stream=lambda: self._stream_down(remote, local_dir, is_dir),
        )

    def upload(self, local: Path, remote_dir: str) -> None:
        if self._try_sftp([f"put {'-r ' if local.is_dir() else ''}{q(str(local))} {q(remote_dir)}"], 3600) is not None:
            return
        self._fallback_transfer(
            scp=lambda: self._scp([*(["-r"] if local.is_dir() else []), str(local),
                                   f"{self.dest}:{scp_quote(remote_dir.rstrip('/') + '/')}"]),
            stream=lambda: self._stream_up(local, remote_dir),
        )

    def _fallback_transfer(self, scp, stream) -> None:
        errors = []
        for method in self.xfer_order:
            try:
                return {"scp": scp, "ssh": stream}[method]()
            except (SftpError, OSError, subprocess.TimeoutExpired) as exc:
                errors.append(f"{method}: {exc}")
        raise SftpError("; ".join(errors))

    def _scp(self, args: list[str]) -> None:
        base = ["scp", *self._mux(), *([] if self.conn.alias else ["-P", str(self.conn.port)])]
        # -O = classic scp protocol, which works without an sftp subsystem (OpenSSH 9 defaults to sftp)
        for legacy in (["-O"], []):
            r = subprocess.run([*base, *legacy, *args], capture_output=True, text=True, timeout=3600)
            if r.returncode == 0:
                return
            if legacy and re.search(r"unknown option|illegal option|usage:", r.stderr, re.I):
                continue
            raise SftpError(r.stderr.strip() or f"scp failed ({r.returncode})")

    def _ssh_cmd(self, command: str) -> list[str]:
        return ["ssh", *self._mux(), *self._port, self.dest, command]

    def _stream_down(self, remote: str, local_dir: Path, is_dir: bool) -> None:
        parent, name = str(PurePosixPath(remote).parent), PurePosixPath(remote).name
        if is_dir:
            src = subprocess.Popen(self._ssh_cmd(f"tar -C {shlex.quote(parent)} -cf - {shlex.quote(name)}"),
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            dst = subprocess.run(["tar", "-C", str(local_dir), "-xf", "-"], stdin=src.stdout, capture_output=True)
            src.stdout.close()
            err = src.stderr.read().decode(errors="replace").strip()
            if src.wait() != 0 or dst.returncode != 0:
                raise SftpError(err or dst.stderr.decode(errors="replace").strip() or "tar failed")
        else:
            with open(local_dir / name, "wb") as fh:
                r = subprocess.run(self._ssh_cmd(f"cat -- {shlex.quote(remote)}"), stdout=fh,
                                   stderr=subprocess.PIPE, timeout=3600)
            if r.returncode != 0:
                (local_dir / name).unlink(missing_ok=True)
                raise SftpError(r.stderr.decode(errors="replace").strip() or "download failed")

    def _stream_up(self, local: Path, remote_dir: str) -> None:
        if local.is_dir():
            src = subprocess.Popen(["tar", "-C", str(local.parent), "-cf", "-", local.name], stdout=subprocess.PIPE)
            dst = subprocess.run(self._ssh_cmd(f"tar -C {shlex.quote(remote_dir)} -xf -"), stdin=src.stdout,
                                 capture_output=True, timeout=3600)
            src.stdout.close()
            if src.wait() != 0 or dst.returncode != 0:
                raise SftpError(dst.stderr.decode(errors="replace").strip() or "upload failed")
        else:
            target = shlex.quote(join(remote_dir, local.name))
            with open(local, "rb") as fh:
                r = subprocess.run(self._ssh_cmd(f"cat > {target}"), stdin=fh, capture_output=True, timeout=3600)
            if r.returncode != 0:
                raise SftpError(r.stderr.decode(errors="replace").strip() or "upload failed")

    def terminal_cwd(self) -> str | None:
        """Working directory of this connection's interactive shell (Linux servers, via /proc).

        The exec'd command and the login shell are siblings under the same sshd
        process, so $PPID finds the shell: the child that owns a tty.
        """
        script = (
            'p=$(ps -o pid=,tty= --ppid $PPID 2>/dev/null | awk \'$2!="?"{print $1; exit}\'); '
            '[ -n "$p" ] && readlink /proc/$p/cwd'
        )
        try:
            out = self.run(script, timeout=5).strip()
        except (SftpError, subprocess.TimeoutExpired):
            return None
        return out or None


def join(base: str, name: str) -> str:
    return str(PurePosixPath(base) / name)
