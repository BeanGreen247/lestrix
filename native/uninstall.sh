#!/usr/bin/env bash
# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
PREFIXES=(/usr/local "$HOME/.local" /usr); PURGE=0; YES=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIXES=("$2"); shift ;;
    --purge) PURGE=1 ;;
    --yes|-y) YES=1 ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
removed=0

rm_path() {
  [ -e "$1" ] || [ -L "$1" ] || return 0
  if rm -rf "$1" 2>/dev/null; then :
  elif [ "$(id -u)" != 0 ] && have sudo && sudo rm -rf "$1"; then :
  else echo "could not remove $1" >&2; return 0; fi
  echo "  removed $1"; removed=$((removed + 1))
}

say "Releasing the default-terminal settings"
cfgd="${XDG_CONFIG_HOME:-$HOME/.config}"
if [ -f "$cfgd/xdg-terminals.list" ] && grep -qx 'lestrix.desktop' "$cfgd/xdg-terminals.list"; then
  grep -vx 'lestrix.desktop' "$cfgd/xdg-terminals.list" > "$cfgd/xdg-terminals.list.new"; mv "$cfgd/xdg-terminals.list.new" "$cfgd/xdg-terminals.list"
  [ -s "$cfgd/xdg-terminals.list" ] || rm -f "$cfgd/xdg-terminals.list"; echo "  removed lestrix from xdg-terminals.list"
fi
if have gsettings && [ "$(gsettings get org.gnome.desktop.default-applications.terminal exec 2>/dev/null)" ]; then
  case "$(gsettings get org.gnome.desktop.default-applications.terminal exec 2>/dev/null)" in *lestrix*) gsettings reset org.gnome.desktop.default-applications.terminal exec; gsettings reset org.gnome.desktop.default-applications.terminal exec-arg; echo "  GNOME default terminal reset" ;; esac
fi
if grep -qs '^TerminalEmulator=lestrix' "$cfgd/xfce4/helpers.rc"; then sed -i 's/^TerminalEmulator=lestrix/TerminalEmulator=xfce4-terminal/' "$cfgd/xfce4/helpers.rc"; echo "  Xfce preferred terminal set back to xfce4-terminal"; fi
rm -f "${XDG_DATA_HOME:-$HOME/.local/share}/xfce4/helpers/lestrix.desktop"
if grep -qs 'TerminalApplication=.*lestrix' "$cfgd/kdeglobals"; then for kw in kwriteconfig6 kwriteconfig5; do have "$kw" && { "$kw" --file kdeglobals --group General --key TerminalApplication konsole; "$kw" --file kdeglobals --group General --key TerminalService org.kde.konsole.desktop; echo "  KDE default terminal set back to konsole"; break; }; done; fi
if have update-alternatives && update-alternatives --query x-terminal-emulator 2>/dev/null | grep -q 'lestrix'; then
  for p in "${PREFIXES[@]}"; do if [ "$(id -u)" = 0 ]; then update-alternatives --remove x-terminal-emulator "$p/bin/lestrix" >/dev/null 2>&1; elif have sudo; then sudo update-alternatives --remove x-terminal-emulator "$p/bin/lestrix" >/dev/null 2>&1; fi; done
  echo "  x-terminal-emulator alternative removed"
fi

say "Removing Lestrix"
for p in "${PREFIXES[@]}"; do
  rm_path "$p/bin/lestrix"
  rm_path "$p/bin/lxcat"
  rm_path "$p/share/applications/lestrix.desktop"
  rm_path "$p/share/applications/lestrix-lite.desktop"
  for sz in 16x16 22x22 24x24 32x32 48x48 64x64 128x128 256x256 512x512; do rm_path "$p/share/icons/hicolor/$sz/apps/lestrix.png"; done
  rm_path "$p/share/icons/hicolor/scalable/apps/lestrix.svg"
  have update-desktop-database && [ -d "$p/share/applications" ] && update-desktop-database "$p/share/applications" 2>/dev/null
  have gtk-update-icon-cache && [ -d "$p/share/icons/hicolor" ] && gtk-update-icon-cache -q -t "$p/share/icons/hicolor" 2>/dev/null
done
rm_path build

for d in "${TMPDIR:-/tmp}" /tmp /var/tmp "${XDG_RUNTIME_DIR:-/nonexistent}"; do
  for f in "$d"/lestrix-hist-*; do [ -e "$f" ] && rm_path "$f"; done
done

if [ "$PURGE" = 1 ]; then
  cfg="${XDG_CONFIG_HOME:-$HOME/.config}/lestrix"
  if [ -d "$cfg" ] && [ "$YES" != 1 ] && [ -t 0 ]; then
    read -r -p "Delete saved connections and settings in $cfg? [y/N] " a
    case "${a:-n}" in [Yy]*) ;; *) echo "  kept $cfg"; cfg="" ;; esac
  fi
  [ -n "$cfg" ] && rm_path "$cfg"
  rm_path "${XDG_CACHE_HOME:-$HOME/.cache}/lestrix"
  rm_path "${XDG_DATA_HOME:-$HOME/.local/share}/lestrix"
else
  echo "  kept saved connections and settings (use --purge to delete them)"
fi

[ "$removed" -gt 0 ] && say "Done." || say "Nothing to remove."
