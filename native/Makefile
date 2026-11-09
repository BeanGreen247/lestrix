# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

CXX ?= g++
CXXFLAGS ?= -O3
GCC_TUNE := $(shell $(CXX) -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited -E -x c++ /dev/null >/dev/null 2>&1 && echo -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited)
CXXFLAGS  += $(GCC_TUNE)
CXXFLAGS  += -std=gnu++20 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-deprecated-declarations -Wno-missing-field-initializers -fvisibility=hidden -fno-rtti -pthread
BUILD   := build

.PHONY: test-fuzz test-soft test-font bench-file bench-terminals all test test-fast test-shadow test-native-cat test-kitty-keys test-gfx test-scan test-gui perf-gate test-lz test-vt-tsan test-vt test-store test-xfer test-ftp bench app install uninstall clean
all: app

$(BUILD):
	mkdir -p $@

$(BUILD)/libvt.so: src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fPIC -shared -fvisibility=default -o $@ src/vt.cpp src/lz.cpp

clean:
	rm -rf $(BUILD)

$(BUILD)/test_vt: tests/test_vt.cpp src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_vt.cpp src/vt.cpp src/lz.cpp

$(BUILD)/bench_vt: tests/bench_vt.cpp src/bench.cpp src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) -O3 -march=native -std=gnu++20 -o $@ tests/bench_vt.cpp src/bench.cpp src/vt.cpp src/lz.cpp

test-vt: $(BUILD)/test_vt $(BUILD)/libvt.so
	$(BUILD)/test_vt
	@PY=$$(for p in ../.venv/bin/python perf-python python3; do $$p -c "import pyte" 2>/dev/null && echo $$p && break; done); \
	  if [ -n "$$PY" ]; then $$PY tests/diff_pyte.py 1500; else echo "(pyte not installed: skipping the differential test)"; fi

test: test-soft test-font test-vt test-fast test-shadow test-native-cat test-kitty-keys test-gfx test-scan test-fuzz test-lz test-vt-tsan test-store test-xfer test-ftp perf-gate

bench: $(BUILD)/bench_vt
	$(BUILD)/bench_vt

$(BUILD)/test_store: tests/test_store.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp src/store.h src/json.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_store.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp

test-store: $(BUILD)/test_store
	$(BUILD)/test_store

CURL_CFLAGS := $(shell pkg-config --cflags libcurl)
CURL_LIBS   := $(shell pkg-config --libs libcurl)
$(BUILD)/test_xfer: tests/test_xfer.cpp src/xfer.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp src/xfer.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CURL_CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_xfer.cpp src/xfer.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp $(CURL_LIBS)

test-xfer: $(BUILD)/test_xfer
	$(BUILD)/test_xfer

$(BUILD)/test_soft: tests/test_soft.cpp src/ui/soft.cpp src/ui/soft.h src/ui/render.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_soft.cpp src/ui/soft.cpp -lm

test-soft: $(BUILD)/test_soft
	$(BUILD)/test_soft

$(BUILD)/test_font: tests/test_font.cpp src/ui/font.cpp src/ui/font.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(shell pkg-config --cflags freetype2 fontconfig harfbuzz) -fsanitize=address,undefined -g -o $@ tests/test_font.cpp src/ui/font.cpp $(shell pkg-config --libs freetype2 fontconfig harfbuzz) -lm

test-font: $(BUILD)/test_font
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_font

GUI_CFLAGS := $(shell pkg-config --cflags sdl2 freetype2 fontconfig harfbuzz)
GUI_LIBS   := $(shell pkg-config --libs sdl2 freetype2 fontconfig harfbuzz)
UI_SRC := src/ui/app.cpp src/ui/dialogs.cpp src/ui/filespane.cpp src/ui/jobs.cpp src/ui/ui.cpp src/ui/gl.cpp src/ui/font.cpp src/ui/atlas.cpp src/ui/workpool.cpp \
          src/ui/render.cpp src/ui/extras.cpp src/ui/soft.cpp src/ui/scan.cpp src/ui/tcore.cpp src/ui/termview.cpp src/ui/uitheme.cpp src/ui/fleetwm.cpp
CORE_SRC := src/bench.cpp src/vt.cpp src/lz.cpp src/pty.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp src/xfer.cpp
APP_SRC := $(UI_SRC) $(CORE_SRC)
$(BUILD)/lestrix: $(APP_SRC) $(wildcard src/*.h src/ui/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(GUI_CFLAGS) $(CURL_CFLAGS) -o $@ $(APP_SRC) $(GUI_LIBS) $(CURL_LIBS) -lutil -lm

app: $(BUILD)/lestrix

$(BUILD)/test_ftp: tests/test_ftp.cpp src/xfer.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp src/xfer.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CURL_CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_ftp.cpp src/xfer.cpp src/store.cpp src/json.cpp src/inventory.cpp src/util.cpp $(CURL_LIBS)

test-ftp: $(BUILD)/test_ftp
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_ftp

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share

install: $(BUILD)/lestrix
	install -d -m 755 $(BINDIR) $(DATADIR)/applications $(DATADIR)/icons/hicolor/256x256/apps
	install -m 755 $(BUILD)/lestrix $(BINDIR)/lestrix
	install -m 644 data/lestrix.png $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png
	for sz in 16 24 32 48 64 128 512; do install -d -m 755 $(DATADIR)/icons/hicolor/$${sz}x$${sz}/apps; install -m 644 data/icons/lestrix-$$sz.png $(DATADIR)/icons/hicolor/$${sz}x$${sz}/apps/lestrix.png; done
	install -d -m 755 $(DATADIR)/icons/hicolor/scalable/apps
	install -m 644 data/lestrix.svg $(DATADIR)/icons/hicolor/scalable/apps/lestrix.svg
	install -d -m 755 $(DATADIR)/lestrix/shell-integration
	install -m 644 data/shell-integration/* $(DATADIR)/lestrix/shell-integration/
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix.desktop.in > $(DATADIR)/applications/lestrix.desktop
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix-lite.desktop.in > $(DATADIR)/applications/lestrix-lite.desktop
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix-basic.desktop.in > $(DATADIR)/applications/lestrix-basic.desktop
	chmod 644 $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/applications/lestrix-basic.desktop
	-desktop-file-validate $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/applications/lestrix-basic.desktop 2>&1 | sed 's/^/desktop-file-validate: /'
	-update-desktop-database $(DATADIR)/applications 2>/dev/null
	-chmod 644 $(DATADIR)/applications/mimeinfo.cache 2>/dev/null
		-chmod -R a+rX $(DATADIR)/icons/hicolor 2>/dev/null

uninstall:
	rm -rf $(DATADIR)/lestrix/shell-integration; rmdir $(DATADIR)/lestrix 2>/dev/null || true
	rm -f $(BINDIR)/lestrix $(BINDIR)/lxcat $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/applications/lestrix-basic.desktop $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png $(DATADIR)/icons/hicolor/scalable/apps/lestrix.svg
	-update-desktop-database $(DATADIR)/applications 2>/dev/null

$(BUILD)/test_lz: tests/test_lz.cpp src/lz.cpp src/lz.h | $(BUILD)
	$(CXX) -std=gnu++20 -O3 -g -fsanitize=address,undefined -o $@ tests/test_lz.cpp src/lz.cpp

test-lz: $(BUILD)/test_lz
	$(BUILD)/test_lz

$(BUILD)/test_vt_tsan: tests/test_vt.cpp src/vt.cpp src/lz.cpp src/vt.h src/lz.h | $(BUILD)
	$(CXX) -std=gnu++20 -O3 -g -pthread -fsanitize=thread -o $@ tests/test_vt.cpp src/vt.cpp src/lz.cpp

test-vt-tsan: $(BUILD)/test_vt_tsan
	$(BUILD)/test_vt_tsan

$(BUILD)/perf_gate: tests/perf_gate.cpp src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) -O3 -std=gnu++20 -pthread -o $@ tests/perf_gate.cpp src/vt.cpp src/lz.cpp

perf-gate: $(BUILD)/perf_gate
	$(BUILD)/perf_gate

test-gui: $(BUILD)/lestrix
	@set -e; for mode in cpu gpu; do export LESTRIX_RENDER=$$mode LESTRIX_SOFT_VERIFY=fatal; d=$$(mktemp -d); mkdir -p $$d/lestrix; \
	 for s in "w:0.5;s:$$d/a.ppm" "w:0.5;c:28,15;w:0.3;s:$$d/b.ppm" "w:0.5;c:303,56;w:0.4;t:name;k:escape;w:0.2;s:$$d/c.ppm" "w:0.5;d:800,600;w:0.6;c:900,46;w:0.3;d:900,46;w:0.6;s:$$d/e.ppm" "w:0.5;k:0;w:0.8;k:ctrl+shift+e;w:0.6;k:ctrl+shift+r;w:0.6;k:ctrl+shift+f;w:0.3;t:abc;w:0.5;k:escape;k:ctrl+shift+left;w:0.3;p:885,300,1000,300;w:0.3;t:exit;k:return;w:0.6;s:$$d/f.ppm"; do \
	   XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --script "$$s" || { echo "test-gui: failed on $$s"; exit 1; }; \
	 done; \
	 if command -v python3 >/dev/null 2>&1; then python3 tests/gfx_demo.py gen $$d; \
	   XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --script "w:0.5;k:0;w:0.8;t:cd $$d;k:return;w:0.3;t:clear;k:return;w:0.3;t:cat six.txt kitty.txt;k:return;w:1.0;s:$$d/g.ppm" || { echo "test-gui: image script failed"; exit 1; }; \
	   python3 tests/gfx_demo.py check $$d/g.ppm || { echo "test-gui ($$mode renderer): images missing"; exit 1; }; fi; \
	 XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --basic --script "w:0.5;t:echo basic;k:return;w:0.3;s:$$d/d.ppm" || { echo "test-gui: --basic failed"; exit 1; }; \
	 for f in a b c d e f; do test -s $$d/$$f.ppm || { echo "test-gui: no frame $$f"; exit 1; }; done; \
	 if [ $$mode = gpu ] && command -v python3 >/dev/null 2>&1; then \
	   LESTRIX_GL=es2 XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --script "w:0.5;k:0;w:0.8;t:cd $$d;k:return;w:0.3;t:clear;k:return;w:0.3;t:cat six.txt kitty.txt;k:return;w:1.0;s:$$d/h.ppm" || { echo "test-gui: ES2 image script failed"; exit 1; }; \
	   python3 tests/gfx_demo.py check $$d/h.ppm || { echo "test-gui (OpenGL ES 2): images missing"; exit 1; }; fi; \
	 if command -v kitten >/dev/null 2>&1 && python3 -c 'import PIL' 2>/dev/null; then \
	   python3 -c "from PIL import Image; Image.new('RGBA',(40,40),(255,0,255,255)).save('$$d/m.png')"; echo "kitten icat m.png" > $$d/run.sh; \
	   XDG_CONFIG_HOME=$$d timeout 40 $(BUILD)/lestrix --script "w:0.5;k:0;w:0.8;t:cd $$d;k:return;w:0.3;t:clear;k:return;w:0.3;t:sh run.sh;k:return;w:4.0;s:$$d/k.ppm" || { echo "test-gui: icat script failed"; exit 1; }; \
	   python3 tests/gfx_demo.py magenta $$d/k.ppm 1600 || { echo "test-gui ($$mode renderer): kitten icat image missing"; exit 1; }; fi; \
	 mkdir -p $$d/bin; printf '#!/bin/sh\necho "$$1" >> %s/opened.txt\n' $$d > $$d/bin/xdg-open; chmod +x $$d/bin/xdg-open; echo "echo https://example.com/lestrix-test" > $$d/link.sh; \
	 PATH=$$d/bin:$$PATH XDG_CONFIG_HOME=$$d timeout 40 $(BUILD)/lestrix --script "w:0.5;k:0;w:0.8;t:cd $$d;k:return;w:0.3;t:clear;k:return;w:0.3;t:sh link.sh;k:return;w:1.0;c:480,102;w:0.5;C:480,102;w:1.0" || { echo "test-gui: link script failed"; exit 1; }; \
	 test "$$(cat $$d/opened.txt 2>/dev/null)" = "https://example.com/lestrix-test" || { echo "test-gui ($$mode renderer): Ctrl+click did not open exactly the link once"; exit 1; }; \
	 echo "python3 $(CURDIR)/tests/kitty_kbd_probe.py $$d/keys.txt" > $$d/kk.sh; \
	 XDG_CONFIG_HOME=$$d timeout 40 $(BUILD)/lestrix --script "w:0.5;k:0;w:0.8;t:sh $$d/kk.sh;k:return;w:1.0;k:escape;k:ctrl+a;k:return;k:tab;k:alt+b;k:up;k:shift+up;w:4;s:$$d/kk.ppm" || { echo "test-gui: keyboard script failed"; exit 1; }; \
	 test "$$(cat $$d/keys.txt 2>/dev/null)" = "b'\\x1b[27u\\x1b[97;5u\\r\\t\\x1b[98;3u\\x1b[A\\x1b[1;2A'" || { echo "test-gui ($$mode renderer): kitty keyboard bytes wrong: $$(cat $$d/keys.txt 2>/dev/null)"; exit 1; }; \
	 if fc-list 2>/dev/null | grep -qi 'fira code' && python3 -c 'import PIL' 2>/dev/null; then \
	   for k in on off; do mkdir -p $$d/lig-$$k/lestrix; printf '[ui]\nfont_family=Fira Code\nligatures=%s\n' $$( [ $$k = on ] && echo true || echo false ) > $$d/lig-$$k/lestrix/settings.ini; \
	     XDG_CONFIG_HOME=$$d/lig-$$k timeout 30 $(BUILD)/lestrix --script "w:0.5;k:0;w:0.8;t:clear;k:return;w:0.3;t:a -> b => c != d == e <= f >= g;w:0.8;s:$$d/lig-$$k.ppm" || { echo "test-gui: ligature script failed"; exit 1; }; done; \
	   python3 tests/lig_check.py $$d/lig-on.ppm $$d/lig-off.ppm || { echo "test-gui ($$mode renderer): ligatures did not change the text"; exit 1; }; fi; \
	 rm -rf $$d; echo "test-gui ($$mode renderer): ok"; done

$(BUILD)/fuzz_vt: tests/fuzz_vt.cpp src/vgfx.h src/vpng.h src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -o $@ tests/fuzz_vt.cpp src/vt.cpp src/lz.cpp

# Mutation fuzzing of the untrusted-input parsers (Sixel, kitty graphics, PNG/zlib, OSC, kitty keyboard); FUZZ_ITER=1000000 for a long run
test-fuzz: $(BUILD)/fuzz_vt
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/fuzz_vt $${FUZZ_ITER:-100000} 1

$(BUILD)/test_gfx: tests/test_gfx.cpp src/vgfx.h src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_gfx.cpp src/vt.cpp src/lz.cpp

test-gfx: $(BUILD)/test_gfx
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_gfx

$(BUILD)/test_fast_paths: tests/test_fast_paths.cpp src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) -std=gnu++20 -O3 -g -pthread -fsanitize=address,undefined -o $@ tests/test_fast_paths.cpp src/vt.cpp src/lz.cpp

test-fast: $(BUILD)/test_fast_paths
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_fast_paths

$(BUILD)/dump_vt_shadow: tests/dump_vt.cpp src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) -std=gnu++20 -O2 -g -pthread -fsanitize=address,undefined -o $@ tests/dump_vt.cpp src/vt.cpp src/lz.cpp

$(BUILD)/dump_vt_plain: tests/dump_vt.cpp src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) -std=gnu++20 -O2 -g -pthread -fsanitize=address,undefined -DVT_NO_SHADOW -o $@ tests/dump_vt.cpp src/vt.cpp src/lz.cpp

test-shadow: $(BUILD)/dump_vt_shadow $(BUILD)/dump_vt_plain
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/dump_vt_shadow 300 > $(BUILD)/shadow.out
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/dump_vt_plain 300 > $(BUILD)/plain.out
	cmp $(BUILD)/shadow.out $(BUILD)/plain.out && echo "shadow rows: 300 random streams identical"

$(BUILD)/test_native_cat: tests/test_native_cat.cpp src/ui/tcore.cpp src/vt.cpp src/lz.cpp src/pty.cpp src/util.cpp src/ui/tcore.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -g -fsanitize=address,undefined -o $@ tests/test_native_cat.cpp src/ui/tcore.cpp src/vt.cpp src/lz.cpp src/pty.cpp src/util.cpp -lutil

test-native-cat: $(BUILD)/test_native_cat
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_native_cat

$(BUILD)/test_kitty_keys: tests/test_kitty_keys.cpp src/ui/tcore.cpp src/vt.cpp src/lz.cpp src/pty.cpp src/util.cpp src/ui/tcore.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -g -fsanitize=address,undefined -o $@ tests/test_kitty_keys.cpp src/ui/tcore.cpp src/vt.cpp src/lz.cpp src/pty.cpp src/util.cpp -lutil

$(BUILD)/test_scan: tests/test_scan.cpp src/ui/scan.cpp src/ui/scan.h src/vt.cpp src/lz.cpp src/vt.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -g -fsanitize=address,undefined -o $@ tests/test_scan.cpp src/ui/scan.cpp src/vt.cpp src/lz.cpp

test-scan: $(BUILD)/test_scan
	$(BUILD)/test_scan

test-kitty-keys: $(BUILD)/test_kitty_keys
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_kitty_keys

bench-file: $(BUILD)/gen_random
	$(BUILD)/gen_random /tmp/lestrix-bench-1g.txt 1024
$(BUILD)/gen_random: bench/gen_random.cpp | $(BUILD)
	$(CXX) -O3 -o $@ $<
bench-terminals: $(BUILD)/lestrix $(BUILD)/gen_random
	./benchmark
