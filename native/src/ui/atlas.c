#include "atlas.h"

#include <stdlib.h>
#include <string.h>

#include "gl.h"

typedef struct { uint32_t font, cp; uint8_t style, used; AtlasGlyph g; } Entry;

/* one 8-bit channel: OpenGL 3 and ES 3 have R8; OpenGL ES 2 only has luminance (the shader reads the red channel either way) */
static inline GLenum atlas_format(void) { return sd_gl_kind == GLK_ES2 ? GL_LUMINANCE : GL_RED; }
static inline GLint atlas_internal(void) { return sd_gl_kind == GLK_ES2 ? GL_LUMINANCE : GL_R8; }

struct Atlas {
    GLuint tex;
    int size;
    uint32_t generation;
    int shelf_y, shelf_h, pen_x;        /* the shelf being filled */
    Entry *tab;
    uint32_t cap, count;
    uint16_t white_x, white_y;
    /* ASCII lookup table: [style][code point] straight to the glyph, no hashing and no key compare. Entries are tagged with the
     * atlas generation and font id, so a cleared atlas or a font/size change invalidates them all at once. */
    struct { const AtlasGlyph *g; uint32_t gen, font; } ascii[4][128];
};

#define CUSTOM_STYLE 0xff

CacheStats sd_cache;

static void atlas_reset(Atlas *a) {
    sd_cache.glyph_recycle += a->count;
    memset(a->tab, 0, (size_t)a->cap * sizeof *a->tab);
    a->count = 0;
    a->shelf_y = 2; a->shelf_h = 0; a->pen_x = 0;
    a->generation++;
    /* texel block (0,0)-(2,2) is solid white */
    static const uint8_t white[4] = {255, 255, 255, 255};
    glBindTexture(GL_TEXTURE_2D, a->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, atlas_format(), GL_UNSIGNED_BYTE, white);
    a->pen_x = 4;   /* leave the corner to the white block */
    a->shelf_h = 2;
}

Atlas *atlas_new(int size) {
    GLint maxtex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
    if (maxtex > 0 && size > maxtex) size = maxtex;   /* small GPUs (VideoCore IV tops out at 2048) */
    Atlas *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->size = size;
    a->cap = 16384;
    a->tab = calloc(a->cap, sizeof *a->tab);
    if (!a->tab) { free(a); return NULL; }
    glGenTextures(1, &a->tex);
    glBindTexture(GL_TEXTURE_2D, a->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* storage only: nothing in the atlas is sampled before it has been written, so there is no need to upload zeros */
    glTexImage2D(GL_TEXTURE_2D, 0, atlas_internal(), size, size, 0, atlas_format(), GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    atlas_reset(a);
    a->white_x = 0; a->white_y = 0;
    return a;
}

void atlas_free(Atlas *a) {
    if (!a) return;
    glDeleteTextures(1, &a->tex);
    free(a->tab);
    free(a);
}

unsigned atlas_texture(const Atlas *a) { return a->tex; }
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

/* room for a w*h block (plus one texel of padding around it); false when the atlas is full */
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
    return (int)g->x + g->w <= a->size && (int)g->y + g->h <= a->size;
}

static const AtlasGlyph *insert(Atlas *a, uint32_t font, uint32_t cp, uint8_t style, const uint8_t *bits, int w, int h,
                                int left, int top, float adv) {
    if (a->count * 10 > a->cap * 7) atlas_reset(a);
    int ox = 0, oy = 0;
    bool blank = !bits || w <= 0 || h <= 0;
    if (!blank && !place(a, w, h, &ox, &oy)) {
        atlas_reset(a);   /* full: start over rather than overwrite anything still in use */
        if (!place(a, w, h, &ox, &oy)) return NULL;
    }
    bool found;
    Entry *e = lookup(a, font, cp, style, &found);
    if (!e) return NULL;
    if (!found) a->count++;
    e->used = 1; e->font = font; e->cp = cp; e->style = style;
    e->g = (AtlasGlyph){(uint16_t)ox, (uint16_t)oy, (uint16_t)(blank ? 0 : w), (uint16_t)(blank ? 0 : h), (int16_t)left, (int16_t)top, adv, blank};
    if (!blank) {
        glBindTexture(GL_TEXTURE_2D, a->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, ox, oy, w, h, atlas_format(), GL_UNSIGNED_BYTE, bits);
    }
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
    font_glyph(f, cp, style, &b);   /* a missing glyph is cached as blank so it is not looked up again */
    return insert(a, fid, cp, (uint8_t)style, b.buf, b.w, b.h, b.left, b.top, b.adv);
}

/* read-only lookup for threads that must not touch the texture: the glyph if it is already in the atlas, otherwise NULL */
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
