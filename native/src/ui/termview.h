/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_TERMVIEW_H
#define SD_TERMVIEW_H
#include <stdbool.h>
#include "render.h"
#include "tcore.h"

typedef struct {
    uint32_t pal[256];
    uint32_t fg, bg, sel, cursor;
    uint32_t epoch;
} TermPalette;

void tv_draw(TermCore *t, Renderer *r, Font *f, const TermPalette *pal, float x, float y, float w, float h, bool cursor_blink_on);
void tv_fit(const Font *f, float w, float h, int *cols, int *rows);
#define TV_PAD 4
#endif
