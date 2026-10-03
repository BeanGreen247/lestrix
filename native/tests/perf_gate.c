/* A speed regression gate: the thresholds are about ten times below what this code does on a laptop,
 * so only a real slowdown (an accidental copy per line, a lock in the parser) trips it. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/vt.h"

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

static double run(const char *data, size_t len, int scrollback) {
    Vt *t = vt_new(176, 49, scrollback);
    double t0 = now();
    for (size_t i = 0; i < len; i += 65536) vt_feed(t, (const uint8_t *)data + i, len - i < 65536 ? len - i : 65536);
    double dt = now() - t0;
    vt_free(t);
    return (double)len / 1e6 / dt;
}

int main(void) {
    size_t want = 8u << 20, n = 0;
    char *buf = malloc(want + 256);
    for (int i = 0; n < want; i++)
        n += (size_t)snprintf(buf + n, 200, "2026-10-03 12:00:%02d INFO worker-%d processed request id=%d in %d ms\r\n", i % 60, i % 8, i, i % 97);
    double best_plain = 0, best_hist = 0;
    for (int k = 0; k < 3; k++) {   /* best of three: a busy machine only makes a run slower */
        double a = run(buf, n, 0), b = run(buf, n, 10000);
        if (a > best_plain) best_plain = a;
        if (b > best_hist) best_hist = b;
    }
    printf("  parse throughput: %.0f MB/s without scrollback (gate 100), %.0f MB/s with a 10,000-line scrollback (gate 40)\n", best_plain, best_hist);
    free(buf);
    return (best_plain >= 100 && best_hist >= 40) ? 0 : 1;
}
