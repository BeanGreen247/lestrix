# Lestrix

<p align="center">
  <img src="native/data/lestrix.png" alt="Lestrix logo" width="96"/>
</p>

A fast, light SSH terminal manager for Linux in plain C and GTK4. Saved connections on the left, terminal tabs on the right, an SFTP/FTP file browser that follows your shell, and a live performance bar along the bottom. Think MobaXterm, built to start instantly and stay small.

<p align="center">
  <img src="native/docs/welcome.png" alt="Lestrix start page with saved connections" width="900"/>
</p>

**Version:** 0.2.0  
**Author:** BeanGreen247  
**License:** MIT

## Highlights

- **Fast.** Its own terminal engine parses ~230 MB/s with scrollback on, ~900 MB/s with it off, and each tab parses on its own thread.
- **Light.** About 75 MB of RAM with the low-memory renderer, and scrollback is compressed and spilled to disk, so even "unlimited" stays bounded.
- **Reads what you already have.** `~/.ssh/config` (with `Include`), your keys and agent, and Ansible inventories (INI with host ranges and group vars, or YAML).
- **SSH, SFTP, scp, FTP and FTPS, X11 forwarding.** SSH uses the system `ssh`, so ProxyJump, IdentityFile and the rest keep working. The file browser rides the same connection, so there is no second login.
- **Works as your terminal.** `lestrix -e htop` works as `x-terminal-emulator`, and it installs a menu entry under System.
- **Six themes** and a custom accent colour.

![Sessions and files in one sidebar, live performance numbers along the bottom](native/docs/files.png)

## Install

You need GTK 4.12 or newer (Debian 13, Ubuntu 24.04, Fedora 39+, Arch, openSUSE Tumbleweed) and the OpenSSH client.

```bash
git clone https://github.com/BeanGreen247/lestrix.git
cd lestrix/native
./install.sh --deps      # installs build packages, builds, installs to ~/.local, adds a menu entry
```

Run it from the application menu or with `lestrix`. The installer is safe to re-run: it rebuilds and replaces the installed copy. `--prefix /usr/local` installs system-wide and `--no-deps` never touches system packages.

**Uninstall**

```bash
./uninstall.sh           # removes the binary, menu entry, icon and build output; keeps your connections
./uninstall.sh --purge   # also deletes saved connections, settings and leftover history files
```

## Using it

```bash
lestrix --benchmark            # measure this machine
lestrix --lite                 # smallest memory footprint
lestrix --theme Nord           # Xylonic Dark/Light, Graphite, Nord, Gruvbox, Solarized Dark
lestrix -e htop                # works as x-terminal-emulator
lestrix --local --working-directory ~/src
```

Connections live in `~/.config/lestrix/connections.json`. No passwords are saved; ssh asks in the terminal or uses your key and agent. FTP and FTPS ask for the password when you connect.

## Under the hood

The repository holds two implementations. The **native edition** in `native/` is plain C (GNU C11, not C++): its own terminal engine, GTK4 for the window and `libcurl` for FTP. The Python/Qt version in `lestrix/` is the earlier prototype where the features were worked out.

### Speed is a feature

A terminal sits between you and everything you run. When it is slow, `tail -f` on a busy log lags, a build scrolls in jerks, and pasting a large file freezes the window. When it is wasteful, it takes memory and battery from the editor, the compiler and the browser next to it. So speed is treated as a requirement, not a nice extra:

- The numbers above are measured on one 8-core machine, not estimated, and `lestrix --benchmark` measures them on your machine.
- The bar along the bottom of the window shows them live: parse rate, CPU, memory, threads, and how much scrollback you hold and where it lives. It is on by default; View > Show performance overlay turns it off.
- `make test` includes a speed gate that fails if parsing gets more than about ten times slower, so a slowdown cannot slip in unnoticed.

### Scrollback: adjustable, up to unlimited

View > Scrollback offers Off, 1,000, 10,000 (default), 100,000, 1,000,000 or Unlimited, and applies to open tabs at once. Settings under `[ui]` in `~/.config/lestrix/settings.ini` go further: `scrollback` (a line count, `-1` for unlimited), `scrollback_ram_mb` (default 32) and `scrollback_disk_mb` (default 4096).

It is built to stay inside its limits:

- The newest ~1,000 lines stay as they are. Older lines are packed 128 at a time: byte-shuffled, then compressed (a repetitive log line costs a few bytes; 118,000 log lines took 4.4 MB), on worker threads so parsing never waits for it.
- When the packed history passes the memory budget, the oldest blocks move to an unlinked temporary file. When that passes the disk budget, the oldest block is dropped. Memory and disk use are always bounded, even with "Unlimited".
- Scrolling back decodes blocks on demand into a small cache. Corrupt or unreadable blocks show as empty lines instead of crashing.
- The compressor's decoder checks every length and offset; a fuzz test throws thousands of damaged blocks at it under ASan/UBSan, and the worker threads run under ThreadSanitizer.

### Cores and threads

Each terminal parses on its own thread, so the window stays responsive during a flood and several busy tabs run on separate cores. Scrollback compression runs on a pool of up to 16 workers, and file transfers, connection checks and the SFTP browser run on background threads too. One terminal's output is a single ordered stream, so one tab is parsed by one core by nature; the cores are used when there is more than one thing to do.

### Memory

- Cells are 8 bytes (a code point plus an interned style id), screen rows are slots that scrolling reorders instead of copying, and scrollback lines drop their trailing blanks.
- Only changed rows are redrawn, and on the CPU path a scroll moves pixels with one `memmove`.
- While you are idle the app packs old scrollback, frees decoded blocks and returns freed heap to the OS (`malloc_trim`); the heap is tuned with two arenas and a low mmap threshold so big buffers cannot fragment it.
- **Low-memory renderer** (View menu, or `lestrix --lite`): the terminal paints on the CPU and GTK never loads the GPU driver stack. On the test machine that is 146 MB RSS down to 75 MB, with no visible speed difference (a flood takes 0.27 s instead of 0.2 s). The GPU renderer stays the default.

An empty GTK4 window is already about 120 MB RSS here with the GPU stack loaded, so most of the remaining footprint is the toolkit, not the terminal. Going far below ~75 MB would mean a frontend that does not use GTK.

### Protocols

Protocols: SSH with the system `ssh` (agent, keys, ProxyJump, X11 all work), SFTP, scp, ssh-only fallbacks, and FTP/FTPS through libcurl. SSH sessions share one connection with the file browser, so there is no second login.

Run `make test` in `native/` for the core, compressor and parser tests under ASan/UBSan and TSan, a 1500-stream comparison with pyte, SFTP, FTP and the speed gate.

## Features (both editions)

- **One sidebar, two tabs.** *Sessions* holds your saved connections (searchable, grouped, with a live up/down dot). *Files* is the SFTP browser for the SSH tab you are in.
- **File browser follows your shell.** Tick *Follow terminal path* and the browser jumps to wherever you `cd`. Drag files in to upload, right-click to download, rename, delete.
- **Works without SFTP.** If `sftp` is missing locally or switched off on the server, Lestrix falls back to ssh commands for browsing and to `scp`, then `cat`/`tar` over ssh, for transfers. FTP is not supported.
- **Start page** with your 9 most recent connections. Press `1`-`9` or click.
- **Tabs you can rename and recolor** (right-click a tab, or double-click to rename). A terminal icon marks local shells, a server icon marks SSH. A blue dot means new output in a background tab, a red dot means a bell or an ended session.
- **~/.ssh/config hosts appear automatically**, including `Include` files, and connect with plain `ssh <alias>` so ProxyJump, IdentityFile and the rest keep working. Ansible `hosts.ini` import is built in.
- **Real shells**: bash, zsh, fish, sh, dash, ksh, tcsh, nushell, PowerShell. Full-screen programs (vim, htop, less, tmux, mc), mouse reporting and bracketed paste work.
- **X11 forwarding** per connection (`-X` or trusted `-Y`).
- **Six themes** (Xylonic Dark and Light, Graphite, Nord, Gruvbox, Solarized Dark) and a custom accent color. View > Theme.
- **GPU rendering** through OpenGL, redrawing at most once per frame, with an automatic software fallback.
- Acts as a terminal app: it takes `-e command` like `x-terminal-emulator`, and can open a local shell on startup.

## Python edition

You need Python 3.10 or newer and the OpenSSH client (`ssh`, and ideally `sftp` and `scp`).

**Linux and macOS**

```bash
git clone https://github.com/BeanGreen247/lestrix.git
cd lestrix
./install.sh                # per-user install, adds a menu entry (an app bundle on macOS)
```

The installer works on Debian/Ubuntu, Fedora, RHEL/Rocky/Alma, Arch and openSUSE. It finds or installs Python 3.10+, installs the Qt system libraries (it asks first, and does not ask when run as root), sets up a private virtualenv, and adds a menu entry. It is safe to re-run: running it again after a `git pull` updates the existing install in place and keeps your saved connections.

Flags: `--system-deps` installs missing system packages without asking, `--no-system-deps` never touches them and just reports what is missing, `--set-default` makes Lestrix the system `x-terminal-emulator` on Debian/Ubuntu, `--uninstall` removes it. Alpine (musl) is not supported because the Qt wheels need glibc.

**Windows** (PowerShell)

```powershell
.\install.ps1
```

**By hand**

```bash
python3 -m venv .venv && .venv/bin/pip install . && .venv/bin/lestrix
```

## Use it

| Action | How |
|---|---|
| New connection | `Ctrl+N` or the `+` button |
| Open a connection | Double-click it, click it on the start page, or press `1`-`9` there |
| Local shell | `Ctrl+Shift+T`, or File > New local shell as... to pick bash, zsh, fish |
| Switch sidebar tab | `Ctrl+Shift+B` toggles Sessions / Files |
| Rename or recolor a tab | Right-click the tab, or double-click it to rename |
| Copy / paste | `Ctrl+Shift+C` / `Ctrl+Shift+V` (`Cmd+C` / `Cmd+V` on macOS), middle click pastes |
| Scrollback | Mouse wheel or `Shift+PageUp` / `Shift+PageDown` |
| Font size | `Ctrl+=` and `Ctrl+-` |
| Hide sidebar | `Ctrl+B` |
| Next / previous tab | `Ctrl+PageDown` / `Ctrl+PageUp` |

Command line: `lestrix --local`, `lestrix -e htop`, `lestrix --working-directory ~/src --local`.

Connections are stored in `~/.config/lestrix/connections.json` (permissions `600`). No passwords are saved: ssh asks in the terminal, or uses your key and agent.

### How the file browser works

The terminal's `ssh` runs as an OpenSSH ControlMaster. The file browser talks over that same connection, so there is no second login and no second password prompt. To follow your shell it listens for the `OSC 7` working-directory escape sequence, and on Linux servers it also polls the shell's directory through `/proc`, so it works without any shell setup.

### X11 forwarding

Set **X11** to *X11 forwarding (-X)* or *Trusted X11 forwarding (-Y)* in the connection dialog, then start a graphical program in the session.

- Linux: works out of the box.
- macOS: install [XQuartz](https://www.xquartz.org/) first.
- Windows: run an X server such as [VcXsrv](https://sourceforge.net/projects/vcxsrv/) and set `DISPLAY`.

The server needs `X11Forwarding yes` in its `sshd_config`.

### GPU rendering

If your driver has no usable OpenGL, set `LESTRIX_RENDERER=software`.

## Develop

```bash
.venv/bin/pip install -e '.[dev]'
QT_QPA_PLATFORM=offscreen .venv/bin/pytest
```

The SFTP tests run against `ssh localhost` when it works without a password, and are skipped otherwise.

## Known limits

- The file browser is disabled on Windows: its OpenSSH has no ControlMaster, so there is no shared connection to ride on.
- Follow-path polling needs a Linux server (`/proc`); on others it relies on the shell sending `OSC 7`.
- Sixel and image protocols are not supported, and the terminal engine ([pyte](https://github.com/selectel/pyte)) ignores a few rare escape sequences.
- zsh and fish support was written against their documented behavior; the automated tests only cover bash and sh.
- The Windows installer and terminal backend (ConPTY through `pywinpty`) are untested.

## Support

If this project is useful to you, consider supporting its development via PayPal:

[![Donate with PayPal](.github/paypal-qr.png)](https://paypal.me/beangreen2471)

**PayPal:** https://paypal.me/beangreen2471
