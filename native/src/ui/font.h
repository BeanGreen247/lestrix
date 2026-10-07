/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_FONT_H
#define SD_FONT_H
#include <stdbool.h>
#include <stdint.h>

enum { FS_REGULAR = 0, FS_BOLD = 1, FS_ITALIC = 2, FS_BOLD_ITALIC = 3 };

typedef struct Font Font;

typedef struct {
    uint8_t *buf;
    int w, h;
    int left, top;
    float adv;
} GlyphBmp;

Font *font_open(const char *family, double px, bool mono);
void font_close(Font *f);
uint32_t font_id(const Font *f);
double font_px(const Font *f);
int font_cell_w(const Font *f);
int font_cell_h(const Font *f);
int font_ascent(const Font *f);
bool font_glyph(Font *f, uint32_t cp, int style, GlyphBmp *out);
float font_advance(Font *f, uint32_t cp, int style);
char *font_pick_family(const char *const *wanted);
void font_prewarm(void);
#endif
