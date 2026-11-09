/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* A small, self-contained, bounds-checked zlib inflater and PNG decoder for the kitty graphics protocol (f=100, o=z).
 * 8-bit gray, gray+alpha, RGB, RGBA and palette (with tRNS) images, no interlacing. Checksums are not verified: the
 * terminal only needs pixels, and every read and write below is checked against the buffer sizes and the caller's caps. */

#ifndef SD_VPNG_H
#define SD_VPNG_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace vg {

namespace zl {

struct Bits {
    const uint8_t *p;
    size_t n, pos;
    uint32_t buf;
    int cnt;
    bool err;
};

static inline uint32_t getbits(Bits &b, int need) {
    uint32_t v = b.buf;
    while (b.cnt < need) {
        if (b.pos >= b.n) { b.err = true; return 0; }
        v |= (uint32_t)b.p[b.pos++] << b.cnt;
        b.cnt += 8;
    }
    b.buf = v >> need;
    b.cnt -= need;
    return v & ((1u << need) - 1u);
}

struct Huff {
    uint16_t count[16];
    uint16_t symbol[288];
};

/* Returns false when the code lengths over-subscribe the code (incomplete codes are allowed, as zlib allows a lone distance code). */
static inline bool build(Huff &h, const uint8_t *len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return false;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + h.count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h.symbol[offs[len[i]]++] = (uint16_t)i;
    return true;
}

static inline int decode(Bits &b, const Huff &h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)getbits(b, 1);
        if (b.err) return -1;
        int c = h.count[l];
        if (code - c < first) return h.symbol[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static inline bool codes(Bits &b, std::vector<uint8_t> &out, size_t max, const Huff &lc, const Huff &dc) {
    for (;;) {
        int s = decode(b, lc);
        if (s < 0) return false;
        if (s < 256) {
            if (out.size() >= max) return false;
            out.push_back((uint8_t)s);
        } else if (s == 256) {
            return true;
        } else {
            s -= 257;
            if (s >= 29) return false;
            size_t len = (size_t)LBASE[s] + getbits(b, LEXT[s]);
            int ds = decode(b, dc);
            if (ds < 0 || ds >= 30) return false;
            size_t dist = (size_t)DBASE[ds] + getbits(b, DEXT[ds]);
            if (b.err || dist > out.size() || out.size() + len > max) return false;
            size_t from = out.size() - dist;
            for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
        }
    }
}

/* Raw deflate stream -> out (at most max bytes). */
static inline bool inflate_raw(const uint8_t *src, size_t n, std::vector<uint8_t> &out, size_t max) {
    Bits b{src, n, 0, 0, 0, false};
    int last;
    do {
        last = (int)getbits(b, 1);
        uint32_t type = getbits(b, 2);
        if (b.err) return false;
        if (type == 0) {
            b.buf = 0; b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            uint32_t len = b.p[b.pos] | (uint32_t)b.p[b.pos + 1] << 8, nlen = b.p[b.pos + 2] | (uint32_t)b.p[b.pos + 3] << 8;
            b.pos += 4;
            if ((len ^ 0xffffu) != nlen || b.pos + len > b.n || out.size() + len > max) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1) {
            struct Fixed {
                Huff lc, dc;
                Fixed() {
                    uint8_t l[288];
                    for (int i = 0; i < 144; i++) l[i] = 8;
                    for (int i = 144; i < 256; i++) l[i] = 9;
                    for (int i = 256; i < 280; i++) l[i] = 7;
                    for (int i = 280; i < 288; i++) l[i] = 8;
                    build(lc, l, 288);
                    for (int i = 0; i < 30; i++) l[i] = 5;
                    build(dc, l, 30);
                }
            };
            static const Fixed fx;   /* thread-safe one-time init */
            const Huff &lc = fx.lc, &dc = fx.dc;
            if (!codes(b, out, max, lc, dc)) return false;
        } else if (type == 2) {
            static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            int nlen = (int)getbits(b, 5) + 257, ndist = (int)getbits(b, 5) + 1, ncode = (int)getbits(b, 4) + 4;
            if (b.err || nlen > 286 || ndist > 30) return false;
            uint8_t lens[320] = {0};
            for (int i = 0; i < ncode; i++) lens[ORDER[i]] = (uint8_t)getbits(b, 3);
            Huff lencode;
            if (b.err || !build(lencode, lens, 19)) return false;
            // build() reads exactly 19 entries; clear and reuse the array for the real lengths
            uint8_t cl[320] = {0};
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode(b, lencode);
                if (sym < 0) return false;
                if (sym < 16) cl[idx++] = (uint8_t)sym;
                else {
                    int rep, val = 0;
                    if (sym == 16) { if (idx == 0) return false; val = cl[idx - 1]; rep = 3 + (int)getbits(b, 2); }
                    else if (sym == 17) rep = 3 + (int)getbits(b, 3);
                    else rep = 11 + (int)getbits(b, 7);
                    if (b.err || idx + rep > nlen + ndist) return false;
                    while (rep--) cl[idx++] = (uint8_t)val;
                }
            }
            if (cl[256] == 0) return false;
            Huff lc, dc;
            if (!build(lc, cl, nlen) || !build(dc, cl + nlen, ndist)) return false;
            if (!codes(b, out, max, lc, dc)) return false;
        } else {
            return false;
        }
        if (b.err) return false;
    } while (!last);
    return true;
}

/* zlib wrapper (RFC 1950): 2-byte header, deflate stream, Adler-32 (not verified). */
static inline bool inflate_zlib(const uint8_t *src, size_t n, std::vector<uint8_t> &out, size_t max) {
    if (n < 2 || (src[0] & 0x0f) != 8 || (src[0] >> 4) > 7 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return false;
    return inflate_raw(src + 2, n - 2, out, max);
}

}  // namespace zl

static inline uint32_t png_u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Decodes a PNG into malloc'd RGBA words (r | g << 8 | b << 16 | a << 24). Returns nullptr and sets *err (protocol wording) on failure.
 * max_px is the most pixels bytes (w * h * 4) the caller accepts; dimension limits come from max_dim. */
static inline uint32_t *png_decode(const uint8_t *d, size_t len, size_t max_bytes, uint32_t max_dim, int *ow, int *oh, const char **err) {
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (len < 8 + 25 || memcmp(d, SIG, 8) != 0) { *err = "EBADPNG:not a PNG"; return nullptr; }
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int ctype = -1, chans = 0;
    bool have_ihdr = false, have_end = false;
    uint8_t pal[256][4];
    int npal = 0;
    for (int i = 0; i < 256; i++) { pal[i][0] = pal[i][1] = pal[i][2] = 0; pal[i][3] = 255; }
    std::vector<uint8_t> idat;
    while (pos + 8 <= len && !have_end) {
        uint32_t cl = png_u32(d + pos);
        const uint8_t *tag = d + pos + 4, *body = d + pos + 8;
        if ((size_t)cl > len - pos - 8 || len - pos - 8 - (size_t)cl < 4) { *err = "EBADPNG:truncated chunk"; return nullptr; }
        if (!memcmp(tag, "IHDR", 4)) {
            if (cl != 13 || have_ihdr) { *err = "EBADPNG:bad IHDR"; return nullptr; }
            w = png_u32(body); h = png_u32(body + 4);
            int depth = body[8];
            ctype = body[9];
            if (depth != 8 || body[10] != 0 || body[11] != 0 || body[12] != 0) { *err = "EBADPNG:only 8-bit non-interlaced PNG is supported"; return nullptr; }
            chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
            if (!chans) { *err = "EBADPNG:bad colour type"; return nullptr; }
            if (w == 0 || h == 0 || w > max_dim || h > max_dim || (uint64_t)w * h * 4 > (uint64_t)max_bytes) { *err = "EFBIG:image too large"; return nullptr; }
            have_ihdr = true;
        } else if (!memcmp(tag, "PLTE", 4)) {
            if (cl % 3 || cl > 768) { *err = "EBADPNG:bad PLTE"; return nullptr; }
            npal = (int)(cl / 3);
            for (int i = 0; i < npal; i++) { pal[i][0] = body[3 * i]; pal[i][1] = body[3 * i + 1]; pal[i][2] = body[3 * i + 2]; }
        } else if (!memcmp(tag, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < cl && i < 256; i++) pal[i][3] = body[i];
        } else if (!memcmp(tag, "IDAT", 4)) {
            if (!have_ihdr || idat.size() + cl > len) { *err = "EBADPNG:bad IDAT"; return nullptr; }
            idat.insert(idat.end(), body, body + cl);
        } else if (!memcmp(tag, "IEND", 4)) {
            have_end = true;
        }
        pos += 12 + (size_t)cl;
    }
    if (!have_ihdr || idat.empty()) { *err = "EBADPNG:no image data"; return nullptr; }
    if (ctype == 3 && npal == 0) { *err = "EBADPNG:missing palette"; return nullptr; }
    const size_t stride = (size_t)w * (size_t)chans, need = (size_t)h * (stride + 1);
    std::vector<uint8_t> raw;
    raw.reserve(need);
    if (!zl::inflate_zlib(idat.data(), idat.size(), raw, need) || raw.size() != need) { *err = "EBADPNG:bad compressed data"; return nullptr; }
    /* unfilter in place, row by row */
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    uint32_t *px = static_cast<uint32_t *>(malloc((size_t)w * (size_t)h * 4));
    if (!px) { *err = "ENOMEM:out of memory"; return nullptr; }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = raw.data() + (size_t)y * (stride + 1);
        int ft = row[0];
        const uint8_t *src = row + 1;
        const size_t bpp = (size_t)chans;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = src[i], v;
            switch (ft) {
            case 0: v = x; break;
            case 1: v = x + a; break;
            case 2: v = x + b; break;
            case 3: v = x + ((a + b) >> 1); break;
            case 4: {
                int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
                v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
                break;
            }
            default: free(px); *err = "EBADPNG:bad filter"; return nullptr;
            }
            cur[i] = (uint8_t)v;
        }
        uint32_t *o = px + (size_t)y * w;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = cur.data() + (size_t)x * bpp;
            uint32_t r, g, b, a = 255;
            if (ctype == 0) r = g = b = s[0];
            else if (ctype == 4) { r = g = b = s[0]; a = s[1]; }
            else if (ctype == 2) { r = s[0]; g = s[1]; b = s[2]; }
            else if (ctype == 6) { r = s[0]; g = s[1]; b = s[2]; a = s[3]; }
            else { const uint8_t *q = pal[s[0]]; r = q[0]; g = q[1]; b = q[2]; a = q[3]; }
            o[x] = r | g << 8 | b << 16 | a << 24;
        }
        prev.swap(cur);
    }
    *ow = (int)w; *oh = (int)h;
    return px;
}

}  // namespace vg

#endif
