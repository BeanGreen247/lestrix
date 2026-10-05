/* atlas.h - glyph atlas: one 8-bit texture, shelf packed, with a cache that is keyed on everything a glyph depends on.
 *
 * Cache safety: an entry is only used when font id, code point, style and the font's pixel size all match (the full key
 * is stored and compared, never just a hash), its rectangle is re-checked against the texture on every hit, and when the
 * atlas fills up it is cleared as a whole and `generation` changes, so no cached quad can refer to texels that were
 * overwritten by a different glyph. Anything holding atlas coordinates must compare the generation before reusing them. */
#ifndef SD_ATLAS_H
#define SD_ATLAS_H
#include <stdbool.h>
#include <stdint.h>
#include "font.h"

typedef struct {
    uint16_t x, y, w, h;   /* texels in the atlas */
    int16_t left, top;     /* bitmap offset from the pen position / baseline */
    float adv;
    bool blank;            /* no ink: nothing to draw */
} AtlasGlyph;

typedef struct Atlas Atlas;

/* cache counters shown in the HUD (render thread only): hits/misses per cache, recycle = an entry that was reused in place
 * (rows) or thrown away to make room (glyph atlas clears) */
typedef struct { uint64_t glyph_hit, glyph_miss, glyph_recycle, row_hit, row_miss, row_recycle; } CacheStats;
extern CacheStats sd_cache;

Atlas *atlas_new(int size);            /* creates the GL texture; a GL context must be current */
void atlas_free(Atlas *a);
unsigned atlas_texture(const Atlas *a);
int atlas_size(const Atlas *a);
uint32_t atlas_generation(const Atlas *a);
/* texel rectangle of a solid white pixel block, for drawing plain rectangles with the same shader */
void atlas_white(const Atlas *a, uint16_t *x, uint16_t *y);
const AtlasGlyph *atlas_glyph(Atlas *a, Font *f, uint32_t cp, int style);
/* an arbitrary 8-bit bitmap (procedural glyphs) under a caller-chosen key */
const AtlasGlyph *atlas_custom(Atlas *a, uint32_t font_id, uint32_t key, const uint8_t *bits, int w, int h);
const AtlasGlyph *atlas_peek(const Atlas *a, uint32_t font_id, uint32_t cp, int style);   /* read-only: NULL if not cached yet */
const AtlasGlyph *atlas_find_custom(Atlas *a, uint32_t font_id, uint32_t key);
#endif
