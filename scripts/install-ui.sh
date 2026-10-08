#!/usr/bin/env bash
# What the installer shows while it works: numbered steps with a plain explanation of what each
# one does and why, and a live progress line (counter, current item, elapsed time) for the
# slow parts, so a long compile or package install never looks like a hang.
# Sourced by install.sh and the build scripts; nothing here changes the system.

UI_TOTAL_STEPS=${UI_TOTAL_STEPS:-6}
UI_STEP=0
UI_TTY=0
[[ -t 1 ]] && UI_TTY=1
if (( UI_TTY )); then UI_BOLD=$'\033[1m'; UI_DIM=$'\033[2m'; UI_OFF=$'\033[0m'; else UI_BOLD=""; UI_DIM=""; UI_OFF=""; fi

ui_time() {  # seconds -> "1h 02m 03s" / "4m 07s" / "12s"
  local s=$1
  if (( s >= 3600 )); then printf "%dh %02dm %02ds" $((s / 3600)) $((s % 3600 / 60)) $((s % 60))
  elif (( s >= 60 )); then printf "%dm %02ds" $((s / 60)) $((s % 60))
  else printf "%ds" "${s}"; fi
}

# ui_step "Title" "explanation line" ...   -> "[3/12] Title" and the explanation under it.
ui_step() {
  UI_STEP=$(( UI_STEP + 1 ))
  echo
  printf "%s[%d/%d] %s%s\n" "${UI_BOLD}" "${UI_STEP}" "${UI_TOTAL_STEPS}" "$1" "${UI_OFF}"
  shift
  local line
  for line in "$@"; do printf "%s      %s%s\n" "${UI_DIM}" "${line}" "${UI_OFF}"; done
}

# ui_note "text" ...   -> indented, dimmed explanation lines.
ui_note() {
  local line
  for line in "$@"; do printf "%s      %s%s\n" "${UI_DIM}" "${line}" "${UI_OFF}"; done
}

# ui_item "package-or-thing" "what it is for"
ui_item() { printf "%s        %-30s %s%s\n" "${UI_DIM}" "$1" "$2" "${UI_OFF}"; }

# The progress text for one kind of command, from its log so far.
#   apt     "[12/37] unpacking libfoo"            (counts come from apt's own output)
#   ninja   "[85/210] Compiling C++ object ..."   (ninja's own counter)
#   tests   "[120/517] tests passed so far"
#   plain   the last line the command printed
ui_progress() {
  local mode=$1 log=$2 last total n
  case "${mode}" in
    apt)
      total=$(sed -n 's/^\([0-9]*\) upgraded, \([0-9]*\) newly installed.*/\1 \2/p' "${log}" | awk '{ t = $1 + $2 } END { if (NR) print t }')
      if [[ -z "${total}" ]]; then echo "reading the package lists"; return; fi
      if (( total == 0 )); then echo "everything is already installed"; return; fi
      local gets unp setup
      gets=$(grep -c '^Get:' "${log}" || true)
      unp=$(grep -c '^Unpacking ' "${log}" || true)
      setup=$(grep -c '^Setting up ' "${log}" || true)
      if (( setup > 0 )); then
        last=$(grep '^Setting up ' "${log}" | tail -1 | awk '{ print $3 }')
        echo "[${setup}/${total}] setting up ${last}"
      elif (( unp > 0 )); then
        last=$(grep '^Unpacking ' "${log}" | tail -1 | awk '{ print $2 }')
        echo "[${unp}/${total}] unpacking ${last}"
      else
        echo "[${gets}/${total}] downloading packages"
      fi ;;
    ninja)
      last=$(grep -E '^\[[0-9]+/[0-9]+\]' "${log}" | tail -1)
      if [[ -z "${last}" ]]; then echo "starting the compiler"; return; fi
      n=${last%%]*}; n="${n}]"
      last=${last#*] }
      case "${last}" in
        *Linking*) echo "${n} ${last:0:48} (linking with LTO: the slow part, can take minutes)" ;;
        *) echo "${n} ${last:0:60}" ;;
      esac ;;
    tests)
      total=$(sed -n 's/^\[==========\] Running \([0-9]*\) tests.*/\1/p' "${log}" | head -1)
      n=$(grep -cE '^\[       OK \]|^\[  FAILED  \]' "${log}" || true)
      if [[ -z "${total}" ]]; then echo "starting the test program"; else echo "[${n}/${total}] tests run"; fi ;;
    *)
      last=$(grep -v '^[[:space:]]*$' "${log}" | tail -1)
      echo "${last:0:70}" ;;
  esac
}

# ui_live <mode> <label> <logfile> <command...>
# Runs the command with its output going to the log, and keeps one progress line current
# (counter, what it is doing right now, time spent) until it ends. A command that fails has
# the end of its log printed. Returns the command's exit status.
ui_live() {
  local mode=$1 label=$2 log=$3
  shift 3
  : > "${log}"
  "$@" > "${log}" 2>&1 &
  local pid=$! t0=$SECONDS text last_text="" elapsed rc cols=$(tput cols 2>/dev/null || echo 100) last_print=-100
  while kill -0 "${pid}" 2>/dev/null; do
    elapsed=$(( SECONDS - t0 ))
    text=$(ui_progress "${mode}" "${log}")
    if (( UI_TTY )); then
      local shown="  ${label}: ${text}  ($(ui_time "${elapsed}"))"
      printf "\r\033[K%s" "${shown:0:$(( cols - 1 ))}"
    elif [[ "${text}" != "${last_text}" ]] && (( elapsed - last_print >= 10 )); then
      printf "  %s: %s  (%s)\n" "${label}" "${text}" "$(ui_time "${elapsed}")"
      last_print=${elapsed}
    fi
    last_text=${text}
    sleep 1
  done
  wait "${pid}" && rc=0 || rc=$?
  elapsed=$(( SECONDS - t0 ))
  (( UI_TTY )) && printf "\r\033[K"
  if (( rc == 0 )); then
    printf "  %s: done in %s\n" "${label}" "$(ui_time "${elapsed}")"
  else
    printf "  %s: FAILED after %s (exit %d). End of %s:\n" "${label}" "$(ui_time "${elapsed}")" "${rc}" "${log}" >&2
    tail -25 "${log}" >&2
  fi
  return "${rc}"
}

# Asks for the sudo password once, up front, and keeps the permission fresh for the whole run,
# so a password prompt can never appear in the middle of a progress line.
ui_sudo_keepalive() {
  sudo -v || return 1
  ( while true; do sleep 50; sudo -n -v 2>/dev/null || exit 0; kill -0 "$$" 2>/dev/null || exit 0; done ) &
  UI_SUDO_KEEPALIVE_PID=$!
  trap 'kill "${UI_SUDO_KEEPALIVE_PID}" 2>/dev/null || true' EXIT
}
