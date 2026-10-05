#!/usr/bin/env bash
# test.sh - installs what the tests need and runs all of them.
#   ./test.sh                 run everything that can run here, ask before installing packages
#   ./test.sh --deps          install the test dependencies first (needs root or sudo)
#   ./test.sh --no-deps       never touch system packages
#   ./test.sh --arm           also build and run the engine tests for aarch64 and armhf under qemu (Raspberry Pi, ODROID, Tinker Board)
#   ./test.sh --bench         also run the quick benchmark (Lestrix only, about a minute)
#   ./test.sh --only NAME     one group: engine, fast, lz, tsan, store, xfer, ftp, perf, gui
# What runs: the terminal engine under ASan/UBSan (and against pyte when it is installed), the UTF-8 fast-path fuzz test, the compressor,
# the engine under ThreadSanitizer, the store/transfer/FTP tests, the speed gate and the window test (a real window driven by scripted
# input; under xvfb when there is no display). Exit status is 0 only if everything that ran passed.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
DEPS=ask; ARM=0; BENCH=0; ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --deps) DEPS=yes ;; --no-deps) DEPS=no ;; --arm) ARM=1 ;; --bench) BENCH=1 ;;
    --only) ONLY="$2"; shift ;;
    -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac; shift
done

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
have() { command -v "$1" >/dev/null 2>&1; }
if [ "$(id -u)" = 0 ]; then SUDO=""; elif have sudo; then SUDO="sudo"; else SUDO="none"; fi
PM=""; for c in apt-get dnf yum pacman zypper; do have "$c" && { PM="$c"; break; }; done

# build + test tools, the SDL/Mesa pieces the window test needs, pyte for the differential test, bc/python3 for the benchmark script
pkgs() {
  local base cross=""
  case "$PM" in
    apt-get) base="build-essential pkg-config libsdl2-dev libfreetype-dev libfontconfig-dev libglib2.0-dev libcurl4-openssl-dev libgl1 libegl1 libgles2 libgl1-mesa-dri python3 python3-pyte bc xvfb xauth"
             [ "$ARM" = 1 ] && cross="gcc-aarch64-linux-gnu gcc-arm-linux-gnueabihf qemu-user" ;;
    dnf|yum) base="gcc make pkgconf-pkg-config libasan libtsan libubsan SDL2-devel freetype-devel fontconfig-devel glib2-devel libcurl-devel mesa-libGL mesa-libEGL mesa-dri-drivers python3 bc xorg-x11-server-Xvfb xauth"
             [ "$ARM" = 1 ] && cross="gcc-aarch64-linux-gnu gcc-arm-linux-gnu qemu-user" ;;
    pacman)  base="base-devel pkgconf sdl2 freetype2 fontconfig glib2 curl mesa python bc xorg-server-xvfb xorg-xauth"
             [ "$ARM" = 1 ] && cross="aarch64-linux-gnu-gcc arm-none-eabi-gcc qemu-user" ;;
    zypper)  base="gcc make pkg-config libasan8 libtsan2 libubsan1 SDL2-devel freetype2-devel fontconfig-devel glib2-devel libcurl-devel Mesa-libGL1 Mesa-libEGL1 Mesa-dri python3 bc xorg-x11-server-Xvfb xauth"
             [ "$ARM" = 1 ] && cross="cross-aarch64-gcc13 qemu-linux-user" ;;
  esac
  echo $base $cross
}

missing() {
  local m=""
  have gcc || m="$m gcc"; have make || m="$m make"; have pkg-config || m="$m pkg-config"
  pkg-config --exists sdl2 freetype2 fontconfig glib-2.0 gio-2.0 libcurl 2>/dev/null || m="$m dev-libraries"
  have python3 || m="$m python3"; have bc || m="$m bc"
  { [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || have xvfb-run; } || m="$m xvfb"
  echo "$m"
  # a test run is only useful with the sanitizer runtimes: check by compiling a one-liner
  echo 'int main(void){return 0;}' | gcc -x c -fsanitize=address,undefined - -o /tmp/lx-san-check 2>/dev/null || echo " sanitizer-runtimes"
  echo 'int main(void){return 0;}' | gcc -x c -fsanitize=thread - -o /tmp/lx-san-check 2>/dev/null || echo " thread-sanitizer-runtime"
  [ "$ARM" = 1 ] && { have aarch64-linux-gnu-gcc || echo " aarch64-cross-compiler"; have qemu-aarch64 || have qemu-aarch64-static || echo " qemu-user"; }
  rm -f /tmp/lx-san-check
}

install_deps() {
  [ -n "$PM" ] && [ "$SUDO" != none ] || { warn "cannot install packages here; install: $(missing | tr '\n' ' ')"; return 1; }
  say "Installing test dependencies with $PM"
  case "$PM" in
    apt-get) $SUDO apt-get update -y >/dev/null; $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $(pkgs) ;;
    dnf)     $SUDO dnf install -y $(pkgs) ;;
    yum)     $SUDO yum install -y $(pkgs) ;;
    pacman)  $SUDO pacman -S --needed --noconfirm $(pkgs) ;;
    zypper)  for p in $(pkgs); do $SUDO zypper --non-interactive install "$p" >/dev/null || warn "skipped $p"; done ;;
  esac
  # pyte (the reference terminal for the differential test) from pip when the distribution has no package
  python3 -c 'import pyte' 2>/dev/null || python3 -m pip install --user pyte >/dev/null 2>&1 || python3 -m pip install --user --break-system-packages pyte >/dev/null 2>&1 || true
}

miss=$(missing | tr '\n' ' ' | xargs)
if [ -n "$miss" ]; then
  if [ "$DEPS" = no ]; then warn "missing: $miss (continuing; groups that need them will be skipped or fail)"
  elif [ "$DEPS" = yes ] || { [ -t 0 ] && read -r -p "Missing for the tests: $miss. Install them with ${PM:-your package manager} now? [Y/n] " a && case "${a:-y}" in [Yy]*) true ;; *) false ;; esac; }; then
    install_deps || true
  else warn "missing: $miss"; fi
fi
python3 -c 'import pyte' 2>/dev/null || warn "pyte is not installed: the differential test against pyte will be skipped (pip install pyte)"

declare -a NAMES RESULTS
record() { NAMES+=("$1"); RESULTS+=("$2"); }
run() {   # run NAME COMMAND...
  local name=$1; shift
  [ -z "$ONLY" ] || [ "$ONLY" = "$name" ] || return 0
  say "$name"
  if "$@"; then record "$name" PASS; else record "$name" FAIL; fi
}

run engine make test-vt
run fast   make test-fast
run lz     make test-lz
run tsan   make test-vt-tsan
run store  make test-store
run xfer   make test-xfer
run ftp    make test-ftp
run perf   make perf-gate

gui_test() {
  if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then make test-gui
  elif have xvfb-run; then xvfb-run -a -s "-screen 0 1600x900x24" make test-gui
  else echo "no display and no xvfb-run: cannot open a window"; return 2; fi
}
if [ -z "$ONLY" ] || [ "$ONLY" = gui ]; then
  say "gui"
  gui_test; rc=$?
  if [ $rc = 0 ]; then record gui PASS; elif [ $rc = 2 ]; then record gui SKIP; else record gui FAIL; fi
fi

arm_tests() {   # the engine and the compressor on 64-bit and 32-bit ARM, run under qemu (same NEON code as a Raspberry Pi 2-5)
  local ok=0 name cc flags qemu out
  mkdir -p build/arm
  for t in "aarch64:aarch64-linux-gnu-gcc:-march=armv8-a" "armhf:arm-linux-gnueabihf-gcc:-march=armv7-a -mfpu=neon-vfpv4"; do
    IFS=: read -r name cc flags <<<"$t"
    have "$cc" || { echo "skip $name: $cc not installed"; continue; }
    qemu=qemu-aarch64; [ "$name" = armhf ] && qemu=qemu-arm
    have "$qemu" || qemu="$qemu-static"; have "$qemu" || { echo "skip $name: qemu-user not installed"; continue; }
    for f in test_lz test_vt test_fast_paths; do
      $cc -O3 -std=gnu11 -pthread $flags -static -I src -o build/arm/${name}_$f tests/$f.c src/vt.c src/lz.c 2>/dev/null || { echo "$name $f: build failed"; ok=1; continue; }
      out=$($qemu build/arm/${name}_$f 2>&1 | tail -1); echo "$name $f: $out"
      case "$out" in *" 0 failures") ;; *) ok=1 ;; esac
    done
  done
  return $ok
}
[ "$ARM" = 1 ] && run arm arm_tests

if [ "$BENCH" = 1 ]; then run bench-quick ./benchmark --quick --terminals lestrix --no-workloads --no-refterm --results "${TMPDIR:-/tmp}/lestrix-test-bench"; fi

echo
say "Summary"
fail=0
for i in "${!NAMES[@]}"; do printf '  %-12s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"; [ "${RESULTS[$i]}" = FAIL ] && fail=1; done
[ "$fail" = 0 ] && say "All tests that ran passed" || say "Some tests failed"
exit $fail
