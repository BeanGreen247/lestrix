# Copyright (c) 2026 BeanGreen247
# SPDX-License-Identifier: MIT

CC      ?= gcc
CFLAGS  ?= -O3
GCC_TUNE := $(shell $(CC) -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited -E -x c /dev/null >/dev/null 2>&1 && echo -freorder-blocks-algorithm=simple -fvect-cost-model=unlimited)
CFLAGS  += $(GCC_TUNE)
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-deprecated-declarations -Wno-missing-field-initializers -fvisibility=hidden -pthread
BUILD   := build

.PHONY: bench-file bench-terminals all test test-fast test-shadow test-lxcat test-gui perf-gate test-lz test-vt-tsan test-vt test-store test-xfer test-ftp bench app install uninstall clean
all: app

$(BUILD):
	mkdir -p $@

$(BUILD)/libvt.so: src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=default -o $@ src/vt.c src/lz.c

clean:
	rm -rf $(BUILD)

$(BUILD)/test_vt: tests/test_vt.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_vt.c src/vt.c src/lz.c

$(BUILD)/bench_vt: tests/bench_vt.c src/bench.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -O3 -march=native -std=gnu11 -o $@ tests/bench_vt.c src/bench.c src/vt.c src/lz.c

test-vt: $(BUILD)/test_vt $(BUILD)/libvt.so
	$(BUILD)/test_vt
	@PY=$$(for p in ../.venv/bin/python python3; do $$p -c "import pyte" 2>/dev/null && echo $$p && break; done); \
	  if [ -n "$$PY" ]; then $$PY tests/diff_pyte.py 1500; else echo "(pyte not installed: skipping the differential test)"; fi

test: test-vt test-fast test-shadow test-lxcat test-lz test-vt-tsan test-store test-xfer test-ftp perf-gate

bench: $(BUILD)/bench_vt
	$(BUILD)/bench_vt

GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0 gio-2.0)
GLIB_LIBS   := $(shell pkg-config --libs glib-2.0 gio-2.0)
$(BUILD)/test_store: tests/test_store.c src/store.c src/json.c src/inventory.c src/store.h src/json.h | $(BUILD)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_store.c src/store.c src/json.c src/inventory.c $(GLIB_LIBS)

test-store: $(BUILD)/test_store
	$(BUILD)/test_store

CURL_CFLAGS := $(shell pkg-config --cflags libcurl)
CURL_LIBS   := $(shell pkg-config --libs libcurl)
$(BUILD)/test_xfer: tests/test_xfer.c src/xfer.c src/store.c src/json.c src/inventory.c src/xfer.h | $(BUILD)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) $(CURL_CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_xfer.c src/xfer.c src/store.c src/json.c src/inventory.c $(GLIB_LIBS) $(CURL_LIBS)

test-xfer: $(BUILD)/test_xfer
	$(BUILD)/test_xfer

GUI_CFLAGS := $(shell pkg-config --cflags sdl2 freetype2 fontconfig)
GUI_LIBS   := $(shell pkg-config --libs sdl2 freetype2 fontconfig)
UI_SRC := src/ui/app.c src/ui/dialogs.c src/ui/filespane.c src/ui/jobs.c src/ui/ui.c src/ui/gl.c src/ui/font.c src/ui/atlas.c src/ui/workpool.c \
          src/ui/render.c src/ui/tcore.c src/ui/termview.c src/ui/uitheme.c src/ui/fleetwm.c
CORE_SRC := src/bench.c src/vt.c src/lz.c src/pty.c src/store.c src/json.c src/inventory.c src/xfer.c
APP_SRC := $(UI_SRC) $(CORE_SRC)
$(BUILD)/lestrix: $(APP_SRC) $(wildcard src/*.h src/ui/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(GUI_CFLAGS) $(GLIB_CFLAGS) $(CURL_CFLAGS) -o $@ $(APP_SRC) $(GUI_LIBS) $(GLIB_LIBS) $(CURL_LIBS) -lutil -lm

app: $(BUILD)/lestrix $(BUILD)/lxcat

$(BUILD)/lxcat: tools/lxcat.c | $(BUILD)
	$(CC) -O3 -std=gnu11 -Wall -o $@ $<

$(BUILD)/test_ftp: tests/test_ftp.c src/xfer.c src/store.c src/json.c src/inventory.c src/xfer.h | $(BUILD)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) $(CURL_CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_ftp.c src/xfer.c src/store.c src/json.c src/inventory.c $(GLIB_LIBS) $(CURL_LIBS)

test-ftp: $(BUILD)/test_ftp
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_ftp

PREFIX ?= /usr/local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share

install: $(BUILD)/lestrix $(BUILD)/lxcat
	install -d -m 755 $(BINDIR) $(DATADIR)/applications $(DATADIR)/icons/hicolor/256x256/apps
	install -m 755 $(BUILD)/lestrix $(BINDIR)/lestrix
	install -m 755 $(BUILD)/lxcat $(BINDIR)/lxcat
	install -m 644 data/lestrix.png $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix.desktop.in > $(DATADIR)/applications/lestrix.desktop
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix-lite.desktop.in > $(DATADIR)/applications/lestrix-lite.desktop
	chmod 644 $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop
	-desktop-file-validate $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop 2>&1 | sed 's/^/desktop-file-validate: /'
	-update-desktop-database $(DATADIR)/applications 2>/dev/null
	-chmod 644 $(DATADIR)/applications/mimeinfo.cache 2>/dev/null
	-gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null || true
	-chmod -R a+rX $(DATADIR)/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(BINDIR)/lestrix $(BINDIR)/lxcat $(DATADIR)/applications/lestrix.desktop $(DATADIR)/applications/lestrix-lite.desktop $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png
	-update-desktop-database $(DATADIR)/applications 2>/dev/null
	-gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null || true

$(BUILD)/test_lz: tests/test_lz.c src/lz.c src/lz.h | $(BUILD)
	$(CC) -std=gnu11 -O3 -g -fsanitize=address,undefined -o $@ tests/test_lz.c src/lz.c

test-lz: $(BUILD)/test_lz
	$(BUILD)/test_lz

$(BUILD)/test_vt_tsan: tests/test_vt.c src/vt.c src/lz.c src/vt.h src/lz.h | $(BUILD)
	$(CC) -std=gnu11 -O3 -g -pthread -fsanitize=thread -o $@ tests/test_vt.c src/vt.c src/lz.c

test-vt-tsan: $(BUILD)/test_vt_tsan
	$(BUILD)/test_vt_tsan

$(BUILD)/perf_gate: tests/perf_gate.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -O3 -std=gnu11 -pthread -o $@ tests/perf_gate.c src/vt.c src/lz.c

perf-gate: $(BUILD)/perf_gate
	$(BUILD)/perf_gate

test-gui: $(BUILD)/lestrix
	@set -e; d=$$(mktemp -d); mkdir -p $$d/lestrix; \
	 for s in "w:0.5;s:$$d/a.ppm" "w:0.5;c:28,15;w:0.3;s:$$d/b.ppm" "w:0.5;c:303,56;w:0.4;t:name;k:escape;w:0.2;s:$$d/c.ppm"; do \
	   XDG_CONFIG_HOME=$$d timeout 30 $(BUILD)/lestrix --script "$$s" || { echo "test-gui: failed on $$s"; exit 1; }; \
	 done; \
	 for f in a b c; do test -s $$d/$$f.ppm || { echo "test-gui: no frame $$f"; exit 1; }; done; \
	 rm -rf $$d; echo "test-gui: ok"

$(BUILD)/test_fast_paths: tests/test_fast_paths.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -std=gnu11 -O3 -g -pthread -fsanitize=address,undefined -o $@ tests/test_fast_paths.c src/vt.c src/lz.c

test-fast: $(BUILD)/test_fast_paths
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_fast_paths

$(BUILD)/dump_vt_shadow: tests/dump_vt.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -std=gnu11 -O2 -g -pthread -fsanitize=address,undefined -o $@ tests/dump_vt.c src/vt.c src/lz.c

$(BUILD)/dump_vt_plain: tests/dump_vt.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -std=gnu11 -O2 -g -pthread -fsanitize=address,undefined -DVT_NO_SHADOW -o $@ tests/dump_vt.c src/vt.c src/lz.c

test-shadow: $(BUILD)/dump_vt_shadow $(BUILD)/dump_vt_plain
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/dump_vt_shadow 300 > $(BUILD)/shadow.out
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/dump_vt_plain 300 > $(BUILD)/plain.out
	cmp $(BUILD)/shadow.out $(BUILD)/plain.out && echo "shadow rows: 300 random streams identical"

test-lxcat: $(BUILD)/lxcat
	python3 tests/test_lxcat.py $(BUILD)/lxcat

bench-file: $(BUILD)/gen_random
	$(BUILD)/gen_random /tmp/lestrix-bench-1g.txt 1024
$(BUILD)/gen_random: bench/gen_random.c | $(BUILD)
	$(CC) -O3 -o $@ $<
bench-terminals: $(BUILD)/lestrix $(BUILD)/gen_random
	./benchmark
