#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

set -euo pipefail
umask 022

cd "$(dirname "${BASH_SOURCE[0]}")"
PREFIX="/usr/local"; DEPS=ask; UNINSTALL=0; DEFAULT_TERM=1
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift ;;
    --deps) DEPS=yes ;;
    --no-deps) DEPS=no ;;
    --uninstall) UNINSTALL=1 ;;
    --no-default-terminal) DEFAULT_TERM=0 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

if [ "$(id -u)" = 0 ]; then SUDO=""; elif have sudo; then SUDO="sudo"; else SUDO="none"; fi
INSTALL_SUDO=""
if [ "$(id -u)" != 0 ]; then
  probe="$PREFIX"; while [ ! -e "$probe" ] && [ "$probe" != / ]; do probe="$(dirname "$probe")"; done
  [ -w "$probe" ] || INSTALL_SUDO="sudo"
fi

if [ "$UNINSTALL" = 1 ]; then
  exec ./uninstall.sh --prefix "$PREFIX"
fi

PM=""
for cand in apt-get dnf yum pacman zypper apk; do have "$cand" && { PM="$cand"; break; }; done

pkgs() {
  case "$PM" in
    apt-get) echo build-essential pkg-config libsdl2-dev libfreetype-dev libfontconfig-dev libglib2.0-dev libcurl4-openssl-dev openssh-client libgl1 libegl1 libgles2 libgl1-mesa-dri ;;
    dnf|yum) echo gcc make pkgconf-pkg-config SDL2-devel freetype-devel fontconfig-devel glib2-devel libcurl-devel openssh-clients mesa-libGL mesa-libEGL mesa-libGLES mesa-dri-drivers ;;
    pacman)  echo base-devel pkgconf sdl2 freetype2 fontconfig glib2 curl openssh mesa ;;
    zypper)  echo gcc make pkg-config SDL2-devel freetype2-devel fontconfig-devel glib2-devel libcurl-devel openssh-clients Mesa-libGL1 Mesa-libEGL1 Mesa-libGLESv2-2 Mesa-dri ;;
  esac
}

profiling_pkgs() {
  case "$PM" in
    apt-get) echo linux-perf perf-tools-unstable autofdo bolt-19 gdb ;;
    dnf|yum) echo perf gdb ;;
    pacman)  echo perf gdb ;;
  esac
}

install_deps() {
  [ -n "$PM" ] && [ "$PM" != apk ] && [ "$SUDO" != none ] || { warn "cannot install packages here; install gcc, make, pkg-config and the SDL2, FreeType, fontconfig, GLib and libcurl development files, and Mesa's OpenGL libraries, yourself"; return 1; }
  say "Installing build dependencies with $PM"
  case "$PM" in
    apt-get) $SUDO apt-get update -y >/dev/null; $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $(pkgs)
             for p in $(profiling_pkgs); do $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y "$p" || warn "$p not available here (optional profiling tool)"; done ;;
    dnf)     $SUDO dnf install -y $(pkgs); for p in $(profiling_pkgs); do $SUDO dnf install -y "$p" || warn "$p not available here (optional profiling tool)"; done ;;
    yum)     $SUDO yum install -y $(pkgs); for p in $(profiling_pkgs); do $SUDO yum install -y "$p" || warn "$p not available here (optional profiling tool)"; done ;;
    pacman)  $SUDO pacman -S --needed --noconfirm $(pkgs) || $SUDO pacman -Sy --needed --noconfirm $(pkgs)
             for p in $(profiling_pkgs); do $SUDO pacman -S --needed --noconfirm "$p" || warn "$p not available here (optional profiling tool)"; done ;;
    zypper)  $SUDO zypper --non-interactive refresh >/dev/null || true
             for p in $(pkgs); do $SUDO zypper --non-interactive install "$p" >/dev/null || warn "skipped $p"; done ;;
  esac
}

need_deps() { pkg-config --exists sdl2 freetype2 fontconfig glib-2.0 gio-2.0 libcurl 2>/dev/null && have gcc && have make; }

if ! need_deps; then
  if [ "$DEPS" = no ]; then :
  elif [ "$DEPS" = yes ] || { [ -t 0 ] && read -r -p "Build dependencies are missing. Install them with ${PM:-your package manager} now? [Y/n] " a && case "${a:-y}" in [Yy]*) true ;; *) false ;; esac; } \
       || [ "$(id -u)" = 0 ]; then
    install_deps || true
  fi
fi
if ! need_deps; then
  echo "Missing build requirements: gcc, make, pkg-config and the development files of SDL2, FreeType, fontconfig, GLib and libcurl." >&2
  echo "  SDL2 found: $(pkg-config --modversion sdl2 2>/dev/null || echo none)   (any current distribution ships it; at run time OpenGL 3.3, OpenGL ES 3.0 or ES 2.0 is needed)" >&2
  exit 1
fi

say "Building"
make -s clean >/dev/null 2>&1 || true
make -s app
say "Installing to $PREFIX"
$INSTALL_SUDO make -s PREFIX="$PREFIX" install
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "Note: add $PREFIX/bin to your PATH to run 'lestrix' from a shell." ;; esac
[ -x "$PREFIX/bin/lestrix" ] || { echo "Install failed: $PREFIX/bin/lestrix was not created." >&2; exit 1; }

set_default_terminal() {
  local bin="$PREFIX/bin/lestrix" cfg="${XDG_CONFIG_HOME:-$HOME/.config}" done_any=0
  mkdir -p "$cfg"
  if ! grep -qx 'lestrix.desktop' "$cfg/xdg-terminals.list" 2>/dev/null; then
    { echo 'lestrix.desktop'; cat "$cfg/xdg-terminals.list" 2>/dev/null || true; } > "$cfg/xdg-terminals.list.new" && mv "$cfg/xdg-terminals.list.new" "$cfg/xdg-terminals.list"
  fi
  echo "  xdg-terminals.list: lestrix first"; done_any=1
  if have gsettings && gsettings list-schemas 2>/dev/null | grep -qx 'org.gnome.desktop.default-applications.terminal'; then
    gsettings set org.gnome.desktop.default-applications.terminal exec "$bin" 2>/dev/null && gsettings set org.gnome.desktop.default-applications.terminal exec-arg '-e' 2>/dev/null && echo "  GNOME default terminal: $bin"
  fi
  if have xfce4-session || [ -d "$cfg/xfce4" ]; then
    local hd="${XDG_DATA_HOME:-$HOME/.local/share}/xfce4/helpers"; mkdir -p "$hd" "$cfg/xfce4"
    printf '[Desktop Entry]\nVersion=1.0\nType=X-XFCE-Helper\nX-XFCE-Category=TerminalEmulator\nX-XFCE-CommandsWithParameter=%s -e "%%s"\nX-XFCE-Commands=%s\nIcon=lestrix\nName=Lestrix\n' "$bin" "$bin" > "$hd/lestrix.desktop"
    if grep -q '^TerminalEmulator=' "$cfg/xfce4/helpers.rc" 2>/dev/null; then sed -i 's/^TerminalEmulator=.*/TerminalEmulator=lestrix/' "$cfg/xfce4/helpers.rc"; else echo 'TerminalEmulator=lestrix' >> "$cfg/xfce4/helpers.rc"; fi
    echo "  Xfce preferred terminal: lestrix"
  fi
  for kw in kwriteconfig6 kwriteconfig5; do
    if have "$kw"; then
      "$kw" --file kdeglobals --group General --key TerminalApplication "$bin" && "$kw" --file kdeglobals --group General --key TerminalService lestrix.desktop && echo "  KDE default terminal: $bin"
      break
    fi
  done
  if have update-alternatives && [ -e /etc/alternatives/x-terminal-emulator -o -e /usr/bin/x-terminal-emulator ]; then
    local ASUDO=""; [ "$(id -u)" = 0 ] || ASUDO="sudo"
    if [ "$ASUDO" = "" ] || have sudo; then
      $ASUDO update-alternatives --install /usr/bin/x-terminal-emulator x-terminal-emulator "$bin" 70 >/dev/null 2>&1 \
        && $ASUDO update-alternatives --set x-terminal-emulator "$bin" >/dev/null 2>&1 \
        && echo "  x-terminal-emulator alternative: $bin" \
        || warn "could not set the x-terminal-emulator alternative (needs sudo); run: sudo update-alternatives --set x-terminal-emulator $bin"
    fi
  fi
  [ "$done_any" = 1 ]
}
if [ "$DEFAULT_TERM" = 1 ]; then
  say "Setting Lestrix as the default terminal (undo with ./uninstall.sh, or skip with --no-default-terminal)"
  set_default_terminal || true
fi
say "Done. Find Lestrix in your application menu (System) or run: lestrix"
say "Remove it any time with: ./uninstall.sh   (add --purge to also delete saved connections)"
