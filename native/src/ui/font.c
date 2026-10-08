/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include <fontconfig/fontconfig.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { FT_Face face; char *path; int index; } FaceEnt;

#define NEG_SLOTS 2048

struct Font {
    FT_Library lib;
    double px;
    bool mono;
    uint32_t id;
    FaceEnt style[4];
    char *family;
    bool tried[4];
    FaceEnt *fb;
    int nfb, capfb;
    uint32_t neg[NEG_SLOTS];
    int cw, ch, ascent;
    uint8_t *scratch;
    size_t scratch_cap;
};

static uint32_t next_id = 1;

static bool open_face(Font *f, FaceEnt *e, const char *path, int index) {
    if (FT_New_Face(f->lib, path, index, &e->face) != 0) { e->face = NULL; return false; }
    if (!FT_IS_SCALABLE(e->face) || FT_Set_Pixel_Sizes(e->face, 0, (FT_UInt)(f->px + 0.5)) != 0) {
        FT_Done_Face(e->face);
        e->face = NULL;
        return false;
    }
    e->path = strdup(path);
    e->index = index;
    return true;
}

static bool match(const char *family, int weight, int slant, uint32_t cp, bool mono, char **path, int *index) {
    FcPattern *pat = FcPatternCreate();
    if (family) FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddInteger(pat, FC_WEIGHT, weight);
    FcPatternAddInteger(pat, FC_SLANT, slant);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    if (mono) FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    FcCharSet *cs = NULL;
    if (cp) { cs = FcCharSetCreate(); FcCharSetAddChar(cs, cp); FcPatternAddCharSet(pat, FC_CHARSET, cs); }
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *m = FcFontMatch(NULL, pat, &res);
    bool ok = false;
    if (m) {
        FcChar8 *file = NULL;
        int idx = 0;
        if (FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
            FcPatternGetInteger(m, FC_INDEX, 0, &idx);
            *path = strdup((const char *)file);
            *index = idx;
            ok = true;
        }
        FcPatternDestroy(m);
    }
    if (cs) FcCharSetDestroy(cs);
    FcPatternDestroy(pat);
    return ok;
}

static const struct { int weight, slant; } STYLE_WANT[4] = {
    {FC_WEIGHT_REGULAR, FC_SLANT_ROMAN}, {FC_WEIGHT_BOLD, FC_SLANT_ROMAN}, {FC_WEIGHT_REGULAR, FC_SLANT_ITALIC}, {FC_WEIGHT_BOLD, FC_SLANT_ITALIC}};

static void load_style(Font *f, int s) {
    if (f->tried[s]) return;
    f->tried[s] = true;
    char *path = NULL;
    int idx = 0;
    if (!match(f->family, STYLE_WANT[s].weight, STYLE_WANT[s].slant, 0, f->mono, &path, &idx)) return;
    if (s > 0 && f->style[0].path && strcmp(path, f->style[0].path) == 0 && idx == f->style[0].index) { free(path); return; }
    open_face(f, &f->style[s], path, idx);
    free(path);
}

Font *font_open(const char *family, double px, bool mono) {
    Font *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->px = px;
    f->mono = mono;
    f->id = next_id++;
    if (FT_Init_FreeType(&f->lib) != 0) { free(f); return NULL; }
    if (!FcInit()) { FT_Done_FreeType(f->lib); free(f); return NULL; }
    f->family = family ? strdup(family) : NULL;
    load_style(f, FS_REGULAR);
    if (!f->style[0].face) { font_close(f); return NULL; }
    FT_Face fc = f->style[0].face;
    if (FT_Load_Char(fc, 'M', FT_LOAD_TARGET_LIGHT) == 0) f->cw = (int)lround(fc->glyph->metrics.horiAdvance / 64.0);
    int asc = (int)ceil(fc->size->metrics.ascender / 64.0), desc = (int)ceil(-fc->size->metrics.descender / 64.0);
    f->ascent = asc;
    f->ch = asc + desc;
    int hh = (int)ceil(fc->size->metrics.height / 64.0);
    if (hh > f->ch) { f->ascent += (hh - f->ch) / 2; f->ch = hh; }
    if (f->cw < 1) f->cw = (int)(px * 0.6 + 0.5);
    if (f->ch < 1) f->ch = (int)(px * 1.3 + 0.5);
    return f;
}

void font_close(Font *f) {
    if (!f) return;
    for (int i = 0; i < 4; i++) { if (f->style[i].face) FT_Done_Face(f->style[i].face); free(f->style[i].path); }
    for (int i = 0; i < f->nfb; i++) { if (f->fb[i].face) FT_Done_Face(f->fb[i].face); free(f->fb[i].path); }
    free(f->fb);
    free(f->family);
    free(f->scratch);
    FT_Done_FreeType(f->lib);
    free(f);
}

uint32_t font_id(const Font *f) { return f->id; }
double font_px(const Font *f) { return f->px; }
int font_cell_w(const Font *f) { return f->cw; }
int font_cell_h(const Font *f) { return f->ch; }
int font_ascent(const Font *f) { return f->ascent; }

static bool neg_has(const Font *f, uint32_t cp) { return f->neg[(cp * 2654435761u) % NEG_SLOTS] == cp; }
static void neg_add(Font *f, uint32_t cp) { f->neg[(cp * 2654435761u) % NEG_SLOTS] = cp; }

static FT_Face pick_face(Font *f, uint32_t cp, int style, bool *synth_bold, bool *synth_italic) {
    bool want_b = style & FS_BOLD, want_i = style & FS_ITALIC;
    *synth_bold = *synth_italic = false;
    if (style) { load_style(f, style); if (want_b && want_i) { load_style(f, FS_BOLD); load_style(f, FS_ITALIC); } else if (want_b || want_i) {  } }
    FaceEnt *e = &f->style[style];
    if (e->face && FT_Get_Char_Index(e->face, cp)) return e->face;
    e = &f->style[0];
    if (e->face && FT_Get_Char_Index(e->face, cp)) {
        *synth_bold = want_b && !f->style[FS_BOLD].face;
        *synth_italic = want_i && !f->style[FS_ITALIC].face;
        if (want_b && f->style[FS_BOLD].face && !(style & FS_ITALIC)) *synth_bold = true;
        return e->face;
    }
    for (int i = 0; i < f->nfb; i++)
        if (f->fb[i].face && FT_Get_Char_Index(f->fb[i].face, cp)) { *synth_bold = want_b; *synth_italic = want_i; return f->fb[i].face; }
    if (neg_has(f, cp)) return NULL;
    char *path = NULL;
    int idx = 0;
    if (match(NULL, FC_WEIGHT_REGULAR, FC_SLANT_ROMAN, cp, f->mono, &path, &idx)) {
        for (int i = 0; i < f->nfb; i++)
            if (f->fb[i].path && strcmp(f->fb[i].path, path) == 0 && f->fb[i].index == idx) { free(path); neg_add(f, cp); return NULL; }
        if (f->nfb == f->capfb) {
            int nc = f->capfb ? f->capfb * 2 : 8;
            if (nc > 64) { free(path); neg_add(f, cp); return NULL; }
            FaceEnt *nf = realloc(f->fb, (size_t)nc * sizeof *nf);
            if (!nf) { free(path); return NULL; }
            f->fb = nf; f->capfb = nc;
        }
        FaceEnt *ne = &f->fb[f->nfb];
        memset(ne, 0, sizeof *ne);
        bool ok = open_face(f, ne, path, idx);
        free(path);
        if (ok && FT_Get_Char_Index(ne->face, cp)) { f->nfb++; *synth_bold = want_b; *synth_italic = want_i; return ne->face; }
        if (ok) { f->nfb++; }
    }
    neg_add(f, cp);
    return NULL;
}

bool font_glyph(Font *f, uint32_t cp, int style, GlyphBmp *out) {
    memset(out, 0, sizeof *out);
    bool sb, si;
    FT_Face face = pick_face(f, cp, style, &sb, &si);
    if (!face) return false;
    FT_UInt gi = FT_Get_Char_Index(face, cp);
    if (FT_Load_Glyph(face, gi, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) != 0) return false;
    FT_GlyphSlot slot = face->glyph;
    if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (si) {
            FT_Matrix m = {0x10000, 0x5800, 0, 0x10000};
            FT_Outline_Transform(&slot->outline, &m);
        }
        if (sb) FT_Outline_Embolden(&slot->outline, (FT_Pos)(f->px * 64.0 / 24.0));
    }
    if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) return false;
    FT_Bitmap *bm = &slot->bitmap;
    out->w = (int)bm->width;
    out->h = (int)bm->rows;
    out->left = slot->bitmap_left;
    out->top = slot->bitmap_top;
    out->adv = (float)(slot->advance.x / 64.0) + (sb ? 0.f : 0.f);
    size_t need = (size_t)out->w * out->h;
    if (need > f->scratch_cap) {
        uint8_t *ns = realloc(f->scratch, need);
        if (!ns) { out->w = out->h = 0; return false; }
        f->scratch = ns; f->scratch_cap = need;
    }
    for (int y = 0; y < out->h; y++) {
        if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) memcpy(f->scratch + (size_t)y * out->w, bm->buffer + (size_t)y * bm->pitch, (size_t)out->w);
        else memset(f->scratch + (size_t)y * out->w, 0, (size_t)out->w);
    }
    out->buf = f->scratch;
    return true;
}

float font_advance(Font *f, uint32_t cp, int style) {
    bool sb, si;
    FT_Face face = pick_face(f, cp, style, &sb, &si);
    if (!face) return (float)f->cw;
    if (FT_Load_Glyph(face, FT_Get_Char_Index(face, cp), FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) != 0) return (float)f->cw;
    return (float)(face->glyph->advance.x / 64.0);
}

char *font_pick_family(const char *const *wanted) {
    if (!FcInit()) return NULL;
    for (int i = 0; wanted[i]; i++) {
        FcPattern *pat = FcPatternCreate();
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)wanted[i]);
        FcConfigSubstitute(NULL, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult res;
        FcPattern *m = FcFontMatch(NULL, pat, &res);
        bool ok = false;
        if (m) {
            FcChar8 *fam = NULL;
            if (FcPatternGetString(m, FC_FAMILY, 0, &fam) == FcResultMatch && strcasecmp((const char *)fam, wanted[i]) == 0) ok = true;
            FcPatternDestroy(m);
        }
        FcPatternDestroy(pat);
        if (ok) return strdup(wanted[i]);
    }
    return NULL;
}

void font_prewarm(void) {
    if (!FcInit()) return;
    static const char *const fams[] = {"sans", "monospace", "JetBrains Mono", "DejaVu Sans Mono", NULL};
    for (int i = 0; fams[i]; i++) {
        FcPattern *pat = FcPatternCreate();
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)fams[i]);
        FcConfigSubstitute(NULL, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult res;
        FcPattern *m = FcFontMatch(NULL, pat, &res);
        if (m) FcPatternDestroy(m);
        FcPatternDestroy(pat);
    }
}
