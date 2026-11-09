/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "../src/ui/font.h"
#include "../src/ui/ligrun.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    Font *f = font_open("monospace", 16, true);
    if (!f) { printf("test_font: no font available, skipped\n"); return 0; }
    GlyphBmp b;
    CHECK(font_glyph_color(f, 'A', FS_REGULAR, &b, true) && !b.color && b.w > 0);
    /* colour emoji: only when a colour bitmap font is installed */
    bool have = font_glyph_color(f, 0x1F600, FS_REGULAR, &b, true) && b.color;
    if (have) {
        CHECK(b.w > 0 && b.h > 0 && b.w <= 2 * font_cell_w(f) && b.h <= font_cell_h(f));
        int opaque = 0, coloured = 0;
        for (int i = 0; i < b.w * b.h; i++) {
            const uint8_t *p = b.buf + i * 4;
            if (p[3] > 200) opaque++;
            if (p[3] > 200 && (p[0] != p[1] || p[1] != p[2])) coloured++;
        }
        CHECK(opaque > b.w * b.h / 4);
        CHECK(coloured > 0);
        /* without a colour atlas the same glyph degrades to coverage */
        CHECK(font_glyph_color(f, 0x1F600, FS_REGULAR, &b, false) && !b.color && b.w > 0);
    } else printf("test_font: no colour emoji font, colour checks skipped\n");
    font_close(f);

    /* ligature run splitting: pure logic */
    {
        auto split = [](const char *txt, const uint32_t *sty, const uint8_t *flg, int brk, LigRun *out) {
            uint32_t cp[64];
            int n = (int)strlen(txt);
            for (int i = 0; i < n; i++) cp[i] = (uint8_t)txt[i];
            uint32_t zero[64] = {0};
            uint8_t nf[64] = {0};
            return lig_split(cp, sty ? sty : zero, flg ? flg : nf, n, brk, out, 8);
        };
        LigRun r[8];
        CHECK(split("a->b", NULL, NULL, -1, r) == 1 && r[0].start == 0 && r[0].len == 4);
        CHECK(split("ab cd", NULL, NULL, -1, r) == 0);              /* letters only: nothing to shape */
        CHECK(split("x == y", NULL, NULL, -1, r) == 1 && r[0].start == 2 && r[0].len == 2);
        CHECK(split("a->b", NULL, NULL, 2, r) == 1 && r[0].start == 0 && r[0].len == 2);   /* cursor on the '>' splits the run; "a-" still shapes */
        CHECK(split("a->b", NULL, NULL, 1, r) == 1 && r[0].start == 2 && r[0].len == 2);   /* cursor on the '-' leaves "a" alone and "> b" ... */
        CHECK(split("=>", NULL, NULL, 0, r) == 0 && split("=>", NULL, NULL, 1, r) == 0);   /* the cursor cell is never part of a run */
        uint32_t sty[8] = {1, 1, 2, 2, 2, 2};
        CHECK(split("==!==!", sty, NULL, -1, r) == 2 && r[0].len == 2 && r[1].start == 2 && r[1].len == 4);   /* style change ends a run */
        uint8_t fl[8] = {0, LR_WIDE, LR_TAIL, 0, 0, 0};
        CHECK(split("-->-->", NULL, fl, -1, r) == 1 && r[0].start == 3 && r[0].len == 3);   /* wide cell, its tail: not shaped */
        uint32_t cp2[3] = {'-', ' ', '>'};
        uint32_t z[3] = {0};
        uint8_t nf2[3] = {0};
        CHECK(lig_split(cp2, z, nf2, 3, -1, r, 8) == 0);            /* a space ends a run */
    }

    /* shaping against a real ligature font when one is installed */
    for (const char *fam : {"Fira Code", "JetBrains Mono", "Cascadia Code NF"}) {
        char *pick = NULL;
        const char *one[2] = {fam, NULL};
        pick = font_pick_family(one);
        if (!pick) continue;
        Font *lf = font_open(pick, 16, true);
        free(pick);
        if (!lf) continue;
        uint32_t arrow[2] = {'-', '>'}, plain[2] = {'a', 'b'};
        ShapedGlyph g[8];
        int na = font_shape(lf, arrow, 2, FS_REGULAR, g, 8, true);
        int np = font_shape(lf, plain, 2, FS_REGULAR, g, 8, true);
        CHECK(np == 0);
        if (na > 0) {
            CHECK(font_has_ligatures(lf));
            ShapedGlyph h[8];
            font_shape(lf, arrow, 2, FS_REGULAR, h, 8, true);
            GlyphBmp gb;
            bool inked = false;   /* some of the glyphs is drawn (a calt font may give the first cell an empty spacer) */
            for (int i = 0; i < na; i++) if (font_glyph_gid(lf, h[i].gid, FS_REGULAR, &gb) && gb.w > 0) inked = true;
            CHECK(inked);
            ShapedGlyph c2[8];   /* the per-thread shape cache: a repeat must give exactly what the first call gave */
            int nc = font_shape(lf, arrow, 2, FS_REGULAR, c2, 8, false);
            CHECK(nc == na);
            for (int i = 0; i < na && nc == na; i++) CHECK(c2[i].gid == h[i].gid && c2[i].cell == h[i].cell && c2[i].span == h[i].span);
            CHECK(font_shape(lf, arrow, 2, FS_BOLD, g, 8, true) >= 0);
            /* the worker-thread form refuses rather than loading a face that is not there yet */
            Font *lf2 = font_open(fam, 16, true);
            if (lf2) { CHECK(font_shape(lf2, arrow, 2, FS_REGULAR, g, 8, false) == -1); font_close(lf2); }
            printf("test_font: ligatures verified with %s (->: %d glyph(s), cell %d span %d)\n", fam, na, h[0].cell, h[0].span);
        } else printf("test_font: %s shapes no ligature for '->' (no calt/liga), skipped\n", fam);
        font_close(lf);
    }
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
