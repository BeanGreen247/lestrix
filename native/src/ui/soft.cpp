/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "soft.h"

#include <math.h>
#include <string.h>

namespace {

struct Fmt { int rs, gs, bs; };

static inline uint32_t pack(const Fmt &f, uint32_t r, uint32_t g, uint32_t b) { return r << f.rs | g << f.gs | b << f.bs; }

/* dst = src * a + dst * (1 - a), a in 0..255; exact to within one level */
static inline uint32_t mix(uint32_t s, uint32_t d, uint32_t a) {
    uint32_t t = s * a + d * (255 - a) + 128;
    return (t + (t >> 8)) >> 8;
}

static inline void blend_px(uint32_t *d, const Fmt &f, uint32_t sr, uint32_t sg, uint32_t sb, uint32_t a) {
    uint32_t v = *d;
    uint32_t dr = (v >> f.rs) & 255, dg = (v >> f.gs) & 255, db = (v >> f.bs) & 255;
    *d = (v & ~(255u << f.rs | 255u << f.gs | 255u << f.bs)) | pack(f, mix(sr, dr, a), mix(sg, dg, a), mix(sb, db, a));
}

static inline int dec(uint16_t u, int size) { return (int)(((uint32_t)u * (uint32_t)size + 32767u) / 65535u); }
static inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

struct Rect { int x0, y0, x1, y1; };   /* half-open */

static inline Rect isect(Rect a, Rect b) {
    return {a.x0 > b.x0 ? a.x0 : b.x0, a.y0 > b.y0 ? a.y0 : b.y0, a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1};
}

static void fill_span(uint32_t *row, int n, uint32_t color) {
    for (int i = 0; i < n; i++) row[i] = color;
}

/* rounded / bordered rectangle, the fragment shader's signed distance field evaluated per pixel near the edge */
static void draw_rrect(const SoftTarget *t, const Fmt &f, const RInst &in, Rect r, int ix0, int iy0, uint32_t cr, uint32_t cg,
                       uint32_t cb, uint32_t ca) {
    float rad_in = (float)(in.pad & 255), border = (float)((in.pad >> 8) & 255);
    float hx = 0.5f * in.w, hy = 0.5f * in.h;
    float rad = rad_in < hx ? rad_in : hx;
    if (rad > hy) rad = hy;
    float band = rad + border + 1.f;
    uint32_t solid = pack(f, cr, cg, cb);
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = t->px + (size_t)y * t->pitch;
        float ly = (float)y + 0.5f - in.y;
        float dy = ly < in.h - ly ? ly : in.h - ly;
        int ia = 0;   /* interior x range that needs no SDF */
        int xa = r.x1, xb = r.x1;
        if (dy > band) {
            xa = (int)ceilf(in.x + band - 0.5f); xb = (int)ceilf(in.x + in.w - band - 0.5f);
            if (xa < r.x0) xa = r.x0;
            if (xb > r.x1) xb = r.x1;
            if (xb < xa) xb = xa;
            ia = 1;
        }
        for (int x = r.x0; x < r.x1; x++) {
            if (ia && x == xa) {
                if (border == 0.f) {
                    if (ca == 255) fill_span(row + xa, xb - xa, solid);
                    else for (int k = xa; k < xb; k++) blend_px(row + k, f, cr, cg, cb, ca);
                }
                x = xb - 1;
                continue;
            }
            float lx = (float)x + 0.5f - in.x;
            float qx = fabsf(lx - hx) - (hx - rad), qy = fabsf(ly - hy) - (hy - rad);
            float mx = qx > 0.f ? qx : 0.f, my = qy > 0.f ? qy : 0.f;
            float m = qx > qy ? qx : qy;
            float d = sqrtf(mx * mx + my * my) + (m < 0.f ? m : 0.f) - rad;
            float cov = clamp01(0.5f - d);
            if (border > 0.f) { float inner = clamp01(0.5f - (d + border)); cov = cov - inner; if (cov < 0.f) cov = 0.f; }
            uint32_t a = (uint32_t)(cov * (float)ca + 0.5f);
            if (a == 0) continue;
            if (a >= 255) row[x] = solid; else blend_px(row + x, f, cr, cg, cb, a);
        }
    }
    (void)ix0; (void)iy0;
}

}  // namespace

void soft_clear(const SoftTarget *t, uint32_t rgba) {
    Fmt f{t->rs, t->gs, t->bs};
    uint32_t c = pack(f, rgba & 255, (rgba >> 8) & 255, (rgba >> 16) & 255) | t->amask;
    for (int y = 0; y < t->h; y++) fill_span(t->px + (size_t)y * t->pitch, t->w, c);
}

void soft_draw(const SoftTarget *t, const SoftClip *clip, const RInst *in, size_t n, const uint8_t *atlas, int asize,
               const uint32_t *catlas, int csize) {
    const Fmt f{t->rs, t->gs, t->bs};
    Rect bounds{0, 0, t->w, t->h};
    if (clip && clip->on) bounds = isect(bounds, Rect{clip->x, clip->y, clip->x + clip->w, clip->y + clip->h});
    if (bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0) return;
    for (size_t k = 0; k < n; k++) {
        const RInst &q = in[k];
        uint32_t ca = q.rgba >> 24;
        if (!ca || q.w <= 0.f || q.h <= 0.f) continue;
        uint32_t cr = q.rgba & 255, cg = (q.rgba >> 8) & 255, cb = (q.rgba >> 16) & 255;
        int ix0 = (int)ceilf(q.x - 0.5f), iy0 = (int)ceilf(q.y - 0.5f);
        int ix1 = (int)ceilf(q.x + q.w - 0.5f), iy1 = (int)ceilf(q.y + q.h - 0.5f);
        Rect r = isect(Rect{ix0, iy0, ix1, iy1}, bounds);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        bool plain = q.u0 == q.u1 && q.v0 == q.v1;
        if (plain) {
            if (q.pad & 0xffff) { draw_rrect(t, f, q, r, ix0, iy0, cr, cg, cb, ca); continue; }
            uint32_t solid = pack(f, cr, cg, cb);
            for (int y = r.y0; y < r.y1; y++) {
                uint32_t *row = t->px + (size_t)y * t->pitch;
                if (ca == 255) fill_span(row + r.x0, r.x1 - r.x0, solid);
                else for (int x = r.x0; x < r.x1; x++) blend_px(row + x, f, cr, cg, cb, ca);
            }
            continue;
        }
        bool color = ((q.pad >> 16) & 255) >= 128 && catlas;
        const int size = color ? csize : asize;
        if (!size || (!color && !atlas)) continue;
        int tx0 = dec(q.u0, size), ty0 = dec(q.v0, size), tx1 = dec(q.u1, size), ty1 = dec(q.v1, size);
        int sw = tx1 - tx0, sh = ty1 - ty0;
        if (sw <= 0 || sh <= 0) continue;
        bool one = sw == ix1 - ix0 && sh == iy1 - iy0;   /* 1:1 copy: the common case for glyphs */
        for (int y = r.y0; y < r.y1; y++) {
            uint32_t *row = t->px + (size_t)y * t->pitch;
            int ty = one ? ty0 + (y - iy0) : ty0 + (int)(((float)y + 0.5f - q.y) / q.h * (float)sh);
            if (ty < ty0) ty = ty0;
            if (ty >= ty1) ty = ty1 - 1;
            if (!color) {
                const uint8_t *src = atlas + (size_t)ty * asize;
                for (int x = r.x0; x < r.x1; x++) {
                    int tx = one ? tx0 + (x - ix0) : tx0 + (int)(((float)x + 0.5f - q.x) / q.w * (float)sw);
                    if (tx >= tx1) tx = tx1 - 1;
                    uint32_t cov = src[tx];
                    if (!cov) continue;
                    uint32_t a = ca == 255 ? cov : (cov * ca * 257u + 32768u) >> 16;
                    if (a >= 255) row[x] = pack(f, cr, cg, cb) | (row[x] & t->amask);
                    else if (a) blend_px(row + x, f, cr, cg, cb, a);
                }
            } else {
                const uint32_t *src = catlas + (size_t)ty * csize;
                for (int x = r.x0; x < r.x1; x++) {
                    int tx = one ? tx0 + (x - ix0) : tx0 + (int)(((float)x + 0.5f - q.x) / q.w * (float)sw);
                    if (tx >= tx1) tx = tx1 - 1;
                    uint32_t p = src[tx], pa = p >> 24;
                    if (!pa) continue;
                    uint32_t a = ca == 255 ? pa : (pa * ca * 257u + 32768u) >> 16;
                    if (a) blend_px(row + x, f, p & 255, (p >> 8) & 255, (p >> 16) & 255, a);
                }
            }
        }
    }
}
