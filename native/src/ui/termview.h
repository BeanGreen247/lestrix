/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_TERMVIEW_H
#define SD_TERMVIEW_H
#include <stdbool.h>
#include "render.h"
#include "scan.h"
#include "tcore.h"

typedef struct {
    uint32_t pal[256];
    uint32_t fg, bg, sel, cursor;
    uint32_t epoch;
} TermPalette;

/* Drawn over the rows without touching their cached geometry: the scrollback finder's matches and the hovered link. */
typedef struct {
    const FindMatch *matches; int nmatches; int current;   /* matches newest first as Finder returns them, current = highlighted one or -1 */
    bool link; int link_idx, link_c0, link_c1;              /* hovered link: line index as vt_line, columns inclusive */
} TvOverlay;
/* Ligatures (HarfBuzz shaping of symbol runs) on or off; off costs nothing per row. */
void tv_set_ligatures(bool on);
bool tv_ligatures();
void tv_draw(TermCore *t, Renderer *r, Font *f, const TermPalette *pal, float x, float y, float w, float h, bool cursor_blink_on, const TvOverlay *ov = nullptr);
void tv_fit(const Font *f, float w, float h, int *cols, int *rows);
#define TV_PAD 4
#endif
