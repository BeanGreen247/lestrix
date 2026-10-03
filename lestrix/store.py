"""Connection model, JSON storage and importers (Ansible INI, ~/.ssh/config)."""

from __future__ import annotations

import json
import os
import sys
import time
import uuid
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

X11_MODES = ("off", "untrusted", "trusted")
DEFAULT_GROUP = "Ungrouped"


def config_dir() -> Path:
    override = os.environ.get("LESTRIX_CONFIG_DIR")
    if override:
        return Path(override)
    if sys.platform == "win32":
        base = Path(os.environ.get("APPDATA", Path.home() / "AppData" / "Roaming"))
    elif sys.platform == "darwin":
        base = Path.home() / "Library" / "Application Support"
    else:
        base = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    return base / "lestrix"


@dataclass
class Connection:
    name: str
    host: str
    group: str = DEFAULT_GROUP
    port: int = 22
    user: str = ""
    key: str = ""
    x11: str = "off"  # off | untrusted (-X) | trusted (-Y)
    options: str = ""  # extra ssh arguments, shell-style
    remote_command: str = ""  # shell/command to run on the host, e.g. "fish" or "tmux new -A -s main"
    id: str = field(default_factory=lambda: uuid.uuid4().hex)
    alias: str = ""  # set for ~/.ssh/config hosts: connect with `ssh <alias>` so the config applies
    color: str = ""  # tab colour as #rrggbb, empty = default
    last_used: float = 0.0  # unix time of the last connect, drives the start page

    def ssh_argv(self, control_path: str | None = None) -> list[str]:
        import shlex

        argv = ["ssh"] if self.alias else ["ssh", "-p", str(self.port), "-o", "StrictHostKeyChecking=accept-new"]
        if control_path:  # lets the file browser reuse this authenticated connection
            argv += ["-o", "ControlMaster=yes", "-o", f"ControlPath={control_path}", "-o", "ControlPersist=no"]
        if self.key and not self.alias:
            argv += ["-i", os.path.expanduser(self.key)]
        if self.x11 == "untrusted":
            argv.append("-X")
        elif self.x11 == "trusted":
            argv.append("-Y")
        if self.options.strip():
            argv += shlex.split(self.options)
        if self.remote_command.strip():
            argv.append("-t")  # force a tty so interactive shells and tmux work
        argv.append(self.dest)
        if self.remote_command.strip():
            argv.append(self.remote_command.strip())
        return argv

    @property
    def dest(self) -> str:
        if self.alias:
            return self.alias
        return f"{self.user}@{self.host}" if self.user else self.host

    @property
    def is_live(self) -> bool:
        return bool(self.alias)

    @classmethod
    def from_dict(cls, data: dict) -> "Connection":
        known = {f.name for f in fields(cls)}
        conn = cls(**{k: v for k, v in data.items() if k in known})
        conn.port = int(conn.port)
        if conn.x11 not in X11_MODES:
            conn.x11 = "off"
        return conn


class Store:
    def __init__(self, path: Path | None = None):
        self.path = path or config_dir() / "connections.json"
        self.connections: list[Connection] = []
        self.live: list[Connection] = []  # hosts read live from ~/.ssh/config, never saved
        self.meta: dict[str, dict] = {}  # colour / last_used for live hosts
        self.load()

    def load(self) -> None:
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            self.connections = [Connection.from_dict(c) for c in raw.get("connections", [])]
            self.meta = raw.get("meta", {})
        except FileNotFoundError:
            self.connections = []

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        payload = {"version": 1, "connections": [asdict(c) for c in self.connections], "meta": self.meta}
        tmp.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.path)

    def groups(self) -> list[str]:
        seen: list[str] = []
        for c in self.connections:
            if c.group not in seen:
                seen.append(c.group)
        return seen

    def all(self) -> list[Connection]:
        return self.connections + self.live

    def reload_ssh_config(self, enabled: bool = True, path: Path | None = None) -> None:
        self.live = []
        if enabled:
            try:
                self.live = parse_ssh_config(path or Path.home() / ".ssh" / "config", live=True)
            except OSError:
                pass
        saved = {c.name for c in self.connections}
        self.live = [c for c in self.live if c.name not in saved]  # a saved copy wins
        for c in self.live:
            for key, value in self.meta.get(c.id, {}).items():
                setattr(c, key, value)

    def get(self, conn_id: str) -> Connection | None:
        return next((c for c in self.all() if c.id == conn_id), None)

    def update_meta(self, conn: Connection, **fields) -> None:
        """Persist colour / last_used; live hosts keep these in `meta`."""
        for key, value in fields.items():
            setattr(conn, key, value)
        if conn.is_live:
            self.meta.setdefault(conn.id, {}).update(fields)
            self.save()
        else:
            self.upsert(conn)

    def upsert(self, conn: Connection) -> None:
        for i, existing in enumerate(self.connections):
            if existing.id == conn.id:
                self.connections[i] = conn
                break
        else:
            self.connections.append(conn)
        self.save()

    def touch(self, conn_id: str) -> None:
        conn = self.get(conn_id)
        if conn:
            self.update_meta(conn, last_used=time.time())

    def recent(self, limit: int = 9) -> list[Connection]:
        used = [c for c in self.all() if c.last_used]
        return sorted(used, key=lambda c: c.last_used, reverse=True)[:limit]

    def remove(self, conn_id: str) -> None:
        self.connections = [c for c in self.connections if c.id != conn_id]
        self.save()

    def add_imported(self, items: list[Connection]) -> int:
        """Add connections not already present (same name+host+port+user)."""
        have = {(c.name, c.host, c.port, c.user) for c in self.connections}
        added = 0
        for c in items:
            if (c.name, c.host, c.port, c.user) not in have:
                self.connections.append(c)
                added += 1
        if added:
            self.save()
        return added


# ── Importers ────────────────────────────────────────────────────────────────


def parse_ansible_ini(path: Path) -> list[Connection]:
    out: list[Connection] = []
    group: str | None = None
    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            name = line[1:-1]
            group = None if ":" in name else name
            continue
        if group is None:
            continue
        parts = line.split()
        kv = dict(p.split("=", 1) for p in parts[1:] if "=" in p)
        out.append(
            Connection(
                name=parts[0],
                host=kv.get("ansible_host", parts[0]),
                group=group,
                port=int(kv.get("ansible_port", 22)),
                user=kv.get("ansible_user", ""),
                key=kv.get("ansible_ssh_private_key_file", ""),
            )
        )
    return out


def _config_lines(path: Path, depth: int = 0):
    """Yield lines of an ssh config, following Include directives."""
    import glob

    for raw in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.lower().startswith("include ") and depth < 5:
            for pattern in line.split()[1:]:
                pattern = os.path.expanduser(pattern)
                if not os.path.isabs(pattern):
                    pattern = os.path.join(Path.home() / ".ssh", pattern)
                for inc in sorted(glob.glob(pattern)):
                    try:
                        yield from _config_lines(Path(inc), depth + 1)
                    except OSError:
                        pass
        else:
            yield raw


def parse_ssh_config(path: Path, live: bool = False) -> list[Connection]:
    """Read concrete `Host` blocks (wildcards are skipped). live=True keeps the alias so
    the connection runs as `ssh <alias>` and honours everything in the config."""
    out: list[Connection] = []
    current: dict[str, str] | None = None
    blocks: list[tuple[str, dict[str, str]]] = []
    for raw in _config_lines(path):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, value = line.replace("=", " ", 1).partition(" ")
        key, value = key.lower(), value.strip()
        if key == "host":
            current = {}
            for alias in value.split():
                if not any(ch in alias for ch in "*?!"):
                    blocks.append((alias, current))
        elif key == "match":
            current = None
        elif current is not None:
            current.setdefault(key, value)
    for alias, opts in blocks:
        options = "" if live else (f"-J {opts['proxyjump']}" if "proxyjump" in opts else "")
        fwd = opts.get("forwardx11", "no").lower() == "yes"
        trusted = opts.get("forwardx11trusted", "no").lower() == "yes"
        out.append(
            Connection(
                name=alias,
                host=opts.get("hostname", alias),
                group="ssh config",
                alias=alias if live else "",
                id=f"sshcfg:{alias}" if live else uuid.uuid4().hex,
                port=int(opts.get("port", 22)),
                user=opts.get("user", ""),
                key=opts.get("identityfile", ""),
                x11="trusted" if fwd and trusted else "untrusted" if fwd else "off",
                options=options,
            )
        )
    return out
