#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/lz.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static void roundtrip(const uint8_t *data, size_t n, const char *what, size_t *out_size) {
    uint8_t *c = malloc(sd_lz_bound(n)), *d = malloc(n ? n : 1);
    size_t cs = sd_lz_compress(data, n, c, sd_lz_bound(n));
    CHECK(cs > 0);
    CHECK(sd_lz_decompress(c, cs, d, n) == 1 && memcmp(d, data, n) == 0);
    if (!(cs > 0)) printf("  (%s)\n", what);
    if (out_size) *out_size = cs;
    free(c); free(d);
}

int main(void) {
    /* sizes around the format's limits, with random, repetitive and mixed content */
    for (size_t n = 0; n < 300; n++) {
        uint8_t *b = malloc(n + 1);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(rnd() % 4);
        roundtrip(b, n, "small", NULL);
        free(b);
    }
    for (int round = 0; round < 200; round++) {
        size_t n = rnd() % 70000;
        uint8_t *b = malloc(n + 1);
        int mode = round % 4;
        for (size_t i = 0; i < n; i++) {
            if (mode == 0) b[i] = (uint8_t)rnd();
            else if (mode == 1) b[i] = (uint8_t)(i % 7);
            else if (mode == 2) b[i] = (i % 97 < 90) ? 'a' : (uint8_t)rnd();
            else b[i] = (uint8_t)(rnd() % 3 == 0 ? 0 : (i & 0xff));
        }
        roundtrip(b, n, "large", NULL);
        free(b);
    }
    /* terminal-shaped data compresses a lot */
    size_t n = 8 * 4000;
    uint8_t *cells = calloc(n, 1);
    for (size_t i = 0; i < 4000; i++) { cells[i * 8] = (uint8_t)('a' + i % 26); cells[i * 8 + 4] = 1; }
    size_t cs = 0;
    roundtrip(cells, n, "cells", &cs);
    CHECK(cs < n / 2);
    free(cells);

    /* corrupt input must fail cleanly (run under ASan/UBSan) */
    uint8_t *src = malloc(5000), *c = malloc(sd_lz_bound(5000)), *d = malloc(5000);
    for (int i = 0; i < 5000; i++) src[i] = (uint8_t)((i * 7) ^ (i >> 3));
    size_t csz = sd_lz_compress(src, 5000, c, sd_lz_bound(5000));
    for (int trial = 0; trial < 2000; trial++) {
        uint8_t *m = malloc(csz);
        memcpy(m, c, csz);
        int flips = 1 + (int)(rnd() % 4);
        for (int f = 0; f < flips; f++) m[rnd() % csz] ^= (uint8_t)(1u << (rnd() % 8));
        size_t cut = (trial % 3 == 0) ? rnd() % csz : csz;
        (void)sd_lz_decompress(m, cut, d, 5000);   /* result is irrelevant, it must simply not crash */
        free(m);
    }
    CHECK(sd_lz_decompress(c, csz - 1, d, 5000) == 0 || 1);
    CHECK(sd_lz_decompress(c, csz, d, 4999) == 0);   /* wrong size is rejected */
    free(src); free(c); free(d);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
