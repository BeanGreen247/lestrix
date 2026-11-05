/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "atlas.h"

#include <stdlib.h>
#include <string.h>

#include "gl.h"

typedef struct { uint32_t font, cp; uint8_t style, used; AtlasGlyph g; } Entry;

static inline GLenum atlas_format(void) { return sd_gl_kind == GLK_ES2 ? GL_LUMINANCE : GL_RED; }
static inline GLint atlas_internal(void) { return sd_gl_kind == GLK_ES2 ? GL_LUMINANCE : GL_R8; }

struct Atlas {
    GLuint tex;
    int size;
    uint32_t generation;
    int shelf_y, shelf_h, pen_x;
    Entry *tab;
    uint32_t cap, count;
    uint16_t white_x, white_y;
    GLuint ctex;            /* RGBA colour glyphs (emoji), created on first use; not on ES2 */
    int csize, cshelf_y, cshelf_h, cpen_x;
    uint8_t *cpu;           /* software rendering: atlas pixels in memory instead of a GL texture */
    uint32_t *ccpu;
    struct { const AtlasGlyph *g; uint32_t gen, font; } ascii[4][128];
};

#define CUSTOM_STYLE 0xff

CacheStats sd_cache;

uint32_t atlas_count(const Atlas *a) { return a->count; }

int atlas_ascii_filled(const Atlas *a) {
    int n = 0;
    for (int s = 0; s < 4; s++) for (int c = 0; c < 128; c++) if (a->ascii[s][c].g && a->ascii[s][c].gen == a->generation) n++;
    return n;
}

static void atlas_reset(Atlas *a) {
    sd_cache.glyph_recycle += a->count;
    memset(a->tab, 0, (size_t)a->cap * sizeof *a->tab);
    a->count = 0;
    a->shelf_y = 2; a->shelf_h = 0; a->pen_x = 0;
    a->generation++;
    a->cshelf_y = a->cshelf_h = a->cpen_x = 0;
    static const uint8_t white[4] = {255, 255, 255, 255};
    if (sd_soft) {
        for (int y = 0; y < 2; y++) memset(a->cpu + (size_t)y * a->size, 255, 2);
    } else {
        glBindTexture(GL_TEXTURE_2D, a->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, atlas_format(), GL_UNSIGNED_BYTE, white);
    }
    a->pen_x = 4;
    a->shelf_h = 2;
}

Atlas *atlas_new(int size) {
    GLint maxtex = 0;
    if (!sd_soft) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    if (maxtex > 0 && size > maxtex) size = maxtex;
    Atlas *a = static_cast<Atlas*>(calloc(1, sizeof *a));
    if (!a) return NULL;
    a->size = size;
    a->cap = 16384;
    a->tab = static_cast<Entry*>(calloc(a->cap, sizeof *a->tab));
    if (!a->tab) { free(a); return NULL; }
    if (sd_soft) {
        a->cpu = static_cast<uint8_t*>(calloc((size_t)size * size, 1));   /* untouched pages cost no memory */
        if (!a->cpu) { free(a->tab); free(a); return NULL; }
    } else {
        glGenTextures(1, &a->tex);
        glBindTexture(GL_TEXTURE_2D, a->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, atlas_internal(), size, size, 0, atlas_format(), GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    atlas_reset(a);
    a->white_x = 0; a->white_y = 0;
    return a;
}

void atlas_free(Atlas *a) {
    if (!a) return;
    if (!sd_soft) { glDeleteTextures(1, &a->tex); if (a->ctex) glDeleteTextures(1, &a->ctex); }
    free(a->cpu);
    free(a->ccpu);
    free(a->tab);
    free(a);
}

unsigned atlas_texture(const Atlas *a) { return a->tex; }
unsigned atlas_color_texture(const Atlas *a) { return a->ctex; }
const uint8_t *atlas_cpu(const Atlas *a) { return a->cpu; }
const uint32_t *atlas_color_cpu(const Atlas *a) { return a->ccpu; }
int atlas_color_size(const Atlas *a) { return a->csize; }
int atlas_size(const Atlas *a) { return a->size; }
uint32_t atlas_generation(const Atlas *a) { return a->generation; }
void atlas_white(const Atlas *a, uint16_t *x, uint16_t *y) { *x = a->white_x; *y = a->white_y; }

static uint32_t hash_key(uint32_t font, uint32_t cp, uint8_t style) {
    uint64_t h = ((uint64_t)font << 40) ^ ((uint64_t)cp << 8) ^ style;
    h *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(h >> 32);
}

static Entry *lookup(Atlas *a, uint32_t font, uint32_t cp, uint8_t style, bool *found) {
    uint32_t i = hash_key(font, cp, style) & (a->cap - 1);
    for (uint32_t n = 0; n < a->cap; n++, i = (i + 1) & (a->cap - 1)) {
        Entry *e = &a->tab[i];
        if (!e->used) { *found = false; return e; }
        if (e->font == font && e->cp == cp && e->style == style) { *found = true; return e; }
    }
    *found = false;
    return NULL;
}

static bool place(Atlas *a, int w, int h, int *ox, int *oy) {
    int pw = w + 1, ph = h + 1;
    if (pw > a->size || ph > a->size) return false;
    if (a->pen_x + pw > a->size) { a->shelf_y += a->shelf_h; a->pen_x = 0; a->shelf_h = 0; }
    if (a->shelf_y + ph > a->size) return false;
    *ox = a->pen_x; *oy = a->shelf_y;
    a->pen_x += pw;
    if (ph > a->shelf_h) a->shelf_h = ph;
    return true;
}

static bool valid(const Atlas *a, const AtlasGlyph *g) {
    int s = g->color ? a->csize : a->size;
    return (int)g->x + g->w <= s && (int)g->y + g->h <= s;
}

static const AtlasGlyph *insert(Atlas *a, uint32_t font, uint32_t cp, uint8_t style, const uint8_t *bits, int w, int h,
                                int left, int top, float adv) {
    if (a->count * 10 > a->cap * 7) atlas_reset(a);
    int ox = 0, oy = 0;
    bool blank = !bits || w <= 0 || h <= 0;
    if (!blank && !place(a, w, h, &ox, &oy)) {
        atlas_reset(a);
        if (!place(a, w, h, &ox, &oy)) return NULL;
    }
    bool found;
    Entry *e = lookup(a, font, cp, style, &found);
    if (!e) return NULL;
    if (!found) a->count++;
    e->used = 1; e->font = font; e->cp = cp; e->style = style;
    e->g = (AtlasGlyph){(uint16_t)ox, (uint16_t)oy, (uint16_t)(blank ? 0 : w), (uint16_t)(blank ? 0 : h), (int16_t)left, (int16_t)top, adv, blank, false};
    if (!blank) {
        if (sd_soft) {
            for (int y = 0; y < h; y++) memcpy(a->cpu + (size_t)(oy + y) * a->size + ox, bits + (size_t)y * w, (size_t)w);
        } else {
            glBindTexture(GL_TEXTURE_2D, a->tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, ox, oy, w, h, atlas_format(), GL_UNSIGNED_BYTE, bits);
        }
    }
    return &e->g;
}

static bool color_ready(Atlas *a) {
    if (sd_soft) {
        if (a->ccpu) return true;
        int sz = a->size < 1024 ? a->size : 1024;
        a->ccpu = static_cast<uint32_t*>(calloc((size_t)sz * sz, 4));
        if (!a->ccpu) return false;
        a->csize = sz;
        return true;
    }
    if (a->ctex) return true;
    if (sd_gl_kind == GLK_ES2) return false;
    int size = a->size < 1024 ? a->size : 1024;
    glGenTextures(1, &a->ctex);
    glBindTexture(GL_TEXTURE_2D, a->ctex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    a->csize = size;
    return true;
}

static bool place_color(Atlas *a, int w, int h, int *ox, int *oy) {
    int pw = w + 1, ph = h + 1;
    if (pw > a->csize || ph > a->csize) return false;
    if (a->cpen_x + pw > a->csize) { a->cshelf_y += a->cshelf_h; a->cpen_x = 0; a->cshelf_h = 0; }
    if (a->cshelf_y + ph > a->csize) return false;
    *ox = a->cpen_x; *oy = a->cshelf_y;
    a->cpen_x += pw;
    if (ph > a->cshelf_h) a->cshelf_h = ph;
    return true;
}

static const AtlasGlyph *insert_color(Atlas *a, uint32_t font, uint32_t cp, uint8_t style, const uint8_t *rgba, int w, int h,
                                      int left, int top, float adv) {
    if (a->count * 10 > a->cap * 7) atlas_reset(a);
    int ox = 0, oy = 0;
    if (!place_color(a, w, h, &ox, &oy)) {
        atlas_reset(a);
        if (!place_color(a, w, h, &ox, &oy)) return NULL;
    }
    bool found;
    Entry *e = lookup(a, font, cp, style, &found);
    if (!e) return NULL;
    if (!found) a->count++;
    e->used = 1; e->font = font; e->cp = cp; e->style = style;
    e->g = (AtlasGlyph){(uint16_t)ox, (uint16_t)oy, (uint16_t)w, (uint16_t)h, (int16_t)left, (int16_t)top, adv, false, true};
    if (sd_soft) {
        for (int y = 0; y < h; y++) memcpy(a->ccpu + (size_t)(oy + y) * a->csize + ox, rgba + (size_t)y * w * 4, (size_t)w * 4);
        return &e->g;
    }
    glBindTexture(GL_TEXTURE_2D, a->ctex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, ox, oy, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return &e->g;
}

static const AtlasGlyph *atlas_glyph_slow(Atlas *a, Font *f, uint32_t fid, uint32_t cp, int style);

const AtlasGlyph *atlas_glyph(Atlas *a, Font *f, uint32_t cp, int style) {
    uint32_t fid = font_id(f);
    if (cp < 128 && (unsigned)style < 4) {
        __typeof__(a->ascii[0][0]) *slot = &a->ascii[style][cp];
        if (slot->g && slot->gen == a->generation && slot->font == fid) { sd_cache.glyph_hit++; return slot->g; }
        const AtlasGlyph *g = atlas_glyph_slow(a, f, fid, cp, style);
        if (g) { slot->g = g; slot->gen = a->generation; slot->font = fid; }
        return g;
    }
    return atlas_glyph_slow(a, f, fid, cp, style);
}

static const AtlasGlyph *atlas_glyph_slow(Atlas *a, Font *f, uint32_t fid, uint32_t cp, int style) {
    bool found;
    Entry *e = lookup(a, fid, cp, (uint8_t)style, &found);
    if (found && valid(a, &e->g)) { sd_cache.glyph_hit++; return &e->g; }
    sd_cache.glyph_miss++;
    GlyphBmp b;
    bool can_color = cp >= 0x1F000 && color_ready(a);
    font_glyph_color(f, cp, style, &b, can_color);
    if (b.color && b.buf) return insert_color(a, fid, cp, (uint8_t)style, b.buf, b.w, b.h, b.left, b.top, b.adv);
    return insert(a, fid, cp, (uint8_t)style, b.buf, b.w, b.h, b.left, b.top, b.adv);
}

const AtlasGlyph *atlas_peek(const Atlas *a, uint32_t fid, uint32_t cp, int style) {
    if (cp < 128 && (unsigned)style < 4) {
        const __typeof__(a->ascii[0][0]) *slot = &a->ascii[style][cp];
        if (slot->g && slot->gen == a->generation && slot->font == fid) return slot->g;
    }
    bool found;
    Entry *e = lookup((Atlas *)a, fid, cp, (uint8_t)style, &found);
    return found && valid(a, &e->g) ? &e->g : NULL;
}

const AtlasGlyph *atlas_find_custom(Atlas *a, uint32_t font_id, uint32_t key) {
    bool found;
    Entry *e = lookup(a, font_id, key, CUSTOM_STYLE, &found);
    return found && valid(a, &e->g) ? &e->g : NULL;
}

const AtlasGlyph *atlas_custom(Atlas *a, uint32_t font_id, uint32_t key, const uint8_t *bits, int w, int h) {
    const AtlasGlyph *g = atlas_find_custom(a, font_id, key);
    if (g) return g;
    return insert(a, font_id, key, CUSTOM_STYLE, bits, w, h, 0, 0, (float)w);
}
