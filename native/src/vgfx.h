/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* Inline images for the terminal engine: the Sixel decoder, the kitty graphics protocol command parser and base64 decoder, and the
 * image/placement store with its memory cap. Header-only on purpose: vt.cpp includes it for the engine and tests/test_gfx.cpp includes
 * it to hammer the decoders directly under the sanitizers. Nothing here is reached by plain text: vt.cpp only calls in when a DCS or
 * APC string arrives. */

#ifndef SD_VGFX_H
#define SD_VGFX_H

#include "vpng.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <vector>

namespace vg {

constexpr int MAX_DIM = 16384;                    /* widest or tallest image accepted, in pixels */
constexpr size_t DEFAULT_CAP = (size_t)128 << 20; /* pixel memory per terminal, evicted least recently used first */
constexpr size_t SIXEL_MAX = (size_t)32 << 20;    /* one DCS payload */
constexpr size_t APC_MAX = (size_t)8 << 20;       /* one APC payload (a kitty chunk is normally 4 KiB) */
constexpr size_t KITTY_RAW_MAX = (size_t)160 << 20; /* an image being assembled from chunks (decoded bytes) */
constexpr size_t MAX_PLACEMENTS = 4096;

/* Pixels are RGBA bytes in memory order (R lowest), straight alpha: the layout RGBA() in render.h and a GL_RGBA upload both use. */
struct Image {
    std::atomic<int> refs{1};   /* the store holds one; each renderer frame that is drawing it holds another */
    uint32_t serial = 0;        /* never reused: renderers key their texture caches on it */
    uint32_t kid = 0;           /* kitty image id (i=), 0 for anonymous images such as sixels */
    uint32_t num = 0;           /* kitty image number (I=) */
    int w = 0, h = 0;
    uint32_t *px = nullptr;
    uint64_t stamp = 0;
    size_t bytes() const { return (size_t)w * (size_t)h * 4; }
};

inline void unref(Image *im) {
    if (!im) return;
    if (im->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) { free(im->px); delete im; }
}

/* w,h are checked against the dimension and byte limits by the caller (fits()); this allocates zeroed pixels. */
inline bool fits(uint64_t w, uint64_t h, size_t cap) { return w > 0 && h > 0 && w <= (uint64_t)MAX_DIM && h <= (uint64_t)MAX_DIM && w * h * 4 <= (uint64_t)cap; }

/* ------------------------------------------------------------------------------------------------------------------ Sixel */

enum { SX_OK = 0, SX_EMPTY = 1, SX_TOO_BIG = 2 };

static inline uint32_t rgba8(unsigned r, unsigned g, unsigned b) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | 0xff000000u; }

static inline uint32_t pct_rgb(int r, int g, int b) {
    auto c = [](int v) { v = v < 0 ? 0 : v > 100 ? 100 : v; return (unsigned)((v * 255 + 50) / 100); };
    return rgba8(c(r), c(g), c(b));
}

/* DEC sixel hue is rotated against the usual wheel: 0 = blue, 120 = red, 240 = green. */
static inline uint32_t hls_rgb(int h, int l, int s) {
    h = ((h % 360) + 360) % 360;
    h = (h + 240) % 360;
    l = l < 0 ? 0 : l > 100 ? 100 : l;
    s = s < 0 ? 0 : s > 100 ? 100 : s;
    double L = l / 100.0, S = s / 100.0, H = h / 60.0;
    double C = (1 - fabs(2 * L - 1)) * S;
    double X = C * (1 - fabs(fmod(H, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    switch ((int)H % 6) {
    case 0: r = C; g = X; break;
    case 1: r = X; g = C; break;
    case 2: g = C; b = X; break;
    case 3: g = X; b = C; break;
    case 4: r = X; b = C; break;
    default: r = C; b = X; break;
    }
    double m = L - C / 2;
    auto q = [](double v) { int i = (int)(v * 255 + 0.5); return (unsigned)(i < 0 ? 0 : i > 255 ? 255 : i); };
    return rgba8(q(r + m), q(g + m), q(b + m));
}

static const uint8_t SIXEL_DEFAULT16[16][3] = {{0, 0, 0}, {20, 20, 80}, {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80}, {80, 80, 20}, {53, 53, 53},
                                               {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33}, {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80}};

/* Reads up to 5 numbers separated by ';' starting at p[i]; returns how many were read and leaves i after them. Values clamp at 1e6. */
static inline int sx_nums(const uint8_t *p, size_t n, size_t &i, int *v, int max) {
    int k = 0;
    bool have = false;
    int cur = 0;
    for (; i < n; i++) {
        uint8_t c = p[i];
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); if (cur > 1000000) cur = 1000000; have = true; }
        else if (c == ';') { if (k < max) v[k] = have ? cur : 0; k++; cur = 0; have = false; }
        else break;
    }
    if (have || k) { if (k < max) v[k] = have ? cur : 0; k++; }
    return k < max ? k : max;
}

/* Decodes the part of a DCS q string after the 'q'. On SX_OK *px is malloc'd w*h RGBA with unpainted pixels transparent. The data is
 * scanned once to size the canvas (and to refuse anything over the dimension or byte limits before allocating) and once to paint. */
inline int sixel_decode(const uint8_t *p, size_t n, size_t cap, int *ow, int *oh, uint32_t **opx) {
    int maxw = 0, maxband = -1, x = 0, band = 0;
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        if (c == '"' || c == '#') { i++; int v[5]; sx_nums(p, n, i, v, 5); continue; }
        if (c == '!') {
            i++;
            int v[1] = {0};
            sx_nums(p, n, i, v, 1);
            if (i < n && p[i] >= 63 && p[i] <= 126) {
                if (v[0] < 1) v[0] = 1;
                if ((int64_t)x + v[0] > MAX_DIM) return SX_TOO_BIG;
                x += v[0];
                if (x > maxw) maxw = x;
                if (band > maxband) maxband = band;
                i++;
            }
            continue;
        }
        i++;
        if (c == '$') x = 0;
        else if (c == '-') { band++; x = 0; if ((int64_t)(band + 1) * 6 > MAX_DIM) return SX_TOO_BIG; }
        else if (c >= 63 && c <= 126) {
            if (++x > MAX_DIM) return SX_TOO_BIG;
            if (x > maxw) maxw = x;
            if (band > maxband) maxband = band;
        }
    }
    if (maxw <= 0 || maxband < 0) return SX_EMPTY;
    const int w = maxw, h = (maxband + 1) * 6;
    if (!fits((uint64_t)w, (uint64_t)h, cap)) return SX_TOO_BIG;
    uint32_t *px = static_cast<uint32_t *>(calloc((size_t)w * (size_t)h, 4));
    if (!px) return SX_TOO_BIG;
    uint32_t pal[256];
    for (int k = 0; k < 256; k++) pal[k] = k < 16 ? pct_rgb(SIXEL_DEFAULT16[k][0], SIXEL_DEFAULT16[k][1], SIXEL_DEFAULT16[k][2]) : rgba8(0, 0, 0);
    uint32_t col = pal[0];
    x = 0; band = 0;
    auto paint = [&](uint8_t c, int rep) {
        unsigned bits = (unsigned)(c - 63);
        if (!bits) { x += rep; return; }
        int x1 = x + rep > w ? w : x + rep;
        for (int k = 0; k < 6; k++) {
            if (!(bits & (1u << k))) continue;
            uint32_t *row = px + (size_t)(band * 6 + k) * (size_t)w;
            for (int xx = x; xx < x1; xx++) row[xx] = col;
        }
        x += rep;
    };
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        if (c == '"') { i++; int v[5]; sx_nums(p, n, i, v, 5); continue; }
        if (c == '#') {
            i++;
            int v[5] = {0, 0, 0, 0, 0};
            int k = sx_nums(p, n, i, v, 5);
            if (k >= 1 && v[0] >= 0 && v[0] < 256) {
                if (k >= 5) pal[v[0]] = v[1] == 1 ? hls_rgb(v[2], v[3], v[4]) : v[1] == 2 ? pct_rgb(v[2], v[3], v[4]) : pal[v[0]];
                col = pal[v[0]];
            }   /* an index past 255 selects and defines nothing: the current colour stays */
            continue;
        }
        if (c == '!') {
            i++;
            int v[1] = {0};
            sx_nums(p, n, i, v, 1);
            if (i < n && p[i] >= 63 && p[i] <= 126) { paint(p[i], v[0] < 1 ? 1 : v[0]); i++; }
            continue;
        }
        i++;
        if (c == '$') x = 0;
        else if (c == '-') { band++; x = 0; }
        else if (c >= 63 && c <= 126) paint(c, 1);
    }
    *ow = w; *oh = h; *opx = px;
    return SX_OK;
}

/* ------------------------------------------------------------------------------------------------------------------ base64 */

struct B64 {
    uint32_t acc = 0;
    int nbits = 0;
    bool bad = false, pad = false;
};

/* Appends decoded bytes to out, stopping with b.bad on a character outside the alphabet or data after padding; stops growing at max. */
inline void b64_feed(B64 &b, const uint8_t *s, size_t n, std::vector<uint8_t> &out, size_t max) {
    for (size_t i = 0; i < n && !b.bad; i++) {
        uint8_t c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') { b.pad = true; continue; }
        else if (c == '\n' || c == '\r' || c == ' ') continue;
        else { b.bad = true; break; }
        if (b.pad) { b.bad = true; break; }
        b.acc = (b.acc << 6) | (uint32_t)v;
        b.nbits += 6;
        if (b.nbits >= 8) {
            b.nbits -= 8;
            if (out.size() >= max) { b.bad = true; break; }
            out.push_back((uint8_t)(b.acc >> b.nbits));
            b.acc &= (1u << b.nbits) - 1;
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------ kitty graphics commands */

struct KCmd {
    char a = 't', t = 'd', o = 0, d = 'a';
    int f = 32, m = 0, q = 0, C = 0;
    uint32_t s = 0, v = 0, i = 0, I = 0, p = 0, c = 0, r = 0, x = 0, y = 0, w = 0, h = 0, S = 0, O = 0;
    int32_t z = 0;
    bool has_i = false, has_I = false;
};

inline void kitty_parse(const uint8_t *s, size_t n, KCmd *k) {
    size_t i = 0;
    while (i < n) {
        uint8_t key = s[i++];
        if (i >= n || s[i] != '=') { while (i < n && s[i] != ',') i++; if (i < n) i++; continue; }
        i++;
        size_t vs = i;
        while (i < n && s[i] != ',') i++;
        const uint8_t *val = s + vs;
        size_t vl = i - vs;
        if (i < n) i++;
        if (!vl) continue;
        bool neg = false;
        size_t j = 0;
        if (val[0] == '-') { neg = true; j = 1; }
        uint64_t num = 0;
        bool isnum = j < vl;
        for (size_t q = j; q < vl; q++) {
            if (val[q] < '0' || val[q] > '9') { isnum = false; break; }
            num = num * 10 + (uint64_t)(val[q] - '0');
            if (num > 0xffffffffull) num = 0xffffffffull;
        }
        uint32_t u = (uint32_t)num;
        if (neg && key != 'z') isnum = false;   /* only the z-index may be negative */
        switch (key) {
        case 'a': k->a = (char)val[0]; break;
        case 't': k->t = (char)val[0]; break;
        case 'o': k->o = (char)val[0]; break;
        case 'd': k->d = (char)val[0]; break;
        case 'f': if (isnum) k->f = (int)u; break;
        case 'm': if (isnum) k->m = (int)u; break;
        case 'q': if (isnum) k->q = (int)u; break;
        case 'C': if (isnum) k->C = (int)u; break;
        case 's': if (isnum) k->s = u; break;
        case 'v': if (isnum) k->v = u; break;
        case 'i': if (isnum) { k->i = u; k->has_i = true; } break;
        case 'I': if (isnum) { k->I = u; k->has_I = true; } break;
        case 'p': if (isnum) k->p = u; break;
        case 'c': if (isnum) k->c = u; break;
        case 'r': if (isnum) k->r = u; break;
        case 'x': if (isnum) k->x = u; break;
        case 'y': if (isnum) k->y = u; break;
        case 'w': if (isnum) k->w = u; break;
        case 'h': if (isnum) k->h = u; break;
        case 'S': if (isnum) k->S = u; break;
        case 'O': if (isnum) k->O = u; break;
        case 'z': if (isnum) k->z = neg ? -(int32_t)(u > 0x7fffffffu ? 0x7fffffffu : u) : (int32_t)(u > 0x7fffffffu ? 0x7fffffffu : u); break;
        default: break;
        }
    }
}

/* Turns decoded kitty pixel data (f=24 RGB or f=32 RGBA, s x v) into an RGBA image buffer. Returns nullptr when the data is too short,
 * the format is unknown or the size is over the limits; *err then says why in the words the protocol replies with. */
inline uint32_t *kitty_pixels(const uint8_t *d, size_t len, int f, uint32_t s, uint32_t v, size_t cap, const char **err) {
    if (f != 24 && f != 32) { *err = f == 100 ? "EINVAL:PNG data is not supported" : "EINVAL:unsupported pixel format"; return nullptr; }
    if (!fits(s, v, cap)) { *err = "EFBIG:image too large"; return nullptr; }
    const size_t bpp = f == 24 ? 3 : 4, need = (size_t)s * (size_t)v * bpp;
    if (len < need) { *err = "EINVAL:not enough image data"; return nullptr; }
    uint32_t *px = static_cast<uint32_t *>(malloc((size_t)s * (size_t)v * 4));
    if (!px) { *err = "ENOMEM:out of memory"; return nullptr; }
    const size_t cnt = (size_t)s * (size_t)v;
    if (bpp == 4) memcpy(px, d, cnt * 4);
    else for (size_t k = 0; k < cnt; k++) px[k] = rgba8(d[3 * k], d[3 * k + 1], d[3 * k + 2]);
    return px;
}

/* The whole kitty pixel pipeline: optional zlib (o=z), then raw RGB/RGBA (f=24/32) or a PNG (f=100, which brings its own size).
 * The image size is written through w and h. Returns malloc'd RGBA words or nullptr with *err set. */
inline uint32_t *kitty_decode(const uint8_t *d, size_t len, int f, char o, uint32_t s, uint32_t v, size_t cap, int *w, int *h, const char **err) {
    std::vector<uint8_t> z;
    if (o) {
        if (o != 'z') { *err = "EINVAL:unknown compression"; return nullptr; }
        const size_t lim = f == 100 ? cap : (size_t)s * (size_t)v * (size_t)(f == 24 ? 3 : 4);
        if (f != 100 && !fits(s, v, cap)) { *err = "EFBIG:image too large"; return nullptr; }
        if (!zl::inflate_zlib(d, len, z, lim)) { *err = "EINVAL:bad compressed data"; return nullptr; }
        d = z.data(); len = z.size();
    }
    if (f == 100) return png_decode(d, len, cap, (uint32_t)MAX_DIM, w, h, err);
    *w = (int)s; *h = (int)v;
    return kitty_pixels(d, len, f, s, v, cap, err);
}

/* ------------------------------------------------------------------------------------------------------------------ the store */

struct Place {
    Image *img = nullptr;
    uint32_t pid = 0;
    bool alt = false;
    uint64_t row = 0;           /* main screen: pushed + y at placement, so the image scrolls into the history with its rows; alt: y */
    int col = 0, cols = 1, rows = 1;
    int z = 0;
    bool stretch = false;       /* c/r given: fill cols x rows cells; otherwise natural pixel size from the top-left of the cell */
    uint32_t sx = 0, sy = 0, sw = 0, sh = 0;   /* source rectangle in pixels (full image when sw/sh are 0) */
    uint64_t seq = 0;
};

struct Store {
    std::vector<Image *> imgs;
    std::vector<Place> places;
    size_t bytes = 0, cap = DEFAULT_CAP;
    uint64_t clock = 0, seq = 0;
    uint32_t next_serial = 1, next_auto_id = 0x7f000000u;
    /* the string being collected */
    std::vector<uint8_t> buf;
    bool overflow = false;
    /* a chunked kitty transmission in progress */
    bool has_pend = false, skip = false;   /* skip: the chunks of a refused transmission are discarded until m=0 */
    size_t raw_limit = 0;                  /* most bytes the transmission in progress may carry, from its declared size */
    KCmd pend;
    std::vector<uint8_t> raw;
    B64 dec;

    ~Store() { for (Image *im : imgs) unref(im); }

    Image *by_kid(uint32_t id) const { for (Image *im : imgs) if (im->kid == id && id) return im; return nullptr; }
    Image *by_num(uint32_t n) const { for (Image *im : imgs) if (im->num == n && n) return im; return nullptr; }

    bool referenced(const Image *im) const { for (const Place &p : places) if (p.img == im) return true; return false; }

    void drop_image(Image *im) {
        for (size_t k = 0; k < places.size();) { if (places[k].img == im) places.erase(places.begin() + (long)k); else k++; }
        for (size_t k = 0; k < imgs.size(); k++) if (imgs[k] == im) { imgs.erase(imgs.begin() + (long)k); break; }
        bytes -= im->bytes();
        unref(im);
    }

    /* Anonymous images (sixels) live only as long as something shows them. */
    void sweep() {
        for (size_t k = 0; k < imgs.size();) {
            Image *im = imgs[k];
            if (!im->kid && !im->num && !referenced(im)) { bytes -= im->bytes(); imgs.erase(imgs.begin() + (long)k); unref(im); }
            else k++;
        }
    }

    /* Room for `need` more bytes: evicts the least recently used images (and their placements) until it fits. */
    bool make_room(size_t need, const Image *keep = nullptr) {
        if (need > cap) return false;
        while (bytes + need > cap) {
            Image *old = nullptr;
            for (Image *im : imgs) if (im != keep && (!old || im->stamp < old->stamp)) old = im;
            if (!old) return false;
            drop_image(old);
        }
        return true;
    }

    /* Takes ownership of px. Returns nullptr (and frees px) when it cannot fit. */
    Image *add_image(int w, int h, uint32_t *px, uint32_t kid, uint32_t num) {
        const size_t need = (size_t)w * (size_t)h * 4;
        if (!make_room(need)) { free(px); return nullptr; }
        Image *im = new Image();
        im->serial = next_serial++ & 0x7fffffffu;
        if (!im->serial) im->serial = next_serial++;
        im->kid = kid; im->num = num; im->w = w; im->h = h; im->px = px; im->stamp = ++clock;
        imgs.push_back(im);
        bytes += need;
        return im;
    }

    void add_place(const Place &pl) {
        if (places.size() >= MAX_PLACEMENTS) { places.erase(places.begin()); sweep(); }
        Place q = pl;
        q.seq = ++seq;
        places.push_back(q);
        pl.img->stamp = ++clock;
    }
};

}  // namespace vg

#endif
