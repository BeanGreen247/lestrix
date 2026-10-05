#!/usr/bin/env bash
# Lestrix installer: a full-screen dialog installer (falls back to whiptail, then to plain questions) that installs, updates or
# removes either edition. The native edition (C, SDL2/OpenGL, fast) is the recommended one; the Python/Qt edition is the one for
# Windows, macOS and systems without a compiler. Both provide the same `lestrix` command, so one replaces the other.
#   ./install.sh                     the dialog installer (asks what you want)
#   ./install.sh --native            install the native edition without asking (plain output)
#   ./install.sh --python            install the Python/Qt edition without asking
#   ./install.sh --uninstall         remove Lestrix (asks which edition; --native / --python picks one)
#   --yes                            no questions: use the defaults
#   --prefix DIR                     native edition: install under DIR (default ~/.local; /usr/local uses sudo)
#   --deps / --no-deps               install missing system packages / never touch them
#   --default-terminal / --no-default-terminal   make Lestrix the default terminal (default: yes)
#   --purge                          with --uninstall: also delete saved connections and settings
#   --plain                          no dialog, ask plain questions in the terminal
#   --dry-run                        show what would be run, change nothing
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC" || exit 1

EDITION=""; ACTION=""; YES=0; PLAIN=0; PREFIX=""; DEPS=""; DEFTERM=""; PURGE=0; DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --native) EDITION=native ;;
    --python) EDITION=python ;;
    --install) ACTION=install ;;
    --uninstall) ACTION=uninstall ;;
    --yes|-y) YES=1 ;;
    --plain) PLAIN=1 ;;
    --dry-run) DRY=1 ;;
    --prefix) PREFIX="$2"; shift ;;
    --deps) DEPS=yes ;;
    --no-deps) DEPS=no ;;
    --default-terminal) DEFTERM=yes ;;
    --no-default-terminal) DEFTERM=no ;;
    --purge) PURGE=1 ;;
    -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

have() { command -v "$1" >/dev/null 2>&1; }
say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }

# ---- which interface -------------------------------------------------------------------------------------------------------------------------
TUI=""
if [ "$PLAIN" = 0 ] && [ "$YES" = 0 ] && [ -t 0 ] && [ -t 1 ]; then
  if have dialog; then TUI=dialog; elif have whiptail; then TUI=whiptail; fi
fi
INTERACTIVE=0; [ "$YES" = 0 ] && [ -t 0 ] && INTERACTIVE=1
BACKTITLE="Lestrix installer"
TERM_ROWS=$(tput lines 2>/dev/null || echo 24); TERM_COLS=$(tput cols 2>/dev/null || echo 80)
H=$(( TERM_ROWS > 28 ? 24 : TERM_ROWS - 4 )); W=$(( TERM_COLS > 90 ? 76 : TERM_COLS - 4 ))

# dialog and whiptail write their answer to stderr; 3>&1 1>&2 2>&3 swaps it into the command substitution
ui_msg() {   # title text
  case "$TUI" in
    dialog|whiptail) $TUI --backtitle "$BACKTITLE" --title "$1" --msgbox "$2" "$H" "$W" ;;
    *) printf '\n== %s ==\n%s\n' "$1" "$2" ;;
  esac
}
ui_yesno() {   # title text [default yes|no] -> status 0 = yes
  local def=${3:-yes}
  case "$TUI" in
    dialog) local d=(); [ "$def" = no ] && d=(--defaultno); dialog --backtitle "$BACKTITLE" --title "$1" "${d[@]}" --yesno "$2" "$H" "$W" ;;
    whiptail) local d=(); [ "$def" = no ] && d=(--defaultno); whiptail --backtitle "$BACKTITLE" --title "$1" "${d[@]}" --yesno "$2" "$H" "$W" ;;
    *)
      [ "$INTERACTIVE" = 0 ] && { [ "$def" = yes ]; return; }
      local a; printf '\n%s\n%s [%s] ' "$1" "$2" "$([ "$def" = yes ] && echo Y/n || echo y/N)"; read -r a
      case "${a:-}" in [Yy]*) return 0 ;; [Nn]*) return 1 ;; *) [ "$def" = yes ] ;; esac ;;
  esac
}
ui_radio() {   # title text tag desc on|off ... -> prints the chosen tag
  local title=$1 text=$2; shift 2
  case "$TUI" in
    dialog|whiptail)
      local n=$(( $# / 3 ))
      $TUI --backtitle "$BACKTITLE" --title "$title" --radiolist "$text" "$H" "$W" "$n" "$@" 3>&1 1>&2 2>&3 ;;
    *)
      local tags=() descs=() def=1 i=0
      while [ $# -ge 3 ]; do tags+=("$1"); descs+=("$2"); [ "$3" = on ] && def=$((i + 1)); i=$((i + 1)); shift 3; done
      if [ "$INTERACTIVE" = 0 ]; then echo "${tags[$((def - 1))]}"; return 0; fi
      printf '\n%s\n%s\n' "$title" "$text" >&2
      for i in "${!tags[@]}"; do printf '  %d) %s - %s\n' "$((i + 1))" "${tags[$i]}" "${descs[$i]}" >&2; done
      local a; printf 'choice [%d]: ' "$def" >&2; read -r a
      a=${a:-$def}; echo "${tags[$((a - 1))]:-${tags[$((def - 1))]}}" ;;
  esac
}
ui_check() {   # title text tag desc on|off ... -> prints the chosen tags, one per line
  local title=$1 text=$2; shift 2
  case "$TUI" in
    dialog|whiptail)
      local n=$(( $# / 3 ))
      $TUI --backtitle "$BACKTITLE" --title "$title" --separate-output --checklist "$text" "$H" "$W" "$n" "$@" 3>&1 1>&2 2>&3 | tr -d '"' ;;
    *)
      local tags=() descs=() ons=() i=0
      while [ $# -ge 3 ]; do tags+=("$1"); descs+=("$2"); ons+=("$3"); shift 3; done
      if [ "$INTERACTIVE" = 0 ]; then for i in "${!tags[@]}"; do [ "${ons[$i]}" = on ] && echo "${tags[$i]}"; done; return 0; fi
      printf '\n%s\n%s\n' "$title" "$text" >&2
      for i in "${!tags[@]}"; do printf '  %d) [%s] %s\n' "$((i + 1))" "$([ "${ons[$i]}" = on ] && echo x || echo ' ')" "${descs[$i]}" >&2; done
      local a; printf 'numbers to turn on (space separated, empty = keep the marked ones): ' >&2; read -r a
      if [ -z "$a" ]; then for i in "${!tags[@]}"; do [ "${ons[$i]}" = on ] && echo "${tags[$i]}"; done
      else for n in $a; do echo "${tags[$((n - 1))]:-}"; done; fi ;;
  esac
}
ui_input() {   # title text default -> prints the text typed
  case "$TUI" in
    dialog|whiptail) $TUI --backtitle "$BACKTITLE" --title "$1" --inputbox "$2" 10 "$W" "$3" 3>&1 1>&2 2>&3 ;;
    *) [ "$INTERACTIVE" = 0 ] && { echo "$3"; return 0; }; local a; printf '\n%s\n%s [%s]: ' "$1" "$2" "$3" >&2; read -r a; echo "${a:-$3}" ;;
  esac
}
chosen() { printf '%s\n' "$1" | grep -qx "$2"; }   # chosen "$list" tag

# run a command in a folder, or only show it with --dry-run
runin() {   # folder command args...
  local d=$1; shift
  if [ "$DRY" = 1 ]; then echo "[dry run] (cd $d && $*)"; return 0; fi
  ( cd "$d" && "$@" )
}

# ---- what is installed now --------------------------------------------------------------------------------------------------------------------
BINPATH="$(command -v lestrix 2>/dev/null || true)"
[ -z "$BINPATH" ] && [ -e "$HOME/.local/bin/lestrix" ] && BINPATH="$HOME/.local/bin/lestrix"
installed_edition() {
  [ -n "$BINPATH" ] || { echo none; return; }
  if head -c 4 "$BINPATH" 2>/dev/null | grep -q 'ELF'; then echo native
  elif [ -d "${XDG_DATA_HOME:-$HOME/.local/share}/lestrix/venv" ]; then echo python
  else echo unknown; fi
}
CURRENT=$(installed_edition)
CURRENT_TEXT="Nothing is installed yet."
case "$CURRENT" in
  native) CURRENT_TEXT="The native edition is installed ($BINPATH)." ;;
  python) CURRENT_TEXT="The Python/Qt edition is installed ($BINPATH)." ;;
  unknown) CURRENT_TEXT="A 'lestrix' command exists at $BINPATH (edition unknown)." ;;
esac

PM=""; for c in apt-get dnf yum pacman zypper; do have "$c" && { PM="$c"; break; }; done
OS=$(uname -s)

# ---- choose what to do ------------------------------------------------------------------------------------------------------------------------
if [ -z "$ACTION" ]; then
  if [ "$INTERACTIVE" = 1 ] && [ -z "$EDITION" ]; then
    ui_msg "Welcome to Lestrix" "SSH sessions, SFTP/FTP and local shells in one window.\n\n$CURRENT_TEXT\n\nThis installer can install, update or remove Lestrix. Nothing is changed until you confirm on the last screen."
    ACTION=$(ui_radio "What do you want to do?" "Choose one:" \
      install "Install or update Lestrix" on \
      uninstall "Remove Lestrix" off \
      quit "Quit without changing anything" off) || exit 0
  else
    ACTION=install
  fi
fi
[ "$ACTION" = quit ] || [ -z "$ACTION" ] && { echo "Nothing changed."; exit 0; }

# ---- uninstall ------------------------------------------------------------------------------------------------------------------------------------
if [ "$ACTION" = uninstall ]; then
  if [ -z "$EDITION" ]; then
    DEF_N=on; DEF_P=off; DEF_B=off
    [ "$CURRENT" = python ] && { DEF_N=off; DEF_P=on; }
    EDITION=$(ui_radio "Remove which edition?" "$CURRENT_TEXT" \
      native "The native edition (and lxcat, and the default-terminal settings)" "$DEF_N" \
      python "The Python/Qt edition" "$DEF_P" \
      both "Both" "$DEF_B") || exit 0
  fi
  if [ "$INTERACTIVE" = 1 ] && [ "$PURGE" = 0 ]; then
    ui_yesno "Saved data" "Also delete your saved connections, settings and leftover history files?\n\nChoose No to keep them (recommended)." no && PURGE=1
  fi
  if [ "$INTERACTIVE" = 1 ]; then ui_yesno "Confirm" "Remove the $EDITION edition of Lestrix now?$([ "$PURGE" = 1 ] && printf '\n\nYour saved connections and settings will be deleted too.')" yes || { echo "Nothing changed."; exit 0; }; fi
  clear 2>/dev/null || true
  rc=0
  if [ "$EDITION" = native ] || [ "$EDITION" = both ]; then
    a=(); [ "$PURGE" = 1 ] && a=(--purge --yes)
    [ -n "$PREFIX" ] && a+=(--prefix "$PREFIX")
    runin native ./uninstall.sh "${a[@]}" || rc=1
  fi
  if [ "$EDITION" = python ] || [ "$EDITION" = both ]; then runin . ./install-python.sh --uninstall || rc=1; fi
  [ "$TUI" = "" ] || ui_msg "Lestrix removed" "Lestrix ($EDITION) was removed.\n\nYour default-terminal settings were given back where they pointed at Lestrix."
  exit $rc
fi

# ---- install: which edition ---------------------------------------------------------------------------------------------------------------------
if [ -z "$EDITION" ]; then
  if [ "$INTERACTIVE" = 1 ]; then
    NAT_NOTE="Native (recommended): fast C program on SDL2/OpenGL, builds in about a minute, needs a compiler"
    EDITION=$(ui_radio "Which edition?" "Both give you the same 'lestrix' command, so installing one replaces the other.\n\n$CURRENT_TEXT" \
      native "$NAT_NOTE" on \
      python "Python/Qt edition: no compiler needed, slower, also runs on Windows and macOS" off) || exit 0
  else
    EDITION=native
  fi
fi

# ---- options -----------------------------------------------------------------------------------------------------------------------------------
OPTS=""
if [ "$INTERACTIVE" = 1 ]; then
  if [ "$EDITION" = native ]; then
    DEPS_ON=on; have gcc && have make && pkg-config --exists sdl2 freetype2 fontconfig glib-2.0 libcurl 2>/dev/null && DEPS_ON=off
    OPTS=$(ui_check "Native edition options" "Space marks or unmarks an option, Enter continues." \
      deps "Install missing build packages (compiler, SDL2, FreeType, ...; asks for sudo)" "$DEPS_ON" \
      defterm "Make Lestrix the default terminal" on \
      system "Install system-wide to /usr/local instead of your home folder (asks for sudo)" off \
      tests "Run the test suite after installing (takes about a minute)" off) || exit 0
  else
    OPTS=$(ui_check "Python edition options" "Space marks or unmarks an option, Enter continues." \
      deps "Install the Qt system libraries (asks for sudo)" on \
      defterm "Make Lestrix the x-terminal-emulator (Debian/Ubuntu; asks for sudo)" on) || exit 0
  fi
  [ -z "$PREFIX" ] && chosen "$OPTS" system && PREFIX=/usr/local
  [ -z "$DEPS" ] && { chosen "$OPTS" deps && DEPS=yes || DEPS=no; }
  [ -z "$DEFTERM" ] && { chosen "$OPTS" defterm && DEFTERM=yes || DEFTERM=no; }
  chosen "$OPTS" tests && RUN_TESTS=1 || RUN_TESTS=0
else
  RUN_TESTS=0
  [ -z "$DEPS" ] && DEPS=ask
  [ -z "$DEFTERM" ] && DEFTERM=yes
fi
[ -z "$DEFTERM" ] && DEFTERM=yes

# ---- confirm --------------------------------------------------------------------------------------------------------------------------------------
SUMMARY="Edition:           $EDITION
Install to:        ${PREFIX:-$HOME/.local}$([ "$EDITION" = python ] && echo ' (the Python edition always installs for your user)')
System packages:   ${DEPS}
Default terminal:  ${DEFTERM}
Run tests:         $([ "${RUN_TESTS:-0}" = 1 ] && echo yes || echo no)
Replaces:          $CURRENT_TEXT"
if [ "$INTERACTIVE" = 1 ]; then
  ui_yesno "Ready to install" "$SUMMARY\n\nContinue?" yes || { echo "Nothing changed."; exit 0; }
else
  say "Installing the $EDITION edition"
fi
clear 2>/dev/null || true

# ---- run ------------------------------------------------------------------------------------------------------------------------------------
rc=0
if [ "$EDITION" = native ]; then
  a=()
  case "$DEPS" in yes) a+=(--deps) ;; no) a+=(--no-deps) ;; esac
  [ "$DEFTERM" = no ] && a+=(--no-default-terminal)
  [ -n "$PREFIX" ] && a+=(--prefix "$PREFIX")
  runin native ./install.sh "${a[@]}" || rc=$?
  if [ $rc = 0 ] && [ "${RUN_TESTS:-0}" = 1 ]; then
    say "Running the test suite"
    runin native ./test.sh $([ "$DEPS" = yes ] && echo --deps || echo --no-deps) || warn "some tests failed (the install itself is done)"
  fi
else
  a=()
  case "$DEPS" in yes) a+=(--system-deps) ;; no) a+=(--no-system-deps) ;; esac
  [ "$DEFTERM" = yes ] && a+=(--set-default)
  runin . ./install-python.sh "${a[@]}" || rc=$?
fi

if [ $rc = 0 ]; then
  MSG="Lestrix ($EDITION edition) is installed.\n\nStart it from your application menu (System) or run:  lestrix\n\nRemove it any time with:  ./install.sh --uninstall"
  [ "$EDITION" = native ] && MSG="$MSG\n\nExtras: 'lxcat' prints big files at memory speed in a Lestrix tab; 'cat' uses it automatically (View > Fast cat)."
  case ":$PATH:" in *":${PREFIX:-$HOME/.local}/bin:"*) ;; *) MSG="$MSG\n\nNote: add ${PREFIX:-$HOME/.local}/bin to your PATH to run 'lestrix' from a shell." ;; esac
  if [ "$INTERACTIVE" = 1 ]; then ui_msg "Done" "$MSG"; else printf '%b\n' "$MSG"; fi
else
  MSG="The installer stopped with an error (code $rc). The messages above say what failed."
  if [ "$INTERACTIVE" = 1 ]; then ui_msg "Install failed" "$MSG"; else echo "$MSG" >&2; fi
fi
exit $rc
