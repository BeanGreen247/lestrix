/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>

#include "../src/ui/font.h"

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
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
