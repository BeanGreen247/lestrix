/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_ATLAS_H
#define SD_ATLAS_H
#include <stdbool.h>
#include <stdint.h>
#include "font.h"

typedef struct {
    uint16_t x, y, w, h;
    int16_t left, top;
    float adv;
    bool blank;
} AtlasGlyph;

typedef struct Atlas Atlas;

typedef struct { uint64_t glyph_hit, glyph_miss, glyph_recycle, row_hit, row_miss, row_recycle; } CacheStats;
extern CacheStats sd_cache;

uint32_t atlas_count(const Atlas *a);
int atlas_ascii_filled(const Atlas *a);
Atlas *atlas_new(int size);
void atlas_free(Atlas *a);
unsigned atlas_texture(const Atlas *a);
int atlas_size(const Atlas *a);
uint32_t atlas_generation(const Atlas *a);
void atlas_white(const Atlas *a, uint16_t *x, uint16_t *y);
const AtlasGlyph *atlas_glyph(Atlas *a, Font *f, uint32_t cp, int style);
const AtlasGlyph *atlas_custom(Atlas *a, uint32_t font_id, uint32_t key, const uint8_t *bits, int w, int h);
const AtlasGlyph *atlas_peek(const Atlas *a, uint32_t font_id, uint32_t cp, int style);
const AtlasGlyph *atlas_find_custom(Atlas *a, uint32_t font_id, uint32_t key);
#endif
