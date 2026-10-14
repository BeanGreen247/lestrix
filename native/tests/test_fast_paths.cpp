/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/vt.h"

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }

static size_t enc(uint32_t cp, uint8_t *o) {
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { o[0] = (uint8_t)(0xc0 | cp >> 6); o[1] = (uint8_t)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (uint8_t)(0xe0 | cp >> 12); o[1] = (uint8_t)(0x80 | ((cp >> 6) & 63)); o[2] = (uint8_t)(0x80 | (cp & 63)); return 3; }
    o[0] = (uint8_t)(0xf0 | cp >> 18); o[1] = (uint8_t)(0x80 | ((cp >> 12) & 63)); o[2] = (uint8_t)(0x80 | ((cp >> 6) & 63)); o[3] = (uint8_t)(0x80 | (cp & 63)); return 4;
}

static const uint32_t POOL[] = {0xe9, 0xfc, 0x3b1, 0x416, 0x2500, 0x2588, 0x6f22, 0x5b57, 0x3042, 0xac00, 0xff21, 0x301, 0x300, 0x1f600, 0x200b, 0x2028, 0x20ac, 0x58f, 0x483, 0x5d0};

static size_t make(uint8_t *b, size_t cap) {
    size_t n = 0;
    while (n + 16 < cap) {
        unsigned k = rnd() % 100;
        if (k < 40) b[n++] = (uint8_t)(0x20 + rnd() % 95);
        else if (k < 80) n += enc(POOL[rnd() % (sizeof POOL / sizeof *POOL)], b + n);
        else if (k < 85) { b[n++] = '\r'; b[n++] = '\n'; }
        else if (k < 88) b[n++] = '\t';
        else if (k < 90) b[n++] = '\b';
        else if (k < 92) b[n++] = (uint8_t)(0x80 + rnd() % 128);
        else if (k < 94) { b[n++] = 0xe4; b[n++] = (uint8_t)(0xb8); }
        else if (k < 95) { b[n++] = 0xc0; b[n++] = 0x80; }
        else if (k < 96) { b[n++] = 0xed; b[n++] = 0xa0; b[n++] = 0x80; }
        else if (k < 98) n += (size_t)snprintf((char *)b + n, 16, "\x1b[%dm", (int)(rnd() % 8) + 30);
        else n += (size_t)snprintf((char *)b + n, 16, "\x1b[%d;%dH", (int)(rnd() % 20) + 1, (int)(rnd() % 90) + 1);
    }
    return n;
}

static int same(Vt *a, Vt *b) {
    if (vt_cols(a) != vt_cols(b) || vt_rows(a) != vt_rows(b)) return 0;
    if (vt_cursor_x(a) != vt_cursor_x(b) || vt_cursor_y(a) != vt_cursor_y(b)) return 0;
    int ha = vt_history_count(a), hb = vt_history_count(b);
    if (ha != hb) return 0;
    for (int idx = -ha; idx < vt_rows(a); idx++) {
        int la = 0, lb = 0;
        VtCell *ca = vt_line(a, idx, &la), *cb = vt_line(b, idx, &lb);
        int m = la > lb ? la : lb;
        if (getenv("FP_DEBUG") && la != lb) { fprintf(stderr, "idx %d len %d/%d hist %d\n", idx, la, lb, ha); }
        for (int x = 0; x < m; x++) {
            VtCell xa = x < la ? ca[x] : (VtCell){0, 0}, xb = x < lb ? cb[x] : (VtCell){0, 0};
            if (xa.cp != xb.cp || (xa.sf & 0xff) != (xb.sf & 0xff)) { if (getenv("FP_DEBUG")) fprintf(stderr, "idx %d x %d: cp %x/%x sf %x/%x len %d/%d hist %d\n", idx, x, xa.cp, xb.cp, xa.sf, xb.sf, la, lb, ha); return 0; }
        }
    }
    return 1;
}


static size_t make_lines(uint8_t *b, size_t cap, int impure_pct, int bare) {
    size_t n = 0;
    while (n + 400 < cap) {
        unsigned kind = rnd() % 100;
        size_t len = 5 + rnd() % 150;
        if (kind < (unsigned)impure_pct) {
            for (size_t i = 0; i < len; i++) {
                unsigned k = rnd() % 100;
                if (k < 6) b[n++] = '\t';
                else if (k < 12) n += enc(POOL[rnd() % (sizeof POOL / sizeof *POOL)], b + n);
                else b[n++] = (uint8_t)(0x20 + rnd() % 95);
            }
        } else {
            for (size_t i = 0; i < len; i++) b[n++] = (uint8_t)(0x20 + rnd() % 95);
        }
        if (!bare) b[n++] = '\r';
        b[n++] = '\n';
    }
    return n;
}

static int bulk_history_round(int cols, int rows, int scrollback, int impure_pct, size_t mb, int bare) {
    static uint8_t buf[1 << 20];
    Vt *fast = vt_new(cols, rows, scrollback), *slow = vt_new(cols, rows, scrollback);
    int ok = 1;
    if (bare) { vt_set_force_nl(fast, true); vt_set_force_nl(slow, true); }
    for (size_t done = 0; done < (mb << 20) && ok; done += sizeof buf) {
        size_t n = make_lines(buf, sizeof buf, impure_pct, bare);
        for (size_t i = 0; i < n;) {
            size_t chunk = 200000 + rnd() % 600000;
            if (chunk > n - i) chunk = n - i;
            vt_set_fast_paths(true);  vt_feed(fast, buf + i, chunk);
            vt_set_fast_paths(false); vt_feed(slow, buf + i, chunk);
            i += chunk;
        }
        vt_set_fast_paths(true);
        if (!same(fast, slow)) ok = 0;
    }
    if (ok && !getenv("FP_NOCOMPACT")) {
        vt_compact(fast); vt_compact(slow);
        usleep(150000);
        vt_compact(fast); vt_compact(slow);
        ok = same(fast, slow);
    }
    vt_free(fast); vt_free(slow);
    return ok;
}

int main(void) {
    int checks = 0, fails = 0;
    for (int round = 0; round < 400; round++) {
        int cols = 20 + (int)(rnd() % 100), rows = 8 + (int)(rnd() % 30);
        Vt *fast = vt_new(cols, rows, 500), *slow = vt_new(cols, rows, 500);
        static uint8_t buf[1 << 16];
        size_t n = make(buf, sizeof buf);
        for (size_t i = 0; i < n;) {
            size_t chunk = 1 + rnd() % 700;
            if (chunk > n - i) chunk = n - i;
            vt_set_fast_paths(true);  vt_feed(fast, buf + i, chunk);
            vt_set_fast_paths(false); vt_feed(slow, buf + i, chunk);
            i += chunk;
        }
        vt_set_fast_paths(true);
        checks++;
        if (!same(fast, slow)) { fails++; fprintf(stderr, "round %d (%dx%d): fast and general paths differ\n", round, cols, rows); }
        vt_free(fast); vt_free(slow);
    }
    {
        static const struct { int cols, rows, sb, impure; size_t mb; int bare; } cfg[] = {
            {80, 24, 3000, 0, 2, 0}, {132, 40, 100000, 0, 3, 0}, {200, 30, 5000, 10, 2, 0}, {97, 17, 100000, 40, 2, 0}, {120, 50, 100000, 2, 4, 0}, {60, 12, 1500, 100, 1, 0},
            {100, 30, 100000, 0, 3, 1}, {140, 35, 5000, 5, 2, 1}, {70, 20, 100000, 30, 2, 1}};
        for (unsigned i = 0; i < sizeof cfg / sizeof *cfg; i++) {
            checks++;
            if (!bulk_history_round(cfg[i].cols, cfg[i].rows, cfg[i].sb, cfg[i].impure, cfg[i].mb, cfg[i].bare)) { fails++; fprintf(stderr, "bulk history config %u: fast and general paths differ\n", i); }
        }
    }
    for (uint32_t cp = 0; cp < 0x10000; cp++) {
        int fw = 0;
        if ((cp >= 0xa0 && cp <= 0x2ff) || (cp >= 0x370 && cp <= 0x482) || (cp >= 0x48a && cp <= 0x58f) || (cp >= 0x2500 && cp <= 0x259f)) fw = 1;
        else if ((cp >= 0x4e00 && cp <= 0x9fff) || ((cp >= 0x3041 && cp <= 0x3098) || (cp >= 0x309b && cp <= 0x30ff)) || (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xff01 && cp <= 0xff60)) fw = 2;
        if (fw) { checks++; if (vt_wcwidth(cp) != fw) { fails++; fprintf(stderr, "width of U+%04X: fast %d, general %d\n", cp, fw, vt_wcwidth(cp)); } }
    }
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
