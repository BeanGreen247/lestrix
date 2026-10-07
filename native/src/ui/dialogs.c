/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <stdlib.h>

#include "app.h"

#define P(v) S(a->ui, (v))

void dlg_push(App *a, Dialog *d) {
    if (a->ndialogs < 4) a->dialogs[a->ndialogs++] = d;
    else { if (d->free_fn) d->free_fn(d); free(d); }
    app_redraw(a);
}

bool dlg_active(const App *a) { return a->ndialogs > 0; }

void dlg_draw_top(App *a) {
    if (!a->ndialogs) return;
    Dialog *d = a->dialogs[a->ndialogs - 1];
    d->draw(a, d);
    if (d->done) {
        a->ndialogs--;
        if (d->free_fn) d->free_fn(d);
        free(d);
        ui_release_focus(a->ui);
        app_redraw(a);
    }
}

Rect dlg_frame(App *a, const char *title, float w, float h) {
    const UiColors *c = ui_colors(a->ui);
    ui_rect(a->ui, R(0, 0, (float)a->W, (float)a->H), 0x99000000u);
    float pw = P(w), ph = P(h);
    if (pw > a->W - P(20)) pw = (float)a->W - P(20);
    if (ph > a->H - P(20)) ph = (float)a->H - P(20);
    float x = (float)floor((a->W - pw) / 2), y = (float)floor((a->H - ph) / 3);
    if (y < P(10)) y = P(10);
    ui_rrect(a->ui, R(x + 3, y + 5, pw, ph), 0x55000000u, P(10));
    ui_rrect(a->ui, R(x, y, pw, ph), c->bg1, P(8));
    ui_outline(a->ui, R(x, y, pw, ph), c->accent, P(8), 1);
    ui_text(a->ui, x + P(20), y + P(14), title, c->ink);
    ui_rect(a->ui, R(x + P(1), y + P(44), pw - P(2), 1), c->border);
    return R(x + P(20), y + P(56), pw - P(40), ph - P(56) - P(16));
}

static bool esc(App *a) { return ui_key(a->ui, SDLK_ESCAPE, 0); }

typedef struct { Dialog d; char title[160], label[160]; char buf[256]; UiText in; bool password, focused; TextCb cb; void *user; } TextDlg;

static void text_draw(App *a, Dialog *d) {
    TextDlg *t = (TextDlg *)d;
    ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
    Rect c = dlg_frame(a, t->title, 400, t->label[0] ? 168 : 150);
    if (!t->focused) { ui_input_focus(a->ui, &t->in); t->focused = true; }
    float y = c.y;
    if (t->label[0]) { ui_text(a->ui, c.x, y, t->label, ui_colors(a->ui)->muted); y += P(24); }
    bool enter = ui_input(a->ui, R(c.x, y, c.w, P(32)), &t->in, NULL, t->password);
    float by = c.y + c.h - P(32);
    bool ok = ui_button(a->ui, R(c.x + c.w - P(90), by, P(90), P(32)), "OK", UB_PRIMARY);
    bool cancel = ui_button(a->ui, R(c.x + c.w - P(190), by, P(90), P(32)), "Cancel", 0);
    if ((ok || enter) && t->in.len) { d->done = true; t->cb(a, t->in.s, t->user); }
    else if (cancel || esc(a)) d->done = true;
}

void dlg_text(App *a, const char *title, const char *label, const char *initial, bool password, TextCb cb, void *user) {
    TextDlg *t = calloc(1, sizeof *t);
    snprintf(t->title, sizeof t->title, "%s", title);
    snprintf(t->label, sizeof t->label, "%s", label ? label : "");
    ui_text_init(&t->in, t->buf, sizeof t->buf);
    if (initial) ui_text_set(&t->in, initial);
    t->password = password; t->cb = cb; t->user = user;
    t->d.draw = text_draw;
    dlg_push(a, &t->d);
}

typedef struct { Dialog d; char title[160], msg[512], ok[40]; ConfirmCb cb; void *user; } ConfirmDlg;

static void wrap_text(App *a, Rect r, const char *s, uint32_t col) {
    char line[256];
    float y = r.y, lh = ui_line_h(a->ui) + P(3);
    const char *p = s;
    while (*p) {
        int n = 0;
        const char *last_space = NULL;
        line[0] = 0;
        while (p[n] && p[n] != '\n') {
            if (p[n] == ' ') last_space = p + n;
            char tmp[256];
            int m = n + 1 < 255 ? n + 1 : 255;
            memcpy(tmp, p, (size_t)m);
            tmp[m] = 0;
            if (ui_text_w(a->ui, tmp) > r.w && last_space) { n = (int)(last_space - p); break; }
            n++;
        }
        if (n > 255) n = 255;
        memcpy(line, p, (size_t)n);
        line[n] = 0;
        ui_text(a->ui, r.x, y, line, col);
        y += lh;
        p += n;
        while (*p == ' ' || *p == '\n') p++;
    }
}

static void confirm_draw(App *a, Dialog *d) {
    ConfirmDlg *c = (ConfirmDlg *)d;
    ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
    Rect r = dlg_frame(a, c->title, 420, 190);
    wrap_text(a, R(r.x, r.y, r.w, r.h - P(40)), c->msg, ui_colors(a->ui)->ink2);
    float by = r.y + r.h - P(32);
    bool ok = ui_button(a->ui, R(r.x + r.w - P(110), by, P(110), P(32)), c->ok, UB_PRIMARY);
    bool cancel = ui_button(a->ui, R(r.x + r.w - P(210), by, P(90), P(32)), "Cancel", 0);
    if (ok) { d->done = true; c->cb(a, c->user); }
    else if (cancel || esc(a)) d->done = true;
}

void dlg_confirm(App *a, const char *title, const char *msg, const char *ok, ConfirmCb cb, void *user) {
    ConfirmDlg *c = calloc(1, sizeof *c);
    snprintf(c->title, sizeof c->title, "%s", title);
    snprintf(c->msg, sizeof c->msg, "%s", msg);
    snprintf(c->ok, sizeof c->ok, "%s", ok);
    c->cb = cb; c->user = user;
    c->d.draw = confirm_draw;
    dlg_push(a, &c->d);
}

typedef struct { Dialog d; char title[160], msg[512]; } MsgDlg;

static void msg_draw(App *a, Dialog *d) {
    MsgDlg *m = (MsgDlg *)d;
    ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
    Rect r = dlg_frame(a, m->title, 440, 200);
    wrap_text(a, R(r.x, r.y, r.w, r.h - P(40)), m->msg, ui_colors(a->ui)->ink2);
    if (ui_button(a->ui, R(r.x + r.w - P(90), r.y + r.h - P(32), P(90), P(32)), "OK", UB_PRIMARY) || esc(a) || ui_key(a->ui, SDLK_RETURN, 0)) d->done = true;
}

void dlg_message(App *a, const char *title, const char *msg) {
    MsgDlg *m = calloc(1, sizeof *m);
    snprintf(m->title, sizeof m->title, "%s", title);
    snprintf(m->msg, sizeof m->msg, "%s", msg);
    m->d.draw = msg_draw;
    dlg_push(a, &m->d);
}

void dlg_about(App *a) {
    char msg[400];
    snprintf(msg, sizeof msg, "Lestrix " APP_VERSION "\nSSH sessions, SFTP/FTP and local shells in one window.\n\nMIT licence.\n\nRenderer: %s", a->gl_desc);
    dlg_message(a, "About Lestrix", msg);
}

typedef struct { Dialog d; char title[120]; char buf[16]; UiText in; bool focused; TextCb cb; void *user; } ColorDlg;

static const char *SWATCHES[] = {"#ef5350", "#ff9800", "#ffca28", "#66bb6a", "#26a69a", "#26c6da", "#42a5f5", "#7e57c2", "#ba68c8", "#ec407a"};

static void color_draw(App *a, Dialog *d) {
    ColorDlg *c = (ColorDlg *)d;
    ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
    Rect r = dlg_frame(a, c->title, 360, 230);
    if (!c->focused) { ui_input_focus(a->ui, &c->in); c->focused = true; }
    for (int i = 0; i < 10; i++) {
        Rect s = R(r.x + i * P(32), r.y, P(26), P(26));
        ui_rrect(a->ui, s, ui_rgba(SWATCHES[i], 255), P(13));
        if (ui_mouse_pressed(a->ui, 1, s)) ui_text_set(&c->in, SWATCHES[i]);
        if (ui_hover(a->ui, s)) ui_outline(a->ui, rect_inset(s, -P(2)), ui_colors(a->ui)->ink, P(15), 2);
    }
    ui_text(a->ui, r.x, r.y + P(42), "Hex colour", ui_colors(a->ui)->muted);
    bool enter = ui_input(a->ui, R(r.x, r.y + P(64), r.w - P(50), P(32)), &c->in, "#rrggbb", false);
    unsigned rr, gg, bb;
    bool valid = c->in.s[0] == '#' && strlen(c->in.s) == 7 && sscanf(c->in.s + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3;
    ui_rrect(a->ui, R(r.x + r.w - P(38), r.y + P(64), P(38), P(32)), valid ? ui_rgba(c->in.s, 255) : ui_colors(a->ui)->bg2, P(5));
    float by = r.y + r.h - P(32);
    bool ok = ui_button(a->ui, R(r.x + r.w - P(90), by, P(90), P(32)), "OK", valid ? UB_PRIMARY : UB_DISABLED);
    bool cancel = ui_button(a->ui, R(r.x + r.w - P(190), by, P(90), P(32)), "Cancel", 0);
    if ((ok || enter) && valid) { d->done = true; c->cb(a, c->in.s, c->user); }
    else if (cancel || esc(a)) d->done = true;
}

void dlg_color(App *a, const char *title, const char *initial, TextCb cb, void *user) {
    ColorDlg *c = calloc(1, sizeof *c);
    snprintf(c->title, sizeof c->title, "%s", title);
    ui_text_init(&c->in, c->buf, sizeof c->buf);
    ui_text_set(&c->in, initial && *initial ? initial : "#00bcd4");
    c->cb = cb; c->user = user;
    c->d.draw = color_draw;
    dlg_push(a, &c->d);
}

typedef struct { char *name; bool dir; } PEntry;

typedef struct {
    Dialog d;
    char title[120];
    bool folder, multi, hidden, focused;
    char *dir;
    PEntry *e; int n;
    bool *sel; int last_click;
    char buf[1024]; UiText path;
    float scroll;
    PickCb cb; void *user;
    double last_click_t;
} PickDlg;

static int pe_cmp(const void *x, const void *y) {
    const PEntry *a = x, *b = y;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    return g_ascii_strcasecmp(a->name, b->name);
}

static void pick_load(PickDlg *p, const char *dir) {
    for (int i = 0; i < p->n; i++) g_free(p->e[i].name);
    g_free(p->e); g_free(p->sel);
    p->e = NULL; p->n = 0; p->sel = NULL;
    char *canon = g_canonicalize_filename(dir, NULL);
    g_free(p->dir);
    p->dir = canon;
    GDir *gd = g_dir_open(p->dir, 0, NULL);
    int cap = 0;
    if (gd) {
        const char *nm;
        while ((nm = g_dir_read_name(gd))) {
            if (!p->hidden && nm[0] == '.') continue;
            char *full = g_build_filename(p->dir, nm, NULL);
            bool isdir = g_file_test(full, G_FILE_TEST_IS_DIR);
            g_free(full);
            if (p->folder && !isdir) continue;
            if (p->n == cap) { cap = cap ? cap * 2 : 64; p->e = g_renew(PEntry, p->e, cap); }
            p->e[p->n].name = g_strdup(nm);
            p->e[p->n].dir = isdir;
            p->n++;
        }
        g_dir_close(gd);
    }
    if (p->n) qsort(p->e, (size_t)p->n, sizeof *p->e, pe_cmp);
    p->sel = g_new0(bool, p->n + 1);
    p->scroll = 0;
    p->last_click = -1;
    ui_text_set(&p->path, p->dir);
}

static void pick_free(Dialog *d) {
    PickDlg *p = (PickDlg *)d;
    for (int i = 0; i < p->n; i++) g_free(p->e[i].name);
    g_free(p->e); g_free(p->sel); g_free(p->dir);
}

static void pick_finish(App *a, PickDlg *p, bool ok) {
    p->d.done = true;
    if (!ok) { p->cb(a, NULL, 0, p->user); return; }
    char **paths = g_new0(char *, p->n + 2);
    int k = 0;
    for (int i = 0; i < p->n; i++) if (p->sel[i]) paths[k++] = g_build_filename(p->dir, p->e[i].name, NULL);
    if (!k && p->folder) paths[k++] = g_strdup(p->dir);
    p->cb(a, paths, k, p->user);
    g_strfreev(paths);
}

static void pick_draw(App *a, Dialog *d) {
    PickDlg *p = (PickDlg *)d;
    const UiColors *c = ui_colors(a->ui);
    ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
    Rect r = dlg_frame(a, p->title, 640, 520);
    if (!p->focused) { p->focused = true; }
    float y = r.y;
    if (ui_icon_button(a->ui, R(r.x, y, P(32), P(32)), IC_UP, "Parent folder", 0)) { char *up = g_path_get_dirname(p->dir); pick_load(p, up); g_free(up); }
    if (ui_icon_button(a->ui, R(r.x + P(36), y, P(32), P(32)), IC_HOME, "Home", 0)) pick_load(p, g_get_home_dir());
    bool enter = ui_input(a->ui, R(r.x + P(76), y, r.w - P(76) - P(120), P(32)), &p->path, NULL, false);
    if (enter) {
        if (g_file_test(p->path.s, G_FILE_TEST_IS_DIR)) pick_load(p, p->path.s);
        else if (!p->folder && g_file_test(p->path.s, G_FILE_TEST_EXISTS)) {
            char *one[1] = {p->path.s};
            p->d.done = true; p->cb(a, one, 1, p->user);
            return;
        }
    }
    bool before = p->hidden;
    ui_checkbox(a->ui, R(r.x + r.w - P(112), y, P(112), P(32)), "Hidden", &p->hidden);
    if (before != p->hidden) { char *keep = g_strdup(p->dir); pick_load(p, keep); g_free(keep); }
    y += P(42);
    float lh = P(28), listh = r.h - P(42) - P(50);
    Rect list = R(r.x, y, r.w, listh);
    ui_rrect(a->ui, list, c->bg0, P(5));
    ui_outline(a->ui, list, c->border, P(5), 1);
    ui_scroll_begin(a->ui, rect_inset(list, 1), p->n * lh, &p->scroll);
    for (int i = 0; i < p->n; i++) {
        Rect row = R(list.x + 1, list.y + 1 + i * lh - p->scroll, list.w - 2, lh);
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        bool hov = ui_hover(a->ui, row) && rect_has(list, ui_mx(a->ui), ui_my(a->ui));
        if (p->sel[i]) ui_rrect(a->ui, row, c->accent, P(8));
        else if (hov) ui_rrect(a->ui, row, c->hover, P(8));
        ui_icon(a->ui, p->e[i].dir ? IC_FOLDER : IC_FILE, R(row.x + P(8), row.y + (lh - P(16)) / 2, P(16), P(16)), p->sel[i] ? c->bg1 : (p->e[i].dir ? c->accent : c->ink2));
        ui_text_fit(a->ui, R(row.x + P(32), row.y, row.w - P(40), row.h), p->e[i].name, p->sel[i] ? c->bg1 : c->ink, 0);
        if (hov && ui_mouse_pressed(a->ui, 1, row)) {
            int mods = SDL_GetModState();
            if (p->multi && (mods & KMOD_CTRL)) p->sel[i] = !p->sel[i];
            else if (p->multi && (mods & KMOD_SHIFT) && p->last_click >= 0) {
                int lo = p->last_click < i ? p->last_click : i, hi = p->last_click < i ? i : p->last_click;
                for (int k = lo; k <= hi; k++) p->sel[k] = true;
            } else { memset(p->sel, 0, (size_t)p->n * sizeof(bool)); p->sel[i] = true; }
            p->last_click = i;
            if (ui_clicks(a->ui) >= 2) {
                if (p->e[i].dir) { char *np = g_build_filename(p->dir, p->e[i].name, NULL); pick_load(p, np); g_free(np); break; }
                else { pick_finish(a, p, true); break; }
            }
        }
    }
    ui_scroll_end(a->ui);
    float by = r.y + r.h - P(32);
    bool any = p->folder;
    for (int i = 0; i < p->n; i++) if (p->sel[i]) any = true;
    bool ok = ui_button(a->ui, R(r.x + r.w - P(130), by, P(130), P(32)), p->folder ? "Choose folder" : "Open", any ? UB_PRIMARY : UB_DISABLED);
    bool cancel = ui_button(a->ui, R(r.x + r.w - P(230), by, P(90), P(32)), "Cancel", 0);
    if (ok) pick_finish(a, p, true);
    else if (cancel || esc(a)) pick_finish(a, p, false);
}

void dlg_pick_file(App *a, const char *title, bool folder, bool multi, const char *start, PickCb cb, void *user) {
    PickDlg *p = calloc(1, sizeof *p);
    snprintf(p->title, sizeof p->title, "%s", title);
    p->folder = folder; p->multi = multi; p->cb = cb; p->user = user;
    ui_text_init(&p->path, p->buf, sizeof p->buf);
    pick_load(p, start && *start ? start : g_get_home_dir());
    p->d.draw = pick_draw;
    p->d.free_fn = pick_free;
    dlg_push(a, &p->d);
}

#define NF 8
typedef struct {
    Dialog d;
    SdConn *conn;
    bool is_new;
    char b[NF][320]; char port_b[12];
    UiText f[NF], port;
    int proto, x11;
    bool focused, error;
} ConnDlg;

static const char *PROTOS[] = {"ssh", "ftp", "ftps", NULL};
static const char *PROTO_L[] = {"SSH (terminal + SFTP)", "FTP (file browser)", "FTPS (file browser)"};
static const char *X11S[] = {"off", "untrusted", "trusted", NULL};
static const char *X11_L[] = {"Off", "X11 forwarding (-X)", "Trusted X11 forwarding (-Y)"};
enum { F_NAME, F_GROUP, F_HOST, F_USER, F_KEY, F_REMOTE, F_OPTS };

static int index_of(const char **list, const char *v) { for (int i = 0; list[i]; i++) if (g_str_equal(list[i], v)) return i; return 0; }

static void conn_free(Dialog *d) { sd_conn_free(((ConnDlg *)d)->conn); }

static void key_picked(App *a, char **paths, int n, void *user) {
    (void)a;
    ConnDlg *c = user;
    if (n > 0) ui_text_set(&c->f[F_KEY], paths[0]);
}

static void conn_draw(App *a, Dialog *d) {
    ConnDlg *c = (ConnDlg *)d;
    const UiColors *col = ui_colors(a->ui);
    bool menu = ui_menu_is_open(a->ui);
    ui_set_blocked(a->ui, menu);
    int tag;
    int chosen = ui_menu_take(a->ui, &tag);
    if (chosen >= 0 && tag == 71) c->proto = chosen;
    if (chosen >= 0 && tag == 72) c->x11 = chosen;
    Rect r = dlg_frame(a, c->is_new ? "New connection" : "Edit connection", 520, 560);
    if (!c->focused) { ui_input_focus(a->ui, &c->f[F_NAME]); c->focused = true; }
    float lw = P(104), rowh = P(44), y = r.y;
    struct { const char *label; int kind; int idx; const char *hint; } rows[] = {
        {"Name", 0, F_NAME, NULL}, {"Group", 0, F_GROUP, NULL}, {"Type", 1, 0, NULL}, {"Host", 0, F_HOST, NULL}, {"Port", 2, 0, NULL}, {"User", 0, F_USER, NULL},
        {"Private key", 3, F_KEY, "optional - defaults to ssh-agent and ~/.ssh keys"}, {"X11", 4, 0, NULL},
        {"Remote shell", 0, F_REMOTE, "empty = the account's login shell (e.g. fish -l)"}, {"Extra ssh args", 0, F_OPTS, "e.g. -J jumphost -L 8080:localhost:80"},
    };
    UiText *order[NF + 1];
    int no = 0;
    for (unsigned i = 0; i < sizeof rows / sizeof *rows; i++) {
        ui_text(a->ui, r.x, y + (rowh - P(10) - ui_line_h(a->ui)) / 2, rows[i].label, col->muted);
        Rect fr = R(r.x + lw, y, r.w - lw, rowh - P(10));
        switch (rows[i].kind) {
        case 0: ui_input(a->ui, fr, &c->f[rows[i].idx], rows[i].hint, false); order[no++] = &c->f[rows[i].idx]; break;
        case 1: {
            if (ui_button(a->ui, fr, PROTO_L[c->proto], 0)) {
                UiMenuItem it[3];
                for (int k = 0; k < 3; k++) it[k] = (UiMenuItem){PROTO_L[k], NULL, k, MI_CHECK | (k == c->proto ? MI_CHECKED : 0), NULL, 0};
                ui_menu_open(a->ui, fr.x, fr.y + fr.h + P(2), it, 3, 71);
            }
            ui_icon(a->ui, IC_CHEV_D, R(fr.x + fr.w - P(24), fr.y + (fr.h - P(14)) / 2, P(14), P(14)), col->ink2);
            break;
        }
        case 2: ui_input(a->ui, R(fr.x, fr.y, P(110), fr.h), &c->port, NULL, false); order[no++] = &c->port; break;
        case 3: {
            ui_input(a->ui, R(fr.x, fr.y, fr.w - P(96), fr.h), &c->f[F_KEY], rows[i].hint, false);
            order[no++] = &c->f[F_KEY];
            if (ui_button(a->ui, R(fr.x + fr.w - P(88), fr.y, P(88), fr.h), "Browse…", 0)) {
                char *ssh = g_build_filename(g_get_home_dir(), ".ssh", NULL);
                dlg_pick_file(a, "Private key", false, false, ssh, key_picked, c);
                g_free(ssh);
            }
            break;
        }
        default: {
            if (ui_button(a->ui, fr, X11_L[c->x11], 0)) {
                UiMenuItem it[3];
                for (int k = 0; k < 3; k++) it[k] = (UiMenuItem){X11_L[k], NULL, k, MI_CHECK | (k == c->x11 ? MI_CHECKED : 0), NULL, 0};
                ui_menu_open(a->ui, fr.x, fr.y + fr.h + P(2), it, 3, 72);
            }
            ui_icon(a->ui, IC_CHEV_D, R(fr.x + fr.w - P(24), fr.y + (fr.h - P(14)) / 2, P(14), P(14)), col->ink2);
            break;
        }
        }
        y += rowh;
    }
    if (c->error) ui_text(a->ui, r.x, y - P(4), "Name and host are required.", col->red);
    if (ui_key(a->ui, SDLK_TAB, 0) || ui_key(a->ui, SDLK_TAB, KMOD_SHIFT)) {
        int cur = -1;
        for (int i = 0; i < no; i++) if (ui_input_focused(a->ui, order[i])) cur = i;
        bool back = ui_key(a->ui, SDLK_TAB, KMOD_SHIFT);
        int nx = cur < 0 ? 0 : (cur + (back ? no - 1 : 1)) % no;
        ui_input_focus(a->ui, order[nx]);
        order[nx]->anchor = 0;
        order[nx]->caret = order[nx]->len;
    }
    float by = r.y + r.h - P(32);
    bool save = ui_button(a->ui, R(r.x + r.w - P(90), by, P(90), P(32)), "Save", UB_PRIMARY);
    bool cancel = ui_button(a->ui, R(r.x + r.w - P(190), by, P(90), P(32)), "Cancel", 0);
    if (ui_key(a->ui, SDLK_RETURN, 0) && !menu) save = true;
    if (save) {
        g_strstrip(c->f[F_NAME].s); g_strstrip(c->f[F_HOST].s);
        c->f[F_NAME].len = (int)strlen(c->f[F_NAME].s); c->f[F_HOST].len = (int)strlen(c->f[F_HOST].s);
        if (!c->f[F_NAME].s[0] || !c->f[F_HOST].s[0]) { c->error = true; return; }
        SdConn *cn = c->conn;
        sd_conn_set(&cn->name, c->f[F_NAME].s);
        sd_conn_set(&cn->host, c->f[F_HOST].s);
        sd_conn_set(&cn->group, c->f[F_GROUP].s[0] ? c->f[F_GROUP].s : "Ungrouped");
        int port = atoi(c->port.s);
        cn->port = port < 1 ? 22 : port > 65535 ? 65535 : port;
        sd_conn_set(&cn->user, c->f[F_USER].s);
        sd_conn_set(&cn->key, c->f[F_KEY].s);
        sd_conn_set(&cn->options, c->f[F_OPTS].s);
        sd_conn_set(&cn->remote_command, c->f[F_REMOTE].s);
        sd_conn_set(&cn->protocol, PROTOS[c->proto]);
        sd_conn_set(&cn->x11, X11S[c->x11]);
        sd_store_upsert(a->store, cn);
        app_refresh(a);
        d->done = true;
    } else if (cancel || (esc(a) && !menu)) d->done = true;
}

void dlg_conn_editor(App *a, const SdConn *conn, bool copy_of_live) {
    ConnDlg *c = calloc(1, sizeof *c);
    c->conn = conn ? sd_conn_copy(conn) : sd_conn_new();
    c->is_new = !conn || copy_of_live;
    if (conn && copy_of_live) {
        char *id = g_uuid_string_random();
        sd_conn_set(&c->conn->id, g_strdelimit(id, "-", 'x'));
        g_free(id);
        sd_conn_set(&c->conn->alias, "");
        sd_conn_set(&c->conn->group, "Saved");
    }
    SdConn *cn = c->conn;
    for (int i = 0; i < NF; i++) ui_text_init(&c->f[i], c->b[i], sizeof c->b[i]);
    ui_text_set(&c->f[F_NAME], cn->name); ui_text_set(&c->f[F_GROUP], cn->group); ui_text_set(&c->f[F_HOST], cn->host);
    ui_text_set(&c->f[F_USER], cn->user); ui_text_set(&c->f[F_KEY], cn->key); ui_text_set(&c->f[F_REMOTE], cn->remote_command);
    ui_text_set(&c->f[F_OPTS], cn->options);
    ui_text_init(&c->port, c->port_b, sizeof c->port_b);
    char pb[12];
    snprintf(pb, sizeof pb, "%d", cn->port);
    ui_text_set(&c->port, pb);
    c->proto = index_of(PROTOS, cn->protocol);
    c->x11 = index_of(X11S, cn->x11);
    c->d.draw = conn_draw;
    c->d.free_fn = conn_free;
    dlg_push(a, &c->d);
}
