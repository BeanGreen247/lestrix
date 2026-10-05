#!/usr/bin/env bash
# Lestrix installer: a full-screen dialog installer (falls back to whiptail, then to plain questions) that installs, updates or
# removes Lestrix. It builds the C program in native/ (SDL2/OpenGL) with native/install.sh and can make it the default terminal.
#   ./install.sh                     the dialog installer (asks what you want)
#   ./install.sh --yes               no questions: install with the defaults
#   ./install.sh --uninstall         remove Lestrix (--purge also deletes saved connections and settings)
#   --prefix DIR                     install under DIR (default /usr/local, which uses sudo; ~/.local is for your user only)
#   --deps / --no-deps               install missing system packages / never touch them
#   --default-terminal / --no-default-terminal   make Lestrix the default terminal (default: yes)
#   --plain                          no dialog, ask plain questions in the terminal
#   --dry-run                        show what would be run, change nothing
set -uo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SRC" || exit 1

ACTION=""; YES=0; PLAIN=0; PREFIX=""; DEPS=""; DEFTERM=""; PURGE=0; DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
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
    -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
  esac
  shift
done

have() { command -v "$1" >/dev/null 2>&1; }
say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }

# ---- which interface -------------------------------------------------------------------------------------------------------------------------
OS=$(uname -s)
PM=""; for c in apt-get dnf yum pacman zypper brew; do have "$c" && { PM="$c"; break; }; done
TUI=""
pick_tui() { TUI=""; if have dialog; then TUI=dialog; elif have whiptail; then TUI=whiptail; fi; }
if [ "$PLAIN" = 0 ] && [ "$YES" = 0 ] && [ -t 0 ] && [ -t 1 ]; then
  pick_tui
  if [ -z "$TUI" ] && [ -n "$PM" ]; then   # the installer is a dialog program: offer to fetch dialog (the only question asked in plain text)
    printf 'The full-screen installer needs the "dialog" program, which is not installed.\nInstall it now with %s? [Y/n] ' "$PM"
    read -r ans
    case "${ans:-y}" in
      [Yy]*) case "$PM" in
               apt-get) sudo apt-get install -y dialog ;; dnf|yum) sudo "$PM" install -y dialog ;;
               pacman) sudo pacman -S --needed --noconfirm dialog ;; zypper) sudo zypper --non-interactive install dialog ;;
               brew) brew install dialog ;;
             esac; pick_tui ;;
    esac
  fi
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

# run a command in a folder: in the dialog interface its output scrolls inside a dialog box (programbox), otherwise it goes to the terminal;
# with --dry-run only the command is shown
step() {   # title folder command args...
  local title=$1 d=$2; shift 2
  if [ "$DRY" = 1 ]; then echo "[dry run] (cd $d && $*)"; return 0; fi
  if [ -z "$TUI" ]; then ( cd "$d" && "$@" ); return; fi
  local rcf; rcf=$(mktemp)
  case "$TUI" in
    dialog)
      ( cd "$d" && "$@" < /dev/null 2>&1; echo $? > "$rcf" ) | sed -u 's/\x1b\[[0-9;?]*[a-zA-Z]//g' \
        | dialog --backtitle "$BACKTITLE" --title "$title" --programbox "$H" "$W" ;;
    whiptail)   # whiptail has no live output box: work behind a notice, then show what was printed
      local log; log=$(mktemp)
      whiptail --backtitle "$BACKTITLE" --title "$title" --infobox "Working, please wait...\n\nThis can take a minute or two." 9 "$W"
      ( cd "$d" && "$@" < /dev/null > "$log" 2>&1; echo $? > "$rcf" )
      sed -i 's/\x1b\[[0-9;?]*[a-zA-Z]//g' "$log"
      whiptail --backtitle "$BACKTITLE" --title "$title - output" --scrolltext --textbox "$log" "$H" "$W"
      rm -f "$log" ;;
  esac
  local rc; rc=$(cat "$rcf" 2>/dev/null); rm -f "$rcf"; return "${rc:-1}"
}

# sudo asks for its password on the terminal, which would tear the dialog screen: ask for it in a dialog box once and keep it warm
SUDO_KEEPALIVE=""
ensure_sudo() {
  [ "$DRY" = 1 ] && return 0
  [ "$(id -u)" = 0 ] && return 0
  have sudo || return 0
  sudo -n true 2>/dev/null && return 0
  if [ -z "$TUI" ]; then sudo -v; return; fi
  local pw tries=0
  while [ $tries -lt 3 ]; do
    pw=$($TUI --backtitle "$BACKTITLE" --title "Administrator password" --insecure --passwordbox "Lestrix needs your password for sudo to install packages or system files.\n\nPassword for $(id -un):" 10 "$W" 3>&1 1>&2 2>&3) || return 1
    if printf '%s\n' "$pw" | sudo -S -v 2>/dev/null; then
      ( while kill -0 $$ 2>/dev/null; do sudo -n true 2>/dev/null; sleep 45; done ) &
      SUDO_KEEPALIVE=$!
      return 0
    fi
    tries=$((tries + 1))
    ui_msg "Wrong password" "That password did not work ($tries of 3)."
  done
  return 1
}
# on the way out: stop the sudo keepalive and, after a dialog session, clear the screen so nothing of the installer is left in the terminal
cleanup() {
  [ -n "$SUDO_KEEPALIVE" ] && kill "$SUDO_KEEPALIVE" 2>/dev/null
  if [ -n "$TUI" ] && [ "$DRY" = 0 ]; then clear 2>/dev/null || printf '\033[H\033[2J\033[3J'; fi
  return 0
}
trap cleanup EXIT

# ---- what is installed now --------------------------------------------------------------------------------------------------------------------
BINPATH="$(command -v lestrix 2>/dev/null || true)"
[ -z "$BINPATH" ] && [ -e "$HOME/.local/bin/lestrix" ] && BINPATH="$HOME/.local/bin/lestrix"
CURRENT_TEXT="Lestrix is not installed yet."
[ -n "$BINPATH" ] && CURRENT_TEXT="Lestrix is installed ($BINPATH); installing again updates it."

# ---- platform -----------------------------------------------------------------------------------------------------------------------------------
if [ "$OS" != Linux ]; then
  MSG="Lestrix has no build for $OS yet: the program uses Linux system calls (pty handling, /proc, epoll-style readers). Only Linux (X11 or Wayland) is supported for now, so nothing was changed."
  if [ -n "$TUI" ]; then ui_msg "Not supported on $OS yet" "$MSG"; else echo "$MSG" >&2; fi
  exit 1
fi

# ---- choose what to do ------------------------------------------------------------------------------------------------------------------------
if [ -z "$ACTION" ]; then
  if [ "$INTERACTIVE" = 1 ]; then
    ui_msg "Welcome to Lestrix" "SSH sessions, SFTP/FTP and local shells in one window.\n\n$CURRENT_TEXT\n\nThis installer can install, update or remove Lestrix. Nothing is changed until you confirm on the last screen."
    ACTION=$(ui_radio "What do you want to do?" "Choose one:" \
      install "Install or update Lestrix" on \
      uninstall "Remove Lestrix" off \
      quit "Quit without changing anything" off) || exit 0
  else
    ACTION=install
  fi
fi
{ [ "$ACTION" = quit ] || [ -z "$ACTION" ]; } && { echo "Nothing changed."; exit 0; }

# ---- uninstall ------------------------------------------------------------------------------------------------------------------------------------
if [ "$ACTION" = uninstall ]; then
  if [ "$INTERACTIVE" = 1 ] && [ "$PURGE" = 0 ]; then
    ui_yesno "Saved data" "Also delete your saved connections, settings and leftover history files?\n\nChoose No to keep them (recommended)." no && PURGE=1
  fi
  if [ "$INTERACTIVE" = 1 ]; then
    ui_yesno "Confirm" "Remove Lestrix now?$([ "$PURGE" = 1 ] && printf '\n\nYour saved connections and settings will be deleted too.')" yes || { echo "Nothing changed."; exit 0; }
  fi
  a=(); [ "$PURGE" = 1 ] && a=(--purge --yes)
  [ -n "$PREFIX" ] && a+=(--prefix "$PREFIX")
  rc=0
  ensure_sudo || warn "no sudo: system-wide parts may stay"
  step "Removing Lestrix" native ./uninstall.sh "${a[@]}" || rc=1
  [ -n "$TUI" ] && ui_msg "Lestrix removed" "Lestrix was removed.\n\nYour default-terminal settings were given back where they pointed at Lestrix."
  exit $rc
fi

# ---- install: options -------------------------------------------------------------------------------------------------------------------------
RUN_TESTS=0; TEST_FAILED=0
if [ "$INTERACTIVE" = 1 ]; then
  DEPS_ON=on; have gcc && have make && pkg-config --exists sdl2 freetype2 fontconfig glib-2.0 libcurl 2>/dev/null && DEPS_ON=off
  OPTS=$(ui_check "Install options" "Space marks or unmarks an option, Enter continues." \
    deps "Install missing build packages (compiler, SDL2, FreeType, ...; asks for sudo)" "$DEPS_ON" \
    defterm "Make Lestrix the default terminal" on \
    user "Install only for my user in ~/.local instead of system-wide in /usr/local (no sudo for the files)" off \
    tests "Run the test suite after installing (takes about a minute)" off) || exit 0
  [ -z "$PREFIX" ] && chosen "$OPTS" user && PREFIX="$HOME/.local"
  [ -z "$DEPS" ] && { chosen "$OPTS" deps && DEPS=yes || DEPS=no; }
  [ -z "$DEFTERM" ] && { chosen "$OPTS" defterm && DEFTERM=yes || DEFTERM=no; }
  chosen "$OPTS" tests && RUN_TESTS=1
else
  [ -z "$DEPS" ] && DEPS=ask
fi
[ -z "$DEFTERM" ] && DEFTERM=yes
[ -z "$PREFIX" ] && PREFIX=/usr/local   # system-wide by default: every user of the machine gets it

# ---- confirm --------------------------------------------------------------------------------------------------------------------------------------
SUMMARY="Install to:        $PREFIX
System packages:   ${DEPS}
Default terminal:  ${DEFTERM}
Run tests:         $([ "$RUN_TESTS" = 1 ] && echo yes || echo no)
Now:               $CURRENT_TEXT"
if [ "$INTERACTIVE" = 1 ]; then
  ui_yesno "Ready to install" "$SUMMARY\n\nContinue?" yes || { echo "Nothing changed."; exit 0; }
else
  say "Installing Lestrix"
fi

# ---- run ------------------------------------------------------------------------------------------------------------------------------------
rc=0
a=()
case "$DEPS" in yes) a+=(--deps) ;; no) a+=(--no-deps) ;; esac
[ "$DEFTERM" = no ] && a+=(--no-default-terminal)
[ -n "$PREFIX" ] && a+=(--prefix "$PREFIX")
NEED_SUDO=0
probe="$PREFIX"; while [ ! -e "$probe" ] && [ "$probe" != / ]; do probe=$(dirname "$probe"); done
{ [ "$DEPS" = yes ] || [ ! -w "$probe" ] || { [ "$DEFTERM" = yes ] && have update-alternatives; }; } && NEED_SUDO=1
if [ "$NEED_SUDO" = 1 ]; then ensure_sudo || { warn "no sudo access: continuing without it (system packages and the x-terminal-emulator setting may be skipped)"; }; fi
step "Installing Lestrix (building takes a minute)" native ./install.sh "${a[@]}" || rc=$?
if [ $rc = 0 ] && [ "$RUN_TESTS" = 1 ]; then
  step "Running the test suite" native ./test.sh $([ "$DEPS" = yes ] && echo --deps || echo --no-deps) || { TEST_FAILED=1; warn "some tests failed (the install itself is done)"; }
fi

if [ $rc = 0 ]; then
  MSG="Lestrix is installed.\n\n$([ "$TEST_FAILED" = 1 ] && printf 'Some tests failed; the output was shown in the test step.\\n\\n')Start it from your application menu (System) or run:  lestrix\n\nRemove it any time with:  ./install.sh --uninstall\n\nExtras: 'lxcat' prints big files at memory speed in a Lestrix tab; 'cat' uses it automatically (View > Fast cat)."
  case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) MSG="$MSG\n\nNote: add $PREFIX/bin to your PATH to run 'lestrix' from a shell." ;; esac
  if [ "$INTERACTIVE" = 1 ]; then ui_msg "Done" "$MSG"; else printf '%b\n' "$MSG"; fi
else
  MSG="The installer stopped with an error (code $rc). The messages above say what failed."
  if [ "$INTERACTIVE" = 1 ]; then ui_msg "Install failed" "$MSG"; else echo "$MSG" >&2; fi
fi
exit $rc
