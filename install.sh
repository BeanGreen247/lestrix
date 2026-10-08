#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT
#
# Lestrix installer. Builds from source and installs to /usr/local (or --prefix).
# No dialogs: numbered steps with a plain explanation of each, one sudo prompt at the start,
# missing packages are installed automatically. Safe to re-run (it updates).
#   ./install.sh                  install or update
#   ./install.sh --uninstall      remove (--purge also deletes saved connections and settings)
#   --prefix DIR                  install under DIR (~/.local needs no sudo for the files)
#   --no-default-terminal         do not make Lestrix the default terminal
#   --no-deps                     never touch system packages
#   --tests                       run the test suite after installing
#   --dry-run                     show the steps, change nothing
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC"

UNINSTALL=0; PURGE=0; PREFIX="/usr/local"; DEPS=yes; DEFTERM=yes; TESTS=0; DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --install|--yes|-y|--plain|--deps|--default-terminal) ;;
    --uninstall) UNINSTALL=1 ;;
    --purge) PURGE=1 ;;
    --prefix) PREFIX="$2"; shift ;;
    --no-deps) DEPS=no ;;
    --no-default-terminal) DEFTERM=no ;;
    --tests) TESTS=1 ;;
    --dry-run) DRY=1 ;;
    -h|--help) sed -n '5,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

UI_TOTAL_STEPS=$(( 4 + TESTS ))
[ "$UNINSTALL" = 1 ] && UI_TOTAL_STEPS=2
source "${SRC}/scripts/install-ui.sh"

if [ "$(uname -s)" != Linux ]; then
  echo "error: Lestrix has no build for $(uname -s) yet (it uses Linux pty, /proc and inotify). Nothing changed." >&2
  exit 1
fi
if [ "${EUID}" -eq 0 ]; then
  echo "error: run ./install.sh as your normal user, not with sudo or as root." >&2
  echo "       It asks for your password itself, once, when it needs it." >&2
  exit 1
fi
run() { if [ "$DRY" = 1 ]; then echo "[dry run] $*"; else "$@"; fi; }
probe="$PREFIX"; while [ ! -e "$probe" ] && [ "$probe" != / ]; do probe=$(dirname "$probe"); done
NEED_SUDO=0
{ [ "$DEPS" = yes ] || [ ! -w "$probe" ] || [ "$DEFTERM" = yes ] || [ "$UNINSTALL" = 1 ]; } && NEED_SUDO=1

ui_step "Checking your account and asking for permission once" \
  "What: confirms you are a normal user and asks for your sudo password a single time." \
  "Why:  installing packages, files under ${PREFIX} and the default-terminal setting need it. Asking now" \
  "      means no password prompt can pop up in the middle of a progress line later."
if [ "$NEED_SUDO" = 1 ] && [ "$DRY" = 0 ]; then
  command -v sudo >/dev/null 2>&1 || { echo "error: sudo is not installed; install it or use --prefix ~/.local --no-deps --no-default-terminal." >&2; exit 1; }
  ui_sudo_keepalive || { echo "error: sudo permission is required." >&2; exit 1; }
fi

if [ "$UNINSTALL" = 1 ]; then
  ui_step "Removing Lestrix" \
    "What: deletes the program, menu entry and icon, and gives the default-terminal settings back." \
    "Why:  $([ "$PURGE" = 1 ] && echo 'you asked for --purge, so saved connections and settings are deleted too.' || echo 'saved connections and settings are kept (add --purge to delete them).')"
  a=(--prefix "$PREFIX"); [ "$PURGE" = 1 ] && a+=(--purge --yes)
  run bash -c 'cd native && ./uninstall.sh "$@"' _ "${a[@]}"
  echo; echo "Lestrix was removed."
  exit 0
fi

ui_step "Installing the compiler and the libraries Lestrix is built from" \
  "What: installs gcc/make and the development files for SDL2, FreeType, fontconfig, GLib and libcurl," \
  "      plus OpenGL drivers and the ssh client. Packages that are already present are skipped." \
  "Why:  Lestrix is built from source on your machine, tuned for your exact CPU."
need_deps() { pkg-config --exists sdl2 freetype2 fontconfig glib-2.0 gio-2.0 libcurl 2>/dev/null && command -v gcc >/dev/null && command -v make >/dev/null; }
if [ "$DEPS" = no ]; then
  ui_note "skipped (--no-deps)"
elif need_deps && [ "$DRY" = 0 ]; then
  ui_note "everything needed is already installed"
else
  mapfile -t DL < <(native/install.sh --list-deps)
  PM=${DL[0]:-}; read -r -a PKGS <<< "${DL[1]:-}"
  for p in "${PKGS[@]}"; do ui_item "$p" ""; done
  log=$(mktemp)
  case "$PM" in
    apt-get)
      run ui_live plain "Refreshing the package lists" "$log" sudo apt-get update -qq
      run ui_live apt "Installing packages" "$log" sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "${PKGS[@]}" ;;
    dnf|yum) run ui_live plain "Installing packages" "$log" sudo "$PM" install -y "${PKGS[@]}" ;;
    pacman)  run ui_live plain "Installing packages" "$log" sudo pacman -S --needed --noconfirm "${PKGS[@]}" ;;
    zypper)  run ui_live plain "Installing packages" "$log" sudo zypper --non-interactive install "${PKGS[@]}" ;;
    *) echo "warning: no supported package manager; install gcc, make, pkg-config and the SDL2, FreeType, fontconfig, GLib and libcurl development files yourself." >&2 ;;
  esac
  rm -f "$log"
  [ "$DRY" = 1 ] || need_deps || { echo "error: build requirements are still missing (see above)." >&2; exit 1; }
fi

ui_step "Building and installing Lestrix" \
  "What: compiles native/ with -O3 for this CPU and installs the program, menu entry and icon to ${PREFIX}$([ "$DEFTERM" = yes ] && echo ', then makes Lestrix the default terminal')." \
  "Why:  this is the whole program; building takes a minute or two."
a=(--no-deps --prefix "$PREFIX"); [ "$DEFTERM" = no ] && a+=(--no-default-terminal)
log=$(mktemp)
run ui_live plain "Building" "$log" bash -c 'cd native && ./install.sh "$@"' _ "${a[@]}"
rm -f "$log"

if [ "$TESTS" = 1 ]; then
  ui_step "Running the test suite" \
    "What: unit tests for the terminal emulator, storage, transfers and fast paths." \
    "Why:  confirms this build behaves; the install is already done either way."
  log=$(mktemp)
  run ui_live plain "Testing" "$log" bash -c 'cd native && ./test.sh --no-deps' || echo "warning: some tests failed (the install itself is done)" >&2
  rm -f "$log"
fi

ui_step "Done" \
  "Start Lestrix from your application menu (System) or run:  lestrix" \
  "Remove it any time with:  ./install.sh --uninstall" \
  "'lxcat' prints big files at memory speed in a Lestrix tab; 'cat' uses it automatically (View > Fast cat)."
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) ui_note "Note: add $PREFIX/bin to your PATH to run 'lestrix' from a shell." ;; esac
