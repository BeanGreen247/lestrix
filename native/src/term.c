#include "term.h"

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <gdk/gdkkeysyms.h>
#include <glib-unix.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pty.h"
#include "vt.h"

#define PAD 8

static int default_scrollback = 10000;
static size_t default_ram_mb = 32, default_disk_mb = 4096;
static gboolean default_spill = TRUE;
void sd_term_set_scrollback(int lines) { default_scrollback = lines; }   /* -1 = unlimited */
void sd_term_set_history_defaults(int lines, size_t ram_mb, size_t disk_mb, gboolean spill) {
    default_scrollback = lines;
    if (ram_mb) default_ram_mb = ram_mb;
    if (disk_mb) default_disk_mb = disk_mb;
    default_spill = spill;
}

struct _SdTerm {
    GtkWidget parent;
    Vt *vt;
    SdPty pty;
    guint child_src;
    gboolean running, eof, child_done;
    /* a worker thread per terminal reads the pty and parses; the UI thread only draws */
    GRecMutex lock;            /* guards vt and everything the renderer reads from it */
    GThread *worker;
    int wake_fd;               /* eventfd: tells the worker there is output to write or to stop */
    gint stop, idle_pending, act_flag, eof_flag, running_flag;
    guint64 bytes_fed;
    GMutex outlock, evlock;
    GList *events;             /* VtEvent notifications waiting for the UI thread */
    int exit_status;
    char **argv;
    char *cwd, *title;
    GByteArray *outq;

    PangoFontDescription *font;
    PangoLayout *layout;
    PangoAttrList *attrs[16];
    double cw, ch;
    int ascent;
    int cols, rows;
    int offset;
    double scroll_acc;

    gboolean sel_on, dragging;
    int sel_a_idx, sel_a_col, sel_b_idx, sel_b_col;

    const SdTheme *theme;
    GdkRGBA pal[256], fg, bg, sel;
    GtkIMContext *im;
    gboolean focused;
    int mouse_btn;

    /* software path: one persistent image, only changed rows are repainted */
    gboolean cpu, cpu_forced;
    cairo_surface_t *surf;
    int surf_w, surf_h, scale;
    VtLineMeta **rendered;   /* which line each screen row of `surf` currently shows */
};

enum { SIG_TITLE, SIG_CWD, SIG_BELL, SIG_ACTIVITY, SIG_EXITED, SIG_RESTARTED, N_SIGS };
static guint sigs[N_SIGS];

G_DEFINE_FINAL_TYPE(SdTerm, sd_term, GTK_TYPE_WIDGET)

#define VT_LOCK(t) g_rec_mutex_lock(&(t)->lock)
#define VT_UNLOCK(t) g_rec_mutex_unlock(&(t)->lock)

static uint32_t term_modes(SdTerm *t) {
    VT_LOCK(t);
    uint32_t m = vt_modes(t->vt);
    VT_UNLOCK(t);
    return m;
}

/* ---- theme / font ---------------------------------------------------------------------- */

static void cpu_invalidate(SdTerm *t);

static void build_palette(SdTerm *t) {
    for (int i = 0; i < 16; i++) sd_color(&t->pal[i], t->theme->ansi[i]);
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; i++) {
        t->pal[16 + i] = (GdkRGBA){lv[i / 36] / 255.f, lv[(i / 6) % 6] / 255.f, lv[i % 6] / 255.f, 1};
    }
    for (int i = 0; i < 24; i++) {
        float v = (8 + 10 * i) / 255.f;
        t->pal[232 + i] = (GdkRGBA){v, v, v, 1};
    }
    sd_color(&t->fg, t->theme->term_fg);
    sd_color(&t->bg, t->theme->term_bg);
    sd_color(&t->sel, t->theme->accent);
    t->sel.alpha = 0.40f;
}

static void measure_font(SdTerm *t) {
    pango_layout_set_font_description(t->layout, t->font);
    pango_layout_set_text(t->layout, "MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM", -1);
    pango_layout_set_attributes(t->layout, NULL);
    int w, h;
    pango_layout_get_size(t->layout, &w, &h);
    t->cw = (double)w / PANGO_SCALE / 100.0;
    PangoFontMetrics *m = pango_context_get_metrics(pango_layout_get_context(t->layout), t->font, NULL);
    t->ascent = pango_font_metrics_get_ascent(m) / PANGO_SCALE;
    pango_font_metrics_unref(m);
    t->ch = ceil((double)h / PANGO_SCALE);
    if (t->cw < 1) t->cw = 8;
    if (t->ch < 1) t->ch = 16;
}

static void make_attr_lists(SdTerm *t) {
    for (int m = 0; m < 16; m++) {
        if (t->attrs[m]) pango_attr_list_unref(t->attrs[m]);
        PangoAttrList *l = pango_attr_list_new();
        if (m & 1) pango_attr_list_insert(l, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        if (m & 2) pango_attr_list_insert(l, pango_attr_style_new(PANGO_STYLE_ITALIC));
        if (m & 4) pango_attr_list_insert(l, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
        if (m & 8) pango_attr_list_insert(l, pango_attr_strikethrough_new(TRUE));
        pango_attr_list_insert(l, pango_attr_fallback_new(TRUE));
        t->attrs[m] = l;
    }
}

static void relayout(SdTerm *t) {
    int w = gtk_widget_get_width(GTK_WIDGET(t)), h = gtk_widget_get_height(GTK_WIDGET(t));
    int cols = (int)((w - 2 * PAD) / t->cw), rows = (int)((h - 2 * PAD) / t->ch);
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    VT_LOCK(t);
    if (cols != t->cols || rows != t->rows) {
        t->cols = cols;
        t->rows = rows;
        vt_resize(t->vt, cols, rows);
        g_free(t->rendered);
        t->rendered = NULL;
        if (t->running) sd_pty_resize(&t->pty, cols, rows);
        t->offset = 0;
        t->sel_on = FALSE;
    }
    vt_mark_all_dirty(t->vt);
    VT_UNLOCK(t);
    cpu_invalidate(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void sd_term_set_theme(SdTerm *t, const SdTheme *theme) {
    VT_LOCK(t);
    t->theme = theme;
    build_palette(t);
    vt_mark_all_dirty(t->vt);
    cpu_invalidate(t);
    VT_UNLOCK(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void sd_term_set_font(SdTerm *t, const char *family, int size_pt) {
    if (t->font) pango_font_description_free(t->font);
    t->font = pango_font_description_new();
    pango_font_description_set_family(t->font, family && *family ? family : "monospace");
    pango_font_description_set_size(t->font, size_pt * PANGO_SCALE);
    measure_font(t);
    if (gtk_widget_get_realized(GTK_WIDGET(t))) relayout(t);
}

/* ---- rendering ------------------------------------------------------------------------- */

static void cache_free_cb(void *cache, void *user) { (void)user; gsk_render_node_unref(cache); }

static void resolve(SdTerm *t, const VtStyle *st, GdkRGBA *fg, GdkRGBA *bg, gboolean *has_bg) {
    GdkRGBA f = t->fg, b = t->bg;
    uint32_t cf = st->fg, cb = st->bg;
    if (VT_COLOR_KIND(cf) == 1) f = t->pal[cf & 0xff];
    else if (VT_COLOR_KIND(cf) == 2) f = (GdkRGBA){((cf >> 16) & 255) / 255.f, ((cf >> 8) & 255) / 255.f, (cf & 255) / 255.f, 1};
    if (VT_COLOR_KIND(cb) == 1) b = t->pal[cb & 0xff];
    else if (VT_COLOR_KIND(cb) == 2) b = (GdkRGBA){((cb >> 16) & 255) / 255.f, ((cb >> 8) & 255) / 255.f, (cb & 255) / 255.f, 1};
    gboolean hb = cb != 0;
    if (st->attrs & VT_REVERSE) { GdkRGBA x = f; f = b; b = x; hb = TRUE; }
    if (st->attrs & VT_DIM) { f.red = (f.red + b.red) / 2; f.green = (f.green + b.green) / 2; f.blue = (f.blue + b.blue) / 2; }
    *fg = f; *bg = b; *has_bg = hb;
}

/* styles are interned, so equal ids mean equal colours and attributes */
static inline gboolean same_style(VtCell a, VtCell b) { return VT_CELL_STYLE(a) == VT_CELL_STYLE(b); }

static inline gboolean plain_cell(VtCell c) {
    return VT_CELL_FLAGS(c) == 0 && c.cp < 0x7f && (c.cp == 0 || c.cp >= 0x20);   /* no combining mark, no wide half */
}

static void draw_text(SdTerm *t, GtkSnapshot *s, double x, const char *text, int len, const GdkRGBA *fg, uint16_t attrs) {
    int mask = ((attrs & VT_BOLD) ? 1 : 0) | ((attrs & VT_ITALIC) ? 2 : 0) | ((attrs & VT_UNDERLINE) ? 4 : 0) |
               ((attrs & VT_STRIKE) ? 8 : 0);
    pango_layout_set_text(t->layout, text, len);
    pango_layout_set_attributes(t->layout, t->attrs[mask]);
    gtk_snapshot_save(s);
    graphene_point_t p = GRAPHENE_POINT_INIT((float)x, 0);
    gtk_snapshot_translate(s, &p);
    gtk_snapshot_append_layout(s, t->layout, fg);
    gtk_snapshot_restore(s);
}

typedef struct {
    void (*bg)(void *ctx, double x0, double x1, const GdkRGBA *c);
    void (*text)(void *ctx, double x, const char *text, int len, const GdkRGBA *fg, uint16_t attrs);
} RowSink;

/* Walk one line and emit background rectangles and text runs. Shared by the GPU (render
 * node) and CPU (cairo image) paths so both draw exactly the same thing. */
static void walk_row(SdTerm *t, int idx, const RowSink *sink, void *ctx) {
    int len = 0;
    VtCell *line = vt_line(t->vt, idx, &len);
    char buf[1024];
    for (int x = 0; line && x < len;) {
        VtCell c = line[x];
        const VtStyle *st = vt_style(t->vt, VT_CELL_STYLE(c));
        GdkRGBA fg, bg;
        gboolean has_bg;
        resolve(t, st, &fg, &bg, &has_bg);
        if (plain_cell(c)) {
            int n = 0, end = x;
            while (end < len && plain_cell(line[end]) && same_style(line[end], c) && n < (int)sizeof buf - 1) {
                buf[n++] = line[end].cp ? (char)line[end].cp : ' ';
                end++;
            }
            if (has_bg) sink->bg(ctx, floor(x * t->cw), ceil(end * t->cw), &bg);
            gboolean blank = TRUE;
            for (int i = 0; i < n; i++) if (buf[i] != ' ') { blank = FALSE; break; }
            if (!(st->attrs & VT_INVIS) && (!blank || (st->attrs & (VT_UNDERLINE | VT_STRIKE)))) sink->text(ctx, x * t->cw, buf, n, &fg, st->attrs);
            x = end;
        } else {
            int w = (VT_CELL_FLAGS(c) & VT_F_WIDE) ? 2 : 1;
            if (VT_CELL_FLAGS(c) & VT_F_TAIL) { x++; continue; }
            if (has_bg) sink->bg(ctx, floor(x * t->cw), ceil((x + w) * t->cw), &bg);
            if (!(st->attrs & VT_INVIS)) {
                char u[16];
                uint32_t ch = VT_CELL_CH(c);
                int n = g_unichar_to_utf8(ch ? ch : ' ', u);
                uint32_t comb = vt_comb_char(t->vt, VT_CELL_COMB(c));
                if (comb) n += g_unichar_to_utf8(comb, u + n);
                sink->text(ctx, x * t->cw, u, n, &fg, st->attrs);
            }
            x += w;
        }
    }
}

/* GPU path: a line becomes a cached render node */
static void node_bg(void *ctx, double x0, double x1, const GdkRGBA *c) {
    SdTerm *t = ((void **)ctx)[0];
    graphene_rect_t r = GRAPHENE_RECT_INIT((float)x0, 0, (float)(x1 - x0), (float)t->ch);
    gtk_snapshot_append_color(((void **)ctx)[1], c, &r);
}
static void node_text(void *ctx, double x, const char *text, int len, const GdkRGBA *fg, uint16_t attrs) {
    draw_text(((void **)ctx)[0], ((void **)ctx)[1], x, text, len, fg, attrs);
}

static GskRenderNode *build_row(SdTerm *t, int idx) {
    GtkSnapshot *s = gtk_snapshot_new();
    void *ctx[2] = {t, s};
    static const RowSink sink = {node_bg, node_text};
    walk_row(t, idx, &sink, ctx);
    GskRenderNode *n = gtk_snapshot_free_to_node(s);
    return n ? n : gsk_container_node_new(NULL, 0); /* an empty row still needs a node */
}

/* CPU path: a line is painted into the persistent image */
typedef struct { SdTerm *t; cairo_t *cr; double y; } CairoCtx;

static void cairo_bg(void *ctx, double x0, double x1, const GdkRGBA *c) {
    CairoCtx *k = ctx;
    cairo_set_source_rgb(k->cr, c->red, c->green, c->blue);
    cairo_rectangle(k->cr, x0, k->y, x1 - x0, k->t->ch);
    cairo_fill(k->cr);
}
static void cairo_text(void *ctx, double x, const char *text, int len, const GdkRGBA *fg, uint16_t attrs) {
    CairoCtx *k = ctx;
    SdTerm *t = k->t;
    int mask = ((attrs & VT_BOLD) ? 1 : 0) | ((attrs & VT_ITALIC) ? 2 : 0) | ((attrs & VT_UNDERLINE) ? 4 : 0) |
               ((attrs & VT_STRIKE) ? 8 : 0);
    pango_layout_set_text(t->layout, text, len);
    pango_layout_set_attributes(t->layout, t->attrs[mask]);
    cairo_set_source_rgb(k->cr, fg->red, fg->green, fg->blue);
    cairo_move_to(k->cr, x, k->y);
    pango_cairo_update_layout(k->cr, t->layout);
    pango_cairo_show_layout(k->cr, t->layout);
}

static void cpu_invalidate(SdTerm *t) {
    if (t->rendered) memset(t->rendered, 0, (size_t)t->rows * sizeof *t->rendered);
}

static void cpu_ensure_surface(SdTerm *t) {
    int sc = gtk_widget_get_scale_factor(GTK_WIDGET(t));
    int w = (int)ceil(t->cols * t->cw), h = (int)(t->rows * t->ch);
    if (t->surf && t->rendered && t->surf_w == w && t->surf_h == h && t->scale == sc) return;
    if (t->surf) cairo_surface_destroy(t->surf);
    t->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * sc, h * sc);
    cairo_surface_set_device_scale(t->surf, sc, sc);
    t->surf_w = w; t->surf_h = h; t->scale = sc;
    g_free(t->rendered);
    t->rendered = g_new0(VtLineMeta *, t->rows);
}

/* Bring the image up to date. A scroll moves existing pixels with one memmove; only the
 * rows that actually changed are painted. */
static void cpu_update(SdTerm *t) {
    cpu_ensure_surface(t);
    VtLineMeta *cur[512];
    int rows = t->rows < 512 ? t->rows : 512;
    for (int y = 0; y < rows; y++) cur[y] = vt_line_meta(t->vt, y - t->offset);

    cairo_surface_flush(t->surf);
    int stride = cairo_image_surface_get_stride(t->surf);
    guchar *data = cairo_image_surface_get_data(t->surf);
    int row_px = (int)t->ch * t->scale;
    for (int k = 1; k < rows; k++) { /* content moved up by k rows? */
        if (t->rendered[k] != cur[0] || !cur[0]) continue;
        gboolean ok = TRUE;
        for (int y = 0; y + k < rows && ok; y++) ok = t->rendered[y + k] == cur[y] && cur[y] && !cur[y]->dirty;
        if (!ok) continue;
        memmove(data, data + (size_t)k * row_px * stride, (size_t)(rows - k) * row_px * stride);
        memmove(t->rendered, t->rendered + k, (size_t)(rows - k) * sizeof *t->rendered);
        for (int y = rows - k; y < rows; y++) t->rendered[y] = NULL;
        break;
    }
    cairo_surface_mark_dirty(t->surf);

    cairo_t *cr = cairo_create(t->surf);
    for (int y = 0; y < rows; y++) {
        if (!cur[y]) continue;
        if (t->rendered[y] == cur[y] && !cur[y]->dirty) continue;
        cairo_set_source_rgb(cr, t->bg.red, t->bg.green, t->bg.blue);
        cairo_rectangle(cr, 0, y * t->ch, t->surf_w, t->ch);
        cairo_fill(cr);
        CairoCtx k = {t, cr, y * t->ch};
        static const RowSink sink = {cairo_bg, cairo_text};
        walk_row(t, y - t->offset, &sink, &k);
        cur[y]->dirty = 0;
        t->rendered[y] = cur[y];
    }
    cairo_destroy(cr);
}

static gboolean use_cpu_path(SdTerm *t) {
    if (t->cpu_forced) return TRUE;
    GtkNative *n = gtk_widget_get_native(GTK_WIDGET(t));
    GskRenderer *r = n ? gtk_native_get_renderer(n) : NULL;
    return r && strstr(G_OBJECT_TYPE_NAME(r), "Cairo") != NULL;
}

static void paint_cursor(SdTerm *t, GtkSnapshot *s) {
    if (t->offset || !(vt_modes(t->vt) & VT_M_CURSOR_VISIBLE)) return;
    int cx = vt_cursor_x(t->vt), cy = vt_cursor_y(t->vt);
    if (cx >= t->cols) cx = t->cols - 1;
    if (cy >= t->rows) return;
    double x = cx * t->cw, y = cy * t->ch;
    int style = vt_cursor_style(t->vt);
    GdkRGBA c = t->fg;
    c.alpha = 0.85f;
    if (!t->focused) {
        graphene_rect_t r = GRAPHENE_RECT_INIT((float)x + 0.5f, (float)y + 0.5f, (float)t->cw - 1, (float)t->ch - 1);
        GdkRGBA bw[4] = {c, c, c, c};
        float widths[4] = {1, 1, 1, 1};
        GskRoundedRect rr;
        gsk_rounded_rect_init_from_rect(&rr, &r, 0);
        gtk_snapshot_append_border(s, &rr, widths, bw);
        return;
    }
    graphene_rect_t r;
    if (style == 3 || style == 4) r = (graphene_rect_t)GRAPHENE_RECT_INIT((float)x, (float)(y + t->ch - 2), (float)t->cw, 2);
    else if (style == 5 || style == 6) r = (graphene_rect_t)GRAPHENE_RECT_INIT((float)x, (float)y, 2, (float)t->ch);
    else r = (graphene_rect_t)GRAPHENE_RECT_INIT((float)x, (float)y, (float)t->cw, (float)t->ch);
    gtk_snapshot_append_color(s, &c, &r);
    if (style < 3) { /* block: redraw the glyph in the background colour */
        int llen = 0;
        VtCell *line = vt_line(t->vt, cy, &llen);
        if (line && cx < llen && VT_CELL_CH(line[cx]) > 0x20) {
            char u[8];
            int n = g_unichar_to_utf8(VT_CELL_CH(line[cx]), u);
            gtk_snapshot_save(s);
            graphene_point_t p = GRAPHENE_POINT_INIT(0, (float)y);
            gtk_snapshot_translate(s, &p);
            draw_text(t, s, x, u, n, &t->bg, vt_style(t->vt, VT_CELL_STYLE(line[cx]))->attrs);
            gtk_snapshot_restore(s);
        }
    }
}

static void paint_selection(SdTerm *t, GtkSnapshot *s) {
    if (!t->sel_on) return;
    int ai = t->sel_a_idx, ac = t->sel_a_col, bi = t->sel_b_idx, bc = t->sel_b_col;
    if (ai > bi || (ai == bi && ac > bc)) { int x = ai; ai = bi; bi = x; x = ac; ac = bc; bc = x; }
    for (int y = 0; y < t->rows; y++) {
        int idx = y - t->offset;
        if (idx < ai || idx > bi) continue;
        int lo = idx == ai ? ac : 0, hi = idx == bi ? bc : t->cols - 1;
        graphene_rect_t r = GRAPHENE_RECT_INIT((float)(lo * t->cw), (float)(y * t->ch), (float)((hi - lo + 1) * t->cw), (float)t->ch);
        gtk_snapshot_append_color(s, &t->sel, &r);
    }
}

static void snapshot_locked(GtkWidget *w, GtkSnapshot *snap) {
    SdTerm *t = SD_TERM(w);
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    graphene_rect_t all = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
    gtk_snapshot_append_color(snap, &t->bg, &all);
    gtk_snapshot_save(snap);
    graphene_point_t pad = GRAPHENE_POINT_INIT(PAD, PAD);
    gtk_snapshot_translate(snap, &pad);
    gboolean cpu = use_cpu_path(t);
    if (cpu != t->cpu) { t->cpu = cpu; vt_mark_all_dirty(t->vt); cpu_invalidate(t); }
    if (cpu) {
        cpu_update(t);
        /* hand the image to GTK as a texture over our own memory: no per-frame copy */
        cairo_surface_flush(t->surf);
        int stride = cairo_image_surface_get_stride(t->surf);
        int pw = cairo_image_surface_get_width(t->surf), ph = cairo_image_surface_get_height(t->surf);
        GBytes *bytes = g_bytes_new_static(cairo_image_surface_get_data(t->surf), (gsize)stride * (gsize)ph);
        GdkTexture *tex = gdk_memory_texture_new(pw, ph, GDK_MEMORY_B8G8R8A8_PREMULTIPLIED, bytes, (gsize)stride);
        graphene_rect_t tr = GRAPHENE_RECT_INIT(0, 0, (float)t->surf_w, (float)t->surf_h);
        gtk_snapshot_append_texture(snap, tex, &tr);
        g_object_unref(tex);
        g_bytes_unref(bytes);
    } else {
        for (int y = 0; y < t->rows; y++) {
            VtLineMeta *m = vt_line_meta(t->vt, y - t->offset);
            if (!m) continue;
            if (m->dirty || !m->cache) {
                if (m->cache) gsk_render_node_unref(m->cache);
                m->cache = build_row(t, y - t->offset);
                m->dirty = 0;
            }
            gtk_snapshot_save(snap);
            graphene_point_t p = GRAPHENE_POINT_INIT(0, (float)(y * t->ch));
            gtk_snapshot_translate(snap, &p);
            gtk_snapshot_append_node(snap, m->cache);
            gtk_snapshot_restore(snap);
        }
    }
    paint_selection(t, snap);
    paint_cursor(t, snap);
    gtk_snapshot_restore(snap);
}

static void sd_term_snapshot(GtkWidget *w, GtkSnapshot *snap) {
    SdTerm *t = SD_TERM(w);
    VT_LOCK(t);   /* the renderer reads cells and styles the parser thread writes */
    snapshot_locked(w, snap);
    VT_UNLOCK(t);
}

static void sd_term_measure(GtkWidget *w, GtkOrientation o, int for_size, int *min, int *nat, int *minb, int *natb) {
    SdTerm *t = SD_TERM(w);
    if (o == GTK_ORIENTATION_HORIZONTAL) { *min = (int)(10 * t->cw) + 2 * PAD; *nat = (int)(80 * t->cw) + 2 * PAD; }
    else { *min = (int)(4 * t->ch) + 2 * PAD; *nat = (int)(24 * t->ch) + 2 * PAD; }
    *minb = *natb = -1;
}

static void sd_term_size_allocate(GtkWidget *w, int width, int height, int baseline) {
    (void)width; (void)height; (void)baseline;
    relayout(SD_TERM(w));
}

/* ---- pty plumbing: worker thread parses, the UI thread draws ------------------------------------- */

typedef struct { VtEvent ev; char *text; } PendingEvent;

static void wake_worker(SdTerm *t) {
    if (t->wake_fd >= 0) { guint64 one = 1; ssize_t r = write(t->wake_fd, &one, sizeof one); (void)r; }
}

/* any thread: queue bytes for the child; the worker writes them */
void sd_term_send(SdTerm *t, const char *data, gsize len) {
    if (!len || !g_atomic_int_get(&t->running_flag)) return;
    g_mutex_lock(&t->outlock);
    g_byte_array_append(t->outq, (const guint8 *)data, (guint)len);
    g_mutex_unlock(&t->outlock);
    wake_worker(t);
}

static void vt_write_cb(const uint8_t *d, size_t n, void *user) { sd_term_send(user, (const char *)d, n); }

/* runs on the worker thread inside vt_feed: hand the notification to the UI thread */
static void post_idle(SdTerm *t);
static void vt_event_cb(VtEvent ev, const char *text, void *user) {
    SdTerm *t = user;
    PendingEvent *pe = g_new0(PendingEvent, 1);
    pe->ev = ev;
    pe->text = g_strdup(text);
    g_mutex_lock(&t->evlock);
    t->events = g_list_append(t->events, pe);
    g_mutex_unlock(&t->evlock);
    post_idle(t);
}

static void term_print(SdTerm *t, const char *s) {
    VT_LOCK(t);
    vt_feed(t->vt, (const uint8_t *)s, strlen(s));
    VT_UNLOCK(t);
}

static void finish(SdTerm *t) {
    if (!t->eof || !t->child_done) return;
    t->running = FALSE;
    g_atomic_int_set(&t->running_flag, 0);
    sd_pty_close(&t->pty);
    char msg[160];
    snprintf(msg, sizeof msg, "\r\n\x1b[2m[session ended, exit code %d - press Enter to reconnect]\x1b[0m\r\n", t->exit_status);
    term_print(t, msg);
    gtk_widget_queue_draw(GTK_WIDGET(t));
    g_signal_emit(t, sigs[SIG_EXITED], 0, t->exit_status);
}

static void join_worker(SdTerm *t) {
    if (!t->worker) return;
    g_atomic_int_set(&t->stop, 1);
    wake_worker(t);
    g_thread_join(t->worker);
    t->worker = NULL;
    if (t->wake_fd >= 0) { close(t->wake_fd); t->wake_fd = -1; }
}

/* UI thread: deliver what the worker queued (one idle per burst, not per chunk) */
static gboolean main_idle(gpointer data) {
    SdTerm *t = data;
    g_atomic_int_set(&t->idle_pending, 0);
    g_mutex_lock(&t->evlock);
    GList *evs = t->events;
    t->events = NULL;
    g_mutex_unlock(&t->evlock);
    for (GList *l = evs; l; l = l->next) {
        PendingEvent *pe = l->data;
        if (pe->ev == VT_EV_TITLE) { g_free(t->title); t->title = g_strdup(pe->text); g_signal_emit(t, sigs[SIG_TITLE], 0, pe->text); }
        else if (pe->ev == VT_EV_CWD) g_signal_emit(t, sigs[SIG_CWD], 0, pe->text);
        else g_signal_emit(t, sigs[SIG_BELL], 0);
        g_free(pe->text);
        g_free(pe);
    }
    g_list_free(evs);
    if (g_atomic_int_compare_and_exchange(&t->act_flag, 1, 0)) {
        t->offset = 0;
        t->sel_on = FALSE;
        gtk_widget_queue_draw(GTK_WIDGET(t));
        g_signal_emit(t, sigs[SIG_ACTIVITY], 0);
    }
    if (g_atomic_int_compare_and_exchange(&t->eof_flag, 1, 0)) {
        join_worker(t);
        t->eof = TRUE;
        finish(t);
    }
    return G_SOURCE_REMOVE;
}

static void post_idle(SdTerm *t) {
    if (g_atomic_int_compare_and_exchange(&t->idle_pending, 0, 1))
        g_idle_add_full(G_PRIORITY_HIGH_IDLE, main_idle, g_object_ref(t), g_object_unref);
}

static void flush_out(SdTerm *t, gboolean *still_pending) {
    g_mutex_lock(&t->outlock);
    while (t->outq->len) {
        ssize_t n = sd_pty_write(&t->pty, t->outq->data, t->outq->len);
        if (n > 0) g_byte_array_remove_range(t->outq, 0, (guint)n);
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN) break;
        else { g_byte_array_set_size(t->outq, 0); break; }
    }
    *still_pending = t->outq->len > 0;
    g_mutex_unlock(&t->outlock);
}

static gpointer worker_main(gpointer data) {
    SdTerm *t = data;
    guint8 *buf = g_malloc(65536);
    gboolean out_pending = FALSE;
    while (!g_atomic_int_get(&t->stop)) {
        struct pollfd fds[2] = {{t->pty.fd, POLLIN | (out_pending ? POLLOUT : 0), 0}, {t->wake_fd, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
        if (g_atomic_int_get(&t->stop)) break;
        if (fds[1].revents & POLLIN) { guint64 v; ssize_t r = read(t->wake_fd, &v, sizeof v); (void)r; }
        flush_out(t, &out_pending);
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            gsize total = 0;
            gboolean eof = FALSE;
            for (;;) {
                ssize_t n = read(t->pty.fd, buf, 65536);
                if (n > 0) {
                    VT_LOCK(t);
                    vt_feed(t->vt, buf, (size_t)n);
                    VT_UNLOCK(t);
                    total += (gsize)n;
                    if (total >= (1u << 20)) break;   /* give the UI thread the lock now and then */
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                eof = TRUE;
                break;
            }
            if (total) {
                g_atomic_pointer_add(&t->bytes_fed, (gssize)total);
                g_atomic_int_set(&t->act_flag, 1);
                post_idle(t);
            }
            flush_out(t, &out_pending);   /* replies the parser produced (DA, cursor position) */
            if (eof) { g_atomic_int_set(&t->eof_flag, 1); post_idle(t); break; }
        }
    }
    g_free(buf);
    return NULL;
}

static void on_child(GPid pid, gint status, gpointer data) {
    (void)pid;
    SdTerm *t = data;
    t->child_src = 0;
    t->child_done = TRUE;
    t->exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    g_spawn_close_pid(pid);
    finish(t);
}

static gboolean spawn(SdTerm *t) {
    VT_LOCK(t);
    int cols = t->cols, rows = t->rows;
    VT_UNLOCK(t);
    if (sd_pty_spawn(&t->pty, t->argv, t->cwd, NULL, cols, rows) != 0) return FALSE;
    t->running = TRUE;
    g_atomic_int_set(&t->running_flag, 1);
    t->eof = t->child_done = FALSE;
    g_atomic_int_set(&t->stop, 0);
    g_atomic_int_set(&t->eof_flag, 0);
    t->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    t->worker = g_thread_new("sd-term", worker_main, t);
    t->child_src = g_child_watch_add(t->pty.pid, on_child, t);
    return TRUE;
}

SdTerm *sd_term_new(char *const argv[], const char *cwd) {
    SdTerm *t = g_object_new(SD_TYPE_TERM, NULL);
    t->argv = g_strdupv((char **)argv);
    t->cwd = g_strdup(cwd);
    if (!spawn(t)) term_print(t, "failed to start the session\r\n");
    return t;
}

gboolean sd_term_is_running(SdTerm *t) { return t->running; }
const char *sd_term_title(SdTerm *t) { return t->title; }

void sd_term_close(SdTerm *t) {
    if (t->running || t->worker) {
        g_atomic_int_set(&t->running_flag, 0);
        sd_pty_hangup(&t->pty);
        join_worker(t);
        t->running = FALSE;
        sd_pty_close(&t->pty);
    }
}

static void reconnect(SdTerm *t) {
    VT_LOCK(t);
    vt_reset(t->vt);
    vt_clear_history(t->vt);
    t->offset = 0;
    VT_UNLOCK(t);
    if (spawn(t)) g_signal_emit(t, sigs[SIG_RESTARTED], 0);
}

void sd_term_set_history(SdTerm *t, int lines, size_t ram_mb, size_t disk_mb, gboolean spill) {
    VT_LOCK(t);
    vt_set_history(t->vt, lines, ram_mb << 20, disk_mb << 20, spill);
    t->offset = MIN(t->offset, vt_history_count(t->vt));
    VT_UNLOCK(t);
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

void sd_term_history_stats(SdTerm *t, VtHistoryStats *out) {
    VT_LOCK(t);
    vt_history_stats(t->vt, out);
    VT_UNLOCK(t);
}

size_t sd_term_compact(SdTerm *t) {
    VT_LOCK(t);
    size_t freed = vt_compact(t->vt);
    VT_UNLOCK(t);
    return freed;
}

guint64 sd_term_bytes_fed(SdTerm *t) { return (guint64)g_atomic_pointer_get(&t->bytes_fed); }

/* ---- clipboard / selection --------------------------------------------------------------- */

static char *selection_text_locked(SdTerm *t) {
    if (!t->sel_on) return NULL;
    int ai = t->sel_a_idx, ac = t->sel_a_col, bi = t->sel_b_idx, bc = t->sel_b_col;
    if (ai > bi || (ai == bi && ac > bc)) { int x = ai; ai = bi; bi = x; x = ac; ac = bc; bc = x; }
    GString *out = g_string_new(NULL);
    for (int idx = ai; idx <= bi; idx++) {
        int llen = 0;
        VtCell *l = vt_line(t->vt, idx, &llen);
        if (!l) continue;
        int lo = idx == ai ? ac : 0, hi = idx == bi ? bc : t->cols - 1;
        GString *row = g_string_new(NULL);
        for (int x = lo; x <= hi && x < t->cols; x++) {
            if (x >= llen) { g_string_append_c(row, ' '); continue; }   /* trimmed history lines end in blanks */
            if (VT_CELL_FLAGS(l[x]) & VT_F_TAIL) continue;
            uint32_t ch = VT_CELL_CH(l[x]);
            g_string_append_unichar(row, ch ? ch : ' ');
            uint32_t comb = vt_comb_char(t->vt, VT_CELL_COMB(l[x]));
            if (comb) g_string_append_unichar(row, comb);
        }
        while (row->len && row->str[row->len - 1] == ' ') g_string_truncate(row, row->len - 1);
        g_string_append(out, row->str);
        g_string_free(row, TRUE);
        if (idx < bi) g_string_append_c(out, '\n');
    }
    return g_string_free(out, FALSE);
}

static char *selection_text(SdTerm *t) {
    VT_LOCK(t);
    char *r = selection_text_locked(t);
    VT_UNLOCK(t);
    return r;
}

void sd_term_copy(SdTerm *t) {
    char *s = selection_text(t);
    if (s && *s) gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(t)), s);
    g_free(s);
}

static void paste_text(SdTerm *t, const char *text) {
    if (!text || !*text) return;
    GString *g = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        g_string_append_c(g, *p == '\n' ? '\r' : *p);
    }
    gboolean bracket = term_modes(t) & VT_M_BRACKETED_PASTE;
    if (bracket) {
        char *clean = g_strdup(g->str);
        char *e;
        while ((e = strstr(clean, "\x1b[201~"))) memmove(e, e + 6, strlen(e + 6) + 1); /* a paste cannot end itself early */
        sd_term_send(t, "\x1b[200~", 6);
        sd_term_send(t, clean, strlen(clean));
        sd_term_send(t, "\x1b[201~", 6);
        g_free(clean);
    } else {
        sd_term_send(t, g->str, g->len);
    }
    g_string_free(g, TRUE);
}

static void on_clip_text(GObject *src, GAsyncResult *res, gpointer data) {
    SdTerm *t = data;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, NULL);
    if (text) paste_text(t, text);
    g_free(text);
    g_object_unref(t);
}

static void paste_from(SdTerm *t, GdkClipboard *cb) {
    gdk_clipboard_read_text_async(cb, NULL, on_clip_text, g_object_ref(t));
}

void sd_term_paste(SdTerm *t) { paste_from(t, gtk_widget_get_clipboard(GTK_WIDGET(t))); }

/* ---- keyboard -------------------------------------------------------------------------------- */

static void send_str(SdTerm *t, const char *s) { sd_term_send(t, s, strlen(s)); }

static int mod_param(GdkModifierType st) {
    return 1 + ((st & GDK_SHIFT_MASK) ? 1 : 0) + ((st & GDK_ALT_MASK) ? 2 : 0) + ((st & GDK_CONTROL_MASK) ? 4 : 0);
}

static gboolean send_key_sequence(SdTerm *t, guint kv, GdkModifierType st) {
    char b[32];
    int m = mod_param(st);
    gboolean app = (term_modes(t) & VT_M_APP_CURSOR) != 0;
    const char *final = NULL;
    switch (kv) {
    case GDK_KEY_Up: case GDK_KEY_KP_Up: final = "A"; break;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: final = "B"; break;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: final = "C"; break;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: final = "D"; break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: final = "H"; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: final = "F"; break;
    default: break;
    }
    if (final) {
        if (m > 1) snprintf(b, sizeof b, "\x1b[1;%d%s", m, final);
        else snprintf(b, sizeof b, app ? "\x1bO%s" : "\x1b[%s", final);
        send_str(t, b);
        return TRUE;
    }
    int tilde = 0;
    switch (kv) {
    case GDK_KEY_Insert: tilde = 2; break;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: tilde = 3; break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: tilde = 5; break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: tilde = 6; break;
    case GDK_KEY_F5: tilde = 15; break; case GDK_KEY_F6: tilde = 17; break; case GDK_KEY_F7: tilde = 18; break;
    case GDK_KEY_F8: tilde = 19; break; case GDK_KEY_F9: tilde = 20; break; case GDK_KEY_F10: tilde = 21; break;
    case GDK_KEY_F11: tilde = 23; break; case GDK_KEY_F12: tilde = 24; break;
    default: break;
    }
    if (tilde) {
        if (m > 1) snprintf(b, sizeof b, "\x1b[%d;%d~", tilde, m);
        else snprintf(b, sizeof b, "\x1b[%d~", tilde);
        send_str(t, b);
        return TRUE;
    }
    if (kv >= GDK_KEY_F1 && kv <= GDK_KEY_F4) {
        char c = (char)('P' + (kv - GDK_KEY_F1));
        if (m > 1) snprintf(b, sizeof b, "\x1b[1;%d%c", m, c);
        else snprintf(b, sizeof b, "\x1bO%c", c);
        send_str(t, b);
        return TRUE;
    }
    return FALSE;
}

static void scroll_view(SdTerm *t, int delta) {
    VT_LOCK(t);
    int max = vt_history_count(t->vt);
    VT_UNLOCK(t);
    int o = t->offset + delta;
    t->offset = o < 0 ? 0 : o > max ? max : o;
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static gboolean on_key_pressed(GtkEventControllerKey *c, guint kv, guint code, GdkModifierType st, gpointer data) {
    (void)c; (void)code;
    SdTerm *t = data;
    gboolean ctrl = (st & GDK_CONTROL_MASK) != 0, shift = (st & GDK_SHIFT_MASK) != 0, alt = (st & GDK_ALT_MASK) != 0;
    if (!t->running) {
        if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) reconnect(t);
        return TRUE;
    }
    if (ctrl && shift && (kv == GDK_KEY_C || kv == GDK_KEY_c)) { sd_term_copy(t); return TRUE; }
    if ((ctrl && shift && (kv == GDK_KEY_V || kv == GDK_KEY_v)) || (shift && kv == GDK_KEY_Insert)) { sd_term_paste(t); return TRUE; }
    if (shift && (kv == GDK_KEY_Page_Up || kv == GDK_KEY_Page_Down)) {
        scroll_view(t, kv == GDK_KEY_Page_Up ? t->rows - 1 : -(t->rows - 1));
        return TRUE;
    }
    if (t->offset) { t->offset = 0; gtk_widget_queue_draw(GTK_WIDGET(t)); }
    if (kv == GDK_KEY_Return || kv == GDK_KEY_KP_Enter) { send_str(t, alt ? "\x1b\r" : "\r"); return TRUE; }
    if (kv == GDK_KEY_BackSpace) { send_str(t, alt ? "\x1b\x7f" : ctrl ? "\x08" : "\x7f"); return TRUE; }
    if (kv == GDK_KEY_Tab) { send_str(t, "\t"); return TRUE; }
    if (kv == GDK_KEY_ISO_Left_Tab) { send_str(t, "\x1b[Z"); return TRUE; }
    if (kv == GDK_KEY_Escape) { send_str(t, "\x1b"); return TRUE; }
    if (send_key_sequence(t, kv, st)) return TRUE;
    gunichar u = gdk_keyval_to_unicode(kv);
    if (ctrl && !alt && u) {
        char c = 0;
        if (u >= 'a' && u <= 'z') c = (char)(u - 'a' + 1);
        else if (u >= 'A' && u <= 'Z') c = (char)(u - 'A' + 1);
        else if (u == ' ' || u == '@' || u == '2') c = 0;
        else if (u == '[') c = 0x1b; else if (u == '\\') c = 0x1c; else if (u == ']') c = 0x1d;
        else if (u == '^' || u == '6') c = 0x1e; else if (u == '_' || u == '/') c = 0x1f;
        else return FALSE;
        sd_term_send(t, &c, 1);
        return TRUE;
    }
    if (alt && !ctrl && u && u >= 0x20) {
        char b[8] = {0x1b};
        int n = g_unichar_to_utf8(u, b + 1);
        sd_term_send(t, b, (gsize)n + 1);
        return TRUE;
    }
    return FALSE; /* printable text goes through the input method */
}

static void on_im_commit(GtkIMContext *im, const char *text, gpointer data) {
    (void)im;
    SdTerm *t = data;
    if (t->offset) { t->offset = 0; gtk_widget_queue_draw(GTK_WIDGET(t)); }
    sd_term_send(t, text, strlen(text));
}

static void on_focus_enter(GtkEventControllerFocus *c, gpointer data) {
    (void)c;
    SdTerm *t = data;
    t->focused = TRUE;
    gtk_im_context_focus_in(t->im);
    if (term_modes(t) & VT_M_FOCUS_EVENTS) send_str(t, "\x1b[I");
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

static void on_focus_leave(GtkEventControllerFocus *c, gpointer data) {
    (void)c;
    SdTerm *t = data;
    t->focused = FALSE;
    gtk_im_context_focus_out(t->im);
    if (term_modes(t) & VT_M_FOCUS_EVENTS) send_str(t, "\x1b[O");
    gtk_widget_queue_draw(GTK_WIDGET(t));
}

/* ---- mouse ---------------------------------------------------------------------------------------- */

static void cell_at(SdTerm *t, double x, double y, int *col, int *row) {
    int c = (int)((x - PAD) / t->cw), r = (int)((y - PAD) / t->ch);
    *col = c < 0 ? 0 : c >= t->cols ? t->cols - 1 : c;
    *row = r < 0 ? 0 : r >= t->rows ? t->rows - 1 : r;
}

static gboolean mouse_reporting(SdTerm *t, GdkModifierType st) {
    return (term_modes(t) & (VT_M_MOUSE_BTN | VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY)) && !(st & GDK_SHIFT_MASK);
}

static void report_mouse(SdTerm *t, int button, int col, int row, gboolean press, gboolean motion, GdkModifierType st) {
    int code = button + (motion ? 32 : 0) + ((st & GDK_SHIFT_MASK) ? 4 : 0) + ((st & GDK_ALT_MASK) ? 8 : 0) +
               ((st & GDK_CONTROL_MASK) ? 16 : 0);
    char b[64];
    if (term_modes(t) & VT_M_MOUSE_SGR) {
        snprintf(b, sizeof b, "\x1b[<%d;%d;%d%c", code, col + 1, row + 1, press ? 'M' : 'm');
        send_str(t, b);
    } else if (col < 223 && row < 223) {
        if (!press && button < 64) code = 3 + (code & ~3);
        b[0] = 0x1b; b[1] = '['; b[2] = 'M';
        b[3] = (char)(32 + code); b[4] = (char)(33 + col); b[5] = (char)(33 + row);
        sd_term_send(t, b, 6);
    }
}

static gboolean is_word(VtCell *l, int len, int x) { if (x >= len) return FALSE; uint32_t c = VT_CELL_CH(l[x]); return c > 0x20 && c != 0x7f; }

static void on_click_pressed(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    SdTerm *t = data;
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    gtk_widget_grab_focus(GTK_WIDGET(t));
    int col, row;
    cell_at(t, x, y, &col, &row);
    if (mouse_reporting(t, st) && button >= 1 && button <= 3) {
        t->mouse_btn = (int)button - 1;
        report_mouse(t, t->mouse_btn, col, row, TRUE, FALSE, st);
        return;
    }
    if (button == 1) {
        int idx = row - t->offset;
        t->sel_a_idx = t->sel_b_idx = idx;
        t->sel_a_col = t->sel_b_col = col;
        t->dragging = TRUE;
        t->sel_on = FALSE;
        VT_LOCK(t);
        int llen = 0;
        VtCell *l = vt_line(t->vt, idx, &llen);
        if (n == 2 && l) {
            int lo = col, hi = col;
            while (lo > 0 && is_word(l, llen, lo - 1)) lo--;
            while (hi < t->cols - 1 && is_word(l, llen, hi + 1)) hi++;
            t->sel_a_col = lo; t->sel_b_col = hi; t->sel_on = TRUE;
        }
        VT_UNLOCK(t);
        if (n == 2) {
        } else if (n >= 3) {
            t->sel_a_col = 0; t->sel_b_col = t->cols - 1; t->sel_on = TRUE;
        }
        gtk_widget_queue_draw(GTK_WIDGET(t));
    } else if (button == 2) {
        paste_from(t, gtk_widget_get_primary_clipboard(GTK_WIDGET(t)));
    } else if (button == 3 && !mouse_reporting(t, st)) {
        /* right click: paste, like most terminals configured for quick paste */
        if (t->sel_on) sd_term_copy(t);
        else sd_term_paste(t);
    }
}

static void on_click_released(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n;
    SdTerm *t = data;
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    int col, row;
    cell_at(t, x, y, &col, &row);
    if (t->mouse_btn >= 0) {
        report_mouse(t, t->mouse_btn, col, row, FALSE, FALSE, st);
        t->mouse_btn = -1;
        return;
    }
    if (t->dragging) {
        t->dragging = FALSE;
        char *s = selection_text(t);
        if (s && *s) gdk_clipboard_set_text(gtk_widget_get_primary_clipboard(GTK_WIDGET(t)), s);
        g_free(s);
    }
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer data) {
    SdTerm *t = data;
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
    int col, row;
    cell_at(t, x, y, &col, &row);
    uint32_t m = term_modes(t);
    if (t->mouse_btn >= 0 && (m & (VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY))) { report_mouse(t, t->mouse_btn, col, row, TRUE, TRUE, st); return; }
    if ((m & VT_M_MOUSE_ANY) && !(st & GDK_SHIFT_MASK)) { report_mouse(t, 3, col, row, TRUE, TRUE, st); return; }
    if (t->dragging) {
        t->sel_b_idx = row - t->offset;
        t->sel_b_col = col;
        t->sel_on = TRUE;
        gtk_widget_queue_draw(GTK_WIDGET(t));
    }
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer data) {
    (void)dx;
    SdTerm *t = data;
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c));
    t->scroll_acc += dy;
    int steps = (int)t->scroll_acc;
    if (!steps) return TRUE;
    t->scroll_acc -= steps;
    int n = steps < 0 ? -steps : steps;
    gboolean up = steps < 0;
    if (mouse_reporting(t, st)) {
        for (int i = 0; i < n; i++) report_mouse(t, up ? 64 : 65, 0, 0, TRUE, FALSE, st);
    } else if (term_modes(t) & VT_M_ALT_SCREEN) {
        const char *k = (term_modes(t) & VT_M_APP_CURSOR) ? (up ? "\x1bOA" : "\x1bOB") : (up ? "\x1b[A" : "\x1b[B");
        for (int i = 0; i < n * 3; i++) send_str(t, k);
    } else {
        scroll_view(t, up ? 3 * n : -3 * n);
    }
    return TRUE;
}

/* ---- GObject ------------------------------------------------------------------------------------------ */

static void sd_term_dispose(GObject *o) {
    SdTerm *t = SD_TERM(o);
    sd_term_close(t);
    join_worker(t);
    if (t->child_src) { g_source_remove(t->child_src); t->child_src = 0; }
    g_clear_object(&t->im);
    G_OBJECT_CLASS(sd_term_parent_class)->dispose(o);
}

static void sd_term_finalize(GObject *o) {
    SdTerm *t = SD_TERM(o);
    vt_free(t->vt);
    g_rec_mutex_clear(&t->lock);
    g_mutex_clear(&t->outlock);
    g_mutex_clear(&t->evlock);
    g_list_free_full(t->events, g_free);
    g_strfreev(t->argv);
    g_free(t->cwd);
    g_free(t->title);
    g_byte_array_free(t->outq, TRUE);
    if (t->font) pango_font_description_free(t->font);
    for (int i = 0; i < 16; i++) if (t->attrs[i]) pango_attr_list_unref(t->attrs[i]);
    g_clear_object(&t->layout);
    if (t->surf) cairo_surface_destroy(t->surf);
    g_free(t->rendered);
    G_OBJECT_CLASS(sd_term_parent_class)->finalize(o);
}

static void sd_term_class_init(SdTermClass *k) {
    GObjectClass *oc = G_OBJECT_CLASS(k);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(k);
    oc->dispose = sd_term_dispose;
    oc->finalize = sd_term_finalize;
    wc->snapshot = sd_term_snapshot;
    wc->measure = sd_term_measure;
    wc->size_allocate = sd_term_size_allocate;
    gtk_widget_class_set_css_name(wc, "sdterm");
    sigs[SIG_TITLE] = g_signal_new("title-changed", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    sigs[SIG_CWD] = g_signal_new("cwd-changed", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
    sigs[SIG_BELL] = g_signal_new("bell", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    sigs[SIG_ACTIVITY] = g_signal_new("activity", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    sigs[SIG_EXITED] = g_signal_new("exited", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
    sigs[SIG_RESTARTED] = g_signal_new("restarted", G_TYPE_FROM_CLASS(k), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void sd_term_init(SdTerm *t) {
    GtkWidget *w = GTK_WIDGET(t);
    gtk_widget_set_focusable(w, TRUE);
    gtk_widget_set_can_focus(w, TRUE);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_widget_set_vexpand(w, TRUE);
    gtk_widget_set_overflow(w, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_cursor_from_name(w, "text");
    t->cols = 80; t->rows = 24;
    t->cpu_forced = g_strcmp0(g_getenv("LESTRIX_RENDERER"), "software") == 0;
    t->mouse_btn = -1;
    t->outq = g_byte_array_new();
    t->pty.fd = -1;
    g_rec_mutex_init(&t->lock);
    g_mutex_init(&t->outlock);
    g_mutex_init(&t->evlock);
    t->wake_fd = -1;
    t->vt = vt_new(t->cols, t->rows, default_scrollback);
    vt_set_history(t->vt, default_scrollback, default_ram_mb << 20, default_disk_mb << 20, default_spill);
    vt_set_callbacks(t->vt, vt_write_cb, vt_event_cb, cache_free_cb, t);
    t->layout = gtk_widget_create_pango_layout(w, NULL);
    pango_context_set_round_glyph_positions(pango_layout_get_context(t->layout), FALSE);
    make_attr_lists(t);
    t->theme = &SD_THEMES[0];
    build_palette(t);
    sd_term_set_font(t, "monospace", 11);

    t->im = gtk_im_multicontext_new();
    g_signal_connect(t->im, "commit", G_CALLBACK(on_im_commit), t);
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_key_set_im_context(GTK_EVENT_CONTROLLER_KEY(key), t->im);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), t);
    gtk_widget_add_controller(w, key);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus_enter), t);
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), t);
    gtk_widget_add_controller(w, focus);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), t);
    g_signal_connect(click, "released", G_CALLBACK(on_click_released), t);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(click));
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), t);
    gtk_widget_add_controller(w, motion);
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), t);
    gtk_widget_add_controller(w, scroll);
}

/* test helper: does any visible row contain `needle`? */
gboolean sd_term_screen_contains(SdTerm *t, const char *needle) {
    char row[1024];
    VT_LOCK(t);
    for (int y = 0; y < t->rows; y++) {
        int llen = 0;
        VtCell *l = vt_line(t->vt, y, &llen);
        int n = 0;
        for (int x = 0; l && x < t->cols && n < 1000; x++) row[n++] = (x < llen && VT_CELL_CH(l[x]) && VT_CELL_CH(l[x]) < 0x7f) ? (char)VT_CELL_CH(l[x]) : ' ';
        row[n] = 0;
        if (strstr(row, needle)) { VT_UNLOCK(t); return TRUE; }
    }
    VT_UNLOCK(t);
    return FALSE;
}
