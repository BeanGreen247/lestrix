/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "../src/vt.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static int failures, checks;
#define FALSE 0
#define TRUE 1
typedef int gboolean;
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
    gboolean found = FALSE;
    for (int k = 1; k <= 20; k++) {
        h = vt_line(t, -k, &len);
        if (h && len == 1 && vt_style(t, VT_CELL_STYLE(h[0]))->bg == VT_COLOR_IDX(4)) found = TRUE;
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

int main(void) {
    test_history_segments();
    test_stream_requests();
    test_basic_and_wrap(); test_scrollback_ring(); test_insert_delete_lines(); test_edit_chars_at_pending_wrap();
    test_scroll_region(); test_save_restore(); test_alt_screen(); test_colours(); test_wide_and_combining();
    test_graphics_charset(); test_replies_and_events(); test_modes(); test_resize(); test_utf8_split_and_invalid();
    test_dirty_and_cache(); test_compact_history_and_styles(); test_scroll_keeps_caches_attached();
    test_history_blocks_roundtrip(); test_history_limit_and_runtime_change(); test_history_spill_to_disk();
    test_history_budgets_bound_memory(); test_budget_holds_under_flood(); test_compact_keeps_content(); test_history_styles_survive_packing();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
