/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_RENDER_H
#define SD_RENDER_H
#include <stddef.h>
#include <stdint.h>
#include "atlas.h"

typedef struct { float x, y, w, h; uint16_t u0, v0, u1, v1; uint32_t rgba; uint32_t pad; } RInst;

typedef struct Renderer Renderer;

Renderer *r_new(Atlas *a);
void r_free(Renderer *r);
Atlas *r_atlas(Renderer *r);
/* software rendering: the 32-bit surface the next frame is drawn into (pitch in pixels, channel shifts, non-colour bits) */
void r_set_target(Renderer *r, uint32_t *px, int w, int h, int pitch_px, int rs, int gs, int bs, uint32_t amask);
/* software rendering, changed-region drawing: after r_end the renderer knows which pixel rectangles it repainted. A frame
 * with no change returns none (nothing to present). r_soft_invalidate forces a full repaint (window exposed or resized). */
typedef struct { int x, y, w, h; } RDamage;
const RDamage *r_damage(const Renderer *r, size_t *n);
void r_soft_invalidate(Renderer *r);
void r_begin(Renderer *r, int w, int h, uint32_t clear_rgba);
void r_end(Renderer *r);
void r_flush(Renderer *r);
void r_clip(Renderer *r, int x, int y, int w, int h);
void r_clip_off(Renderer *r);
void r_rect(Renderer *r, float x, float y, float w, float h, uint32_t rgba);
void r_rrect(Renderer *r, float x, float y, float w, float h, uint32_t rgba, int radius, int border);
void r_push(Renderer *r, const RInst *inst, int n);
void r_push_at(Renderer *r, const RInst *inst, int n, float dx, float dy);
size_t r_mark(const Renderer *r);
void r_rewind(Renderer *r, size_t mark);
/* Inline images. px is iw x ih RGBA bytes (straight alpha); serial names that pixel data and is never reused, so a changed image is a new
 * serial. The rectangle x,y,w,h shows the (u0,v0)-(u1,v1) part (0..1) of it. owner/release keep the pixels alive until the frame is drawn:
 * release(owner) is called exactly once, when the renderer is done with them. Needs GL 3.3 / ES 3 or the CPU renderer; on ES 2 nothing is drawn. */
void r_image(Renderer *r, uint32_t serial, const uint32_t *px, int iw, int ih, void *owner, void (*release)(void *), float x, float y, float w, float h,
             float u0, float v0, float u1, float v1);
void r_make_rect(Renderer *r, RInst *o, float x, float y, float w, float h, uint32_t rgba);
void r_make_glyph(Renderer *r, RInst *o, const AtlasGlyph *g, float pen_x, float baseline_y, uint32_t rgba);
#define RGBA(r, g, b, a) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | (uint32_t)(a) << 24)
#endif
