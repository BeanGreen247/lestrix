# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

CXX ?= g++
CXXFLAGS ?= -O3
GCC_TUNE := $(shell $(CXX) -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited -E -x c++ /dev/null >/dev/null 2>&1 && echo -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited)
CXXFLAGS  += $(GCC_TUNE)
CXXFLAGS  += -std=gnu++20 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-deprecated-declarations -Wno-missing-field-initializers -fvisibility=hidden -fno-rtti -pthread
BUILD   := build

.PHONY: bench-file bench-terminals all test test-fast test-shadow test-native-cat test-gui perf-gate test-lz test-vt-tsan test-vt test-store test-xfer test-ftp bench app install uninstall clean
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
	@PY=$$(for p in ../.venv/bin/python python3; do $$p -c "import pyte" 2>/dev/null && echo $$p && break; done); \
	  if [ -n "$$PY" ]; then $$PY tests/diff_pyte.py 1500; else echo "(pyte not installed: skipping the differential test)"; fi

test: test-vt test-fast test-shadow test-native-cat test-lz test-vt-tsan test-store test-xfer test-ftp perf-gate

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

GUI_CFLAGS := $(shell pkg-config --cflags sdl2 freetype2 fontconfig)
GUI_LIBS   := $(shell pkg-config --libs sdl2 freetype2 fontconfig)
UI_SRC := src/ui/app.cpp src/ui/dialogs.cpp src/ui/filespane.cpp src/ui/jobs.cpp src/ui/ui.cpp src/ui/gl.cpp src/ui/font.cpp src/ui/atlas.cpp src/ui/workpool.cpp \
          src/ui/render.cpp src/ui/tcore.cpp src/ui/termview.cpp src/ui/uitheme.cpp src/ui/fleetwm.cpp
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
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix.desktop.in > $(DATADIR)/applications/lestrix.desktop
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix-lite.desktop.in > $(DATADIR)/applications/lestrix-lite.desktop
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix-basic.desktop.in > $(DATADIR)/applications/lestrix-basic.desktop
	chmod 644 $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/applications/lestrix-basic.desktop
	-desktop-file-validate $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/applications/lestrix-basic.desktop 2>&1 | sed 's/^/desktop-file-validate: /'
	-update-desktop-database $(DATADIR)/applications 2>/dev/null
	-chmod 644 $(DATADIR)/applications/mimeinfo.cache 2>/dev/null
		-chmod -R a+rX $(DATADIR)/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(BINDIR)/lestrix $(BINDIR)/lxcat $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/applications/lestrix-basic.desktop $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png
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
	@set -e; d=$$(mktemp -d); mkdir -p $$d/lestrix; \
	 for s in "w:0.5;s:$$d/a.ppm" "w:0.5;c:28,15;w:0.3;s:$$d/b.ppm" "w:0.5;c:303,56;w:0.4;t:name;k:escape;w:0.2;s:$$d/c.ppm"; do \
	   XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --script "$$s" || { echo "test-gui: failed on $$s"; exit 1; }; \
	 done; \
	 XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --basic --script "w:0.5;t:echo basic;k:return;w:0.3;s:$$d/d.ppm" || { echo "test-gui: --basic failed"; exit 1; }; \
	 for f in a b c d; do test -s $$d/$$f.ppm || { echo "test-gui: no frame $$f"; exit 1; }; done; \
	 rm -rf $$d; echo "test-gui: ok"

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

bench-file: $(BUILD)/gen_random
	$(BUILD)/gen_random /tmp/lestrix-bench-1g.txt 1024
$(BUILD)/gen_random: bench/gen_random.cpp | $(BUILD)
	$(CXX) -O3 -o $@ $<
bench-terminals: $(BUILD)/lestrix $(BUILD)/gen_random
	./benchmark
