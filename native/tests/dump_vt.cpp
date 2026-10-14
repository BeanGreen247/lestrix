/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* Feeds seeded random terminal streams and prints a checksum of every history line and screen cell.
 * Built twice (with and without -DVT_NO_SHADOW); the two outputs must be identical. */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/vt.h"

static uint64_t rs;
static unsigned rnd(unsigned n) { rs = rs * 6364136223846793005ull + 1442695040888963407ull; return (unsigned)(rs >> 33) % n; }

static size_t gen(char *b, size_t cap) {
    static const char *const seq[] = {"\x1b[31m", "\x1b[0m", "\x1b[1;32m", "\x1b[38;5;99m", "\x1b[C", "\x1b[5C", "\x1b[D", "\x1b[3D", "\x1b[K", "\x1b[1K", "\x1b[2K",
        "\x1b[3X", "\x1b[2@", "\x1b[2P", "\x1b[H", "\x1b[5;7H", "\x1b[2J", "\x1b[L", "\x1b[M", "\x1b[2S", "\x1b[T", "\b", "\t", "\x1b[?1049h", "\x1b[?1049l",
        "\x1b[?7l", "\x1b[?7h", "\x1b[4h", "\x1b[4l", "\x1b[3;20r", "\x1b[r", "\x1bM", "\x1b" "D", "\xc3\xa9", "\xe2\x82\xac", "\xe4\xb8\xad", "\r", "\n", "\r\n", "\x1b[?1049h\x1b[2J"};
    size_t n = 0;
    while (n + 400 < cap) {
        unsigned k = rnd(10);
        if (k < 5) { unsigned len = 1 + rnd(k < 2 ? 300 : 90); for (unsigned i = 0; i < len; i++) b[n++] = (char)(0x20 + rnd(0x5f)); }
        else if (k < 7) { b[n++] = '\r'; b[n++] = '\n'; }
        else { const char *s = seq[rnd(sizeof seq / sizeof *seq)]; size_t l = strlen(s); memcpy(b + n, s, l); n += l; }
    }
    return n;
}

static uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h; }

int main(int argc, char **argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 200;
    static char buf[1 << 18];
    for (int s = 1; s <= seeds; s++) {
        rs = (uint64_t)s * 7919;
        int cols = 20 + (int)rnd(120), rows = 5 + (int)rnd(30);
        Vt *t = vt_new(cols, rows, (s % 5 == 0) ? 0 : 50 + (int)rnd(2000));
        size_t n = gen(buf, sizeof buf), i = 0;
        while (i < n) { size_t k = 1 + rnd(9000); if (k > n - i) k = n - i; vt_feed(t, (uint8_t *)buf + i, k); i += k; }
        uint64_t h = 1469598103934665603ull;
        int hc = vt_history_count(t);
        for (int y = -hc; y < vt_rows(t); y++) {
            int len = 0; VtCell *c = vt_line(t, y, &len);
            if (!c) continue;
            h = mix(h, (uint64_t)len);
            for (int x = 0; x < len; x++) h = mix(h, ((uint64_t)c[x].cp << 32) | c[x].sf);
        }
        printf("seed %d cols %d rows %d hist %d hash %016llx\n", s, cols, rows, hc, (unsigned long long)h);
        vt_free(t);
    }
    return 0;
}
