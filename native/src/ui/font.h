/* font.h - FreeType faces found through fontconfig, with per-character fallback. No toolkit involved. */
#ifndef SD_FONT_H
#define SD_FONT_H
#include <stdbool.h>
#include <stdint.h>

enum { FS_REGULAR = 0, FS_BOLD = 1, FS_ITALIC = 2, FS_BOLD_ITALIC = 3 };

typedef struct Font Font;

typedef struct {
    uint8_t *buf;        /* 8-bit coverage, w*h bytes, owned by the font until the next call */
    int w, h;
    int left, top;       /* offset of the bitmap's top-left from the pen position / baseline */
    float adv;           /* horizontal advance in pixels */
} GlyphBmp;

/* family: "monospace", "DejaVu Sans Mono", ... ; px: pixel size. NULL on failure. */
Font *font_open(const char *family, double px, bool mono);
void font_close(Font *f);
uint32_t font_id(const Font *f);     /* changes whenever the face set or size does: part of every cache key */
double font_px(const Font *f);
int font_cell_w(const Font *f);      /* monospace cell width in pixels */
int font_cell_h(const Font *f);      /* line height */
int font_ascent(const Font *f);
/* false when no installed font has the character; out is then empty */
bool font_glyph(Font *f, uint32_t cp, int style, GlyphBmp *out);
float font_advance(Font *f, uint32_t cp, int style);   /* proportional layout */
/* the first of the wanted families that is really installed (fontconfig would otherwise substitute silently); malloc'd, or NULL */
char *font_pick_family(const char *const *wanted);
void font_prewarm(void);   /* fontconfig start-up work, callable from a helper thread */
#endif
