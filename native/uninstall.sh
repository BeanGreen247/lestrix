#!/usr/bin/env bash
# Removes everything the native Lestrix installer created.
#   ./uninstall.sh                 remove the app, menu entry, icon and build output
#   ./uninstall.sh --purge         also delete saved connections, settings and leftover history files
#   ./uninstall.sh --prefix DIR    uninstall from DIR instead of ~/.local (and /usr/local)
#   ./uninstall.sh --yes           do not ask before --purge deletes your data
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
PREFIXES=("$HOME/.local" /usr/local /usr); PURGE=0; YES=0
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

# rm_path PATH: delete a file or directory, using sudo only when the user cannot write there
rm_path() {
  [ -e "$1" ] || [ -L "$1" ] || return 0
  if rm -rf "$1" 2>/dev/null; then :
  elif [ "$(id -u)" != 0 ] && have sudo && sudo rm -rf "$1"; then :
  else echo "could not remove $1" >&2; return 0; fi
  echo "  removed $1"; removed=$((removed + 1))
}

say "Removing Lestrix"
for p in "${PREFIXES[@]}"; do
  rm_path "$p/bin/lestrix"
  rm_path "$p/share/applications/lestrix.desktop"
  for sz in 16x16 22x22 24x24 32x32 48x48 64x64 128x128 256x256 512x512; do rm_path "$p/share/icons/hicolor/$sz/apps/lestrix.png"; done
  rm_path "$p/share/icons/hicolor/scalable/apps/lestrix.svg"
  have update-desktop-database && [ -d "$p/share/applications" ] && update-desktop-database "$p/share/applications" 2>/dev/null
  have gtk-update-icon-cache && [ -d "$p/share/icons/hicolor" ] && gtk-update-icon-cache -q -t "$p/share/icons/hicolor" 2>/dev/null
done
rm_path build

# Spill files are unlinked while in use; only a crash can leave a named one behind.
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
