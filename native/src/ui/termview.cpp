/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "termview.h"
#include "workpool.h"
#include "ligrun.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char BOX[] = "11002200001100221100220000110022110022000011002201010201010202021001200110022002011002100120022010102010102020200111021101210112012202210212022210112011102110121022202120122022110121011201220111022102120222021110211012102210112021201220222011112111121122111121111211222121122121121212222122122122122222221100220000110022330000330301010303033001100330030310013003303010103030300311013303333011103330333301110333033310113033303311113333330101100110100110aaaabbbbcccc100000100100000120000020020000021200001221000021";

static void fill(uint8_t *b, int w, int h, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++) memset(b + (size_t)y * w + x0, 255, (size_t)(x1 > x0 ? x1 - x0 : 0));
}

static bool proc_bits(uint32_t cp, int w, int h, uint8_t *b) {
    memset(b, 0, (size_t)w * h);
    if (cp >= 0x2580 && cp <= 0x259f) {
        if (cp == 0x2580) fill(b, w, h, 0, 0, w, h / 2);
        else if (cp <= 0x2588) fill(b, w, h, 0, h - (int)lround(h * (cp - 0x2580) / 8.0), w, h);
        else if (cp <= 0x258f) fill(b, w, h, 0, 0, (int)lround(w * (0x2590 - cp) / 8.0), h);
        else if (cp == 0x2590) fill(b, w, h, w / 2, 0, w, h);
        else if (cp >= 0x2591 && cp <= 0x2593) {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                bool on = cp == 0x2592 ? ((x + y) & 1) == 0 : cp == 0x2591 ? (x % 2 == 0 && y % 2 == 0) : !(x % 2 == 0 && y % 2 == 0);
                b[(size_t)y * w + x] = on ? 255 : 0;
            }
        } else if (cp == 0x2594) fill(b, w, h, 0, 0, w, h / 8 ? h / 8 : 1);
        else if (cp == 0x2595) fill(b, w, h, w - (w / 8 ? w / 8 : 1), 0, w, h);
        else {
            static const unsigned char quad[] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
            unsigned m = quad[cp - 0x2596];
            int mx = w / 2, my = h / 2;
            if (m & 1) fill(b, w, h, 0, 0, mx, my);
            if (m & 2) fill(b, w, h, mx, 0, w, my);
            if (m & 4) fill(b, w, h, 0, my, mx, h);
            if (m & 8) fill(b, w, h, mx, my, w, h);
        }
        return true;
    }
    if (cp < 0x2500 || cp > 0x257f) return false;
    const char *e = BOX + (cp - 0x2500) * 4;
    if (e[0] >= 'a') {
        for (int x = 0; x < w; x++) {
            int y1 = (int)lround((double)x * (h - 1) / (w > 1 ? w - 1 : 1)), y2 = h - 1 - y1;
            for (int d = 0; d < 2; d++) {
                if ((e[0] == 'a' || e[0] == 'c') && y2 + d < h) b[(size_t)(y2 + d) * w + x] = 255;
                if ((e[0] == 'b' || e[0] == 'c') && y1 + d < h) b[(size_t)(y1 + d) * w + x] = 255;
            }
        }
        return true;
    }
    int t1 = (int)lround(h / 14.0);
    if (t1 < 1) t1 = 1;
    int t2 = t1 * 2 < 2 ? 2 : t1 * 2;
    int cx = w / 2, cy = h / 2;
    int gap = t1 + 1;
    for (int arm = 0; arm < 4; arm++) {
        int kind = e[arm] - '0';
        if (!kind) continue;
        int t = kind == 2 ? t2 : t1;
        for (int line = 0; line < (kind == 3 ? 2 : 1); line++) {
            int off = kind == 3 ? (line ? gap : -gap) : 0;
            int xs = cx - t / 2 + off;
            int ext_x = kind == 3 ? gap + t1 : t, ext_y = kind == 3 ? gap + t1 : t;
            (void)ext_x; (void)ext_y;
            switch (arm) {
            case 0: fill(b, w, h, 0, cy - t / 2 + off, cx + t - t / 2 + (kind == 3 ? gap : 0), cy - t / 2 + off + t); break;
            case 1: fill(b, w, h, cx - t / 2 - (kind == 3 ? gap : 0), cy - t / 2 + off, w, cy - t / 2 + off + t); break;
            case 2: fill(b, w, h, xs, 0, xs + t, cy + t - t / 2 + (kind == 3 ? gap : 0)); break;
            case 3: fill(b, w, h, xs, cy - t / 2 - (kind == 3 ? gap : 0), xs + t, h); break;
            }
        }
    }
    return true;
}

static bool g_lig = false;   /* ligatures on */
void tv_set_ligatures(bool on) { g_lig = on; }
bool tv_ligatures() { return g_lig; }

typedef struct { uint32_t gen, fontid, epoch; int cols; int brk; bool lig; int n, cap; RInst v[]; } RowCache;

typedef struct {
    VtCell *cells; int len;
    VtStyle *styles; uint32_t *ids; int nstyles;
    uint32_t *comb;
    bool lig; int brk;   /* shape ligature runs; the cursor column that must stay unshaped, or -1 */
} RowCopy;

static void copy_free(RowCopy *c) { free(c->cells); free(c->styles); free(c->ids); free(c->comb); memset(c, 0, sizeof *c); }

static void copy_row(Vt *vt, int idx, RowCopy *o) {
    int len = 0;
    VtCell *line = vt_line(vt, idx, &len);
    memset(o, 0, sizeof *o);
    if (!line || len <= 0) return;
    o->len = len;
    o->cells = static_cast<VtCell*>(malloc((size_t)len * sizeof(VtCell)));
    memcpy(o->cells, line, (size_t)len * sizeof(VtCell));
    int cap = 8, n = 0;
    o->ids = static_cast<uint32_t*>(malloc((size_t)cap * sizeof *o->ids));
    o->styles = static_cast<VtStyle*>(malloc((size_t)cap * sizeof *o->styles));
    o->ids[0] = 0; o->styles[0] = *vt_style(vt, 0); n = 1;
    uint32_t last = 0;
    for (int x = 0; x < len; x++) {
        uint32_t id = VT_CELL_STYLE(line[x]);
        if (id != last) {
            int i = 0;
            while (i < n && o->ids[i] != id) i++;
            if (i == n) {
                if (n == cap) { cap *= 2; o->ids = static_cast<uint32_t*>(realloc(o->ids, (size_t)cap * sizeof *o->ids)); o->styles = static_cast<VtStyle*>(realloc(o->styles, (size_t)cap * sizeof *o->styles)); }
                o->ids[n] = id; o->styles[n] = *vt_style(vt, id); n++;
            }
            last = id;
        }
        if (VT_CELL_COMB(line[x])) {
            if (!o->comb) o->comb = static_cast<uint32_t*>(calloc((size_t)len, sizeof *o->comb));
            o->comb[x] = vt_comb_char(vt, VT_CELL_COMB(line[x]));
        }
    }
    o->nstyles = n;
}

static const VtStyle *style_of(const RowCopy *c, uint32_t id) {
    for (int i = 0; i < c->nstyles; i++) if (c->ids[i] == id) return &c->styles[i];
    return &c->styles[0];
}

static uint32_t rgb_u32(uint32_t c) { return RGBA((c >> 16) & 255, (c >> 8) & 255, c & 255, 255); }

static void resolve(const TermPalette *p, const VtStyle *st, uint32_t *fg, uint32_t *bg, bool *has_bg) {
    uint32_t f = p->fg, b = p->bg;
    uint32_t cf = st->fg, cb = st->bg;
    if (VT_COLOR_KIND(cf) == 1) f = p->pal[cf & 0xff]; else if (VT_COLOR_KIND(cf) == 2) f = rgb_u32(cf);
    if (VT_COLOR_KIND(cb) == 1) b = p->pal[cb & 0xff]; else if (VT_COLOR_KIND(cb) == 2) b = rgb_u32(cb);
    bool hb = cb != 0;
    if (st->attrs & VT_REVERSE) { uint32_t x = f; f = b; b = x; hb = true; }
    if (st->attrs & VT_DIM) {
        uint32_t m = 0;
        for (int s = 0; s < 24; s += 8) m |= (((f >> s) & 255) + ((b >> s) & 255)) / 2 << s;
        f = m | 0xff000000u;
    }
    *fg = f; *bg = b; *has_bg = hb;
}

typedef struct { RInst *v; int n, cap; } Out;

static void out_add(Out *o, const RInst *i) {
    if (o->n == o->cap) { o->cap = o->cap ? o->cap * 2 : 256; o->v = static_cast<RInst*>(realloc(o->v, (size_t)o->cap * sizeof *o->v)); }
    o->v[o->n++] = *i;
}

/* One cell's underline: single, double, curly (a sine wave one cell wide, so neighbours join up), dotted or dashed. Everything is
 * built from small rectangles so the GL and the CPU renderer draw it the same way; plain single lines stay one rectangle. */
static void underline(Renderer *r, Out *o, int x0, int w, int cw, int ch, int asc, int lh, int kind, uint32_t col) {
    RInst inst;
    int y = asc + 1;
    if (y + lh > ch) y = ch - lh;
    auto rect = [&](int rx, int ry, int rw, int rh) {
        if (rw <= 0 || rh <= 0) return;
        r_make_rect(r, &inst, (float)rx, (float)ry, (float)rw, (float)rh, col);
        out_add(o, &inst);
    };
    switch (kind) {
    case VT_UL_DOUBLE: {
        int y1 = y - lh - 1 < 0 ? 0 : y - lh - 1;
        if (y + lh > ch - 1 && y1 > 0) { y = ch - lh; y1 = y - lh - 1 < 0 ? 0 : y - lh - 1; }
        rect(x0, y1, w, lh);
        rect(x0, y1 + lh + 1 > ch - lh ? ch - lh : y1 + lh + 1, w, lh);
        break;
    }
    case VT_UL_CURLY: {
        int amp = ch / 14 < 1 ? 1 : ch / 14;
        int base = y - amp < 0 ? 0 : y - amp;
        if (base + 2 * amp + lh > ch) base = ch - 2 * amp - lh < 0 ? 0 : ch - 2 * amp - lh;
        int cell = cw > 0 ? cw : 1;
        for (int px = 0; px < w; px++) {
            double ph = (double)((x0 + px) % cell) / (double)cell;
            int dy = (int)((1.0 - cos(ph * 6.283185307179586)) * 0.5 * (double)(2 * amp) + 0.5);
            rect(x0 + px, base + dy, 1, lh);
        }
        break;
    }
    case VT_UL_DOTTED: {
        int d = lh < 1 ? 1 : lh;
        for (int px = 0; px < w; px += 2 * d) rect(x0 + px, y, px + d > w ? w - px : d, lh);
        break;
    }
    case VT_UL_DASHED: {
        int half = w / 2 > 0 ? w / 2 : 1, gap = half / 3 > 0 ? half / 3 : 1;
        for (int px = 0; px < w; px += half) rect(x0 + px, y, half - gap > 0 ? (px + half - gap > w ? w - px : half - gap) : 1, lh);
        break;
    }
    default: rect(x0, y, w, lh); break;
    }
}

/* Shapes the ligature runs of one row: lgid[x] is the glyph index to draw at cell x instead of the character, lskip[x] marks the cells a
 * multi-cell ligature covers. Returns false when a worker pass must hand the row to the main thread (a face or shaper still to be set up). */
static bool lig_prepare(Font *f, const RowCopy *c, bool peek, uint32_t **lgid, uint8_t **lskip) {
    int n = c->len;
    uint32_t *cp = static_cast<uint32_t*>(malloc((size_t)n * (sizeof(uint32_t) * 2 + 1)));
    if (!cp) return true;
    uint32_t *sty = cp + n;
    uint8_t *flg = reinterpret_cast<uint8_t*>(sty + n);
    for (int x = 0; x < n; x++) {
        cp[x] = VT_CELL_CH(c->cells[x]);
        sty[x] = VT_CELL_STYLE(c->cells[x]);
        flg[x] = (uint8_t)((VT_CELL_FLAGS(c->cells[x]) & (VT_F_WIDE | VT_F_TAIL)) | (c->comb && c->comb[x] ? LR_COMB : 0));
        if (cp[x] == 0) cp[x] = ' ';
    }
    LigRun runs[64];
    int nr = lig_split(cp, sty, flg, n, c->brk, runs, 64);
    bool ok = true;
    for (int k = 0; k < nr && ok; k++) {
        const VtStyle *st = style_of(c, sty[runs[k].start]);
        if (st->attrs & VT_INVIS) continue;
        int fs = ((st->attrs & VT_BOLD) ? FS_BOLD : 0) | ((st->attrs & VT_ITALIC) ? FS_ITALIC : 0);
        ShapedGlyph sg[LIG_MAX_RUN];
        int ng = font_shape(f, cp + runs[k].start, runs[k].len, fs, sg, LIG_MAX_RUN, !peek);
        if (ng < 0) { ok = false; break; }
        for (int i = 0; i < ng; i++) {
            if (!*lgid) { *lgid = static_cast<uint32_t*>(calloc((size_t)n, sizeof(uint32_t))); *lskip = static_cast<uint8_t*>(calloc((size_t)n, 1)); }
            if (!*lgid || !*lskip) { ok = true; break; }
            int x = runs[k].start + sg[i].cell;
            (*lgid)[x] = sg[i].gid + 1;   /* +1: glyph index 0 is .notdef and means "none" here */
            for (int s2 = 1; s2 < sg[i].span && x + s2 < n; s2++) (*lskip)[x + s2] = 1;
        }
    }
    free(cp);
    return ok;
}

static void build_row(Renderer *r, Font *f, const TermPalette *pal, const RowCopy *c, Out *o, bool peek, bool *miss) {
    Atlas *a = r_atlas(r);
    int cw = font_cell_w(f), ch = font_cell_h(f), asc = font_ascent(f);
    uint32_t fid = font_id(f);
    uint8_t *procbuf = NULL;
    int bg_start = -1, bg_end = 0;
    uint32_t bg_col = 0;
    RInst inst;
    Out fgl = {0};
    uint32_t *lgid = NULL;
    uint8_t *lskip = NULL;
    if (c->lig && c->len > 1 && !lig_prepare(f, c, peek, &lgid, &lskip)) { *miss = true; goto abandon; }
    for (int x = 0; x < c->len;) {
        VtCell cell = c->cells[x];
        if (VT_CELL_FLAGS(cell) & VT_F_TAIL) { x++; continue; }
        const VtStyle *st = style_of(c, VT_CELL_STYLE(cell));
        uint32_t fg, bg;
        bool has_bg;
        resolve(pal, st, &fg, &bg, &has_bg);
        int wcells = (VT_CELL_FLAGS(cell) & VT_F_WIDE) ? 2 : 1;
        if (has_bg) {
            if (bg_start >= 0 && bg_col == bg && bg_end == x) bg_end = x + wcells;
            else {
                if (bg_start >= 0) { r_make_rect(r, &inst, (float)(bg_start * cw), 0, (float)((bg_end - bg_start) * cw), (float)ch, bg_col); out_add(o, &inst); }
                bg_start = x; bg_end = x + wcells; bg_col = bg;
            }
        } else if (bg_start >= 0) {
            r_make_rect(r, &inst, (float)(bg_start * cw), 0, (float)((bg_end - bg_start) * cw), (float)ch, bg_col); out_add(o, &inst);
            bg_start = -1;
        }
        uint32_t cp = VT_CELL_CH(cell);
        if (!(st->attrs & VT_INVIS)) {
            int fs = ((st->attrs & VT_BOLD) ? FS_BOLD : 0) | ((st->attrs & VT_ITALIC) ? FS_ITALIC : 0);
            if (cp > 0x20 && cp != 0x7f && !(lskip && lskip[x])) {
                const AtlasGlyph *g = NULL;
                bool proc = (cp >= 0x2500 && cp <= 0x259f) && wcells == 1;
                if (lgid && lgid[x]) {
                    uint32_t gid = lgid[x] - 1;
                    g = peek ? atlas_peek_gid(a, fid, gid, fs) : atlas_glyph_gid(a, f, gid, fs);
                    if (peek && !g) { *miss = true; goto abandon; }
                    if (g && !g->blank) { r_make_glyph(r, &inst, g, (float)(x * cw), (float)asc, fg); out_add(&fgl, &inst); }
                } else if (proc) {
                    g = atlas_find_custom(a, fid, cp);
                    if (!g && peek) { *miss = true; goto abandon; }
                    if (!g) {
                        if (!procbuf) procbuf = static_cast<uint8_t*>(malloc((size_t)cw * ch));
                        proc_bits(cp, cw, ch, procbuf);
                        g = atlas_custom(a, fid, cp, procbuf, cw, ch);
                    }
                    if (g && !g->blank) { r_make_glyph(r, &inst, g, (float)(x * cw), 0, fg); out_add(&fgl, &inst); }
                } else {
                    if (peek) { g = atlas_peek(a, fid, cp, fs); if (!g) { *miss = true; goto abandon; } }
                    else g = atlas_glyph(a, f, cp, fs);
                    if (g && !g->blank) { r_make_glyph(r, &inst, g, (float)(x * cw), (float)asc, fg); out_add(&fgl, &inst); }
                }
                if (c->comb && c->comb[x]) {
                    const AtlasGlyph *cg = peek ? atlas_peek(a, fid, c->comb[x], fs) : atlas_glyph(a, f, c->comb[x], fs);
                    if (peek && !cg) { *miss = true; goto abandon; }
                    if (cg && !cg->blank) { r_make_glyph(r, &inst, cg, (float)(x * cw), (float)asc, fg); out_add(&fgl, &inst); }
                }
            }
            int lh = ch >= 28 ? 2 : 1;
            if (st->attrs & VT_UNDERLINE) {
                uint32_t ulc = fg, cu = st->ul;
                if (VT_COLOR_KIND(cu) == 1) ulc = pal->pal[cu & 0xff]; else if (VT_COLOR_KIND(cu) == 2) ulc = rgb_u32(cu);
                underline(r, &fgl, x * cw, wcells * cw, cw, ch, asc, lh, VT_UL_KIND(st->attrs), ulc);
            }
            if (st->attrs & VT_STRIKE) { r_make_rect(r, &inst, (float)(x * cw), (float)(asc * 5 / 8), (float)(wcells * cw), (float)lh, fg); out_add(&fgl, &inst); }
        }
        x += wcells;
    }
    if (bg_start >= 0) { r_make_rect(r, &inst, (float)(bg_start * cw), 0, (float)((bg_end - bg_start) * cw), (float)ch, bg_col); out_add(o, &inst); }
    for (int i = 0; i < fgl.n; i++) out_add(o, &fgl.v[i]);
abandon:
    free(fgl.v);
    free(procbuf);
    free(lgid);
    free(lskip);
}

void tv_fit(const Font *f, float w, float h, int *cols, int *rows) {
    int c = (int)((w - 2 * TV_PAD) / font_cell_w(f)), r = (int)((h - 2 * TV_PAD) / font_cell_h(f));
    *cols = c < 1 ? 1 : c;
    *rows = r < 1 ? 1 : r;
}

#define MAX_ROWS 512

/* One inline image placement. The view holds a reference on the pixels; r_image drops it once the renderer has them. */
static void draw_image_view(Renderer *r, const VtImageView &v, float ox, float oy, int cw, int ch, int off) {
    float x = ox + (float)(v.col * cw), y = oy + (float)((v.idx + off) * ch);
    float w = v.stretch ? (float)(v.cols * cw) : (float)v.sw, h = v.stretch ? (float)(v.rows * ch) : (float)v.sh;
    r_image(r, v.serial, v.px, v.iw, v.ih, v.img, vt_image_unref, x, y, w, h, (float)v.sx / (float)v.iw, (float)v.sy / (float)v.ih,
            (float)(v.sx + v.sw) / (float)v.iw, (float)(v.sy + v.sh) / (float)v.ih);
}

typedef struct { Renderer *r; Font *f; const TermPalette *pal; RowCopy *copies; Out *outs; bool *missed; const int *todo; } RTask;

static void rtask_fn(void *arg, int i) {
    RTask *c = static_cast<RTask*>(arg);
    int row = c->todo[i];
    bool miss = false;
    build_row(c->r, c->f, c->pal, &c->copies[row], &c->outs[row], true, &miss);
    if (miss) { free(c->outs[row].v); c->outs[row] = (Out){0}; c->missed[row] = true; }
}

void tv_draw(TermCore *t, Renderer *r, Font *f, const TermPalette *pal, float x, float y, float w, float h, bool blink_on, const TvOverlay *ov) {
    Vt *vt = tcore_vt(t);
    Atlas *a = r_atlas(r);
    int cols = tcore_cols(t), rows = tcore_rows(t), off = tcore_offset(t);
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    int cw = font_cell_w(f), ch = font_cell_h(f);
    float ox = x + TV_PAD, oy = y + TV_PAD;
    r_rect(r, x, y, w, h, pal->bg);
    r_clip(r, (int)x, (int)y, (int)w, (int)h);

    RowCopy copies[MAX_ROWS];
    VtLineMeta *metas[MAX_ROWS];
    bool need[MAX_ROWS];
    int cursor_x = -1, cursor_y = -1, cursor_style = 0;
    VtCell cur_cell = {0, 0};
    VtImageView views[32];
    int nviews = 0;
    tcore_lock(t);
    vt_set_cell_px(vt, cw, ch);
    nviews = vt_images_in_view(vt, -off, rows, views, 32);
    tcore_unlock(t);
    for (int i = 0; i < nviews; i++) if (views[i].z < 0) draw_image_view(r, views[i], ox, oy, cw, ch, off);   /* under the text */
    size_t mark = r_mark(r);

    for (int attempt = 0; attempt < 2; attempt++) {
        r_rewind(r, mark);
        uint32_t gen0 = atlas_generation(a);
        uint32_t fid = font_id(f);
        memset(copies, 0, (size_t)(rows > 0 ? rows : 0) * sizeof *copies);
        tcore_lock(t);
        const bool lig = g_lig;
        int lig_cy = -1, lig_cx = -1;   /* the cursor cell stays unshaped so a ligature is broken under the cursor */
        if (lig && off == 0 && (vt_modes(vt) & VT_M_CURSOR_VISIBLE)) { lig_cy = vt_cursor_y(vt); lig_cx = vt_cursor_x(vt); if (lig_cx >= cols) lig_cx = cols - 1; }
        for (int row = 0; row < rows; row++) {
            metas[row] = vt_line_meta(vt, row - off);
            need[row] = false;
            VtLineMeta *m = metas[row];
            if (!m) continue;
            RowCache *rc = static_cast<RowCache*>(m->cache);
            if (rc && !m->dirty && rc->gen == gen0 && rc->fontid == fid && rc->epoch == pal->epoch && rc->cols == cols && rc->lig == lig && rc->brk == (row == lig_cy ? lig_cx : -1)) {
                r_push_at(r, rc->v, rc->n, ox, oy + (float)(row * ch));
                sd_cache.row_hit++;
            } else {
                sd_cache.row_miss++;
                copy_row(vt, row - off, &copies[row]);
                copies[row].lig = lig; copies[row].brk = row == lig_cy ? lig_cx : -1;
                m->dirty = 0;
                need[row] = true;
            }
        }
        if (off == 0 && (vt_modes(vt) & VT_M_CURSOR_VISIBLE)) {
            cursor_x = vt_cursor_x(vt); cursor_y = vt_cursor_y(vt); cursor_style = vt_cursor_style(vt);
            if (cursor_x >= cols) cursor_x = cols - 1;
            int ll = 0;
            VtCell *l = cursor_y < rows ? vt_line(vt, cursor_y, &ll) : NULL;
            if (l && cursor_x < ll) cur_cell = l[cursor_x];
            else cur_cell = (VtCell){0, 0};
        } else cursor_x = -1;
        tcore_unlock(t);

        Out outs[MAX_ROWS];
        memset(outs, 0, (size_t)(rows > 0 ? rows : 0) * sizeof *outs);
        {
            int todo[MAX_ROWS], nt = 0;
            for (int row = 0; row < rows; row++) if (need[row]) todo[nt++] = row;
            bool missed[MAX_ROWS];
            memset(missed, 0, (size_t)(rows > 0 ? rows : 0) * sizeof *missed);
            if (nt >= 8 && wp_workers() > 0) {
                RTask ctx = {r, f, pal, copies, outs, missed, todo};
                wp_run(rtask_fn, &ctx, nt);
                for (int k = 0; k < nt; k++) {
                    int row = todo[k];
                    if (missed[row]) build_row(r, f, pal, &copies[row], &outs[row], false, NULL);
                    else sd_cache.glyph_hit += (uint64_t)outs[row].n;
                }
            } else {
                for (int k = 0; k < nt; k++) build_row(r, f, pal, &copies[todo[k]], &outs[todo[k]], false, NULL);
            }
            for (int k = 0; k < nt; k++) copy_free(&copies[todo[k]]);
        }
        bool reset = atlas_generation(a) != gen0;
        if (reset && attempt == 0) {
            for (int row = 0; row < rows; row++) free(outs[row].v);
            tcore_lock(t);
            vt_mark_all_dirty(vt);
            tcore_unlock(t);
            continue;
        }
        for (int row = 0; row < rows; row++) if (need[row]) r_push_at(r, outs[row].v, outs[row].n, ox, oy + (float)(row * ch));
        tcore_lock(t);
        for (int row = 0; row < rows; row++) {
            if (!need[row]) continue;
            VtLineMeta *m = metas[row];
            if (m && !m->dirty && !reset) {
                RowCache *rc = static_cast<RowCache*>(m->cache);
                if (rc && rc->cap >= outs[row].n && rc->cap <= outs[row].n + 64) sd_cache.row_recycle++;
                else {
                    int cap = outs[row].n + 16;
                    RowCache *nr = static_cast<RowCache*>(malloc(sizeof(RowCache) + (size_t)cap * sizeof(RInst)));
                    if (nr) { nr->cap = cap; free(m->cache); m->cache = rc = nr; } else rc = NULL;
                }
                if (rc) {
                    rc->gen = gen0; rc->fontid = fid; rc->epoch = pal->epoch; rc->cols = cols; rc->lig = lig; rc->brk = row == lig_cy ? lig_cx : -1; rc->n = outs[row].n;
                    if (outs[row].n) memcpy(rc->v, outs[row].v, (size_t)outs[row].n * sizeof(RInst));
                }
            }
        }
        tcore_unlock(t);
        for (int row = 0; row < rows; row++) free(outs[row].v);
        break;
    }

    for (int i = 0; i < nviews; i++) if (views[i].z >= 0) draw_image_view(r, views[i], ox, oy, cw, ch, off);   /* over the text */

    int ai, ac, bi, bc;
    if (tcore_selection(t, &ai, &ac, &bi, &bc)) {
        for (int row = 0; row < rows; row++) {
            int idx = row - off;
            if (idx < ai || idx > bi) continue;
            int lo = idx == ai ? ac : 0, hi = idx == bi ? bc : cols - 1;
            r_rect(r, ox + (float)(lo * cw), oy + (float)(row * ch), (float)((hi - lo + 1) * cw), (float)ch, pal->sel);
        }
    }
    if (ov && ov->nmatches > 0) {
        int hi_idx = rows - 1 - off, lo_idx = -off;
        int lo = 0, hi = ov->nmatches;   /* first match with idx <= hi_idx: the list is newest first */
        while (lo < hi) { int mid = (lo + hi) / 2; if (ov->matches[mid].idx > hi_idx) lo = mid + 1; else hi = mid; }
        for (int i = lo; i < ov->nmatches && ov->matches[i].idx >= lo_idx; i++) {
            const FindMatch &m = ov->matches[i];
            int c0 = m.col, c1 = m.col + m.len;
            if (c0 >= cols) continue;
            if (c1 > cols) c1 = cols;
            uint32_t col = i == ov->current ? RGBA(255, 150, 30, 190) : RGBA(255, 214, 0, 110);
            r_rect(r, ox + (float)(c0 * cw), oy + (float)((m.idx + off) * ch), (float)((c1 - c0) * cw), (float)ch, col);
        }
    }
    if (ov && ov->link) {
        int row = ov->link_idx + off;
        if (row >= 0 && row < rows) {
            int lh = ch >= 28 ? 2 : 1, c1 = ov->link_c1 >= cols ? cols - 1 : ov->link_c1;
            if (c1 >= ov->link_c0) r_rect(r, ox + (float)(ov->link_c0 * cw), oy + (float)(row * ch + ch - 1 - lh), (float)((c1 - ov->link_c0 + 1) * cw), (float)lh + 1, RGBA(90, 170, 255, 255));
        }
    }
    if (cursor_x >= 0 && cursor_y >= 0 && cursor_y < rows) {
        float cx = ox + (float)(cursor_x * cw), cy = oy + (float)(cursor_y * ch);
        uint32_t col = (pal->cursor & 0x00ffffffu) | (217u << 24);
        if (!tcore_focused(t)) {
            r_rect(r, cx, cy, (float)cw, 1, col); r_rect(r, cx, cy + ch - 1, (float)cw, 1, col);
            r_rect(r, cx, cy, 1, (float)ch, col); r_rect(r, cx + cw - 1, cy, 1, (float)ch, col);
        } else if (blink_on || cursor_style == 2 || cursor_style == 4 || cursor_style == 6 || cursor_style == 0) {
            if (cursor_style == 3 || cursor_style == 4) r_rect(r, cx, cy + ch - 2, (float)cw, 2, col);
            else if (cursor_style == 5 || cursor_style == 6) r_rect(r, cx, cy, 2, (float)ch, col);
            else {
                r_rect(r, cx, cy, (float)cw, (float)ch, col);
                uint32_t cp = VT_CELL_CH(cur_cell);
                if (cp > 0x20) {
                    const AtlasGlyph *g = atlas_glyph(a, f, cp, 0);
                    if (g && !g->blank) { RInst i; r_make_glyph(r, &i, g, cx, cy + (float)font_ascent(f), pal->bg); r_push(r, &i, 1); }
                }
            }
        }
    }
    r_clip_off(r);
}
