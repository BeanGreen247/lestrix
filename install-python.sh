#!/usr/bin/env bash
# Lestrix Python/Qt edition installer for Linux and macOS (the full-screen installer in install.sh calls this one for the Python edition). Safe to re-run: it updates an existing install in place.
#   ./install-python.sh                  install or update for the current user
#   ./install-python.sh --system-deps    install missing system packages without asking (needs root or sudo)
#   ./install-python.sh --no-system-deps never touch system packages; just print what is missing
#   ./install-python.sh --set-default    make Lestrix the x-terminal-emulator (Debian/Ubuntu, needs sudo)
#   ./install-python.sh --uninstall      remove everything this script created (saved connections are kept)
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OS="$(uname -s)"
DEPS_MODE=ask; SET_DEFAULT=0; UNINSTALL=0
for arg in "$@"; do
  case "$arg" in
    --system-deps)    DEPS_MODE=yes ;;
    --no-system-deps) DEPS_MODE=no ;;
    --set-default)    SET_DEFAULT=1 ;;
    --uninstall)      UNINSTALL=1 ;;
    -h|--help)        sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

if [ "$OS" = "Darwin" ]; then
  DATA="$HOME/Library/Application Support/Lestrix"
else
  DATA="${XDG_DATA_HOME:-$HOME/.local/share}/lestrix"
fi
VENV="$DATA/venv"
BIN="$HOME/.local/bin"
DESKTOP="${XDG_DATA_HOME:-$HOME/.local/share}/applications/lestrix.desktop"
ICON_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
APP="$HOME/Applications/Lestrix.app"

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

if [ "$(id -u)" = 0 ]; then SUDO=""; elif have sudo; then SUDO="sudo"; else SUDO="none"; fi

if [ "$UNINSTALL" = 1 ]; then
  say "Removing Lestrix"
  if have update-alternatives && update-alternatives --query x-terminal-emulator 2>/dev/null | grep -q "$BIN/lestrix"; then
    [ "${SUDO:-}" = none ] || ${SUDO:-} update-alternatives --remove x-terminal-emulator "$BIN/lestrix" || true
  fi
  rm -rf "$DATA" "$APP" "$BIN/lestrix" "$DESKTOP" "$ICON_DIR/lestrix.png"
  have update-desktop-database && update-desktop-database "$(dirname "$DESKTOP")" 2>/dev/null || true
  echo "Saved connections in the config directory were left alone."
  exit 0
fi

# ---- system packages ------------------------------------------------------------------
if [ "$(id -u)" = 0 ]; then SUDO=""; elif have sudo; then SUDO="sudo"; else SUDO="none"; fi

PM=""
for cand in apt-get dnf yum pacman zypper apk; do have "$cand" && { PM="$cand"; break; }; done

# What Qt's xcb platform plugin, OpenGL and fonts need, plus python and the ssh client.
pkg_list() {
  case "$PM" in
    apt-get) echo python3 python3-venv python3-pip libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
                  libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0 \
                  libegl1 libgl1 libfontconfig1 libdbus-1-3 libglib2.0-0 openssh-client ;;
    dnf|yum) echo python3 python3-pip xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm \
                  libxkbcommon-x11 mesa-libEGL mesa-libGL fontconfig dbus-libs glib2 openssh-clients ;;
    pacman)  echo python python-pip xcb-util-cursor xcb-util-image xcb-util-keysyms xcb-util-renderutil xcb-util-wm \
                  libxkbcommon-x11 libglvnd fontconfig dbus glib2 openssh ;;
    zypper)  echo python3 python3-pip libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
                  libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libxkbcommon-x11-0 libEGL1 \
                  Mesa-libGL1 fontconfig libdbus-1-3 libglib-2_0-0 libgthread-2_0-0 openssh-clients ;;
  esac
}

pm_install() {  # pm_install pkg...
  case "$PM" in
    apt-get) $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y "$@" ;;
    dnf)     $SUDO dnf install -y "$@" ;;
    yum)     $SUDO yum install -y "$@" ;;
    pacman)  $SUDO pacman -S --needed --noconfirm "$@" || $SUDO pacman -Sy --needed --noconfirm "$@" ;;  # -Sy only if the sync db is empty
    zypper)  # one at a time: a package name missing on this release must not abort the rest
             local pkg rc=0
             for pkg in "$@"; do $SUDO zypper --non-interactive install "$pkg" >/dev/null 2>&1 || { warn "skipped $pkg"; rc=1; }; done
             return 0 ;;
  esac
}

deps_allowed() {
  [ "$OS" = Linux ] || return 1
  [ "$DEPS_MODE" = no ] && return 1
  [ -n "$PM" ] && [ "$PM" != apk ] && [ "$SUDO" != none ] || return 1
  [ "$DEPS_MODE" = yes ] && return 0
  if [ -t 0 ]; then
    read -r -p "Install missing system packages with $PM now? [Y/n] " ans
    case "${ans:-y}" in [Yy]*) return 0 ;; *) return 1 ;; esac
  fi
  [ "$(id -u)" = 0 ]   # unattended: only when already root
}

DEPS_DONE=0
system_deps() {
  [ "$DEPS_DONE" = 1 ] && return 0
  if deps_allowed; then
    say "Installing system packages with $PM"
    [ "$PM" = apt-get ] && $SUDO apt-get update -y >/dev/null
    [ "$PM" = zypper ] && $SUDO zypper --non-interactive refresh >/dev/null || true
    # shellcheck disable=SC2046
    if ! pm_install $(pkg_list); then
      # RHEL-family: some Qt helper libraries live in EPEL
      if [ "$PM" = dnf ] || [ "$PM" = yum ]; then
        say "Retrying with EPEL enabled"
        pm_install epel-release || true
        # shellcheck disable=SC2046
        pm_install $(pkg_list) || warn "some system packages could not be installed"
      else
        warn "some system packages could not be installed"
      fi
    fi
    DEPS_DONE=1
  else
    [ "$OS" = Linux ] && warn "system packages not installed. If Lestrix fails to start, run: ./install.sh --system-deps"
    return 1
  fi
}

# ---- Python 3.10+ -------------------------------------------------------------------------
find_python() {
  for cand in python3.14 python3.13 python3.12 python3.11 python3.10 python3 python; do
    if have "$cand" && "$cand" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
      echo "$cand"; return 0
    fi
  done
  return 1
}

PY="$(find_python || true)"
if [ -z "$PY" ] && [ "$OS" = Linux ]; then
  system_deps || true
  PY="$(find_python || true)"
  if [ -z "$PY" ] && { [ "$PM" = dnf ] || [ "$PM" = yum ] || [ "$PM" = zypper ]; } && deps_allowed; then
    # enterprise distros ship an old default python3 next to versioned ones
    for v in 3.13 3.12 3.11 3.10; do
      pm_install "python${v}" 2>/dev/null && pm_install "python${v}-pip" 2>/dev/null || true
      PY="$(find_python || true)"; [ -n "$PY" ] && break
      [ "$PM" = zypper ] && { pm_install "python${v/./}" 2>/dev/null || true; PY="$(find_python || true)"; [ -n "$PY" ] && break; }
    done
  fi
fi
if [ -z "$PY" ]; then
  echo "Python 3.10 or newer is required and was not found." >&2
  case "$OS:$PM" in
    Darwin:*) echo "  brew install python" >&2 ;;
    *:apt-get) echo "  sudo apt install python3 python3-venv" >&2 ;;
    *:dnf|*:yum) echo "  sudo dnf install python3.12   (or python3.11)" >&2 ;;
    *:pacman) echo "  sudo pacman -S python" >&2 ;;
    *:zypper) echo "  sudo zypper install python311" >&2 ;;
    *:apk) echo "  Alpine (musl) is not supported: the Qt wheels need glibc." >&2 ;;
  esac
  exit 1
fi

# ---- virtualenv + package (re-running updates in place) ------------------------------------------
say "Installing into $VENV (using $PY)"
mkdir -p "$DATA" "$BIN"
if [ -d "$VENV" ] && ! "$VENV/bin/python" -c 'import sys' 2>/dev/null; then
  say "Existing environment is broken (Python upgraded?) - recreating it"
  rm -rf "$VENV"
fi
if [ ! -d "$VENV" ]; then
  if ! "$PY" -m venv "$VENV" 2>/dev/null; then
    rm -rf "$VENV"
    system_deps || true
    "$PY" -m venv "$VENV" || { echo "Could not create a virtualenv (Debian/Ubuntu: sudo apt install python3-venv)." >&2; exit 1; }
  fi
fi
"$VENV/bin/python" -m pip install --quiet --upgrade pip
rm -rf "$SRC/build" "$SRC"/*.egg-info
"$VENV/bin/python" -m pip install --quiet --upgrade "$SRC"            # dependencies
"$VENV/bin/python" -m pip install --quiet --force-reinstall --no-deps "$SRC"  # always refresh Lestrix itself
rm -rf "$SRC/build" "$SRC"/*.egg-info

say "Checking the install"
check_import() {
  QT_QPA_PLATFORM=offscreen "$VENV/bin/python" -c "import PyQt6.QtWidgets, PyQt6.QtOpenGLWidgets, pyte, lestrix" 2>"$DATA/check.log"
}
if ! check_import; then  # usually a missing system library such as libGL
  system_deps || true
  # rpm distros can install a missing library by its soname, whatever the package is called here
  for _ in 1 2 3 4 5 6 7 8; do
    check_import && break
    so="$(grep -o 'lib[A-Za-z0-9_.+-]*\.so\.[0-9]*' "$DATA/check.log" | head -1)"
    [ -n "$so" ] && { [ "$PM" = dnf ] || [ "$PM" = yum ] || [ "$PM" = zypper ]; } && deps_allowed || break
    say "Installing the package that provides $so"
    pm_install "${so}()(64bit)" || break
  done
  check_import || { cat "$DATA/check.log" >&2; echo "Install check failed" >&2; exit 1; }
fi
rm -f "$DATA/check.log"

if [ "$OS" = Linux ]; then  # shared libraries of the X11 plugin Qt will load
  XCB="$("$VENV/bin/python" - <<'PYEOF'
import pathlib, PyQt6
p = pathlib.Path(PyQt6.__file__).parent / "Qt6" / "plugins" / "platforms" / "libqxcb.so"
print(p if p.exists() else "")
PYEOF
)"
  if [ -n "$XCB" ] && have ldd && ldd "$XCB" 2>/dev/null | grep -q "not found"; then
    say "Missing shared libraries for Qt:"
    ldd "$XCB" | grep "not found" | sed 's/^/    /'
    system_deps || true
    if ldd "$XCB" 2>/dev/null | grep -q "not found"; then
      warn "still missing: $(ldd "$XCB" | awk '/not found/{print $1}' | tr '\n' ' ')"
    fi
  fi
fi

ln -sf "$VENV/bin/lestrix" "$BIN/lestrix"
ASSETS="$("$VENV/bin/python" -c 'import lestrix, pathlib; print(pathlib.Path(lestrix.__file__).parent / "assets")')"

# ---- launcher / menu entry -----------------------------------------------------------------------------
if [ "$OS" = "Darwin" ]; then
  say "Creating $APP"
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
  cat > "$APP/Contents/MacOS/Lestrix" <<APPEOF
#!/bin/bash
exec "$VENV/bin/lestrix" "\$@"
APPEOF
  chmod +x "$APP/Contents/MacOS/Lestrix"
  cp "$ASSETS/lestrix.icns" "$APP/Contents/Resources/lestrix.icns"
  cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Lestrix</string>
  <key>CFBundleDisplayName</key><string>Lestrix</string>
  <key>CFBundleIdentifier</key><string>term.beangreen247.lestrix</string>
  <key>CFBundleExecutable</key><string>Lestrix</string>
  <key>CFBundleIconFile</key><string>lestrix</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
else
  say "Creating the application menu entry"
  mkdir -p "$ICON_DIR" "$(dirname "$DESKTOP")"
  cp "$ASSETS/lestrix.png" "$ICON_DIR/lestrix.png"
  cat > "$DESKTOP" <<DESK
[Desktop Entry]
Type=Application
Name=Lestrix
GenericName=Terminal Emulator
Comment=SSH sessions, SFTP and local shells in one window
Exec=$BIN/lestrix
Icon=lestrix
Terminal=false
Categories=System;TerminalEmulator;
Keywords=ssh;sftp;terminal;shell;console;
StartupWMClass=Lestrix
Actions=local;

[Desktop Action local]
Name=New local shell
Exec=$BIN/lestrix --local
DESK
  have update-desktop-database && update-desktop-database "$(dirname "$DESKTOP")" 2>/dev/null || true
fi

if [ "$SET_DEFAULT" = 1 ]; then
  if have update-alternatives && [ "$SUDO" != none ]; then
    say "Registering as x-terminal-emulator"
    $SUDO update-alternatives --install /usr/bin/x-terminal-emulator x-terminal-emulator "$BIN/lestrix" 60
    $SUDO update-alternatives --set x-terminal-emulator "$BIN/lestrix"
  else
    warn "update-alternatives (or sudo) not available; set Lestrix as your default terminal in your desktop settings."
  fi
fi

case ":$PATH:" in *":$BIN:"*) ;; *) echo "Note: add $BIN to your PATH to run 'lestrix' from a shell." ;; esac
say "Done. Start it from your application menu or run: lestrix"
