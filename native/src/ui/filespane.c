/* filespane.c - the remote file browser (SFTP / ssh fallbacks / FTP), drawn with the toolkit. */
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "app.h"
#include "jobs.h"

#define P(v) S(a->ui, (v))

struct FilesPane {
    App *app;
    SdXfer *xfer;
    char *path, *pending_cwd;
    bool ready, osc7_seen, active, polling, follow, ftp;
    double last_poll, next_ready, next_poll, drawn_at;
    int refs;
    bool alive;
    char status[300];
    SdEntry **ent; int n;
    bool *sel; int last_click;
    float scroll;
    int sort_col; bool sort_desc;
    char pathbuf[1024]; UiText pathin;
    char label_buf[96];
    bool want_path_sync;
};

static GPtrArray *live;   /* every pane that exists: dialogs hold plain pointers and check here first */

static bool pane_ok(FilesPane *f) { return live && f && g_ptr_array_find(live, f, NULL); }

static void set_status(FilesPane *f, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void set_status(FilesPane *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->status, sizeof f->status, fmt, ap);
    va_end(ap);
    app_redraw(f->app);
}

static char *fmt_size(gint64 n) {
    double v = (double)n;
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    return i == 0 ? g_strdup_printf("%d B", (int)v) : g_strdup_printf("%.1f %s", v, u[i]);
}

static char *join(const char *dir, const char *name) {
    if (g_str_equal(dir, "/")) return g_strconcat("/", name, NULL);
    return g_str_has_suffix(dir, "/") ? g_strconcat(dir, name, NULL) : g_strconcat(dir, "/", name, NULL);
}

/* ---- tasks: one struct for every background operation --------------------------------------------------------------- */

enum { K_LIST, K_READY, K_CWD, K_MKDIR, K_RENAME, K_DELETE, K_DOWNLOAD, K_UPLOAD };
typedef struct { char *path; bool is_dir; } Item;
typedef struct { FilesPane *f; int kind; char *a, *b; GPtrArray *items; GError *err; char *out_path; GPtrArray *out_entries; bool ok; } Task;

static void item_free(gpointer p) { Item *i = p; g_free(i->path); g_free(i); }

static void task_free(Task *t) {
    g_free(t->a); g_free(t->b); g_free(t->out_path);
    if (t->items) g_ptr_array_free(t->items, TRUE);
    if (t->out_entries) g_ptr_array_free(t->out_entries, TRUE);
    g_clear_error(&t->err);
    g_free(t);
}

static void pane_free(FilesPane *f) {
    for (int i = 0; i < f->n; i++) sd_entry_free(f->ent[i]);
    g_free(f->ent); g_free(f->sel); g_free(f->path); g_free(f->pending_cwd);
    sd_xfer_free(f->xfer);
    g_free(f);
}

static void navigate_to(FilesPane *f, const char *path);

static void *task_work(void *arg) {
    Task *t = arg;
    FilesPane *f = t->f;
    switch (t->kind) {
    case K_READY: t->ok = sd_xfer_ready(f->xfer); break;
    case K_CWD: t->out_path = sd_xfer_terminal_cwd(f->xfer); t->ok = true; break;
    case K_LIST: {
        char *path = t->a ? g_strdup(t->a) : sd_xfer_home(f->xfer, &t->err);
        if (!path) break;
        GPtrArray *e = sd_xfer_list(f->xfer, path, &t->err);
        if (!e) { g_free(path); break; }
        t->out_path = path; t->out_entries = e; t->ok = true;
        break;
    }
    case K_MKDIR: t->ok = sd_xfer_mkdir(f->xfer, t->a, &t->err); break;
    case K_RENAME: t->ok = sd_xfer_rename(f->xfer, t->a, t->b, &t->err); break;
    case K_DELETE:
        t->ok = true;
        for (guint i = 0; i < t->items->len && t->ok; i++) { Item *it = t->items->pdata[i]; t->ok = sd_xfer_remove(f->xfer, it->path, it->is_dir, &t->err); }
        break;
    case K_DOWNLOAD:
        t->ok = true;
        for (guint i = 0; i < t->items->len && t->ok; i++) { Item *it = t->items->pdata[i]; t->ok = sd_xfer_download(f->xfer, it->path, t->b, it->is_dir, &t->err); }
        break;
    case K_UPLOAD:
        t->ok = true;
        for (guint i = 0; i < t->items->len && t->ok; i++) t->ok = sd_xfer_upload(f->xfer, ((Item *)t->items->pdata[i])->path, t->a, &t->err);
        break;
    }
    return NULL;
}

static int cmp_col, cmp_desc;
static int ent_cmp(const void *x, const void *y) {
    const SdEntry *a = *(SdEntry *const *)x, *b = *(SdEntry *const *)y;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    int r;
    if (cmp_col == 1) r = a->size < b->size ? -1 : a->size > b->size;
    else if (cmp_col == 2) r = g_strcmp0(a->modified, b->modified);
    else r = g_ascii_strcasecmp(a->name, b->name);
    return cmp_desc ? -r : r;
}

static void sort_entries(FilesPane *f) {
    cmp_col = f->sort_col; cmp_desc = f->sort_desc;
    if (f->n) {
        /* keep the selection by name across the sort */
        qsort(f->ent, (size_t)f->n, sizeof *f->ent, ent_cmp);
    }
}

static void task_done(void *arg, void *result) {
    (void)result;
    Task *t = arg;
    FilesPane *f = t->f;
    f->refs--;
    if (!f->alive) { task_free(t); if (!f->refs) pane_free(f); return; }
    switch (t->kind) {
    case K_READY:
        if (t->ok && !f->ready) { f->ready = true; set_status(f, "Connected"); navigate_to(f, NULL); }
        break;
    case K_CWD:
        f->polling = false;
        if (t->out_path && f->path && !g_str_equal(t->out_path, f->path)) navigate_to(f, t->out_path);
        break;
    case K_LIST:
        if (!t->ok) { set_status(f, "\xe2\x9a\xa0 %s", t->err ? t->err->message : "failed"); break; }
        for (int i = 0; i < f->n; i++) sd_entry_free(f->ent[i]);
        g_free(f->ent); g_free(f->sel);
        g_free(f->path);
        f->path = g_strdup(t->out_path);
        ui_text_set(&f->pathin, f->path);
        f->n = (int)t->out_entries->len;
        f->ent = g_new0(SdEntry *, f->n + 1);
        for (int i = 0; i < f->n; i++) { f->ent[i] = t->out_entries->pdata[i]; t->out_entries->pdata[i] = NULL; }
        f->sel = g_new0(bool, f->n + 1);
        f->last_click = -1;
        f->scroll = 0;
        sort_entries(f);
        {
            char *label = sd_xfer_label(f->xfer);
            set_status(f, "%d item(s) \xc2\xb7 %s", f->n, label);
            g_free(label);
        }
        if (f->pending_cwd && !g_str_equal(f->pending_cwd, f->path) && f->follow) {
            char *next = f->pending_cwd;
            f->pending_cwd = NULL;
            navigate_to(f, next);
            g_free(next);
        }
        break;
    case K_MKDIR: case K_RENAME: case K_DELETE:
        if (!t->ok) set_status(f, "\xe2\x9a\xa0 %s", t->err ? t->err->message : "failed");
        else navigate_to(f, f->path);
        break;
    case K_DOWNLOAD:
        if (!t->ok) set_status(f, "\xe2\x9a\xa0 %s", t->err ? t->err->message : "failed");
        else set_status(f, "Downloaded to %s", t->b);
        break;
    case K_UPLOAD:
        if (!t->ok) set_status(f, "\xe2\x9a\xa0 %s", t->err ? t->err->message : "failed");
        else { set_status(f, "Upload complete"); navigate_to(f, f->path); }
        break;
    }
    task_free(t);
    app_redraw(f->app);
}

static Task *task_new(FilesPane *f, int kind) {
    Task *t = g_new0(Task, 1);
    t->f = f; t->kind = kind;
    return t;
}

static void task_run(Task *t) {
    t->f->refs++;
    jobs_run(task_work, task_done, t);
}

static void navigate_to(FilesPane *f, const char *path) {
    if (!f->ready) return;
    Task *t = task_new(f, K_LIST);
    t->a = path ? g_strdup(path) : NULL;
    task_run(t);
}

/* ---- life cycle -------------------------------------------------------------------------------------------------------------- */

FilesPane *files_new(App *a, SdXfer *x, bool follow) {
    FilesPane *f = g_new0(FilesPane, 1);
    f->app = a; f->xfer = x; f->alive = true;
    f->ftp = sd_xfer_is_ftp(x);
    f->follow = follow && !f->ftp;
    ui_text_init(&f->pathin, f->pathbuf, sizeof f->pathbuf);
    snprintf(f->status, sizeof f->status, "Waiting for the connection\xe2\x80\xa6");
    f->last_click = -1;
    if (!live) live = g_ptr_array_new();
    g_ptr_array_add(live, f);
    if (f->ftp) { f->ready = true; navigate_to(f, NULL); }
    return f;
}

void files_free(FilesPane *f) {
    if (!f) return;
    g_ptr_array_remove(live, f);
    f->alive = false;
    if (!f->refs) pane_free(f);   /* otherwise the last finishing job frees it */
}

bool files_follow(const FilesPane *f) { return f->follow; }
void files_set_follow(FilesPane *f, bool v) { f->follow = v && !f->ftp; }
void files_note_activity(FilesPane *f) { f->active = true; }

const char *files_label(FilesPane *f) {
    char *l = sd_xfer_label(f->xfer);
    snprintf(f->label_buf, sizeof f->label_buf, "%s", l ? l : "");
    g_free(l);
    return f->label_buf;
}

void files_set_cwd(FilesPane *f, const char *path) {
    f->osc7_seen = true;
    if (f->follow && (!f->path || !g_str_equal(path, f->path))) {
        g_free(f->pending_cwd);
        f->pending_cwd = g_strdup(path);
        if (f->ready) navigate_to(f, path);
    }
}

void files_tick(App *a, FilesPane *f) {
    if (!f->ready && a->now >= f->next_ready) {
        f->next_ready = a->now + 1.0;
        Task *t = task_new(f, K_READY);
        task_run(t);
    }
    bool visible = a->now - f->drawn_at < 0.6;
    if (f->ready && !f->polling && !f->osc7_seen && !f->ftp && f->follow && a->now >= f->next_poll) {
        f->next_poll = a->now + 2.0;
        if (visible && (f->active || a->now - f->last_poll >= 10.0)) {
            f->active = false;
            f->last_poll = a->now;
            f->polling = true;
            task_run(task_new(f, K_CWD));
        }
    }
}

/* ---- operations ----------------------------------------------------------------------------------------------------------------- */

static GPtrArray *selected(FilesPane *f) {
    GPtrArray *out = g_ptr_array_new();
    for (int i = 0; i < f->n; i++) if (f->sel[i]) g_ptr_array_add(out, f->ent[i]);
    return out;
}

static void start_download(FilesPane *f, GPtrArray *entries, const char *dest) {
    Task *t = task_new(f, K_DOWNLOAD);
    t->b = g_strdup(dest);
    t->items = g_ptr_array_new_with_free_func(item_free);
    for (guint i = 0; i < entries->len; i++) {
        SdEntry *e = entries->pdata[i];
        Item *it = g_new0(Item, 1);
        it->path = join(f->path, e->name);
        it->is_dir = e->is_dir;
        g_ptr_array_add(t->items, it);
    }
    set_status(f, "Downloading %u item(s)\xe2\x80\xa6", entries->len);
    task_run(t);
}

static void start_upload(FilesPane *f, char **paths, int n) {
    if (!f->ready || !f->path || n <= 0) return;
    Task *t = task_new(f, K_UPLOAD);
    t->a = g_strdup(f->path);
    t->items = g_ptr_array_new_with_free_func(item_free);
    for (int i = 0; i < n; i++) { Item *it = g_new0(Item, 1); it->path = g_strdup(paths[i]); g_ptr_array_add(t->items, it); }
    set_status(f, "Uploading %d item(s)\xe2\x80\xa6", n);
    task_run(t);
}

void files_drop(FilesPane *f, const char *path) {
    char *one[1] = {(char *)path};
    start_upload(f, one, 1);
}

static void mkdir_cb(App *a, const char *text, void *user) {
    (void)a;
    FilesPane *f = user;
    if (!pane_ok(f) || !f->path) return;
    Task *t = task_new(f, K_MKDIR);
    t->a = join(f->path, text);
    task_run(t);
}

typedef struct { FilesPane *f; char *old; } RenameCtx;

static void rename_cb(App *a, const char *text, void *user) {
    (void)a;
    RenameCtx *r = user;
    if (pane_ok(r->f) && r->f->path && !g_str_equal(text, r->old)) {
        Task *t = task_new(r->f, K_RENAME);
        t->a = join(r->f->path, r->old);
        t->b = join(r->f->path, text);
        task_run(t);
    }
    g_free(r->old);
    g_free(r);
}

typedef struct { FilesPane *f; GPtrArray *items; } DelCtx;

static void delete_cb(App *a, void *user) {
    (void)a;
    DelCtx *d = user;
    if (pane_ok(d->f) && d->items->len) {
        Task *t = task_new(d->f, K_DELETE);
        t->items = d->items;
        task_run(t);
    } else g_ptr_array_free(d->items, TRUE);
    g_free(d);
}

static void download_folder_cb(App *a, char **paths, int n, void *user) {
    (void)a;
    FilesPane *f = user;
    if (!pane_ok(f) || n < 1) return;
    GPtrArray *sel = selected(f);
    if (sel->len) start_download(f, sel, paths[0]);
    g_ptr_array_free(sel, TRUE);
}

static void upload_cb(App *a, char **paths, int n, void *user) {
    (void)a;
    FilesPane *f = user;
    if (pane_ok(f)) start_upload(f, paths, n);
}

static void act_download(App *a, FilesPane *f) {
    GPtrArray *sel = selected(f);
    bool any = sel->len > 0;
    g_ptr_array_free(sel, TRUE);
    if (!any) return;
    const char *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    dlg_pick_file(a, "Download to folder", true, false, dl ? dl : g_get_home_dir(), download_folder_cb, f);
}

static void act_delete(App *a, FilesPane *f) {
    GPtrArray *sel = selected(f);
    if (!sel->len) { g_ptr_array_free(sel, TRUE); return; }
    DelCtx *d = g_new0(DelCtx, 1);
    d->f = f;
    d->items = g_ptr_array_new_with_free_func(item_free);
    GString *names = g_string_new(NULL);
    for (guint i = 0; i < sel->len; i++) {
        SdEntry *e = sel->pdata[i];
        Item *it = g_new0(Item, 1);
        it->path = join(f->path, e->name);
        it->is_dir = e->is_dir;
        g_ptr_array_add(d->items, it);
        if (i < 5) g_string_append_printf(names, "%s%s", i ? ", " : "", e->name);
    }
    char *msg = g_strdup_printf("Delete %u item(s) on the server?\n%s", sel->len, names->str);
    dlg_confirm(a, "Delete", msg, "Delete", delete_cb, d);
    g_free(msg);
    g_string_free(names, TRUE);
    g_ptr_array_free(sel, TRUE);
}

static void act_rename(App *a, FilesPane *f) {
    GPtrArray *sel = selected(f);
    if (sel->len == 1) {
        RenameCtx *r = g_new0(RenameCtx, 1);
        r->f = f; r->old = g_strdup(((SdEntry *)sel->pdata[0])->name);
        dlg_text(a, "Rename", NULL, r->old, false, rename_cb, r);
    }
    g_ptr_array_free(sel, TRUE);
}

static void activate(App *a, FilesPane *f, int i) {
    if (!f->path) return;
    SdEntry *e = f->ent[i];
    char *target = join(f->path, e->name);
    if (e->is_dir || e->is_link) navigate_to(f, target);   /* a symlink to a file just fails to list and reports it */
    else {
        GPtrArray *one = g_ptr_array_new();
        g_ptr_array_add(one, e);
        const char *dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        start_download(f, one, dl ? dl : g_get_home_dir());
        g_ptr_array_free(one, TRUE);
    }
    g_free(target);
    (void)a;
}

/* ---- drawing ---------------------------------------------------------------------------------------------------------------------- */

enum { M_DOWNLOAD = 1, M_UPLOAD, M_RENAME, M_DELETE, M_MKDIR, M_REFRESH, M_COPYPATH };
#define MENU_TAG 900

void files_draw(App *a, FilesPane *f, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = ui_colors(u);
    f->drawn_at = a->now;
    int tag;
    int chosen = ui_menu_take(u, &tag);
    if (chosen >= 0 && tag == MENU_TAG) {
        switch (chosen) {
        case M_DOWNLOAD: act_download(a, f); break;
        case M_UPLOAD: dlg_pick_file(a, "Upload files", false, true, g_get_home_dir(), upload_cb, f); break;
        case M_RENAME: act_rename(a, f); break;
        case M_DELETE: act_delete(a, f); break;
        case M_MKDIR: dlg_text(a, "New folder", NULL, "", false, mkdir_cb, f); break;
        case M_REFRESH: if (f->path) navigate_to(f, f->path); break;
        case M_COPYPATH: {
            GPtrArray *sel = selected(f);
            if (sel->len == 1 && f->path) { char *p = join(f->path, ((SdEntry *)sel->pdata[0])->name); SDL_SetClipboardText(p); g_free(p); }
            g_ptr_array_free(sel, TRUE);
            break;
        }
        }
    }
    ui_rect(u, r, c->bg1);
    float pad = P(8), y = r.y + pad, bh = P(30);
    /* toolbar */
    if (ui_icon_button(u, R(r.x + pad, y, bh, bh), IC_UP, "Parent folder", 0) && f->path && !g_str_equal(f->path, "/")) {
        char *up = g_path_get_dirname(f->path); navigate_to(f, up); g_free(up);
    }
    if (ui_icon_button(u, R(r.x + pad + bh + P(2), y, bh, bh), IC_HOME, "Home", 0)) navigate_to(f, NULL);
    if (ui_icon_button(u, R(r.x + pad + 2 * (bh + P(2)), y, bh, bh), IC_REFRESH, "Refresh", 0) && f->path) navigate_to(f, f->path);
    float right = r.x + r.w - pad - 3 * (bh + P(2));
    float px = r.x + pad + 3 * (bh + P(2)) + P(4);
    if (ui_input(u, R(px, y, right - px, bh), &f->pathin, NULL, false)) navigate_to(f, f->pathin.s[0] ? f->pathin.s : "/");
    if (ui_icon_button(u, R(right + P(2), y, bh, bh), IC_NEWFOLDER, "New folder", 0)) dlg_text(a, "New folder", NULL, "", false, mkdir_cb, f);
    if (ui_icon_button(u, R(right + P(2) + bh + P(2), y, bh, bh), IC_UPLOAD, "Upload files", 0)) dlg_pick_file(a, "Upload files", false, true, g_get_home_dir(), upload_cb, f);
    if (ui_icon_button(u, R(right + P(2) + 2 * (bh + P(2)), y, bh, bh), IC_DOWNLOAD, "Download selected", 0)) act_download(a, f);
    y += bh + P(6);
    if (!f->ftp) {
        bool v = f->follow;
        if (ui_checkbox(u, R(r.x + pad, y, r.w - 2 * pad, P(24)), "Follow terminal path", &v)) { f->follow = v; if (v) f->next_poll = 0; app_settings_save(a); }
        y += P(28);
    }
    /* header */
    float nameW = r.w - 2 * pad - P(86) - P(130);
    Rect hdr = R(r.x + pad, y, r.w - 2 * pad, P(24));
    ui_rect(u, hdr, c->bg2);
    const char *hn[3] = {"Name", "Size", "Modified"};
    float hx[3] = {hdr.x + P(8), hdr.x + nameW, hdr.x + nameW + P(86)};
    float hw[3] = {nameW - P(8), P(80), P(124)};
    for (int i = 0; i < 3; i++) {
        Rect hr = R(i == 0 ? hdr.x : hx[i] - P(6), hdr.y, i == 0 ? nameW : hw[i] + P(6), hdr.h);
        if (ui_hover(u, hr) && ui_mouse_pressed(u, 1, hr)) {
            if (f->sort_col == i) f->sort_desc = !f->sort_desc; else { f->sort_col = i; f->sort_desc = false; }
            GPtrArray *keep = selected(f);   /* the selection follows the entries, not the rows */
            sort_entries(f);
            for (int k = 0; k < f->n; k++) f->sel[k] = g_ptr_array_find(keep, f->ent[k], NULL);
            g_ptr_array_free(keep, TRUE);
        }
        char lab[40];
        snprintf(lab, sizeof lab, "%s%s", hn[i], f->sort_col == i ? (f->sort_desc ? "  \xe2\x96\xbe" : "  \xe2\x96\xb4") : "");
        ui_text_fit(u, R(hx[i], hdr.y, hw[i], hdr.h), lab, c->muted, i == 1 ? 2 : 0);
    }
    y += hdr.h;
    /* list */
    float sh = P(26);
    Rect list = R(r.x + pad, y, r.w - 2 * pad, r.y + r.h - y - P(28));
    float lh = P(26);
    bool in_list = ui_hover(u, list);
    ui_scroll_begin(u, list, f->n * lh, &f->scroll);
    int clicked_row = -1;
    for (int i = 0; i < f->n; i++) {
        Rect row = R(list.x, list.y + i * lh - f->scroll, list.w, lh);
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        SdEntry *e = f->ent[i];
        bool hov = in_list && ui_hover(u, row);
        uint32_t tx = f->sel[i] ? c->bg1 : c->ink, tm = f->sel[i] ? c->bg1 : c->muted;
        if (f->sel[i]) ui_rrect(u, row, c->accent, P(8));
        else if (hov) ui_rrect(u, row, c->hover, P(8));
        ui_icon(u, e->is_dir ? IC_FOLDER : e->is_link ? IC_LINK : IC_FILE, R(row.x + P(6), row.y + (lh - P(16)) / 2, P(16), P(16)), f->sel[i] ? c->bg1 : (e->is_dir ? c->accent : c->ink2));
        ui_text_fit(u, R(row.x + P(28), row.y, nameW - P(28), lh), e->name, tx, 0);
        if (!e->is_dir) { char *sz = fmt_size(e->size); ui_text_fit(u, R(hx[1], row.y, hw[1], lh), sz, tm, 2); g_free(sz); }
        if (e->modified) ui_text_fit(u, R(hx[2], row.y, hw[2], lh), e->modified, tm, 0);
        if (hov && (ui_mouse_pressed(u, 1, row) || ui_mouse_pressed(u, 3, row))) {
            bool right_click = ui_mouse_pressed(u, 3, row);
            int mods = SDL_GetModState();
            if (right_click && f->sel[i]) { /* keep the multi-selection for the menu */ }
            else if (mods & KMOD_CTRL) f->sel[i] = !f->sel[i];
            else if ((mods & KMOD_SHIFT) && f->last_click >= 0) {
                int lo = f->last_click < i ? f->last_click : i, hi = f->last_click < i ? i : f->last_click;
                for (int k = lo; k <= hi; k++) f->sel[k] = true;
            } else { memset(f->sel, 0, (size_t)f->n * sizeof(bool)); f->sel[i] = true; }
            f->last_click = i;
            clicked_row = right_click ? -2 - i : i;
            if (!right_click && ui_clicks(u) >= 2) { activate(a, f, i); break; }
        }
    }
    ui_scroll_end(u);
    if (in_list && clicked_row <= -2) {
        GPtrArray *sel = selected(f);
        bool one = sel->len == 1, some = sel->len > 0;
        g_ptr_array_free(sel, TRUE);
        UiMenuItem it[] = {
            {"Download\xe2\x80\xa6", NULL, M_DOWNLOAD, some ? 0 : MI_DISABLED, NULL, 0},
            {"Upload\xe2\x80\xa6", NULL, M_UPLOAD, 0, NULL, 0},
            {NULL, NULL, 0, MI_SEP, NULL, 0},
            {"Rename\xe2\x80\xa6", NULL, M_RENAME, one ? 0 : MI_DISABLED, NULL, 0},
            {"Delete\xe2\x80\xa6", NULL, M_DELETE, some ? 0 : MI_DISABLED, NULL, 0},
            {"New folder\xe2\x80\xa6", NULL, M_MKDIR, 0, NULL, 0},
            {NULL, NULL, 0, MI_SEP, NULL, 0},
            {"Copy path", NULL, M_COPYPATH, one ? 0 : MI_DISABLED, NULL, 0},
            {"Refresh", NULL, M_REFRESH, 0, NULL, 0},
        };
        ui_menu_open(u, ui_mx(u), ui_my(u), it, (int)(sizeof it / sizeof *it), MENU_TAG);
    } else if (in_list && ui_mouse_pressed(u, 3, list) && clicked_row == -1) {
        UiMenuItem it[] = {
            {"Upload\xe2\x80\xa6", NULL, M_UPLOAD, 0, NULL, 0}, {"New folder\xe2\x80\xa6", NULL, M_MKDIR, 0, NULL, 0}, {"Refresh", NULL, M_REFRESH, 0, NULL, 0},
        };
        ui_menu_open(u, ui_mx(u), ui_my(u), it, 3, MENU_TAG);
    }
    /* keyboard: Delete, F2 when nothing else owns the keyboard is handled by the app; here only the pane's own shortcuts */
    (void)sh;
    ui_text_fit(u, R(r.x + pad, r.y + r.h - P(26), r.w - 2 * pad, P(24)), f->status, c->muted, 0);
}
