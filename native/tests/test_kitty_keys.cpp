/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* The kitty keyboard protocol encoding (tcore_kitty_encode, pure) and the whole path in a real terminal core: a program turns the
 * protocol on with CSI > flags u, the test presses keys, `cat -v` on a raw tty shows the exact bytes the program received. */

#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../src/ui/tcore.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static double now() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

static std::string enc(int flags, TKey k, uint32_t cp, int mods, int ev = 1) {
    char b[32];
    size_t n = tcore_kitty_encode(flags, k, cp, mods, ev, b);
    return n ? std::string(b, n) : std::string("-");
}

static void test_encoding() {
    const int D = VT_KB_DISAMBIGUATE, E = VT_KB_EVENTS, A = VT_KB_ALL_KEYS;
    CHECK(enc(0, TK_ESCAPE, 0, 0) == "-");                           /* protocol off: legacy everywhere */
    CHECK(enc(D, TK_ESCAPE, 0, 0) == "\x1b[27u");
    CHECK(enc(D, TK_NONE, 'a', TM_CTRL) == "\x1b[97;5u");
    CHECK(enc(D, TK_NONE, 'A', TM_CTRL | TM_SHIFT) == "\x1b[97;6u");
    CHECK(enc(D, TK_NONE, 'i', TM_CTRL) == "\x1b[105;5u");           /* Ctrl+I is no longer Tab */
    CHECK(enc(D, TK_NONE, 'x', TM_ALT) == "\x1b[120;3u");
    CHECK(enc(D, TK_NONE, ' ', TM_CTRL) == "\x1b[32;5u");
    CHECK(enc(D, TK_NONE, 'a', 0) == "-");                           /* plain and shifted text stays text */
    CHECK(enc(D, TK_NONE, 'a', TM_SHIFT) == "-");
    CHECK(enc(D, TK_ENTER, 0, 0) == "-" && enc(D, TK_TAB, 0, 0) == "-" && enc(D, TK_BACKSPACE, 0, 0) == "-");
    CHECK(enc(D, TK_ENTER, 0, TM_SHIFT) == "\x1b[13;2u");
    CHECK(enc(D, TK_TAB, 0, TM_SHIFT) == "\x1b[9;2u");
    CHECK(enc(D, TK_BACKSPACE, 0, TM_CTRL) == "\x1b[127;5u");
    CHECK(enc(D, TK_UP, 0, 0) == "-");                               /* unmodified arrows: the legacy form, app-cursor aware */
    CHECK(enc(D, TK_UP, 0, TM_CTRL) == "\x1b[1;5A");
    CHECK(enc(D, TK_END, 0, TM_META) == "\x1b[1;9F");
    CHECK(enc(D, TK_DELETE, 0, TM_SHIFT | TM_ALT) == "\x1b[3;4~");
    CHECK(enc(D, TK_F5, 0, TM_CTRL) == "\x1b[15;5~");
    CHECK(enc(D, TK_F1, 0, TM_ALT) == "\x1b[1;3P");
    CHECK(enc(D | A, TK_NONE, 'a', 0) == "\x1b[97u");                /* every key an escape code */
    CHECK(enc(D | A, TK_NONE, 'a', TM_SHIFT) == "\x1b[97;2u");
    CHECK(enc(D | A, TK_ENTER, 0, 0) == "\x1b[13u" && enc(D | A, TK_TAB, 0, 0) == "\x1b[9u" && enc(D | A, TK_BACKSPACE, 0, 0) == "\x1b[127u");
    CHECK(enc(D | E, TK_NONE, 'a', TM_CTRL, 3) == "\x1b[97;5:3u");   /* event types */
    CHECK(enc(D | E, TK_NONE, 'a', TM_CTRL, 2) == "\x1b[97;5:2u");
    CHECK(enc(D | E, TK_ESCAPE, 0, 0, 3) == "\x1b[27;1:3u");
    CHECK(enc(D | E, TK_UP, 0, 0, 3) == "\x1b[1;1:3A");
    CHECK(enc(D | E, TK_ENTER, 0, 0, 3) == "-");                     /* no Enter release without every-key mode */
    CHECK(enc(D | E | A, TK_ENTER, 0, 0, 3) == "\x1b[13;1:3u");
    CHECK(enc(D | E, TK_NONE, 'a', 0, 3) == "-");
    CHECK(enc(D | E | A, TK_NONE, 'a', 0, 3) == "\x1b[97;1:3u");
}

static bool wait_for(TermCore *t, const char *text, double secs) {
    double end = now() + secs;
    while (now() < end) {
        tcore_pump(t);
        if (tcore_screen_contains(t, text)) return true;
        usleep(2000);
    }
    return false;
}

static bool settle(TermCore *t, double secs) {
    double end = now() + secs;
    while (now() < end) { tcore_pump(t); usleep(2000); }
    return true;
}

/* Starts a raw tty program that enables the protocol with `flags` (a CSI > ... u string) and echoes what it receives through cat -v. */
static TermCore *start(const char *push) {
    static std::string cmd;
    cmd = std::string("stty raw -echo; printf 'READY'; printf '") + push + "'; exec cat -v";
    char *argv[] = {const_cast<char *>("/bin/sh"), const_cast<char *>("-c"), const_cast<char *>(cmd.c_str()), nullptr};
    TermCore *t = tcore_new(argv, "/", 80, 24, nullptr, nullptr, false);
    CHECK(t && wait_for(t, "READY", 5.0));
    settle(t, 0.15);
    return t;
}

static void stop(TermCore *t) { tcore_close(t); tcore_free(t); }

static void test_end_to_end() {
    TermCore *t = start("\\033[>1u");
    CHECK(tcore_kitty_flags(t) == 1);
    tcore_key(t, TK_ESCAPE, 0, 0);
    CHECK(wait_for(t, "^[[27u", 3.0));
    tcore_key(t, TK_NONE, 'a', TM_CTRL);
    CHECK(wait_for(t, "^[[97;5u", 3.0));
    tcore_key(t, TK_UP, 0, TM_CTRL | TM_ALT);
    CHECK(wait_for(t, "^[[1;7A", 3.0));
    tcore_text(t, "q");                                   /* plain text still goes through */
    CHECK(wait_for(t, "q", 3.0));
    stop(t);

    t = start("\\033[>11u");                            /* disambiguate + event types + all keys */
    CHECK(tcore_kitty_flags(t) == 11);
    tcore_key_ev(t, TK_NONE, 'z', 0, 1);
    CHECK(wait_for(t, "^[[122u", 3.0));
    tcore_text(t, "z");                                   /* the text of that key press must not be sent a second time */
    tcore_key_ev(t, TK_NONE, 'z', 0, 3);
    CHECK(wait_for(t, "^[[122;1:3u", 3.0));
    CHECK(!tcore_screen_contains(t, "^[[122u^[[122u"));
    CHECK(!tcore_screen_contains(t, "uz"));
    stop(t);

    t = start("");                                         /* protocol off: Esc and Ctrl+A are the old bytes */
    tcore_key(t, TK_ESCAPE, 0, 0);
    tcore_key(t, TK_NONE, 'a', TM_CTRL);
    CHECK(wait_for(t, "^[^A", 3.0));
    tcore_key_ev(t, TK_NONE, 'a', TM_CTRL, 3);            /* releases are never sent without the flag */
    settle(t, 0.15);
    CHECK(!tcore_screen_contains(t, "[97;5:3u"));
    stop(t);
}

int main() {
    test_encoding();
    test_end_to_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
