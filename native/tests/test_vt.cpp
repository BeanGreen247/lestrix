/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "../src/vt.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <string>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void feed(Vt *t, const char *s) { vt_feed(t, (const uint8_t *)s, strlen(s)); }

static void row_text(Vt *t, int idx, char *out, size_t cap) {
    int len = 0;
    VtCell *l = vt_line(t, idx, &len);
    size_t o = 0;
    for (int x = 0; l && x < vt_cols(t) && o + 4 < cap; x++) {
        if (x >= len) { out[o++] = ' '; continue; }
        if (VT_CELL_FLAGS(l[x]) & VT_F_TAIL) continue;
        uint32_t c = VT_CELL_CH(l[x]) ? VT_CELL_CH(l[x]) : ' ';
        if (c < 0x80) out[o++] = (char)c;
        else out[o++] = '?';
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}
#define ROW(t, i, expect) do { char b[512]; row_text(t, i, b, sizeof b); checks++; if (strcmp(b, expect)) { failures++; printf("FAIL %s:%d: row %d is \"%s\", want \"%s\"\n", __FILE__, __LINE__, i, b, expect); } } while (0)

static char replies[256];
static void on_write(const uint8_t *d, size_t n, void *u) { (void)u; strncat(replies, (const char *)d, n < 100 ? n : 100); }
static char last_title[256], last_cwd[256];
static int bells;
static void on_event(VtEvent e, const char *s, void *u) {
    (void)u;
    if (e == VT_EV_TITLE) snprintf(last_title, sizeof last_title, "%s", s);
    else if (e == VT_EV_CWD) snprintf(last_cwd, sizeof last_cwd, "%s", s);
    else bells++;
}

static void test_basic_and_wrap(void) {
    Vt *t = vt_new(10, 4, 50);
    feed(t, "hello\r\nworld");
    ROW(t, 0, "hello"); ROW(t, 1, "world");
    CHECK(vt_cursor_x(t) == 5 && vt_cursor_y(t) == 1);
    feed(t, "0123456789AB");
    ROW(t, 1, "world01234"); ROW(t, 2, "56789AB");
    vt_free(t);
}

static void test_scrollback_ring(void) {
    Vt *t = vt_new(8, 3, 5);
    for (int i = 0; i < 12; i++) { char b[16]; snprintf(b, sizeof b, "L%d\r\n", i); feed(t, b); }
    ROW(t, 0, "L10"); ROW(t, 1, "L11"); ROW(t, 2, "");
    CHECK(vt_history_count(t) == 5);
    ROW(t, -1, "L9"); ROW(t, -5, "L5");
    CHECK(vt_line(t, -6, NULL) == NULL);
    vt_free(t);
}

static void test_insert_delete_lines(void) {
    Vt *t = vt_new(6, 5, 20);
    feed(t, "a\r\nb\r\nc\r\nd\r\ne");
    feed(t, "\x1b[2;1H\x1b[L");
    ROW(t, 0, "a"); ROW(t, 1, ""); ROW(t, 2, "b"); ROW(t, 3, "c"); ROW(t, 4, "d");
    CHECK(vt_history_count(t) == 0);
    feed(t, "\x1b[M");
    ROW(t, 1, "b"); ROW(t, 2, "c"); ROW(t, 3, "d"); ROW(t, 4, "");
    vt_free(t);
}

static void test_edit_chars_at_pending_wrap(void) {
    Vt *t = vt_new(5, 2, 0);
    feed(t, "abcde");
    feed(t, "\x1b[P");
    ROW(t, 0, "abcd");
    feed(t, "\x1b[1;2H\x1b[2@");
    ROW(t, 0, "a  bc");
    feed(t, "\x1b[1;1H\x1b[3X");
    ROW(t, 0, "   bc");
    feed(t, "\x1b[K");
    ROW(t, 0, "");
    vt_free(t);
}

static void test_scroll_region(void) {
    Vt *t = vt_new(6, 5, 20);
    feed(t, "1\r\n2\r\n3\r\n4\r\n5");
    feed(t, "\x1b[2;4r");
    CHECK(vt_cursor_x(t) == 0 && vt_cursor_y(t) == 0);
    feed(t, "\x1b[4;1H\n");
    ROW(t, 0, "1"); ROW(t, 1, "3"); ROW(t, 2, "4"); ROW(t, 3, ""); ROW(t, 4, "5");
    CHECK(vt_history_count(t) == 0);
    feed(t, "\x1b[2;1H\x1bM");
    ROW(t, 1, ""); ROW(t, 2, "3"); ROW(t, 3, "4");
    vt_free(t);
}

static void test_scroll_window_slide(void) {
    /* full-screen scrolls slide a window over the row order; run past several screenfuls with partial regions in between */
    Vt *t = vt_new(6, 5, 50);
    char ln[16];
    int n = 0;
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 13; i++) { snprintf(ln, sizeof ln, "L%d\r\n", n++); feed(t, ln); }
        snprintf(ln, sizeof ln, "L%d", n++); feed(t, ln);
        /* last five lines are n-5 .. n-1 */
        for (int r = 0; r < 5; r++) { char want[16]; snprintf(want, sizeof want, "L%d", n - 5 + r); ROW(t, r, want); }
        feed(t, "\x1b[2;5r\x1b[5;1H\n\x1b[r");   /* region one row shorter than the screen, top > 0 */
        { char a[16], b[16], c[16]; snprintf(a, sizeof a, "L%d", n - 5); snprintf(b, sizeof b, "L%d", n - 3); snprintf(c, sizeof c, "L%d", n - 2);
          ROW(t, 0, a); ROW(t, 1, b); ROW(t, 2, c); ROW(t, 4, ""); }
        feed(t, "\x1b[1;4r\x1b[4;1H\n\x1b[r");   /* and one ending above the last row */
        { char a[16], c[16]; snprintf(a, sizeof a, "L%d", n - 3); snprintf(c, sizeof c, "L%d", n - 2);
          ROW(t, 0, a); ROW(t, 1, c); ROW(t, 3, ""); }
        feed(t, "\x1b[5;1H\r\n");
    }
    vt_free(t);
}

static void test_save_restore(void) {
    Vt *t = vt_new(10, 4, 0);
    feed(t, "\x1b[2;5H\x1b[31m\x1b" "7\x1b[H\x1b[0m\x1b" "8");
    CHECK(vt_cursor_x(t) == 4 && vt_cursor_y(t) == 1);
    feed(t, "X");
    VtCell *l = vt_line(t, 1, NULL);
    CHECK(VT_CELL_CH(l[4]) == 'X' && vt_style(t, VT_CELL_STYLE(l[4]))->fg == VT_COLOR_IDX(1));
    feed(t, "\x1b" "8\x1b" "8");
    CHECK(vt_cursor_x(t) == 4 && vt_cursor_y(t) == 1);
    vt_free(t);
}

static void test_alt_screen(void) {
    Vt *t = vt_new(10, 3, 10);
    feed(t, "main1\r\nmain2");
    feed(t, "\x1b[?1049h");
    CHECK(vt_modes(t) & VT_M_ALT_SCREEN);
    ROW(t, 0, ""); 
    feed(t, "\x1b[Halt");
    ROW(t, 0, "alt");
    CHECK(vt_history_count(t) == 0);
    feed(t, "\x1b[?1049l");
    CHECK(!(vt_modes(t) & VT_M_ALT_SCREEN));
    ROW(t, 0, "main1"); ROW(t, 1, "main2");
    CHECK(vt_cursor_x(t) == 5 && vt_cursor_y(t) == 1);
    vt_free(t);
}

static void test_colours(void) {
    Vt *t = vt_new(20, 2, 0);
    feed(t, "\x1b[38;2;1;2;3mA\x1b[48;5;200mB\x1b[38:2::9:8:7mC\x1b[0m\x1b[91;102mD");
    VtCell *l = vt_line(t, 0, NULL);
#define ST(i) vt_style(t, VT_CELL_STYLE(l[i]))
    CHECK(ST(0)->fg == VT_COLOR_RGB(1, 2, 3));
    CHECK(ST(1)->bg == VT_COLOR_IDX(200) && ST(1)->fg == VT_COLOR_RGB(1, 2, 3));
    CHECK(ST(2)->fg == VT_COLOR_RGB(9, 8, 7));
    CHECK(ST(3)->fg == VT_COLOR_IDX(9) && ST(3)->bg == VT_COLOR_IDX(10));
#undef ST
    vt_free(t);
}

static void test_wide_and_combining(void) {
    Vt *t = vt_new(10, 2, 0);
    feed(t, "\xe6\xbc\xa2" "a" "e\xcc\x81");
    VtCell *l = vt_line(t, 0, NULL);
    CHECK((VT_CELL_FLAGS(l[0]) & VT_F_WIDE) && VT_CELL_CH(l[0]) == 0x6f22 && (VT_CELL_FLAGS(l[1]) & VT_F_TAIL));
    CHECK(VT_CELL_CH(l[2]) == 'a' && VT_CELL_CH(l[3]) == 'e' && vt_comb_char(t, VT_CELL_COMB(l[3])) == 0x301);
    CHECK(vt_cursor_x(t) == 4);
    feed(t, "\r\n12345678" "9" "e\xcc\x81");
    CHECK(vt_cursor_y(t) == 1 && vt_cursor_x(t) == 10);
    vt_free(t);
}

static void test_graphics_charset(void) {
    Vt *t = vt_new(10, 2, 0);
    feed(t, "\x1b(0lqk\x1b(Bq");
    VtCell *l = vt_line(t, 0, NULL);
    CHECK(VT_CELL_CH(l[0]) == 0x250C && VT_CELL_CH(l[1]) == 0x2500 && VT_CELL_CH(l[2]) == 0x2510 && VT_CELL_CH(l[3]) == 'q');
    vt_free(t);
}

static void test_replies_and_events(void) {
    Vt *t = vt_new(10, 4, 0);
    vt_set_callbacks(t, on_write, on_event, NULL, NULL);
    replies[0] = 0;
    feed(t, "\x1b[3;4H\x1b[6n");
    CHECK(strcmp(replies, "\x1b[3;4R") == 0);
    replies[0] = 0;
    feed(t, "\x1b[c");
    CHECK(strstr(replies, "\x1b[?62") == replies);
    feed(t, "\x1b]0;my title\x07\x1b]7;file://host/home/a%20b\x1b\\\a");
    CHECK(strcmp(last_title, "my title") == 0);
    CHECK(strcmp(last_cwd, "/home/a b") == 0);
    CHECK(bells == 1);
    vt_free(t);
}

static void test_modes(void) {
    Vt *t = vt_new(10, 4, 0);
    feed(t, "\x1b[?2004h\x1b[?1000h\x1b[?1006h\x1b[?1h\x1b[?25l");
    uint32_t m = vt_modes(t);
    CHECK((m & VT_M_BRACKETED_PASTE) && (m & VT_M_MOUSE_BTN) && (m & VT_M_MOUSE_SGR) && (m & VT_M_APP_CURSOR));
    CHECK(!(m & VT_M_CURSOR_VISIBLE));
    feed(t, "\x1b[?1002h");
    CHECK((vt_modes(t) & VT_M_MOUSE_DRAG) && !(vt_modes(t) & VT_M_MOUSE_BTN));
    vt_free(t);
}

static void test_resize(void) {
    Vt *t = vt_new(10, 4, 20);
    feed(t, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
    vt_resize(t, 6, 3);
    ROW(t, 2, "five");
    CHECK(vt_cursor_y(t) == 2);
    CHECK(vt_history_count(t) >= 2);
    vt_resize(t, 12, 6);
    CHECK(vt_cols(t) == 12 && vt_rows(t) == 6);
    feed(t, "\r\nsix");
    vt_free(t);
}

static void test_utf8_split_and_invalid(void) {
    Vt *t = vt_new(10, 2, 0);
    vt_feed(t, (const uint8_t *)"\xe6\xbc", 2);
    vt_feed(t, (const uint8_t *)"\xa2z", 2);
    VtCell *l = vt_line(t, 0, NULL);
    CHECK(VT_CELL_CH(l[0]) == 0x6f22 && VT_CELL_CH(l[2]) == 'z');
    feed(t, "\xff!");
    CHECK(VT_CELL_CH(l[3]) == 0xFFFD && VT_CELL_CH(l[4]) == '!');
    vt_free(t);
}

static void test_dirty_and_cache(void) {
    Vt *t = vt_new(10, 3, 0);
    for (int y = 0; y < 3; y++) vt_line_meta(t, y)->dirty = 0;
    feed(t, "x");
    CHECK(vt_line_meta(t, 0)->dirty == 1 && vt_line_meta(t, 1)->dirty == 0);
    vt_free(t);
}

static void test_compact_history_and_styles(void) {
    Vt *t = vt_new(200, 10, 1000);
    for (int i = 0; i < 1500; i++) feed(t, "short line of about forty characters ....\r\n");
    CHECK(vt_history_count(t) == 1000);
    int len = 0;
    VtCell *h = vt_line(t, -1, &len);
    CHECK(h && len == 41);
    size_t used = vt_memory_used(t);
    CHECK(used < 1000 * (41 * sizeof(VtCell) + 64) + 200 * 1024);
    feed(t, "\x1b[31mA\x1b[0m\x1b[31mB\x1b[0mC");
    VtCell *l = vt_line(t, vt_cursor_y(t), NULL);
    CHECK(VT_CELL_STYLE(l[0]) == VT_CELL_STYLE(l[1]) && VT_CELL_STYLE(l[1]) != VT_CELL_STYLE(l[2]));
    CHECK(VT_CELL_STYLE(l[2]) == 0);
    feed(t, "\r\n\x1b[44m \x1b[0m");
    for (int i = 0; i < 15; i++) feed(t, "\r\n");
    bool found = false;
    for (int k = 1; k <= 20; k++) {
        h = vt_line(t, -k, &len);
        if (h && len == 1 && vt_style(t, VT_CELL_STYLE(h[0]))->bg == VT_COLOR_IDX(4)) found = true;
    }
    CHECK(found);
    vt_free(t);
}

static void test_scroll_keeps_caches_attached(void) {
    Vt *t = vt_new(10, 3, 10);
    feed(t, "a\r\nb\r\nc");
    VtLineMeta *m1 = vt_line_meta(t, 1);
    m1->dirty = 0;
    feed(t, "\r\nd");
    VtLineMeta *now = vt_line_meta(t, 0);
    CHECK(now == m1 && now->dirty == 0);
    vt_free(t);
}


static void check_numbered(Vt *t, int fed, int rows, const char *what) {
    long H = vt_history_count(t);
    int bad = 0;
    for (long k = 1; k <= H; k += (k < 300 ? 1 : 97)) {
        char want[32], got[64];
        snprintf(want, sizeof want, "L%ld", (long)(fed - (rows - 1)) - k);
        row_text(t, -(int)k, got, sizeof got);
        if (strcmp(want, got)) { if (bad++ < 3) printf("  %s: line -%ld is \"%s\", want \"%s\"\n", what, k, got, want); }
    }
    CHECK(bad == 0);
}

static void feed_numbered(Vt *t, int from, int to) {
    char b[32];
    for (int i = from; i < to; i++) { snprintf(b, sizeof b, "L%d\r\n", i); feed(t, b); }
}

static void test_history_blocks_roundtrip(void) {
    Vt *t = vt_new(40, 5, -1);
    feed_numbered(t, 0, 20000);
    VtHistoryStats st;
    vt_history_stats(t, &st);
    CHECK(st.lines == vt_history_count(t) && st.lines == 20000 - 4);
    CHECK(st.hot_lines <= 1152 && st.block_count > 100);
    check_numbered(t, 20000, 5, "unlimited");
    CHECK(st.hot_bytes + st.packed_bytes < 20000 * 64);
    vt_free(t);
}

static void test_history_limit_and_runtime_change(void) {
    Vt *t = vt_new(40, 5, 3000);
    feed_numbered(t, 0, 10000);
    long n = vt_history_count(t);
    CHECK(n <= 3000 && n >= 3000 - 128);
    check_numbered(t, 10000, 5, "limited");
    vt_set_history(t, 500, 0, 0, true);
    n = vt_history_count(t);
    CHECK(n <= 500 && n >= 500 - 128);
    check_numbered(t, 10000, 5, "shrunk");
    vt_set_history(t, -1, 0, 0, true);
    feed_numbered(t, 10000, 30000);
    check_numbered(t, 30000, 5, "lifted");
    vt_set_history(t, 0, 0, 0, true);
    CHECK(vt_history_count(t) == 0);
    vt_free(t);
}

static void test_history_spill_to_disk(void) {
    Vt *t = vt_new(60, 5, -1);
    vt_set_history(t, -1, 8 * 1024, 64u << 20, true);
    feed_numbered(t, 0, 60000);
    for (int i = 0; i < 200; i++) { usleep(2000); vt_compact(t); }
    VtHistoryStats st;
    vt_history_stats(t, &st);
    CHECK(st.disk_bytes > 0);
    CHECK(st.packed_bytes <= 8 * 1024 + 64 * 1024);
    check_numbered(t, 60000, 5, "spilled");
    vt_free(t);
}

static void test_history_budgets_bound_memory(void) {
    Vt *t = vt_new(60, 5, -1);
    vt_set_history(t, -1, 16 * 1024, 32 * 1024, true);
    feed_numbered(t, 0, 80000);
    for (int i = 0; i < 100; i++) { usleep(2000); vt_compact(t); }
    VtHistoryStats st;
    vt_history_stats(t, &st);
    CHECK(st.disk_bytes <= 32 * 1024 + 64 * 1024);
    CHECK(vt_history_count(t) < 80000 - 4);
    CHECK(vt_history_count(t) > 0);
    char got[64];
    row_text(t, -1, got, sizeof got);
    CHECK(strcmp(got, "L79995") == 0);
    vt_free(t);

    t = vt_new(60, 5, -1);
    vt_set_history(t, -1, 16 * 1024, 1u << 30, false);
    feed_numbered(t, 0, 80000);
    for (int i = 0; i < 100; i++) { usleep(2000); vt_compact(t); }
    vt_history_stats(t, &st);
    CHECK(st.disk_bytes == 0 && st.packed_bytes <= 16 * 1024 + 64 * 1024);
    vt_free(t);
}

static void test_budget_holds_under_flood(void) {
    Vt *t = vt_new(80, 10, -1);
    vt_set_history(t, -1, 256 * 1024, 256u << 20, true);
    size_t worst = 0;
    for (int i = 0; i < 400000; i++) {
        char b[96];
        int n = snprintf(b, sizeof b, "2026-10-03 12:00:00 INFO request processed id=%d\r\n", i);
        vt_feed(t, (const uint8_t *)b, (size_t)n);
        if (i % 20000 == 0) {
            VtHistoryStats st;
            vt_history_stats(t, &st);
            if (st.packed_bytes > worst) worst = st.packed_bytes;
        }
    }
    for (int i = 0; i < 50; i++) { usleep(5000); vt_compact(t); }
    VtHistoryStats st;
    vt_history_stats(t, &st);
    CHECK(st.disk_bytes > 0);
    CHECK(st.packed_bytes <= 256 * 1024 + 9 * 1024 * 1024);
    CHECK(worst <= 256 * 1024 + 9 * 1024 * 1024);
    CHECK(st.lines == 400000 - 9);
    vt_free(t);
}

static void test_compact_keeps_content(void) {
    Vt *t = vt_new(60, 5, 20000);
    feed_numbered(t, 0, 15000);
    VtHistoryStats a, b;
    vt_history_stats(t, &a);
    size_t freed = vt_compact(t);
    vt_history_stats(t, &b);
    CHECK(b.hot_lines <= 2 * 128 && freed > 0);
    CHECK(b.hot_bytes + b.packed_bytes < a.hot_bytes + a.packed_bytes);
    check_numbered(t, 15000, 5, "compacted");
    vt_free(t);
}

static void test_history_styles_survive_packing(void) {
    Vt *t = vt_new(30, 4, -1);
    for (int i = 0; i < 4000; i++) feed(t, "\x1b[1;31mred\x1b[0m plain \x1b[38;2;10;20;30mrgb\x1b[0m\r\n");
    int len = 0;
    VtCell *l = vt_line(t, -3000, &len);
    CHECK(l && len == 3 + 7 + 3);
    CHECK(vt_style(t, VT_CELL_STYLE(l[0]))->attrs & VT_BOLD);
    CHECK(vt_style(t, VT_CELL_STYLE(l[0]))->fg == VT_COLOR_IDX(1));
    CHECK(VT_CELL_STYLE(l[3]) == 0);
    CHECK(vt_style(t, VT_CELL_STYLE(l[10]))->fg == VT_COLOR_RGB(10, 20, 30));
    vt_free(t);
}

static void test_stream_requests(void) {
    char path[4200]; uint64_t off, len; unsigned flags;
    const char *good = "ab\x1b]7777;cat;s3cret;/tmp/a%3Bb%25c;5;9;3\acd";
    Vt *t = vt_new(40, 5, 100);
    vt_feed(t, (const uint8_t *)good, strlen(good));
    CHECK(!vt_take_stream(t, path, sizeof path, &off, &len, &flags));
    vt_set_stream_token(t, "s3cret");
    size_t used = vt_feed_stream(t, (const uint8_t *)good, strlen(good));
    CHECK(used < strlen(good));
    CHECK(vt_take_stream(t, path, sizeof path, &off, &len, &flags));
    CHECK(strcmp(path, "/tmp/a;b%c") == 0 && off == 5 && len == 9 && flags == 3);
    CHECK(!vt_take_stream(t, path, sizeof path, &off, &len, &flags));
    vt_feed(t, (const uint8_t *)good + used, strlen(good) - used);
    const char *bad_token = "\x1b]7777;cat;wrong;/tmp/x;0;0;0\a", *relative = "\x1b]7777;cat;s3cret;rel/x;0;0;0\a";
    vt_feed_stream(t, (const uint8_t *)bad_token, strlen(bad_token));
    CHECK(!vt_take_stream(t, path, sizeof path, &off, &len, &flags));
    vt_feed_stream(t, (const uint8_t *)relative, strlen(relative));
    CHECK(!vt_take_stream(t, path, sizeof path, &off, &len, &flags));
    vt_free(t);
}

static void test_history_segments(void) {
    Vt *t = vt_new(40, 5, 1400);
    char line[64];
    for (int i = 0; i < 6000; i++) {
        if (i == 2500) vt_resize(t, 90, 7);
        if (i == 4200) vt_set_history(t, 1300, 0, 0, true);
        int n = snprintf(line, sizeof line, "line %d\r\n", i);
        vt_feed(t, (const uint8_t *)line, (size_t)n);
    }
    int hc = vt_history_count(t);
    CHECK(hc <= 1300 && hc > 1300 - 129);
    int bad = 0;
    for (int k = 1; k <= hc; k++) {
        int len = 0;
        VtCell *c = vt_line(t, -k, &len);
        char want[64];
        int wl = snprintf(want, sizeof want, "line %d", 6000 - (vt_rows(t) - 1) - k);
        if (!c || len != wl) { bad++; continue; }
        for (int x = 0; x < len; x++) if (VT_CELL_CH(c[x]) != (uint32_t)(unsigned char)want[x]) { bad++; break; }
    }
    CHECK(bad == 0);
    vt_compact(t);
    int bad2 = 0;
    for (int k = 1; k <= hc; k += 37) { int len = 0; VtCell *c = vt_line(t, -k, &len); char want[64]; int wl = snprintf(want, sizeof want, "line %d", 6000 - (vt_rows(t) - 1) - k); if (!c || len != wl || VT_CELL_CH(c[0]) != 'l') bad2++; }
    CHECK(bad2 == 0);
    vt_clear_history(t);
    CHECK(vt_history_count(t) == 0);
    vt_feed(t, (const uint8_t *)"a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh\r\n", 16);
    CHECK(vt_history_count(t) > 0);
    vt_free(t);
}

/* The one-call SGR shortcut must give exactly what the general parser gives, so feed every "ESC [ N m" form whole and byte by byte. */
static void test_sgr_quick_path_matches_general_path() {
    char seq[32];
    for (int pre = 0; pre < 3; pre++) {
        const char *setup = pre == 0 ? "" : pre == 1 ? "\x1b[1;33;44m" : "\x1b[7;4;96;102m";
        for (int n = -2; n < 112; n++) {
            if (n == -2) snprintf(seq, sizeof seq, "\x1b[m");
            else if (n == -1) snprintf(seq, sizeof seq, "\x1b[00m");
            else snprintf(seq, sizeof seq, "\x1b[%dm", n);
            std::string text = std::string(setup) + "p" + seq + "Q";
            Vt *a = vt_new(20, 3, 100), *b = vt_new(20, 3, 100);
            vt_feed(a, (const uint8_t *)text.data(), text.size());
            for (char c : text) vt_feed(b, (const uint8_t *)&c, 1);
            int la = 0, lb = 0;
            VtCell *ra = vt_line(a, vt_history_count(a), &la), *rb = vt_line(b, vt_history_count(b), &lb);
            bool same = ra && rb && la >= 2 && lb >= 2;
            if (same) {
                const VtStyle *sa = vt_style(a, VT_CELL_STYLE(ra[1])), *sb = vt_style(b, VT_CELL_STYLE(rb[1]));
                same = sa->fg == sb->fg && sa->bg == sb->bg && sa->attrs == sb->attrs;
            }
            CHECK(same);
            vt_free(a); vt_free(b);
        }
    }
}


static int ev_prompt, ev_start, ev_end, ev_code = -2;
static void on_event_133(VtEvent e, const char *s, void *u) {
    (void)u;
    if (e == VT_EV_PROMPT) ev_prompt++;
    else if (e == VT_EV_CMD_START) ev_start++;
    else if (e == VT_EV_CMD_END) { ev_end++; ev_code = atoi(s); }
}

static char nt_last[300]; static int nt_n;
static void on_event_notify(VtEvent e, const char *s, void *u) {
    (void)u;
    if (e == VT_EV_NOTIFY) { nt_n++; snprintf(nt_last, sizeof nt_last, "%s", s); }
}

static void test_notify(void) {
    Vt *t = vt_new(20, 5, 100);
    vt_set_callbacks(t, NULL, on_event_notify, NULL, NULL);
    feed(t, "\x1b]9;build done\x07");
    CHECK(nt_n == 1 && strcmp(nt_last, "\x1f" "build done") == 0);
    feed(t, "\x1b]777;notify;Title;the body; with semicolon\x1b\\");
    CHECK(nt_n == 2 && strcmp(nt_last, "Title\x1fthe body; with semicolon") == 0);
    feed(t, "\x1b]777;notify;OnlyTitle\x07");
    CHECK(nt_n == 3 && strcmp(nt_last, "OnlyTitle\x1f") == 0);
    feed(t, "\x1b]9;4;3;50\x07\x1b]777;other;x;y\x07\x1b]9\x07");   /* progress report, other 777 verbs, no text */
    CHECK(nt_n == 3);
    vt_free(t);
}

static void test_shell_integration(void) {
    Vt *t = vt_new(20, 5, 100);
    vt_set_callbacks(t, NULL, on_event_133, NULL, NULL);
    CHECK(vt_prompt_find(t, 0, -1) == INT_MIN && vt_last_exit(t) == -1);
    /* one command: prompt on row 0, output on rows 1-2 */
    feed(t, "\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07out1\r\nout2\r\n\x1b]133;D;3\x07");
    CHECK(ev_prompt == 1 && ev_start == 1 && ev_end == 1 && ev_code == 3 && vt_last_exit(t) == 3);
    int a, b;
    CHECK(vt_last_output(t, &a, &b) && a == 1 && b == 2);
    CHECK(vt_prompt_find(t, 4, -1) == 0 && vt_prompt_find(t, 0, 1) == INT_MIN);
    /* an empty Enter (D without C) is not a command end and keeps the earlier output */
    feed(t, "\x1b]133;A\x07$ \x1b]133;B\x07\r\n\x1b]133;D;0\x07");
    CHECK(ev_end == 1 && vt_last_exit(t) == 3 && vt_last_output(t, &a, &b) && a == 1 && b == 2);
    /* the marks follow their lines into the history */
    for (int i = 0; i < 12; i++) feed(t, "x\r\n");
    int p = vt_prompt_find(t, 100, -1);
    CHECK(p < 0 && p != INT_MIN);
    char buf[64];
    row_text(t, p, buf, sizeof buf);
    CHECK(strcmp(buf, "$") == 0);
    CHECK(vt_last_output(t, &a, &b) && a < 0 && b == a + 1);
    row_text(t, a, buf, sizeof buf); CHECK(strcmp(buf, "out1") == 0);
    row_text(t, b, buf, sizeof buf); CHECK(strcmp(buf, "out2") == 0);
    int first = vt_prompt_find(t, 100, -1), second = vt_prompt_find(t, first, -1);
    CHECK(second != INT_MIN && second < first && vt_prompt_find(t, second, 1) == first);
    /* unknown kinds, junk and the alternate screen are ignored */
    int before = ev_prompt;
    feed(t, "\x1b]133;Z\x07\x1b]133;AB\x07\x1b[?1049h\x1b]133;A\x07\x1b[?1049l");
    CHECK(ev_prompt == before + 1 && vt_prompt_find(t, 100, -1) == first);
    /* a resize drops the marks (rows shift), a clear of the history too */
    vt_resize(t, 30, 6);
    CHECK(vt_prompt_find(t, 100, -1) == INT_MIN);
    feed(t, "\x1b]133;A\x07p\r\n\x1b[3J");
    CHECK(vt_prompt_find(t, 100, -1) == INT_MIN);
    vt_free(t);

    /* more marks than the table holds, with the history limit dropping the old ones: no crash, newest prompt still found */
    t = vt_new(20, 5, 50);
    for (int i = 0; i < 20000; i++) feed(t, "\x1b]133;A\a$ \x1b]133;B\ac\r\n\x1b]133;C\ao\r\n\x1b]133;D;0\a");
    int last = vt_prompt_find(t, 100, -1);
    CHECK(last != INT_MIN && vt_last_output(t, &a, &b));
    vt_free(t);
}


static const VtStyle *style_at(Vt *t, int row, int col) {
    int len = 0;
    VtCell *l = vt_line(t, row, &len);
    return vt_style(t, l && col < len ? VT_CELL_STYLE(l[col]) : 0);
}

static void test_underline_styles_and_colour(void) {
    Vt *t = vt_new(20, 3, 0);
    feed(t, "\x1b[4mA\x1b[4:3mB\x1b[4:2mC\x1b[4:4mD\x1b[4:5mE\x1b[4:1mF\x1b[4:0mG\x1b[0mH");
    const VtStyle *s = style_at(t, 0, 0);
    CHECK((s->attrs & VT_UNDERLINE) && VT_UL_KIND(s->attrs) == 0);
    s = style_at(t, 0, 1); CHECK((s->attrs & VT_UNDERLINE) && VT_UL_KIND(s->attrs) == VT_UL_CURLY);
    s = style_at(t, 0, 2); CHECK((s->attrs & VT_UNDERLINE) && VT_UL_KIND(s->attrs) == VT_UL_DOUBLE);
    s = style_at(t, 0, 3); CHECK((s->attrs & VT_UNDERLINE) && VT_UL_KIND(s->attrs) == VT_UL_DOTTED);
    s = style_at(t, 0, 4); CHECK((s->attrs & VT_UNDERLINE) && VT_UL_KIND(s->attrs) == VT_UL_DASHED);
    s = style_at(t, 0, 5); CHECK((s->attrs & VT_UNDERLINE) && VT_UL_KIND(s->attrs) == 0);
    s = style_at(t, 0, 6); CHECK(!(s->attrs & (VT_UNDERLINE | VT_UL_MASK)));
    s = style_at(t, 0, 7); CHECK(s->attrs == 0 && s->ul == 0);
    CHECK(style_at(t, 0, 0) == style_at(t, 0, 5));   /* SGR 4 and 4:1 intern to the same style */

    /* underline colour: 58;2;r;g;b, 58:2::r:g:b, 58:2:r:g:b, 58;5;n, reset by 59 and 0, saved with the cursor */
    feed(t, "\r\n\x1b[4:3;58;2;255;0;128mA\x1b[58:2::1:2:3mB\x1b[58:2:4:5:6mC\x1b[58;5;9mD\x1b[59mE\x1b[58;5;3m\x1b[0mF");
    s = style_at(t, 1, 0); CHECK(s->ul == VT_COLOR_RGB(255, 0, 128) && VT_UL_KIND(s->attrs) == VT_UL_CURLY);
    CHECK(style_at(t, 1, 1)->ul == VT_COLOR_RGB(1, 2, 3));
    CHECK(style_at(t, 1, 2)->ul == VT_COLOR_RGB(4, 5, 6));
    CHECK(style_at(t, 1, 3)->ul == VT_COLOR_IDX(9));
    CHECK(style_at(t, 1, 4)->ul == 0);
    CHECK(style_at(t, 1, 5)->ul == 0 && style_at(t, 1, 5)->attrs == 0);
    feed(t, "\r\n\x1b[58;5;5m\x1b" "7\x1b[0m\x1b" "8X");
    CHECK(style_at(t, 2, 0)->ul == VT_COLOR_IDX(5));
    /* semicolon form: underline then italic, not an underline kind */
    feed(t, "\x1b[0m\x1b[4;3mY");
    s = style_at(t, 2, 1); CHECK((s->attrs & VT_UNDERLINE) && (s->attrs & VT_ITALIC) && VT_UL_KIND(s->attrs) == 0);
    vt_free(t);
}

static void test_sync_update_mode(void) {
    Vt *t = vt_new(20, 3, 0);
    replies[0] = 0;
    vt_set_callbacks(t, on_write, NULL, NULL, NULL);
    CHECK(!(vt_modes(t) & VT_M_SYNC_UPDATE));
    feed(t, "\x1b[?2026h");
    CHECK(vt_modes(t) & VT_M_SYNC_UPDATE);
    feed(t, "\x1b[?2026$p");
    CHECK(strstr(replies, "\x1b[?2026;1$y"));
    feed(t, "\x1b[?2026l");
    CHECK(!(vt_modes(t) & VT_M_SYNC_UPDATE));
    replies[0] = 0;
    feed(t, "\x1b[?2026$p");
    CHECK(strstr(replies, "\x1b[?2026;2$y"));
    replies[0] = 0;
    feed(t, "\x1b[?9999$p");
    CHECK(strstr(replies, "\x1b[?9999;0$y"));
    feed(t, "\x1b[?2026h\x1b[?1049h\x1b[?1049l");
    CHECK(vt_modes(t) & VT_M_SYNC_UPDATE);
    vt_reset(t);
    CHECK(!(vt_modes(t) & VT_M_SYNC_UPDATE));
    vt_free(t);
}

static void test_kitty_keyboard_flags(void) {
    Vt *t = vt_new(20, 3, 0);
    replies[0] = 0;
    vt_set_callbacks(t, on_write, NULL, NULL, NULL);
    CHECK(vt_kitty_flags(t) == 0);
    feed(t, "\x1b[?u");
    CHECK(strstr(replies, "\x1b[?0u"));
    feed(t, "\x1b[>1u");
    CHECK(vt_kitty_flags(t) == 1);
    feed(t, "\x1b[>9u");                       /* push: 8 | 1 */
    CHECK(vt_kitty_flags(t) == 9);
    replies[0] = 0;
    feed(t, "\x1b[?u");
    CHECK(strstr(replies, "\x1b[?9u"));
    feed(t, "\x1b[<u");                         /* pop one: back to 1 */
    CHECK(vt_kitty_flags(t) == 1);
    feed(t, "\x1b[=8;2u");                      /* OR in 8 */
    CHECK(vt_kitty_flags(t) == 9);
    feed(t, "\x1b[=1;3u");                      /* clear bit 1 */
    CHECK(vt_kitty_flags(t) == 8);
    feed(t, "\x1b[=3u");                        /* set exactly 3 */
    CHECK(vt_kitty_flags(t) == 3);
    feed(t, "\x1b[=31;1u");                     /* unsupported bits (4, 16) are dropped */
    CHECK(vt_kitty_flags(t) == 11);
    /* each screen has its own stack */
    feed(t, "\x1b[?1049h");
    CHECK(vt_kitty_flags(t) == 0);
    feed(t, "\x1b[>8u");
    CHECK(vt_kitty_flags(t) == 8);
    feed(t, "\x1b[?1049l");
    CHECK(vt_kitty_flags(t) == 11);
    feed(t, "\x1b[?1049h");
    CHECK(vt_kitty_flags(t) == 8);
    feed(t, "\x1b[<u\x1b[?1049l");              /* a program that pops on exit leaves the main screen alone */
    CHECK(vt_kitty_flags(t) == 11);
    feed(t, "\x1b[<99u");                       /* more pops than pushes: everything reset */
    CHECK(vt_kitty_flags(t) == 0);
    for (int i = 0; i < 40; i++) feed(t, "\x1b[>1u");   /* the stack is bounded, no overflow */
    CHECK(vt_kitty_flags(t) == 1);
    vt_reset(t);
    CHECK(vt_kitty_flags(t) == 0);
    /* CSI u without a prefix is still restore-cursor */
    feed(t, "\x1b[3;4H\x1b[s\x1b[1;1H\x1b[u");
    CHECK(vt_cursor_x(t) == 3 && vt_cursor_y(t) == 2);
    vt_free(t);
}

static const char *link_at(Vt *t, int row, int col) {
    int len = 0;
    VtCell *l = vt_line(t, row, &len);
    return vt_link(t, vt_style(t, l && col < len ? VT_CELL_STYLE(l[col]) : 0)->link);
}

static void test_hyperlinks_osc8(void) {
    Vt *t = vt_new(40, 4, 100);
    feed(t, "a\x1b]8;;http://x.test/p?q=1;r\x1b\\LINK\x1b]8;;\x1b\\b\x1b]8;id=7;https://y.test\x07Y\x1b[1;31mR\x1b[0mZ\x1b]8;;\x07");
    CHECK(link_at(t, 0, 0) == NULL);
    for (int c = 1; c <= 4; c++) CHECK(link_at(t, 0, c) && !strcmp(link_at(t, 0, c), "http://x.test/p?q=1;r"));   /* the URI keeps its own semicolons */
    CHECK(link_at(t, 0, 5) == NULL);
    CHECK(link_at(t, 0, 6) && !strcmp(link_at(t, 0, 6), "https://y.test"));
    CHECK(link_at(t, 0, 7) && !strcmp(link_at(t, 0, 7), "https://y.test"));   /* SGR changes inside a link keep it */
    CHECK(link_at(t, 0, 8) && !strcmp(link_at(t, 0, 8), "https://y.test"));   /* SGR 0 does not end it, only OSC 8 does */
    CHECK(vt_style(t, 0)->link == 0);
    CHECK(vt_style(t, VT_CELL_STYLE(vt_line(t, 0, NULL)[1]))->link == vt_style(t, VT_CELL_STYLE(vt_line(t, 0, NULL)[4]))->link);   /* one id per URI */
    feed(t, "\r\n\x1b]8;;http://x.test/p?q=1;r\x1b\\Q\x1b]8;;\x1b\\");
    CHECK(vt_style(t, VT_CELL_STYLE(vt_line(t, 1, NULL)[0]))->link == vt_style(t, VT_CELL_STYLE(vt_line(t, 0, NULL)[1]))->link);   /* same URI again, same id */
    feed(t, "\r\n\x1b]8;;bad\x01uri\x07X\x1b]8;;\x07\x1b]8;nosemicolon\x07Y");   /* control characters and a missing URI field: no link */
    CHECK(link_at(t, 2, 0) == NULL && link_at(t, 2, 1) == NULL);
    /* links survive scrolling into the history */
    for (int i = 0; i < 12; i++) feed(t, "\r\nfiller");
    int hc = vt_history_count(t);
    CHECK(hc > 4);
    CHECK(link_at(t, -hc, 1) && !strcmp(link_at(t, -hc, 1), "http://x.test/p?q=1;r"));
    /* many unique links stay bounded and never crash, the cap just stops new ones */
    char buf[96];
    for (int i = 0; i < 70000; i++) { snprintf(buf, sizeof buf, "\x1b]8;;http://h/%d\x07x\x1b]8;;\x07", i); feed(t, buf); }
    CHECK(vt_link(t, 0) == NULL && vt_link(t, 0x7fffffff) == NULL);
    feed(t, "\x1b" "c");   /* RIS ends an open link */
    feed(t, "\x1b]8;;http://z\x07\x1b" "cQ");
    CHECK(link_at(t, 0, 0) == NULL);
    vt_free(t);
}

int main(void) {
    test_hyperlinks_osc8();
    test_underline_styles_and_colour(); test_sync_update_mode(); test_kitty_keyboard_flags();
    test_sgr_quick_path_matches_general_path();
    test_history_segments();
    test_stream_requests(); test_notify(); test_shell_integration();
    test_basic_and_wrap(); test_scrollback_ring(); test_insert_delete_lines(); test_edit_chars_at_pending_wrap();
    test_scroll_region(); test_scroll_window_slide(); test_save_restore(); test_alt_screen(); test_colours(); test_wide_and_combining();
    test_graphics_charset(); test_replies_and_events(); test_modes(); test_resize(); test_utf8_split_and_invalid();
    test_dirty_and_cache(); test_compact_history_and_styles(); test_scroll_keeps_caches_attached();
    test_history_blocks_roundtrip(); test_history_limit_and_runtime_change(); test_history_spill_to_disk();
    test_history_budgets_bound_memory(); test_budget_holds_under_flood(); test_compact_keeps_content(); test_history_styles_survive_packing();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
