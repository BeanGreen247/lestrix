#!/usr/bin/env bash
# Builds and installs the native Lestrix. Safe to re-run: it rebuilds and replaces the installed binary.
#   ./install.sh                   build and install for the current user (~/.local)
#   ./install.sh --prefix /usr/local   install system-wide (uses sudo for the copy)
#   ./install.sh --deps            install build dependencies first (needs root or sudo)
#   ./install.sh --no-deps         never touch system packages
#   ./install.sh --uninstall       remove it (same as ./uninstall.sh)
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
PREFIX="$HOME/.local"; DEPS=ask; UNINSTALL=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift ;;
    --deps) DEPS=yes ;;
    --no-deps) DEPS=no ;;
    --uninstall) UNINSTALL=1 ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }

if [ "$(id -u)" = 0 ]; then SUDO=""; elif have sudo; then SUDO="sudo"; else SUDO="none"; fi
INSTALL_SUDO=""
if [ "$(id -u)" != 0 ]; then  # sudo only when the prefix is not ours to write
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
    apt-get) echo build-essential pkg-config libgtk-4-dev libcurl4-openssl-dev openssh-client ;;
    dnf|yum) echo gcc make pkgconf-pkg-config gtk4-devel libcurl-devel openssh-clients ;;
    pacman)  echo base-devel pkgconf gtk4 curl openssh ;;
    zypper)  echo gcc make pkg-config gtk4-devel libcurl-devel openssh-clients ;;
  esac
}

install_deps() {
  [ -n "$PM" ] && [ "$PM" != apk ] && [ "$SUDO" != none ] || { warn "cannot install packages here; install gcc, make, pkg-config, GTK 4 and libcurl development files yourself"; return 1; }
  say "Installing build dependencies with $PM"
  case "$PM" in
    apt-get) $SUDO apt-get update -y >/dev/null; $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $(pkgs) ;;
    dnf)     $SUDO dnf install -y $(pkgs) ;;
    yum)     $SUDO yum install -y $(pkgs) ;;
    pacman)  $SUDO pacman -S --needed --noconfirm $(pkgs) || $SUDO pacman -Sy --needed --noconfirm $(pkgs) ;;
    zypper)  $SUDO zypper --non-interactive refresh >/dev/null || true
             for p in $(pkgs); do $SUDO zypper --non-interactive install "$p" >/dev/null || warn "skipped $p"; done ;;
  esac
}

need_gtk() { pkg-config --atleast-version=4.12 gtk4 2>/dev/null && pkg-config --exists libcurl 2>/dev/null && have gcc && have make; }

if ! need_gtk; then
  if [ "$DEPS" = no ]; then :
  elif [ "$DEPS" = yes ] || { [ -t 0 ] && read -r -p "Build dependencies are missing. Install them with ${PM:-your package manager} now? [Y/n] " a && case "${a:-y}" in [Yy]*) true ;; *) false ;; esac; } \
       || [ "$(id -u)" = 0 ]; then
    install_deps || true
  fi
fi
if ! need_gtk; then
  echo "Missing build requirements: gcc, make, pkg-config, GTK >= 4.12 and libcurl development files." >&2
  echo "  GTK found: $(pkg-config --modversion gtk4 2>/dev/null || echo none)   (Debian 13+/Ubuntu 24.04+/Fedora 39+ ship 4.12 or newer)" >&2
  exit 1
fi

say "Building"
make -s clean >/dev/null 2>&1 || true
make -s app
say "Installing to $PREFIX"
$INSTALL_SUDO make -s PREFIX="$PREFIX" install
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "Note: add $PREFIX/bin to your PATH to run 'lestrix' from a shell." ;; esac
[ -x "$PREFIX/bin/lestrix" ] || { echo "Install failed: $PREFIX/bin/lestrix was not created." >&2; exit 1; }
say "Done. Find Lestrix in your application menu (System) or run: lestrix"
say "Remove it any time with: ./uninstall.sh   (add --purge to also delete saved connections)"
