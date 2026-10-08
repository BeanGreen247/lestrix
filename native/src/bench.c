/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#define _DEFAULT_SOURCE
#include "bench.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lz.h"
#include "vt.h"

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

static char *make_plain(size_t want, size_t *len) {
    char *b = malloc(want + 256);
    size_t n = 0;
    for (int i = 0; n < want; i++)
        n += (size_t)snprintf(b + n, 200, "2026-10-03 12:00:%02d INFO worker-%d processed request id=%d in %d ms\r\n", i % 60, i % 8, i, i % 97);
    *len = n;
    return b;
}

static char *make_colour(size_t want, size_t *len) {
    char *b = malloc(want + 512);
    size_t n = 0;
    for (int i = 0; n < want; i++)
        n += (size_t)snprintf(b + n, 300, "\x1b[%dm%s\x1b[0m alpha \x1b[1;%dm/usr/lib/x86_64-linux-gnu\x1b[0m beta %d \x1b[31merror\x1b[0m\r\n", 30 + i % 8, "gamma", 90 + i % 8, i);
    *len = n;
    return b;
}

static double feed_all(const char *data, size_t len, int scrollback, size_t *mem, long *lines, double *ratio) {
    Vt *t = vt_new(176, 49, scrollback);
    double t0 = now();
    for (size_t i = 0; i < len; i += 65536) vt_feed(t, (const uint8_t *)data + i, len - i < 65536 ? len - i : 65536);
    double dt = now() - t0;
    if (ratio) {
        usleep(100000);
        vt_compact(t);
        VtHistoryStats st;
        vt_history_stats(t, &st);
        *ratio = st.packed_bytes + st.disk_bytes ? (double)st.raw_bytes / (double)(st.packed_bytes + st.disk_bytes) : 0;
    }
    if (mem) *mem = vt_memory_used(t);
    if (lines) *lines = vt_history_count(t);
    vt_free(t);
    return (double)len / 1e6 / dt;
}

int sd_benchmark(FILE *out, double megabytes) {
    size_t want = (size_t)(megabytes * 1048576), plen, clen;
    char *plain = make_plain(want, &plen), *colour = make_colour(want, &clen);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    fprintf(out, "Lestrix terminal core, %.0f MB per test, %ld core%s\n\n", (double)plen / 1048576, cores, cores == 1 ? "" : "s");
    size_t mem;
    long lines;
    double r, ratio = 0;
    r = feed_all(plain, plen, 0, NULL, NULL, NULL);
    fprintf(out, "  plain log output, no scrollback            %7.0f MB/s\n", r);
    r = feed_all(plain, plen, 10000, &mem, &lines, NULL);
    fprintf(out, "  plain log output, 10,000-line scrollback   %7.0f MB/s   (%.1f MB for %ld lines)\n", r, (double)mem / 1e6, lines);
    r = feed_all(plain, plen, -1, &mem, &lines, &ratio);
    fprintf(out, "  plain log output, unlimited scrollback     %7.0f MB/s   (%.1f MB for %ld lines, packed %.0fx smaller)\n", r, (double)mem / 1e6, lines, ratio);
    r = feed_all(colour, clen, 10000, &mem, &lines, NULL);
    fprintf(out, "  colour-heavy output, 10,000-line scrollback %6.0f MB/s   (%.1f MB for %ld lines)\n", r, (double)mem / 1e6, lines);

    size_t n = 4096 * 16, ncells = n * 8;
    uint8_t *cells = calloc(ncells, 1), *sh = malloc(ncells), *comp = malloc(sd_lz_bound(ncells));
    for (size_t i = 0; i < n; i++) { cells[i * 8] = (uint8_t)(' ' + (i * 7 + (i >> 5)) % 90); cells[i * 8 + 4] = (uint8_t)((i / 40) % 3); }
    for (size_t c = 0; c < n; c++) for (int pl = 0; pl < 8; pl++) sh[(size_t)pl * n + c] = cells[c * 8 + (size_t)pl];
    double t0 = now();
    for (int k = 0; k < 20; k++) (void)sd_lz_compress(sh, ncells, comp, sd_lz_bound(ncells));
    double dt = (now() - t0) / 20;
    fprintf(out, "\n  scrollback compressor                      %7.0f MB/s per core\n", (double)ncells / 1e6 / dt);
    free(cells); free(sh); free(comp); free(plain); free(colour);
    return 0;
}
