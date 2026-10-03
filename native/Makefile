# Native build. Needs: gcc, make, pkg-config, libgtk-4-dev, libcurl4-openssl-dev.
CC      ?= gcc
CFLAGS  ?= -O2
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-deprecated-declarations -Wno-missing-field-initializers -fvisibility=hidden -pthread
BUILD   := build

.PHONY: all test perf-gate test-lz test-vt-tsan test-vt test-store test-xfer test-ftp bench app install uninstall clean
all: app

$(BUILD):
	mkdir -p $@

# shared build of the terminal core, used by the differential tests
$(BUILD)/libvt.so: src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=default -o $@ src/vt.c src/lz.c

clean:
	rm -rf $(BUILD)

$(BUILD)/test_vt: tests/test_vt.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_vt.c src/vt.c src/lz.c

$(BUILD)/bench_vt: tests/bench_vt.c src/bench.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -O3 -march=native -std=gnu11 -o $@ tests/bench_vt.c src/bench.c src/vt.c src/lz.c

# terminal core: unit tests under ASan/UBSan, then a differential test against pyte when it is importable
test-vt: $(BUILD)/test_vt $(BUILD)/libvt.so
	$(BUILD)/test_vt
	@PY=$$(for p in ../.venv/bin/python python3; do $$p -c "import pyte" 2>/dev/null && echo $$p && break; done); \
	  if [ -n "$$PY" ]; then $$PY tests/diff_pyte.py 1500; else echo "(pyte not installed: skipping the differential test)"; fi

test: test-vt test-lz test-vt-tsan test-store test-xfer test-ftp perf-gate

bench: $(BUILD)/bench_vt
	$(BUILD)/bench_vt

GTK_CFLAGS := $(shell pkg-config --cflags gtk4)
GTK_LIBS   := $(shell pkg-config --libs gtk4)
$(BUILD)/termdemo: tests/termdemo.c src/term.c src/vt.c src/lz.c src/pty.c src/theme.c | $(BUILD)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ tests/termdemo.c src/term.c src/vt.c src/lz.c src/pty.c src/theme.c $(GTK_LIBS) -lutil -lm

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

APP_SRC := src/bench.c src/app.c src/term.c src/vt.c src/lz.c src/pty.c src/theme.c src/store.c src/json.c src/inventory.c src/xfer.c src/files.c
$(BUILD)/lestrix: $(APP_SRC) $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) $(GTK_CFLAGS) $(CURL_CFLAGS) -o $@ $(APP_SRC) $(GTK_LIBS) $(CURL_LIBS) -lutil -lm

app: $(BUILD)/lestrix

$(BUILD)/test_ftp: tests/test_ftp.c src/xfer.c src/store.c src/json.c src/inventory.c src/xfer.h | $(BUILD)
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) $(CURL_CFLAGS) -fsanitize=address,undefined -g -o $@ tests/test_ftp.c src/xfer.c src/store.c src/json.c src/inventory.c $(GLIB_LIBS) $(CURL_LIBS)

test-ftp: $(BUILD)/test_ftp
	ASAN_OPTIONS=detect_leaks=0 $(BUILD)/test_ftp

PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin
DATADIR := $(PREFIX)/share

install: $(BUILD)/lestrix
	install -d $(BINDIR) $(DATADIR)/applications $(DATADIR)/icons/hicolor/256x256/apps
	install -m 755 $(BUILD)/lestrix $(BINDIR)/lestrix
	install -m 644 data/lestrix.png $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png
	sed 's|@BINDIR@|$(BINDIR)|g' data/lestrix.desktop.in > $(DATADIR)/applications/lestrix.desktop
	-desktop-file-validate $(DATADIR)/applications/lestrix.desktop 2>&1 | sed 's/^/desktop-file-validate: /'
	-update-desktop-database $(DATADIR)/applications 2>/dev/null
	-gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null

uninstall:
	rm -f $(BINDIR)/lestrix $(DATADIR)/applications/lestrix.desktop $(DATADIR)/icons/hicolor/256x256/apps/lestrix.png
	-update-desktop-database $(DATADIR)/applications 2>/dev/null
	-gtk-update-icon-cache -q -t $(DATADIR)/icons/hicolor 2>/dev/null

$(BUILD)/test_lz: tests/test_lz.c src/lz.c src/lz.h | $(BUILD)
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -o $@ tests/test_lz.c src/lz.c

test-lz: $(BUILD)/test_lz
	$(BUILD)/test_lz

# the history worker threads, under ThreadSanitizer
$(BUILD)/test_vt_tsan: tests/test_vt.c src/vt.c src/lz.c src/vt.h src/lz.h | $(BUILD)
	$(CC) -std=gnu11 -O1 -g -pthread -fsanitize=thread -o $@ tests/test_vt.c src/vt.c src/lz.c

test-vt-tsan: $(BUILD)/test_vt_tsan
	$(BUILD)/test_vt_tsan

$(BUILD)/perf_gate: tests/perf_gate.c src/vt.c src/lz.c src/vt.h | $(BUILD)
	$(CC) -O2 -std=gnu11 -pthread -o $@ tests/perf_gate.c src/vt.c src/lz.c

perf-gate: $(BUILD)/perf_gate
	$(BUILD)/perf_gate
