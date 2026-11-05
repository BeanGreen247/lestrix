/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_SOFT_H
#define SD_SOFT_H
#include <stddef.h>
#include <stdint.h>
#include "render.h"

/* CPU rasteriser for the same RInst stream the GL path draws: solid rects, rounded and bordered rects (the shader's
 * SDF), mono glyphs from the 8-bit atlas, colour glyphs from the RGBA atlas. Used when there is no real GPU. */
typedef struct {
    uint32_t *px;
    int w, h, pitch;          /* pixels, pitch in pixels */
    int rs, gs, bs;           /* channel shifts of the 32-bit surface format */
    uint32_t amask;           /* bits that are not colour (set to opaque on clear) */
} SoftTarget;

typedef struct { int on, x, y, w, h; } SoftClip;   /* top-left origin, pixels */

void soft_clear(const SoftTarget *t, uint32_t rgba);
void soft_clear_rect(const SoftTarget *t, int x0, int y0, int x1, int y1, uint32_t rgba);   /* half-open, clipped to the target */
void soft_draw(const SoftTarget *t, const SoftClip *clip, const RInst *in, size_t n,
               const uint8_t *atlas, int asize, const uint32_t *catlas, int csize);
#endif
