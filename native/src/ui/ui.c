#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atlas.h"
#include "font.h"

static uint32_t ui_mix_u(uint32_t a, uint32_t b, float f) {
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) { float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255); o |= (uint32_t)(x + (y - x) * f + 0.5f) << s; }
    return o | 0xff000000u;
}

#define MAX_KEYS 32
#define MAX_CLIPS 6

struct Ui {
    Renderer *r;
    Font *font, *mono;
    float scale;
    bool rounded;
    UiColors c;
    int W, H;
    double now;
    float mx, my;
    bool down[4], pressed[4], released[4];
    int clicks;
    float wheel;
    int mods;
    struct { SDL_Keycode sym; int mods; } keys[MAX_KEYS];
    int nkeys;
    char text[128];
    bool blocked, menu_at_start;
    uint64_t active, focus, focus_claimed;
    bool press_this_frame, input_claimed;
    double want_until;
    int want_cursor;
    SDL_Cursor *cursors[3];
    SDL_Cursor *cur_set;
    /* tooltip */
    char tip[256];
    Rect tip_r;
    uint64_t tip_id, tip_prev;
    double tip_since;
    bool tip_shown_this_frame;
    /* clip stack */
    Rect clips[MAX_CLIPS];
    int nclips;
    /* scroll */
    Rect sc_view; float sc_content; float *sc_off; bool sc_active;
    uint64_t sc_drag; float sc_drag_dy;
    /* menu */
    bool menu_open;
    float menu_x, menu_y;
    UiMenuItem *menu_items; int menu_n; int menu_tag;
    int menu_result, menu_result_tag;
    int menu_hover;
    float menu_w;
    double menu_opened_at;
};

/* ---- utf-8 --------------------------------------------------------------------------------------------- */

static uint32_t next_cp(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = *s++;
    if (c < 0x80) { *p = (const char *)s; return c; }
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= 0x3f >> n;
    while (n-- > 0 && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
    *p = (const char *)s;
    return c;
}

static int cp_len_at(const char *s) { const char *p = s; next_cp(&p); return (int)(p - s); }

static int prev_boundary(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

/* ---- setup ------------------------------------------------------------------------------------------------ */

Ui *ui_new(Renderer *r, void *font, void *mono, float scale) {
    Ui *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->r = r; u->font = font; u->mono = mono; u->scale = scale;
    u->cursors[0] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    u->cursors[1] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
    u->cursors[2] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    u->menu_result = -1;
    u->rounded = true;
    return u;
}

void ui_free(Ui *u) {
    if (!u) return;
    for (int i = 0; i < 3; i++) if (u->cursors[i]) SDL_FreeCursor(u->cursors[i]);
    free(u->menu_items);
    free(u);
}

void ui_set_colors(Ui *u, const UiColors *c) { u->c = *c; }
void ui_set_rounded(Ui *u, bool rounded) { u->rounded = rounded; }
const UiColors *ui_colors(const Ui *u) { return &u->c; }
float ui_scale(const Ui *u) { return u->scale; }
float S(const Ui *u, float v) { return (float)floor(v * u->scale + 0.5); }

void ui_event(Ui *u, const SDL_Event *e) {
    switch (e->type) {
    case SDL_MOUSEMOTION: u->mx = e->motion.x * u->scale; u->my = e->motion.y * u->scale; break;
    case SDL_MOUSEBUTTONDOWN:
        u->mx = e->button.x * u->scale; u->my = e->button.y * u->scale;
        if (e->button.button < 4) { u->down[e->button.button] = true; u->pressed[e->button.button] = true; }
        u->clicks = e->button.clicks;
        u->press_this_frame = true;
        break;
    case SDL_MOUSEBUTTONUP:
        u->mx = e->button.x * u->scale; u->my = e->button.y * u->scale;
        if (e->button.button < 4) { u->down[e->button.button] = false; u->released[e->button.button] = true; }
        break;
    case SDL_MOUSEWHEEL: u->wheel += e->wheel.preciseY; break;
    case SDL_KEYDOWN:
        if (u->nkeys < MAX_KEYS) { u->keys[u->nkeys].sym = e->key.keysym.sym; u->keys[u->nkeys].mods = e->key.keysym.mod; u->nkeys++; }
        break;
    case SDL_TEXTINPUT: {
        size_t l = strlen(u->text), n = strlen(e->text.text);
        if (l + n < sizeof u->text) memcpy(u->text + l, e->text.text, n + 1);
        break;
    }
    default: break;
    }
}

void ui_begin(Ui *u, int w, int h, double now) {
    u->W = w; u->H = h; u->now = now;
    u->menu_at_start = u->menu_open;
    u->blocked = u->menu_open;
    u->want_cursor = 0;
    u->nclips = 0;
    u->tip_shown_this_frame = false;
    u->input_claimed = false;
}

void ui_set_blocked(Ui *u, bool b) { u->blocked = b; }
bool ui_blocked(const Ui *u) { return u->blocked; }
float ui_mx(const Ui *u) { return u->mx; }
float ui_my(const Ui *u) { return u->my; }
bool ui_hover(const Ui *u, Rect r) { return !u->blocked && rect_has(r, u->mx, u->my); }
bool ui_mouse_pressed(const Ui *u, int b, Rect r) { return !u->blocked && u->pressed[b] && rect_has(r, u->mx, u->my); }
bool ui_mouse_released(const Ui *u, int b) { return !u->blocked && u->released[b]; }
bool ui_mouse_down(const Ui *u, int b) { return !u->blocked && u->down[b]; }
int ui_clicks(const Ui *u) { return u->clicks; }
float ui_wheel(const Ui *u) { return u->blocked ? 0 : u->wheel; }
float ui_take_wheel(Ui *u, Rect r) {
    if (u->blocked || !rect_has(r, u->mx, u->my)) return 0;
    float w = u->wheel;
    u->wheel = 0;
    return w;
}
bool ui_keyboard_taken(const Ui *u) { return u->focus != 0 || u->menu_open; }
const char *ui_text_in(const Ui *u) { return u->text; }
void ui_release_focus(Ui *u) { u->focus = 0; }

bool ui_key(Ui *u, SDL_Keycode k, int mods) {
    for (int i = 0; i < u->nkeys; i++)
        if (u->keys[i].sym == k && (u->keys[i].mods & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) == mods) return true;
    return false;
}

bool ui_animating(const Ui *u) { return u->now < u->want_until; }
void ui_want_frames(Ui *u, double seconds) { if (u->now + seconds > u->want_until) u->want_until = u->now + seconds; }

SDL_Cursor *ui_wanted_cursor(Ui *u) { return u->cursors[u->want_cursor]; }
void ui_set_cursor(Ui *u, int kind) { if (kind >= 0 && kind < 3) u->want_cursor = kind; }

/* ---- drawing -------------------------------------------------------------------------------------------------- */

void ui_rect(Ui *u, Rect r, uint32_t c) { if (r.w > 0 && r.h > 0) r_rect(u->r, r.x, r.y, r.w, r.h, c); }
void ui_rrect(Ui *u, Rect r, uint32_t c, float radius) {
    if (r.w <= 0 || r.h <= 0) return;
    if (!u->rounded) radius = 0;
    if (radius <= 0) r_rect(u->r, r.x, r.y, r.w, r.h, c); else r_rrect(u->r, r.x, r.y, r.w, r.h, c, (int)(radius + 0.5f), 0);
}
void ui_outline(Ui *u, Rect r, uint32_t c, float radius, float width) {
    if (r.w <= 0 || r.h <= 0) return;
    if (!u->rounded) radius = 0;
    r_rrect(u->r, r.x, r.y, r.w, r.h, c, (int)(radius + 0.5f), width < 1 ? 1 : (int)(width + 0.5f));
}

void ui_clip(Ui *u, Rect r) {
    if (u->nclips < MAX_CLIPS) u->clips[u->nclips++] = r;
    r_clip(u->r, (int)r.x, (int)r.y, (int)r.w, (int)r.h);
}

void ui_unclip(Ui *u) {
    if (u->nclips > 0) u->nclips--;
    if (u->nclips > 0) { Rect r = u->clips[u->nclips - 1]; r_clip(u->r, (int)r.x, (int)r.y, (int)r.w, (int)r.h); }
    else r_clip_off(u->r);
}

/* ---- text ----------------------------------------------------------------------------------------------------------- */

float ui_line_h(const Ui *u) { return (float)font_cell_h(u->font); }

float ui_text_w(Ui *u, const char *s) {
    float w = 0;
    Atlas *a = r_atlas(u->r);
    for (const char *p = s; *p;) {
        uint32_t cp = next_cp(&p);
        const AtlasGlyph *g = atlas_glyph(a, u->font, cp, FS_REGULAR);
        w += g ? g->adv : (float)font_cell_w(u->font);
    }
    return w;
}

static void draw_run(Ui *u, Font *f, float x, float y, const char *s, int n, uint32_t color, bool mono) {
    Atlas *a = r_atlas(u->r);
    float base = y + (float)font_ascent(f);
    float pen = x;
    for (const char *p = s; *p && (n < 0 || p < s + n);) {
        uint32_t cp = next_cp(&p);
        const AtlasGlyph *g = atlas_glyph(a, f, cp, FS_REGULAR);
        if (!g) continue;
        if (!g->blank) { RInst i; r_make_glyph(u->r, &i, g, (float)floor(pen + 0.5), base, color); r_push(u->r, &i, 1); }
        pen += mono ? (float)font_cell_w(f) : g->adv;
    }
}

void ui_text(Ui *u, float x, float y, const char *s, uint32_t color) { draw_run(u, u->font, x, y, s, -1, color, false); }
void ui_text_mono(Ui *u, float x, float y, const char *s, uint32_t color) { draw_run(u, u->mono ? u->mono : u->font, x, y, s, -1, color, true); }

float ui_text_mono_w(Ui *u, const char *s) { return ui_text_font_w(u, u->mono ? u->mono : u->font, 0, s); }

void ui_text_fit(Ui *u, Rect r, const char *s, uint32_t color, int align) {
    float w = ui_text_w(u, s), y = r.y + (r.h - ui_line_h(u)) / 2;
    if (w <= r.w) {
        float x = align == 1 ? r.x + (r.w - w) / 2 : align == 2 ? r.x + r.w - w : r.x;
        ui_text(u, x, (float)floor(y), s, color);
        return;
    }
    float dots = ui_text_w(u, "\xe2\x80\xa6");
    float acc = 0;
    Atlas *a = r_atlas(u->r);
    const char *p = s, *cut = s;
    while (*p) {
        const char *q = p;
        uint32_t cp = next_cp(&q);
        const AtlasGlyph *g = atlas_glyph(a, u->font, cp, FS_REGULAR);
        float adv = g ? g->adv : 8;
        if (acc + adv + dots > r.w) break;
        acc += adv;
        p = q;
        cut = p;
    }
    draw_run(u, u->font, r.x, (float)floor(y), s, (int)(cut - s), color, false);
    ui_text(u, r.x + acc, (float)floor(y), "\xe2\x80\xa6", color);
}

void ui_text_font(Ui *u, void *font, int style, float x, float y, const char *s, uint32_t color) {
    Font *f = font;
    Atlas *a = r_atlas(u->r);
    float base = y + (float)font_ascent(f), pen = x;
    for (const char *p = s; *p;) {
        uint32_t cp = next_cp(&p);
        const AtlasGlyph *g = atlas_glyph(a, f, cp, style);
        if (!g) continue;
        if (!g->blank) { RInst i; r_make_glyph(u->r, &i, g, (float)floor(pen + 0.5), base, color); r_push(u->r, &i, 1); }
        pen += g->adv;
    }
}

float ui_text_font_w(Ui *u, void *font, int style, const char *s) {
    float w = 0;
    Atlas *a = r_atlas(u->r);
    for (const char *p = s; *p;) { uint32_t cp = next_cp(&p); const AtlasGlyph *g = atlas_glyph(a, font, cp, style); w += g ? g->adv : 8; }
    return w;
}

/* vertical text: the string is laid out normally into a bitmap, then turned a quarter turn; keyed on the text and font */
float ui_text_vertical_len(Ui *u, const char *s) { return ui_text_w(u, s) * 0.84f + (float)(strlen(s)) * S(u, 1.2f); }

void ui_text_vertical(Ui *u, Rect r, const char *s, uint32_t color) {
    Atlas *a = r_atlas(u->r);
    uint32_t key = 0x7e57u;
    for (const char *p = s; *p; p++) key = key * 31u + (unsigned char)*p;
    uint32_t fid = font_id(u->font) ^ 0x5a5a0000u;
    int lh = font_cell_h(u->font), asc = font_ascent(u->font);
    const AtlasGlyph *g = atlas_find_custom(a, fid, key);
    if (!g) {
        float adv_total = 0;
        for (const char *p = s; *p;) { uint32_t cp = next_cp(&p); const AtlasGlyph *gg = atlas_glyph(a, u->font, cp, FS_BOLD); adv_total += (gg ? gg->adv : 8) + S(u, 1.2f); }
        int W = (int)adv_total + 2, H = lh;
        uint8_t *src = calloc((size_t)W * H, 1);
        float pen = 0;
        for (const char *p = s; *p;) {
            uint32_t cp = next_cp(&p);
            GlyphBmp b;
            if (font_glyph(u->font, cp, FS_BOLD, &b)) {
                for (int yy = 0; yy < b.h; yy++) for (int xx = 0; xx < b.w; xx++) {
                    int dx = (int)pen + b.left + xx, dy = asc - b.top + yy;
                    if (dx >= 0 && dx < W && dy >= 0 && dy < H) src[(size_t)dy * W + dx] = b.buf[(size_t)yy * b.w + xx];
                }
                pen += b.adv + S(u, 1.2f);
            }
        }
        /* quarter turn so the text reads bottom to top: (x, y) -> (y, W - 1 - x) */
        uint8_t *dst = calloc((size_t)W * H, 1);
        for (int yy = 0; yy < H; yy++) for (int xx = 0; xx < W; xx++) dst[(size_t)(W - 1 - xx) * H + yy] = src[(size_t)yy * W + xx];
        g = atlas_custom(a, fid, key, dst, H, W);
        free(src); free(dst);
    }
    if (!g || g->blank) return;
    RInst i;
    r_make_glyph(u->r, &i, g, 0, 0, color);
    i.x = (float)floor(r.x + (r.w - g->w) / 2 + 0.5f);
    i.y = (float)floor(r.y + (r.h - g->h) / 2 + 0.5f);
    r_push(u->r, &i, 1);
}

/* ---- icons: tiny vector drawings rasterised once into the atlas ------------------------------------------------------- */

typedef struct { char k; float a, b, c, d, e, f; } Prim;   /* l line  r rect outline  R filled rect  o circle outline  O filled circle  t filled triangle */

static float sd_seg(float px, float py, float ax, float ay, float bx, float by) {
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float h = (pax * bax + pay * bay) / (bax * bax + bay * bay + 1e-9f);
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    float dx = pax - bax * h, dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}

static float sd_box(float px, float py, float x0, float y0, float x1, float y1) {
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
    float qx = fabsf(px - cx) - hx, qy = fabsf(py - cy) - hy;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.f);
}

static float sd_tri(float px, float py, const float *t) {
    float d = 1e9f, inside = 1;
    for (int i = 0; i < 3; i++) {
        float ax = t[i * 2], ay = t[i * 2 + 1], bx = t[((i + 1) % 3) * 2], by = t[((i + 1) % 3) * 2 + 1];
        d = fminf(d, sd_seg(px, py, ax, ay, bx, by));
        float cr = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        float orient = (t[2] - t[0]) * (t[5] - t[1]) - (t[3] - t[1]) * (t[4] - t[0]);
        if ((orient > 0) != (cr > 0)) inside = 0;
    }
    return inside ? -d : d;
}

#define LN(a, b, c, d) {'l', a, b, c, d, 0, 0}
#define RO(a, b, c, d) {'r', a, b, c, d, 0, 0}
#define RF(a, b, c, d) {'R', a, b, c, d, 0, 0}
#define CO(a, b, r) {'o', a, b, r, 0, 0, 0}
#define CF(a, b, r) {'O', a, b, r, 0, 0, 0}
#define TF(a, b, c, d, e, f) {'t', a, b, c, d, e, f}

static const Prim P_PIN[] = {CF(8, 5.2f, 3.2f), LN(8, 8.4f, 8, 14), LN(4.5f, 8.4f, 11.5f, 8.4f)};
static const Prim P_CLOSE[] = {LN(4, 4, 12, 12), LN(12, 4, 4, 12)};
static const Prim P_PLUS[] = {LN(8, 3, 8, 13), LN(3, 8, 13, 8)};
static const Prim P_COPY[] = {RO(4, 3.5f, 12, 14), RF(6, 2, 10, 4.5f)};
static const Prim P_COPYALL[] = {RO(3, 2.5f, 13, 13.5f), LN(5.5f, 5.5f, 10.5f, 5.5f), LN(5.5f, 8, 10.5f, 8), LN(5.5f, 10.5f, 9, 10.5f)};
static const Prim P_PASTE[] = {RO(3, 3.5f, 13, 12.5f), RF(5.5f, 2, 10.5f, 4), LN(5.5f, 8, 10.5f, 8), LN(5.5f, 10.5f, 9, 10.5f)};
static const Prim P_DUP[] = {RO(3, 5, 10, 12.5f), RO(6, 3, 13, 10.5f)};
static const Prim P_TERM[] = {RO(2, 3, 14, 13), LN(4.8f, 6, 7.5f, 8), LN(7.5f, 8, 4.8f, 10), LN(8.8f, 10.5f, 11.5f, 10.5f)};
static const Prim P_SERVER[] = {RO(3, 3, 13, 7.5f), RO(3, 8.5f, 13, 13), CF(5.2f, 5.25f, 0.9f), CF(5.2f, 10.75f, 0.9f)};
static const Prim P_FOLDER[] = {LN(2, 4, 6.5f, 4), LN(6.5f, 4, 8, 6), LN(8, 6, 14, 6), LN(14, 6, 14, 13), LN(14, 13, 2, 13), LN(2, 13, 2, 4)};
static const Prim P_FILE[] = {LN(4, 2, 9.5f, 2), LN(9.5f, 2, 12, 4.5f), LN(12, 4.5f, 12, 14), LN(12, 14, 4, 14), LN(4, 14, 4, 2), LN(9.5f, 2, 9.5f, 4.5f), LN(9.5f, 4.5f, 12, 4.5f)};
static const Prim P_CHEV_R[] = {LN(6, 3.5f, 10.5f, 8), LN(10.5f, 8, 6, 12.5f)};
static const Prim P_CHEV_D[] = {LN(3.5f, 6, 8, 10.5f), LN(8, 10.5f, 12.5f, 6)};
static const Prim P_CHEV_L[] = {LN(10, 3.5f, 5.5f, 8), LN(5.5f, 8, 10, 12.5f)};
static const Prim P_UP[] = {LN(8, 13, 8, 3.5f), LN(4, 7.5f, 8, 3.5f), LN(8, 3.5f, 12, 7.5f)};
static const Prim P_REFRESH[] = {CO(8, 8.5f, 4.6f), TF(8.5f, 1.5f, 12.5f, 4.5f, 8.5f, 7)};
static const Prim P_UPLOAD[] = {LN(8, 11, 8, 3), LN(4.5f, 6.5f, 8, 3), LN(8, 3, 11.5f, 6.5f), LN(3, 13, 13, 13)};
static const Prim P_DOWNLOAD[] = {LN(8, 3, 8, 11), LN(4.5f, 7.5f, 8, 11), LN(8, 11, 11.5f, 7.5f), LN(3, 13, 13, 13)};
static const Prim P_SEARCH[] = {CO(6.8f, 6.8f, 4), LN(9.8f, 9.8f, 13.5f, 13.5f)};
static const Prim P_TRASH[] = {RO(4, 5.5f, 12, 14), LN(3, 4, 13, 4), RO(6.5f, 2, 9.5f, 4), LN(7, 7.5f, 7, 12), LN(9, 7.5f, 9, 12)};
static const Prim P_EDIT[] = {LN(3, 13, 5.5f, 12.5f), LN(5.5f, 12.5f, 12.5f, 5.5f), LN(12.5f, 5.5f, 10.5f, 3.5f), LN(10.5f, 3.5f, 3.5f, 10.5f), LN(3.5f, 10.5f, 3, 13)};
static const Prim P_CHECK[] = {LN(3.5f, 8.5f, 6.5f, 11.5f), LN(6.5f, 11.5f, 12.5f, 4.5f)};
static const Prim P_LINK[] = {RO(1.5f, 6, 8.5f, 10), RO(7.5f, 6, 14.5f, 10)};
static const Prim P_HOME[] = {LN(2, 8, 8, 2.5f), LN(8, 2.5f, 14, 8), LN(4, 7, 4, 13.5f), LN(4, 13.5f, 12, 13.5f), LN(12, 13.5f, 12, 7)};
static const Prim P_NEWFOLDER[] = {LN(1.5f, 4, 5.5f, 4), LN(5.5f, 4, 7, 6), LN(7, 6, 13, 6), LN(13, 6, 13, 13), LN(13, 13, 1.5f, 13), LN(1.5f, 13, 1.5f, 4), LN(7.5f, 7.5f, 7.5f, 11.5f), LN(5.5f, 9.5f, 9.5f, 9.5f)};

static const struct { const Prim *p; int n; } ICONS[IC_COUNT] = {
    [IC_PIN] = {P_PIN, 3}, [IC_CLOSE] = {P_CLOSE, 2}, [IC_PLUS] = {P_PLUS, 2}, [IC_COPY] = {P_COPY, 2}, [IC_COPYALL] = {P_COPYALL, 4}, [IC_PASTE] = {P_PASTE, 4},
    [IC_DUPLICATE] = {P_DUP, 2}, [IC_TERMINAL] = {P_TERM, 4}, [IC_SERVER] = {P_SERVER, 4}, [IC_FOLDER] = {P_FOLDER, 6}, [IC_FILE] = {P_FILE, 7},
    [IC_CHEV_R] = {P_CHEV_R, 2}, [IC_CHEV_D] = {P_CHEV_D, 2}, [IC_CHEV_L] = {P_CHEV_L, 2}, [IC_UP] = {P_UP, 3}, [IC_REFRESH] = {P_REFRESH, 2},
    [IC_UPLOAD] = {P_UPLOAD, 4}, [IC_DOWNLOAD] = {P_DOWNLOAD, 4}, [IC_SEARCH] = {P_SEARCH, 2}, [IC_TRASH] = {P_TRASH, 5}, [IC_EDIT] = {P_EDIT, 5},
    [IC_CHECK] = {P_CHECK, 2}, [IC_LINK] = {P_LINK, 2}, [IC_HOME] = {P_HOME, 5}, [IC_NEWFOLDER] = {P_NEWFOLDER, 8},
};

static const AtlasGlyph *icon_glyph(Ui *u, UiIcon ic, int px) {
    Atlas *a = r_atlas(u->r);
    const uint32_t fid = 0x1c0de000u;
    uint32_t key = (uint32_t)ic * 1024u + (uint32_t)px;
    const AtlasGlyph *g = atlas_find_custom(a, fid, key);
    if (g) return g;
    uint8_t *bits = calloc((size_t)px * px, 1);
    float k = (float)px / 16.f, sw = fmaxf(1.f, 1.35f * k);
    for (int y = 0; y < px; y++) for (int x = 0; x < px; x++) {
        float fx = (x + 0.5f), fy = (y + 0.5f), best = 0;
        for (int i = 0; i < ICONS[ic].n; i++) {
            const Prim *p = &ICONS[ic].p[i];
            float d;
            switch (p->k) {
            case 'l': d = sd_seg(fx, fy, p->a * k, p->b * k, p->c * k, p->d * k) - sw / 2; break;
            case 'r': d = fabsf(sd_box(fx, fy, p->a * k, p->b * k, p->c * k, p->d * k)) - sw / 2; break;
            case 'R': d = sd_box(fx, fy, p->a * k, p->b * k, p->c * k, p->d * k); break;
            case 'o': d = fabsf(sqrtf((fx - p->a * k) * (fx - p->a * k) + (fy - p->b * k) * (fy - p->b * k)) - p->c * k) - sw / 2; break;
            case 'O': d = sqrtf((fx - p->a * k) * (fx - p->a * k) + (fy - p->b * k) * (fy - p->b * k)) - p->c * k; break;
            default: { float t[6] = {p->a * k, p->b * k, p->c * k, p->d * k, p->e * k, p->f * k}; d = sd_tri(fx, fy, t); break; }
            }
            float cov = 0.5f - d;
            cov = cov < 0 ? 0 : cov > 1 ? 1 : cov;
            if (cov > best) best = cov;
        }
        bits[(size_t)y * px + x] = (uint8_t)(best * 255.f + 0.5f);
    }
    g = atlas_custom(a, fid, key, bits, px, px);
    free(bits);
    return g;
}

void ui_icon(Ui *u, UiIcon ic, Rect r, uint32_t color) {
    if (ic <= IC_NONE || ic >= IC_COUNT) return;
    int px = (int)(fminf(r.w, r.h) + 0.5f);
    if (px < 8) px = 8;
    const AtlasGlyph *g = icon_glyph(u, ic, px);
    if (!g || g->blank) return;
    RInst i;
    r_make_glyph(u->r, &i, g, (float)floor(r.x + (r.w - px) / 2 + 0.5f), (float)floor(r.y + (r.h - px) / 2 + 0.5f), color);
    i.y = (float)floor(r.y + (r.h - px) / 2 + 0.5f);   /* custom bitmaps are placed by their top-left corner */
    r_push(u->r, &i, 1);
}

/* ---- tooltips ----------------------------------------------------------------------------------------------------------------- */

void ui_tip(Ui *u, Rect r, const char *text) {
    if (u->blocked || !text || !rect_has(r, u->mx, u->my)) return;
    uint64_t id = (uint64_t)(uintptr_t)text ^ ((uint64_t)(int)r.x * 7919u) ^ ((uint64_t)(int)r.y << 20);
    if (u->tip_id != id) { u->tip_id = id; u->tip_since = u->now; }
    u->tip_shown_this_frame = true;
    if (u->now - u->tip_since < 0.45) { ui_want_frames(u, 0.5); return; }
    snprintf(u->tip, sizeof u->tip, "%s", text);
    u->tip_r = r;
}

static void draw_tip(Ui *u) {
    if (!u->tip[0]) return;
    float pad = S(u, 6), w = ui_text_w(u, u->tip) + 2 * pad, h = ui_line_h(u) + S(u, 6);
    float x = u->tip_r.x, y = u->tip_r.y + u->tip_r.h + S(u, 6);
    if (x + w > u->W) x = u->W - w - 2;
    if (y + h > u->H) y = u->tip_r.y - h - S(u, 4);
    if (x < 2) x = 2;
    ui_rrect(u, R(x, y, w, h), u->c.bg2, S(u, 4));
    ui_outline(u, R(x, y, w, h), u->c.border2, S(u, 4), 1);
    ui_text(u, x + pad, y + S(u, 3), u->tip, u->c.ink);
    u->tip[0] = 0;
}

/* ---- buttons -------------------------------------------------------------------------------------------------------------------- */

static uint64_t rid(Rect r, const void *p) { return (uint64_t)(uintptr_t)p ^ ((uint64_t)(int)r.x << 24) ^ ((uint64_t)(int)r.y << 4) ^ 0x9e3779b97f4a7c15ull; }

static bool click_area(Ui *u, Rect r, uint64_t id) {
    bool hov = ui_hover(u, r);
    if (hov && u->pressed[1]) u->active = id;
    bool clicked = false;
    if (u->released[1] && !u->blocked) {
        if (u->active == id && hov) clicked = true;
        if (u->active == id) u->active = 0;
    }
    return clicked;
}

bool ui_button(Ui *u, Rect r, const char *label, int flags) {
    bool dis = flags & UB_DISABLED;
    uint64_t id = rid(r, label);
    bool hov = !dis && ui_hover(u, r);
    bool clicked = !dis && click_area(u, r, id);
    uint32_t bg = (flags & UB_PRIMARY) ? u->c.accent : (flags & UB_ACTIVE) ? u->c.hover : (flags & UB_FLAT) ? 0 : u->c.bg2;
    if (hov && !(flags & UB_PRIMARY)) bg = u->c.hover;
    if (hov && (flags & UB_PRIMARY)) bg = ui_mix_u(u->c.accent, 0xffffffffu, 0.12f);
    if (bg) ui_rrect(u, r, bg, S(u, 6));
    if (!(flags & (UB_PRIMARY | UB_FLAT))) ui_outline(u, r, u->c.border2, S(u, 6), 1);
    uint32_t fg = dis ? u->c.muted : (flags & UB_PRIMARY) ? u->c.on_accent : u->c.ink;
    ui_text_fit(u, rect_inset(r, S(u, 6)), label, fg, 1);
    if (hov) u->want_cursor = 2;
    return clicked;
}

bool ui_icon_button(Ui *u, Rect r, UiIcon ic, const char *tip, int flags) {
    bool dis = flags & UB_DISABLED;
    uint64_t id = rid(r, (const void *)(uintptr_t)(ic + 1));
    bool hov = !dis && ui_hover(u, r);
    bool clicked = !dis && click_area(u, r, id);
    if (hov || (flags & UB_ACTIVE)) ui_rrect(u, r, u->c.hover, S(u, 5));
    uint32_t fg = dis ? u->c.muted : (flags & UB_ACTIVE) ? u->c.accent : hov ? u->c.ink : u->c.ink2;
    float pad = S(u, 5);
    ui_icon(u, ic, rect_inset(r, pad), fg);
    if (tip) ui_tip(u, r, tip);
    if (hov) u->want_cursor = 2;
    return clicked;
}

bool ui_checkbox(Ui *u, Rect r, const char *label, bool *v) {
    uint64_t id = rid(r, v);
    bool hov = ui_hover(u, r);
    bool clicked = click_area(u, r, id);
    if (clicked) *v = !*v;
    float b = S(u, 16);
    Rect box = R(r.x, r.y + (r.h - b) / 2, b, b);
    ui_rrect(u, box, *v ? u->c.accent : u->c.bg2, S(u, 3));
    ui_outline(u, box, *v ? u->c.accent : (hov ? u->c.ink2 : u->c.border2), S(u, 3), 1);
    if (*v) ui_icon(u, IC_CHECK, rect_inset(box, S(u, 1)), u->c.on_accent);
    ui_text(u, r.x + b + S(u, 8), (float)floor(r.y + (r.h - ui_line_h(u)) / 2), label, u->c.ink);
    if (hov) u->want_cursor = 2;
    return clicked;
}

/* ---- text input -------------------------------------------------------------------------------------------------------------------- */

void ui_text_init(UiText *t, char *buf, int cap) { memset(t, 0, sizeof *t); t->s = buf; t->cap = cap; if (cap) buf[0] = 0; }
void ui_text_set(UiText *t, const char *s) {
    snprintf(t->s, (size_t)t->cap, "%s", s ? s : "");
    t->len = (int)strlen(t->s);
    t->caret = t->anchor = t->len;
    t->scroll = 0;
}

bool ui_input_focused(const Ui *u, const UiText *t) { return u->focus == (uint64_t)(uintptr_t)t; }
void ui_input_focus(Ui *u, const UiText *t) { u->focus = (uint64_t)(uintptr_t)t; }

static void sel_range(const UiText *t, int *a, int *b) { *a = t->caret < t->anchor ? t->caret : t->anchor; *b = t->caret < t->anchor ? t->anchor : t->caret; }

static void delete_sel(UiText *t) {
    int a, b;
    sel_range(t, &a, &b);
    if (a == b) return;
    memmove(t->s + a, t->s + b, (size_t)(t->len - b + 1));
    t->len -= b - a;
    t->caret = t->anchor = a;
}

static void insert_text(UiText *t, const char *s) {
    delete_sel(t);
    int n = (int)strlen(s);
    if (t->len + n >= t->cap) return;
    memmove(t->s + t->caret + n, t->s + t->caret, (size_t)(t->len - t->caret + 1));
    memcpy(t->s + t->caret, s, (size_t)n);
    t->len += n;
    t->caret += n;
    t->anchor = t->caret;
}

static float width_to(Ui *u, const char *s, int bytes, bool password) {
    if (!password) { char tmp[512]; int n = bytes < 511 ? bytes : 511; memcpy(tmp, s, (size_t)n); tmp[n] = 0; return ui_text_w(u, tmp); }
    int cps = 0;
    for (const char *p = s; p < s + bytes && *p;) { next_cp(&p); cps++; }
    return cps * ui_text_w(u, "\xe2\x80\xa2");
}

bool ui_input(Ui *u, Rect r, UiText *t, const char *placeholder, bool password) {
    uint64_t id = (uint64_t)(uintptr_t)t;
    bool hov = ui_hover(u, r);
    bool entered = false;
    float pad = S(u, 8);
    if (hov) u->want_cursor = 1;
    if (hov && u->pressed[1]) {
        u->focus = id;
        u->active = id;
        u->input_claimed = true;
        float rel = u->mx - (r.x + pad) + t->scroll;
        int best = 0;
        float prev = 0;
        for (int i = 0; i <= t->len; i = i + (i < t->len ? cp_len_at(t->s + i) : 1)) {
            float w = width_to(u, t->s, i, password);
            if (w >= rel) { best = (rel - prev < w - rel) ? prev_boundary(t->s, i) : i; if (i == 0) best = 0; break; }
            prev = w; best = i;
            if (i >= t->len) break;
        }
        t->caret = best;
        if (!(SDL_GetModState() & KMOD_SHIFT)) t->anchor = best;
    }
    if (u->active == id && u->down[1] && !u->pressed[1]) {   /* drag to select */
        float rel = u->mx - (r.x + pad) + t->scroll;
        int best = 0;
        for (int i = 0; i <= t->len; i += (i < t->len ? cp_len_at(t->s + i) : 1)) { if (width_to(u, t->s, i, password) <= rel) best = i; else break; if (i >= t->len) break; }
        t->caret = best;
    }
    if (u->released[1] && u->active == id) u->active = 0;
    bool focused = u->focus == id;
    if (focused) {
        for (int i = 0; i < u->nkeys; i++) {
            SDL_Keycode k = u->keys[i].sym;
            int m = u->keys[i].mods;
            bool ctrl = m & KMOD_CTRL, shift = m & KMOD_SHIFT;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { entered = true; continue; }
            if (k == SDLK_LEFT) { if (t->caret != t->anchor && !shift) t->caret = t->anchor = t->caret < t->anchor ? t->caret : t->anchor; else { t->caret = prev_boundary(t->s, t->caret); if (!shift) t->anchor = t->caret; } }
            else if (k == SDLK_RIGHT) { if (t->caret != t->anchor && !shift) t->caret = t->anchor = t->caret > t->anchor ? t->caret : t->anchor; else { if (t->caret < t->len) t->caret += cp_len_at(t->s + t->caret); if (!shift) t->anchor = t->caret; } }
            else if (k == SDLK_HOME) { t->caret = 0; if (!shift) t->anchor = 0; }
            else if (k == SDLK_END) { t->caret = t->len; if (!shift) t->anchor = t->len; }
            else if (k == SDLK_BACKSPACE) { if (t->caret == t->anchor && t->caret > 0) t->anchor = prev_boundary(t->s, t->caret); delete_sel(t); }
            else if (k == SDLK_DELETE) { if (t->caret == t->anchor && t->caret < t->len) t->anchor = t->caret + cp_len_at(t->s + t->caret); delete_sel(t); }
            else if (ctrl && k == SDLK_a) { t->anchor = 0; t->caret = t->len; }
            else if (ctrl && (k == SDLK_c || k == SDLK_x)) {
                int a, b;
                sel_range(t, &a, &b);
                if (a != b && !password) { char tmp[1024]; int n = b - a < 1023 ? b - a : 1023; memcpy(tmp, t->s + a, (size_t)n); tmp[n] = 0; SDL_SetClipboardText(tmp); }
                if (k == SDLK_x) delete_sel(t);
            } else if (ctrl && k == SDLK_v) {
                char *c = SDL_GetClipboardText();
                if (c) { for (char *p = c; *p; p++) if (*p == '\n' || *p == '\r') *p = ' '; insert_text(t, c); SDL_free(c); }
            }
        }
        if (u->text[0] && !(SDL_GetModState() & KMOD_CTRL)) insert_text(t, u->text);
        u->text[0] = 0;
        ui_want_frames(u, 0.6);
    }
    /* draw */
    ui_rrect(u, r, u->c.bg2, S(u, 8));
    ui_outline(u, r, focused ? u->c.accent : (hov ? u->c.border2 : u->c.border), S(u, 8), 1);
    Rect inner = R(r.x + pad, r.y, r.w - 2 * pad, r.h);
    float cw = width_to(u, t->s, t->caret, password);
    if (cw - t->scroll > inner.w - 2) t->scroll = cw - inner.w + 2;
    if (cw - t->scroll < 0) t->scroll = cw;
    if (t->scroll < 0) t->scroll = 0;
    ui_clip(u, inner);
    float ty = (float)floor(r.y + (r.h - ui_line_h(u)) / 2);
    if (!t->len && placeholder && !focused) ui_text(u, inner.x, ty, placeholder, u->c.muted);
    int a, b;
    sel_range(t, &a, &b);
    if (focused && a != b) {
        float xa = width_to(u, t->s, a, password), xb = width_to(u, t->s, b, password);
        ui_rect(u, R(inner.x + xa - t->scroll, ty, xb - xa, ui_line_h(u)), (u->c.accent & 0x00ffffffu) | (90u << 24));
    }
    if (password) {
        float x = inner.x - t->scroll;
        for (const char *p = t->s; *p;) { next_cp(&p); ui_text(u, x, ty, "\xe2\x80\xa2", u->c.ink); x += ui_text_w(u, "\xe2\x80\xa2"); }
    } else ui_text(u, inner.x - t->scroll, ty, t->s, u->c.ink);
    if (focused && fmod(u->now, 1.06) < 0.6) ui_rect(u, R(inner.x + cw - t->scroll, ty, S(u, 1) > 1 ? S(u, 1) : 1, ui_line_h(u)), u->c.ink);
    ui_unclip(u);
    return entered;
}

/* ---- scroll areas ------------------------------------------------------------------------------------------------------------------------ */

void ui_scroll_begin(Ui *u, Rect view, float content_h, float *offset) {
    float maxo = content_h > view.h ? content_h - view.h : 0;
    float dw = ui_take_wheel(u, view);
    if (dw != 0) *offset -= dw * ui_line_h(u) * 3;
    if (*offset > maxo) *offset = maxo;
    if (*offset < 0) *offset = 0;
    u->sc_view = view; u->sc_content = content_h; u->sc_off = offset;
    ui_clip(u, view);
}

void ui_scroll_end(Ui *u) {
    ui_unclip(u);
    Rect v = u->sc_view;
    if (u->sc_content <= v.h) return;
    float track = v.h, thumb_h = fmaxf(S(u, 24), track * v.h / u->sc_content);
    float maxo = u->sc_content - v.h;
    float ty = v.y + (track - thumb_h) * (*u->sc_off / maxo);
    float bw = S(u, 6);
    Rect thumb = R(v.x + v.w - bw - S(u, 2), ty, bw, thumb_h);
    uint64_t id = (uint64_t)(uintptr_t)u->sc_off;
    Rect grab = R(thumb.x - S(u, 4), v.y, bw + S(u, 8), v.h);
    bool hov = ui_hover(u, grab);
    if (hov && u->pressed[1]) { u->sc_drag = id; u->sc_drag_dy = u->my - ty; if (u->my < ty || u->my > ty + thumb_h) u->sc_drag_dy = thumb_h / 2; }
    if (u->sc_drag == id) {
        if (u->down[1]) {
            float ny = u->my - u->sc_drag_dy - v.y;
            float f = ny / (track - thumb_h);
            *u->sc_off = (f < 0 ? 0 : f > 1 ? 1 : f) * maxo;
        } else u->sc_drag = 0;
    }
    ui_rrect(u, thumb, (u->sc_drag == id || hov) ? u->c.border2 : u->c.border, bw / 2);
}

/* ---- menus ----------------------------------------------------------------------------------------------------------------------------------- */

void ui_menu_open(Ui *u, float x, float y, const UiMenuItem *items, int n, int tag) {
    free(u->menu_items);
    u->menu_items = malloc((size_t)n * sizeof *items);
    memcpy(u->menu_items, items, (size_t)n * sizeof *items);
    u->menu_n = n; u->menu_tag = tag; u->menu_x = x; u->menu_y = y;
    u->menu_open = true;
    u->menu_hover = -1;
    u->menu_opened_at = u->now;
    u->menu_result = -1;
    ui_want_frames(u, 0.1);
}

bool ui_menu_is_open(const Ui *u) { return u->menu_open; }
int ui_menu_tag(const Ui *u) { return u->menu_open ? u->menu_tag : -1; }
void ui_menu_close(Ui *u) { u->menu_open = false; }

int ui_menu_take(Ui *u, int *tag) {
    int r = u->menu_result;
    if (r >= 0 && tag) *tag = u->menu_result_tag;
    u->menu_result = -1;
    return r;
}

int ui_menu_peek(const Ui *u, int *tag) {
    if (u->menu_result >= 0 && tag) *tag = u->menu_result_tag;
    return u->menu_result;
}

static void draw_menu(Ui *u) {
    if (!u->menu_open) return;
    float row = S(u, 28), pad = S(u, 12), sep = S(u, 9);
    float w = 0, h = S(u, 6);
    for (int i = 0; i < u->menu_n; i++) {
        const UiMenuItem *it = &u->menu_items[i];
        if (it->flags & MI_SEP) { h += sep; continue; }
        if (it->swatch) { float sw = it->nswatch * S(u, 26) + 2 * pad; if (sw > w) w = sw; h += S(u, 34); continue; }
        float lw = ui_text_w(u, it->label) + (it->hint ? ui_text_w(u, it->hint) + S(u, 28) : 0) + 2 * pad + ((it->flags & (MI_CHECK)) ? S(u, 20) : 0);
        if (lw > w) w = lw;
        h += (it->flags & MI_HEADER) ? S(u, 24) : row;
    }
    h += S(u, 6);
    if (w < S(u, 180)) w = S(u, 180);
    float x = u->menu_x, y = u->menu_y;
    if (x + w > u->W) x = (float)u->W - w - 2;
    if (y + h > u->H) y = (float)u->H - h - 2;
    if (x < 2) x = 2;
    if (y < 2) y = 2;
    ui_rrect(u, R(x + 2, y + 3, w, h), 0x66000000u, S(u, 8));
    ui_rrect(u, R(x, y, w, h), u->c.bg2, S(u, 8));
    ui_outline(u, R(x, y, w, h), u->c.border2, S(u, 8), 1);
    float cy = y + S(u, 6);
    int hover = -1;
    int choose = -1;
    for (int i = 0; i < u->menu_n; i++) {
        const UiMenuItem *it = &u->menu_items[i];
        if (it->flags & MI_SEP) { ui_rect(u, R(x + pad, cy + sep / 2, w - 2 * pad, 1), u->c.border); cy += sep; continue; }
        if (it->flags & MI_HEADER) { ui_text(u, x + pad, cy + S(u, 4), it->label, u->c.muted); cy += S(u, 24); continue; }
        if (it->swatch) {
            float sx = x + pad, sy = cy + S(u, 5), d = S(u, 20);
            for (int k = 0; k < it->nswatch; k++) {
                Rect sr = R(sx + k * S(u, 26), sy, d, d);
                bool sh = rect_has(sr, u->mx, u->my);
                ui_rrect(u, sr, it->swatch[k], d / 2);
                if (sh) { ui_outline(u, rect_inset(sr, -S(u, 2)), u->c.ink, d / 2 + 2, 2); hover = -2; if (u->pressed[1] || u->released[1]) choose = it->id + k; }
            }
            cy += S(u, 34);
            continue;
        }
        Rect rr = R(x + S(u, 4), cy, w - S(u, 8), row);
        bool dis = it->flags & MI_DISABLED;
        bool hv = !dis && rect_has(rr, u->mx, u->my);
        if (hv) { hover = i; ui_rrect(u, rr, u->c.hover, S(u, 4)); }
        uint32_t col = dis ? u->c.muted : u->c.ink;
        float tx = x + pad;
        if (it->flags & MI_CHECK) { if (it->flags & MI_CHECKED) ui_icon(u, IC_CHECK, R(tx - S(u, 2), cy + (row - S(u, 16)) / 2, S(u, 16), S(u, 16)), u->c.accent); tx += S(u, 20); }
        ui_text(u, tx, (float)floor(cy + (row - ui_line_h(u)) / 2), it->label, col);
        if (it->hint) ui_text(u, x + w - pad - ui_text_w(u, it->hint), (float)floor(cy + (row - ui_line_h(u)) / 2), it->hint, u->c.muted);
        if (hv && u->released[1] && u->now - u->menu_opened_at > 0.08) choose = it->id;
        cy += row;
    }
    (void)hover;
    bool inside = rect_has(R(x, y, w, h), u->mx, u->my);
    if (choose >= 0 && (u->released[1] || u->pressed[1])) {
        u->menu_result = choose;
        u->menu_result_tag = u->menu_tag;
        u->menu_open = false;
    } else if (!inside && (u->pressed[1] || u->pressed[3]) && u->now - u->menu_opened_at > 0.05) {
        u->menu_open = false;
    }
    for (int i = 0; i < u->nkeys; i++) if (u->keys[i].sym == SDLK_ESCAPE) u->menu_open = false;
    u->want_cursor = 0;
}

void ui_end(Ui *u) {
    if (!u->tip_shown_this_frame) { u->tip_id = 0; u->tip[0] = 0; }
    draw_tip(u);
    draw_menu(u);
    if (u->press_this_frame && !u->input_claimed && u->focus) u->focus = 0;   /* a click elsewhere leaves the text field */
    memset(u->pressed, 0, sizeof u->pressed);
    memset(u->released, 0, sizeof u->released);
    u->wheel = 0;
    u->nkeys = 0;
    u->text[0] = 0;
    u->press_this_frame = false;
    SDL_Cursor *c = u->cursors[u->want_cursor];
    if (c != u->cur_set) { SDL_SetCursor(c); u->cur_set = c; }
}
