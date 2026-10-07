/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t s;
static inline uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

static const char *const wide[] = {"é", "ß", "ñ", "Ω", "λ", "→", "★", "│", "─", "█", "░", "€", "£", "ж", "日", "本", "語", "한", "😀"};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s OUTFILE [MiB] [seed]\n", argv[0]); return 2; }
    size_t total = (argc > 2 ? (size_t)atol(argv[2]) : 1024) << 20;
    s = argc > 3 ? strtoull(argv[3], 0, 10) | 1 : 0x9E3779B97F4A7C15ull;
    const char *mode = argc > 4 ? argv[4] : "text";
    int longline = !strcmp(mode, "longline"), manyline = !strcmp(mode, "manyline");
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 1; }
    enum { BUF = 1 << 20 };
    char *b = malloc(BUF + 16);
    size_t done = 0, n = 0;
    int col = 0, linelen = 1 + (int)(rnd() % 160);
    while (done < total) {
        n = 0;
        while (n < BUF) {
            uint64_t r = rnd();
            if (longline || manyline) {
                unsigned pick = (unsigned)((r >> 20) % (manyline ? 27 : 26));
                b[n++] = pick == 26 ? '\n' : (char)('a' + pick);
                continue;
            }
            unsigned k = (unsigned)(r & 0xff);
            if (col >= linelen) { b[n++] = '\n'; col = 0; linelen = 1 + (int)((r >> 8) % 160); continue; }
            if (k < 8) { b[n++] = '\t'; col += 8; }
            else if (k < 20) { const char *w = wide[(r >> 16) % (sizeof wide / sizeof *wide)]; size_t l = strlen(w); memcpy(b + n, w, l); n += l; col += 1; }
            else { b[n++] = (char)(0x20 + (r >> 24) % 95); col++; }
        }
        size_t w = done + n > total ? total - done : n;
        if (fwrite(b, 1, w, f) != w) { perror("write"); return 1; }
        done += w;
    }
    if (!longline) fputc('\n', f);
    fclose(f);
    free(b);
    return 0;
}
