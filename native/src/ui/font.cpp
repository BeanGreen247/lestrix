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
#include <hb.h>
#include <hb-ot.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { FT_Face face; char *path; int index; bool bitmap; hb_font_t *hb; } FaceEnt;

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
    bool emoji_tried;
    uint8_t *scratch;
    size_t scratch_cap;
};

static uint32_t next_id = 1;

static bool open_face(Font *f, FaceEnt *e, const char *path, int index) {
    if (FT_New_Face(f->lib, path, index, &e->face) != 0) { e->face = NULL; return false; }
    e->bitmap = false;
    if (!FT_IS_SCALABLE(e->face) && FT_HAS_COLOR(e->face) && e->face->num_fixed_sizes > 0) {
        /* colour bitmap strike (CBDT/sbix, e.g. Noto Color Emoji): pick the strike nearest the font size */
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < e->face->num_fixed_sizes; i++) {
            int d = abs(e->face->available_sizes[i].height - (int)(f->px + 0.5));
            if (d < bd) { bd = d; best = i; }
        }
        if (FT_Select_Size(e->face, best) == 0) e->bitmap = true;
    }
    if (!e->bitmap && (!FT_IS_SCALABLE(e->face) || FT_Set_Pixel_Sizes(e->face, 0, (FT_UInt)(f->px + 0.5)) != 0)) {
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
    Font *f = static_cast<Font*>(calloc(1, sizeof *f));
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
    for (int i = 0; i < 4; i++) { if (f->style[i].hb) hb_font_destroy(f->style[i].hb); if (f->style[i].face) FT_Done_Face(f->style[i].face); free(f->style[i].path); }
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

static bool is_bitmap_face(const Font *f, FT_Face face);

/* Match a fallback font for cp and append it to the fallback list. Returns the face only when it has the glyph and may
 * be used for it (a colour bitmap face only for emoji); a font that is already listed or lacks the glyph returns NULL. */
static FT_Face load_fallback(Font *f, const char *family, uint32_t cp, bool mono, bool emoji) {
    char *path = NULL;
    int idx = 0;
    if (!match(family, FC_WEIGHT_REGULAR, FC_SLANT_ROMAN, cp, mono, &path, &idx)) return NULL;
    for (int i = 0; i < f->nfb; i++)
        if (f->fb[i].path && strcmp(f->fb[i].path, path) == 0 && f->fb[i].index == idx) { free(path); return NULL; }
    if (f->nfb == f->capfb) {
        int nc = f->capfb ? f->capfb * 2 : 8;
        FaceEnt *nf = nc > 64 ? NULL : static_cast<FaceEnt*>(realloc(f->fb, (size_t)nc * sizeof *nf));
        if (!nf) { free(path); return NULL; }
        f->fb = nf; f->capfb = nc;
    }
    FaceEnt *ne = &f->fb[f->nfb];
    memset(ne, 0, sizeof *ne);
    bool ok = open_face(f, ne, path, idx);
    free(path);
    if (!ok) return NULL;
    f->nfb++;
    if (ne->bitmap && !emoji) return NULL;
    return FT_Get_Char_Index(ne->face, cp) ? ne->face : NULL;
}

static bool is_emoji_cp(uint32_t cp) {
    return (cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x1F000 && cp <= 0x1F2FF);
}

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
    bool emoji = is_emoji_cp(cp);
    if (emoji)
        for (int i = 0; i < f->nfb; i++)
            if (f->fb[i].face && f->fb[i].bitmap && FT_Get_Char_Index(f->fb[i].face, cp)) { *synth_bold = *synth_italic = false; return f->fb[i].face; }
    for (int i = 0; i < f->nfb; i++) {
        if (!f->fb[i].face || (f->fb[i].bitmap && !emoji)) continue;   /* colour bitmap fonts only serve emoji */
        if (FT_Get_Char_Index(f->fb[i].face, cp)) { *synth_bold = want_b; *synth_italic = want_i; return f->fb[i].face; }
    }
    if (neg_has(f, cp)) return NULL;
    FT_Face r = NULL;
    if (emoji && !f->emoji_tried) { f->emoji_tried = true; r = load_fallback(f, "Noto Color Emoji", cp, false, true); }
    if (!r) r = load_fallback(f, NULL, cp, f->mono, emoji);
    if (r) { bool bm = is_bitmap_face(f, r); *synth_bold = want_b && !bm; *synth_italic = want_i && !bm; return r; }
    neg_add(f, cp);
    return NULL;
}

static bool is_bitmap_face(const Font *f, FT_Face face) {
    for (int i = 0; i < f->nfb; i++) if (f->fb[i].face == face) return f->fb[i].bitmap;
    return false;
}

/* Colour bitmap glyph, box-scaled to fit the cell height and at most two cells wide. With color it returns straight
 * RGBA for the colour atlas; without (ES2, no colour atlas) 8-bit coverage = alpha * luma, tinted with the text colour. */
static bool bitmap_glyph(Font *f, FT_Face face, FT_UInt gi, GlyphBmp *out, bool color) {
    if (FT_Load_Glyph(face, gi, FT_LOAD_COLOR | FT_LOAD_RENDER) != 0) return false;
    FT_Bitmap *bm = &face->glyph->bitmap;
    if (bm->pixel_mode != FT_PIXEL_MODE_BGRA || !bm->width || !bm->rows) return false;
    int sw = (int)bm->width, sh = (int)bm->rows;
    int th = f->ch > 2 ? f->ch - 1 : f->ch;
    int tw = (int)((long)sw * th / sh);
    if (tw > 2 * f->cw) { tw = 2 * f->cw; th = (int)((long)sh * tw / sw); }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    size_t need = (size_t)tw * th * (color ? 4 : 1);
    if (need > f->scratch_cap) {
        uint8_t *ns = static_cast<uint8_t*>(realloc(f->scratch, need));
        if (!ns) return false;
        f->scratch = ns; f->scratch_cap = need;
    }
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long)y * sh / th), y1 = (int)((long)(y + 1) * sh / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long)x * sw / tw), x1 = (int)((long)(x + 1) * sw / tw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned acc = 0, n = 0, sb = 0, sg = 0, sr = 0, sa = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++) {
                const uint8_t *row = bm->buffer + (size_t)yy * bm->pitch;
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const uint8_t *px = row + (size_t)xx * 4; /* premultiplied BGRA */
                    unsigned luma = (px[0] * 29u + px[1] * 150u + px[2] * 77u) >> 8;
                    acc += luma > 255 ? 255 : luma;
                    sb += px[0]; sg += px[1]; sr += px[2]; sa += px[3];
                    n++;
                }
            }
            if (!color) { f->scratch[(size_t)y * tw + x] = n ? (uint8_t)(acc / n) : 0; continue; }
            uint8_t *o = f->scratch + ((size_t)y * tw + x) * 4;
            if (!n || !sa) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
            /* sums of premultiplied samples: straight colour = sum(rgb) / sum(alpha), coverage = mean alpha */
            unsigned r = sr * 255 / sa, g = sg * 255 / sa, b = sb * 255 / sa;
            o[0] = r > 255 ? 255 : r; o[1] = g > 255 ? 255 : g; o[2] = b > 255 ? 255 : b; o[3] = (uint8_t)(sa / n);
        }
    }
    out->buf = f->scratch;
    out->color = color;
    out->w = tw; out->h = th;
    out->left = tw < 2 * f->cw ? (2 * f->cw - tw) / 2 : 0;
    out->top = f->ascent - (f->ch - th) / 2;
    out->adv = (float)(2 * f->cw);
    return true;
}

static bool render_gi(Font *f, FT_Face face, FT_UInt gi, bool sb, bool si, GlyphBmp *out, bool allow_color) {
    if (is_bitmap_face(f, face)) return bitmap_glyph(f, face, gi, out, allow_color);
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
    out->adv = (float)(slot->advance.x / 64.0);
    size_t need = (size_t)out->w * out->h;
    if (need > f->scratch_cap) {
        uint8_t *ns = static_cast<uint8_t*>(realloc(f->scratch, need));
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

bool font_glyph(Font *f, uint32_t cp, int style, GlyphBmp *out) { return font_glyph_color(f, cp, style, out, false); }

bool font_glyph_color(Font *f, uint32_t cp, int style, GlyphBmp *out, bool allow_color) {
    memset(out, 0, sizeof *out);
    bool sb, si;
    FT_Face face = pick_face(f, cp, style, &sb, &si);
    if (!face) return false;
    return render_gi(f, face, FT_Get_Char_Index(face, cp), sb, si, out, allow_color);
}

/* ---- ligatures: HarfBuzz shaping of one run of same-style cells (OpenType funcs, no FreeType, so shaping itself holds no shared state) ---- */

static FaceEnt *ent_of(Font *f, FT_Face face) {
    for (int i = 0; i < 4; i++) if (f->style[i].face == face) return &f->style[i];
    return NULL;
}

static hb_font_t *hb_of(FaceEnt *e) {
    if (e->hb) return e->hb;
    if (!e->path) return NULL;
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(e->path);
    if (!blob) return NULL;
    hb_face_t *hf = hb_face_create(blob, (unsigned)e->index);
    hb_blob_destroy(blob);
    e->hb = hb_font_create(hf);
    hb_face_destroy(hf);
    if (e->hb) hb_ot_font_set_funcs(e->hb);
    return e->hb;
}

/* The face a run in this style is shaped with: the style's own face once loaded, else the regular one. No FreeType call, so worker threads may use it. */
static FT_Face run_face(Font *f, int style, bool init, bool *sb, bool *si) {
    if ((unsigned)style > 3) style = 0;
    *sb = *si = false;
    if (init && style) { load_style(f, style); if (style == FS_BOLD_ITALIC) { load_style(f, FS_BOLD); load_style(f, FS_ITALIC); } }
    else if (!init && style && !f->tried[style]) return NULL;   /* a worker thread must not load faces */
    return f->style[style].face ? f->style[style].face : f->style[0].face;
}

enum { SHAPE_CACHE_MAX = 64, SHAPE_CACHE_SLOTS = 512 };

int font_shape(Font *f, const uint32_t *cps, int n, int style, ShapedGlyph *out, int cap, bool init) {
    if (n <= 0 || n > 256) return 0;
    bool sb, si;
    FT_Face face = run_face(f, style, init, &sb, &si);
    if (!face || is_bitmap_face(f, face)) return face ? 0 : -1;
    FaceEnt *e = ent_of(f, face);
    if (!e) return 0;
    /* Short runs repeat all the time ("-", "::", "=" in a log), so a small per-thread cache keeps HarfBuzz off the flood path: in a 107 MB
     * `cat` of log lines the shaper was about 9% of all CPU before this cache. Keyed on the font id (never reused), the face and the text. */
    struct ShapeSlot { uint32_t fid; const void *face; int n, ng; uint32_t cps[SHAPE_CACHE_MAX]; ShapedGlyph g[SHAPE_CACHE_MAX]; };
    static thread_local ShapeSlot cache[SHAPE_CACHE_SLOTS];
    ShapeSlot *slot = NULL;
    if (n <= SHAPE_CACHE_MAX) {
        uint32_t h = f->id * 2654435761u ^ (uint32_t)(uintptr_t)face;
        for (int i = 0; i < n; i++) h = (h ^ cps[i]) * 16777619u;
        slot = &cache[h % SHAPE_CACHE_SLOTS];
        if (slot->fid == f->id && slot->face == face && slot->n == n && memcmp(slot->cps, cps, (size_t)n * sizeof(uint32_t)) == 0) {
            int k = slot->ng < cap ? slot->ng : cap;
            memcpy(out, slot->g, (size_t)k * sizeof(ShapedGlyph));
            return k;
        }
    }
    if (!e->hb && !init) return -1;
    hb_font_t *hf = hb_of(e);
    if (!hf) return 0;
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf32(buf, cps, n, 0, n);
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
    hb_buffer_set_language(buf, hb_language_from_string("en", 2));
    hb_shape(hf, buf, NULL, 0);
    unsigned ng = 0;
    hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(buf, &ng);
    int no = 0;
    for (unsigned i = 0; i < ng && no < cap; i++) {
        int cl = (int)gi[i].cluster;
        if (cl < 0 || cl >= n) continue;
        int next = n;
        for (unsigned j = i + 1; j < ng; j++) if ((int)gi[j].cluster > cl) { next = (int)gi[j].cluster; break; }
        int span = next - cl;
        hb_codepoint_t dflt = 0;
        hb_font_get_nominal_glyph(hf, cps[cl], &dflt);
        if (gi[i].codepoint == dflt && span == 1) continue;
        out[no].gid = gi[i].codepoint;
        out[no].cell = cl;
        out[no].span = span;
        no++;
    }
    hb_buffer_destroy(buf);
    if (slot && no <= SHAPE_CACHE_MAX) {
        slot->fid = f->id; slot->face = face; slot->n = n; slot->ng = no;
        memcpy(slot->cps, cps, (size_t)n * sizeof(uint32_t));
        memcpy(slot->g, out, (size_t)no * sizeof(ShapedGlyph));
    }
    return no;
}

bool font_glyph_gid(Font *f, uint32_t gid, int style, GlyphBmp *out) {
    memset(out, 0, sizeof *out);
    bool sb, si;
    FT_Face face = run_face(f, style, true, &sb, &si);
    if (!face) return false;
    bool wb = style & FS_BOLD, wi = style & FS_ITALIC;
    if (face == f->style[0].face) { sb = wb && !f->style[FS_BOLD].face; si = wi && !f->style[FS_ITALIC].face; }
    return render_gi(f, face, gid, sb, si, out, false);
}

bool font_has_ligatures(Font *f) {
    static const char *const tests[] = {"->", "=>", "!=", "==", "<=", ">=", "|>", "::", "www", "<!--", "&&", "||", NULL};
    for (int t = 0; tests[t]; t++) {
        uint32_t cps[8];
        int n = 0;
        for (const char *p = tests[t]; *p; p++) cps[n++] = (uint8_t)*p;
        ShapedGlyph g[8];
        if (font_shape(f, cps, n, FS_REGULAR, g, 8, true) > 0) return true;
    }
    return false;
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
