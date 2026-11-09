/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_LIGRUN_H
#define SD_LIGRUN_H
#include <stdint.h>

/* Which cells of a row are handed to the shaper: runs of at least two printable ASCII cells in one style that contain a symbol (letters and
 * digits alone never form a programming ligature worth the cost). Wide cells, tails, cells with combining marks (flag bit LR_COMB) and the
 * cursor cell (brk) end a run, so a ligature is broken at the cursor. */
typedef struct { int start, len; } LigRun;
enum { LR_WIDE = 1, LR_TAIL = 2, LR_COMB = 0x80 };
#define LIG_MAX_RUN 64

static inline bool lig_cand(uint32_t cp) { return cp > 0x20 && cp < 0x7f; }
static inline bool lig_sym(uint32_t cp) { return !((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')); }

static inline int lig_split(const uint32_t *cp, const uint32_t *style, const uint8_t *flags, int n, int brk, LigRun *out, int cap) {
    int nr = 0, i = 0;
    while (i < n && nr < cap) {
        if (!lig_cand(cp[i]) || (flags[i] & (LR_WIDE | LR_TAIL | LR_COMB)) || i == brk) { i++; continue; }
        int j = i + 1;
        bool sym = lig_sym(cp[i]);
        while (j < n && j - i < LIG_MAX_RUN && lig_cand(cp[j]) && !(flags[j] & (LR_WIDE | LR_TAIL | LR_COMB)) && j != brk && style[j] == style[i]) {
            sym = sym || lig_sym(cp[j]);
            j++;
        }
        if (j - i >= 2 && sym) { out[nr].start = i; out[nr].len = j - i; nr++; }
        i = j;
    }
    return nr;
}
#endif
