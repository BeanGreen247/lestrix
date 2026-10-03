from lestrix.store import Connection, Store, parse_ansible_ini, parse_ssh_config


def test_ssh_argv_variants():
    c = Connection(name="a", host="h", user="u", port=2222, key="/k", x11="trusted", options="-J jump")
    argv = c.ssh_argv()
    assert argv[0] == "ssh" and argv[-1] == "u@h"
    assert "-Y" in argv and ["-i", "/k"] == argv[argv.index("-i"):argv.index("-i") + 2]
    assert ["-J", "jump"] == argv[argv.index("-J"):argv.index("-J") + 2]
    assert Connection(name="a", host="h").ssh_argv()[-1] == "h"


def test_store_roundtrip_and_dedupe(tmp_path):
    s = Store(tmp_path / "c.json")
    c = Connection(name="a", host="h")
    s.upsert(c)
    assert (tmp_path / "c.json").stat().st_mode & 0o777 == 0o600
    s2 = Store(tmp_path / "c.json")
    assert s2.get(c.id).host == "h"
    assert s2.add_imported([Connection(name="a", host="h")]) == 0
    s2.remove(c.id)
    assert Store(tmp_path / "c.json").connections == []


def test_ansible_ini(tmp_path):
    p = tmp_path / "hosts.ini"
    p.write_text(
        "[web]\nw1 ansible_host=10.0.0.1 ansible_port=2200 ansible_user=bob\n"
        "[all:vars]\nx=1\n[db]\nd1\n"
    )
    conns = parse_ansible_ini(p)
    assert [(c.name, c.host, c.port, c.user, c.group) for c in conns] == [
        ("w1", "10.0.0.1", 2200, "bob", "web"), ("d1", "d1", 22, "", "db")]


def test_ssh_config(tmp_path):
    p = tmp_path / "config"
    p.write_text(
        "Host *\n  ServerAliveInterval 5\nHost box\n  HostName 1.2.3.4\n  User me\n"
        "  ForwardX11 yes\n  ProxyJump gw\n"
    )
    (c,) = parse_ssh_config(p)
    assert (c.name, c.host, c.user, c.x11, c.options) == ("box", "1.2.3.4", "me", "untrusted", "-J gw")


def test_recent_orders_and_limits(tmp_path):
    s = Store(tmp_path / "c.json")
    conns = [Connection(name=f"n{i}", host=f"h{i}") for i in range(12)]
    for c in conns:
        s.upsert(c)
    assert s.recent() == []
    for c in conns:
        s.touch(c.id)
    recent = s.recent()
    assert len(recent) == 9 and recent[0].name == "n11"
    assert Store(tmp_path / "c.json").recent()[0].name == "n11"


def test_live_ssh_config_with_include_and_alias(tmp_path, monkeypatch):
    (tmp_path / ".ssh").mkdir()
    (tmp_path / ".ssh" / "extra").write_text("Host jump\n  HostName 9.9.9.9\n  User ops\n")
    cfg = tmp_path / ".ssh" / "config"
    cfg.write_text("Include extra\nHost box\n  HostName 1.2.3.4\n  ProxyJump jump\n")
    monkeypatch.setattr("pathlib.Path.home", lambda: tmp_path)
    s = Store(tmp_path / "c.json")
    s.reload_ssh_config(True, cfg)
    names = {c.name: c for c in s.live}
    assert set(names) == {"jump", "box"}
    box = names["box"]
    assert box.is_live and box.ssh_argv()[-1] == "box" and "-p" not in box.ssh_argv()
    s.update_meta(box, color="#ff0000")
    s2 = Store(tmp_path / "c.json")
    s2.reload_ssh_config(True, cfg)
    assert [c.color for c in s2.live if c.name == "box"] == ["#ff0000"]
    assert s2.get("sshcfg:box") is not None
