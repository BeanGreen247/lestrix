/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <math.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "../bench.h"
#include "fleetwm.h"
#include <signal.h>
#include "app.h"
#include "appicon.h"
#include "workpool.h"
#include "gl.h"
#include "jobs.h"

#define P(v) S(a->ui, (v))

App *g_app;
static Uint32 EV_WAKE;
static atomic_int wake_pending;

void app_wake(void) {
    int exp = 0;
    if (atomic_compare_exchange_strong(&wake_pending, &exp, 1)) {
        SDL_Event e;
        SDL_zero(e);
        e.type = EV_WAKE;
        SDL_PushEvent(&e);
    }
}

void app_redraw(App *a) { a->dirty = true; (void)a; }

static void tmark(const char *what) {
    static int on = -1;
    static double t0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    if (on < 0) { on = getenv("LESTRIX_TIMING") != NULL; t0 = now; }
    if (on) fprintf(stderr, "%7.1f ms  %s\n", (now - t0) * 1000.0, what);
}

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }

static char *relative_age(double ts) {
    double secs = (double)g_get_real_time() / 1e6 - ts;
    if (secs < 90) return g_strdup("just now");
    if (secs < 5400) return g_strdup_printf("%dm ago", (int)(secs / 60));
    if (secs < 129600) return g_strdup_printf("%dh ago", (int)(secs / 3600));
    return g_strdup_printf("%dd ago", (int)(secs / 86400));
}

static const char *TAB_COLORS[][2] = {
    {"Red", "#e06c75"}, {"Orange", "#e59a5a"}, {"Yellow", "#e5c07b"}, {"Green", "#98c379"},
    {"Teal", "#4fb39a"}, {"Blue", "#61afef"}, {"Purple", "#c678dd"}, {"Pink", "#e88ab8"},
};

static long proc_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

static long proc_cpu_ticks(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    long ut = 0, st = 0;
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        char *rp = strrchr(buf, ')');
        if (rp) { if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st) != 2) ut = st = 0; }
        fclose(f);
    }
    return ut + st;
}

static int proc_threads(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    int n = 0;
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "Threads: %d", &n) == 1) break;
    if (f) fclose(f);
    return n;
}

static uint64_t proc_interrupts(void) {
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    unsigned long long n = 0;
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "intr %llu", &n) == 1) break;
    if (f) fclose(f);
    return n;
}

static void fmt_count(double v, char *out, size_t cap) {
    if (v >= 1e9) snprintf(out, cap, "%.2fG", v / 1e9);
    else if (v >= 1e6) snprintf(out, cap, "%.2fM", v / 1e6);
    else if (v >= 1e4) snprintf(out, cap, "%.1fk", v / 1e3);
    else snprintf(out, cap, "%.0f", v);
}

static char *fmt_mb(double bytes) { return bytes >= 1048576 ? g_strdup_printf("%.1f MB", bytes / 1048576) : g_strdup_printf("%.0f KB", bytes / 1024); }

static bool g_lite;

static void settings_load(App *a) {
    a->settings = g_key_file_new();
    char *dir = sd_config_dir();
    a->settings_path = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    g_key_file_load_from_file(a->settings, a->settings_path, G_KEY_FILE_NONE, NULL);
    char *theme = g_key_file_get_string(a->settings, "ui", "theme", NULL);
    a->theme = ui_theme_find(theme);
    g_free(theme);
    a->accent = g_key_file_get_string(a->settings, "ui", "accent", NULL);
    a->font_size = g_key_file_has_key(a->settings, "ui", "font_size", NULL) ? g_key_file_get_integer(a->settings, "ui", "font_size", NULL) : 11;
    if (a->font_size < 7 || a->font_size > 32) a->font_size = 11;
    a->font_family = g_key_file_get_string(a->settings, "ui", "font_family", NULL);
    a->show_ssh_config = !g_key_file_has_key(a->settings, "ui", "show_ssh_config", NULL) || g_key_file_get_boolean(a->settings, "ui", "show_ssh_config", NULL);
    a->startup_shell = g_key_file_get_boolean(a->settings, "ui", "startup_shell", NULL);
    a->follow_default = !g_key_file_has_key(a->settings, "ui", "follow_path", NULL) || g_key_file_get_boolean(a->settings, "ui", "follow_path", NULL);
    a->scrollback = g_key_file_has_key(a->settings, "ui", "scrollback", NULL) ? g_key_file_get_integer(a->settings, "ui", "scrollback", NULL) : 10000;
    a->sb_ram_mb = g_key_file_has_key(a->settings, "ui", "scrollback_ram_mb", NULL) ? (size_t)g_key_file_get_integer(a->settings, "ui", "scrollback_ram_mb", NULL) : 32;
    a->sb_disk_mb = g_key_file_has_key(a->settings, "ui", "scrollback_disk_mb", NULL) ? (size_t)g_key_file_get_integer(a->settings, "ui", "scrollback_disk_mb", NULL) : 4096;
    a->sb_spill = !g_key_file_has_key(a->settings, "ui", "scrollback_spill", NULL) || g_key_file_get_boolean(a->settings, "ui", "scrollback_spill", NULL);
    a->fast_cat = !g_key_file_has_key(a->settings, "ui", "fast_cat", NULL) || g_key_file_get_boolean(a->settings, "ui", "fast_cat", NULL);
    a->hud = !g_key_file_has_key(a->settings, "ui", "performance_overlay", NULL) || g_key_file_get_boolean(a->settings, "ui", "performance_overlay", NULL);
    a->follow_fleet = !g_key_file_has_key(a->settings, "ui", "follow_fleetwm", NULL) || g_key_file_get_boolean(a->settings, "ui", "follow_fleetwm", NULL);
    a->rounded = !g_key_file_has_key(a->settings, "ui", "rounded_corners", NULL) || g_key_file_get_boolean(a->settings, "ui", "rounded_corners", NULL);
    a->pinned = !g_key_file_has_key(a->settings, "ui", "sidebar_pinned", NULL) || g_key_file_get_boolean(a->settings, "ui", "sidebar_pinned", NULL);
    if (g_key_file_has_key(a->settings, "ui", "sidebar_shown", NULL)) a->dock_shown = g_key_file_get_boolean(a->settings, "ui", "sidebar_shown", NULL);
    if (g_key_file_has_key(a->settings, "ui", "sidebar_page", NULL)) a->side_page = g_key_file_get_integer(a->settings, "ui", "sidebar_page", NULL);
    a->panel_w = g_key_file_has_key(a->settings, "ui", "panel_width", NULL) ? g_key_file_get_integer(a->settings, "ui", "panel_width", NULL) : 300;
    if (a->panel_w < 200 || a->panel_w > 700) a->panel_w = 300;
    if (g_lite) { a->lite = true; a->hud = true; a->pinned = false; a->dock_shown = false; a->side_open = false; }
}

void app_settings_save(App *a) {
    g_key_file_set_string(a->settings, "ui", "theme", a->theme->name);
    g_key_file_set_string(a->settings, "ui", "accent", a->accent ? a->accent : "");
    g_key_file_set_integer(a->settings, "ui", "font_size", a->font_size);
    g_key_file_set_boolean(a->settings, "ui", "show_ssh_config", a->show_ssh_config);
    g_key_file_set_boolean(a->settings, "ui", "startup_shell", a->startup_shell);
    bool follow = a->follow_default;
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->kind == TAB_SSH && t->files) follow = files_follow(t->files); }
    a->follow_default = follow;
    g_key_file_set_boolean(a->settings, "ui", "follow_path", a->follow_default);
    g_key_file_set_integer(a->settings, "ui", "scrollback", a->scrollback);
    g_key_file_set_integer(a->settings, "ui", "scrollback_ram_mb", (int)a->sb_ram_mb);
    g_key_file_set_integer(a->settings, "ui", "scrollback_disk_mb", (int)a->sb_disk_mb);
    g_key_file_set_boolean(a->settings, "ui", "scrollback_spill", a->sb_spill);
    if (!a->lite) g_key_file_set_boolean(a->settings, "ui", "performance_overlay", a->hud);
    if (!a->lite) g_key_file_set_boolean(a->settings, "ui", "sidebar_pinned", a->pinned);
    g_key_file_set_boolean(a->settings, "ui", "fast_cat", a->fast_cat);
    if (!a->lite) g_key_file_set_boolean(a->settings, "ui", "sidebar_shown", a->dock_shown);
    if (!a->lite) g_key_file_set_integer(a->settings, "ui", "sidebar_page", (int)a->side_page);
    g_key_file_set_boolean(a->settings, "ui", "follow_fleetwm", a->follow_fleet);
    g_key_file_set_boolean(a->settings, "ui", "rounded_corners", a->rounded);
    g_key_file_set_integer(a->settings, "ui", "panel_width", a->panel_w);
    char *dir = g_path_get_dirname(a->settings_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(a->settings, a->settings_path, NULL);
}

void app_apply_theme(App *a, const UiTheme *t) {
    a->theme = t;
    const char *acc = a->follow_fleet && a->fleet.found && a->fleet.accent[0] ? a->fleet.accent : (a->accent && *a->accent ? a->accent : t->accent);
    ui_set_rounded(a->ui, a->rounded);
    uint32_t bg0 = ui_rgba(t->bg0, 255), bg1 = ui_rgba(t->bg1, 255), bg2 = ui_rgba(t->bg2, 255), ink = ui_rgba(t->ink, 255);
    uint32_t accent = ui_rgba(acc, 255);
    UiColors *c = &a->colors;
    c->bg0 = bg0; c->bg1 = bg1; c->bg2 = bg2; c->ink = ink; c->accent = accent;
    c->ink2 = ui_mix(bg1, ink, 0.70); c->muted = ui_mix(bg1, ink, 0.48);
    c->border = ui_mix(bg1, ink, 0.14); c->border2 = ui_mix(bg1, ink, 0.28); c->hover = ui_mix(bg1, ink, 0.06);
    unsigned r = (accent & 255), g = (accent >> 8) & 255, b = (accent >> 16) & 255;
    c->on_accent = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0 > 0.47 ? RGBA(16, 18, 20, 255) : RGBA(255, 255, 255, 255);
    c->up = ui_rgba(t->up, 255); c->down = ui_rgba(t->down, 255); c->blue = ui_rgba(t->blue, 255); c->red = ui_rgba(t->red, 255);
    c->term_bg = ui_rgba(t->term_bg, 255);
    ui_set_colors(a->ui, c);
    ui_palette(&a->pal, t, acc);
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *tb = a->tabs->pdata[i];
        if (tb->term) { tcore_lock(tb->term); vt_mark_all_dirty(tcore_vt(tb->term)); tcore_unlock(tb->term); }
    }
    app_redraw(a);
}

void app_set_accent(App *a, const char *hex) {
    g_free(a->accent);
    a->accent = g_strdup(hex ? hex : "");
    app_apply_theme(a, a->theme);
    app_settings_save(a);
}

void app_follow_fleetwm(App *a) {
    fleetwm_read(&a->fleet);
    if (!a->follow_fleet || !a->fleet.found) return;
    const char *name = fleetwm_theme_name(a->fleet.theme);
    a->rounded = a->fleet.rounded;
    app_apply_theme(a, ui_theme_find(name ? name : "fleetwm Dark"));
}

static char *pick_font_family(void) {
    const char *want[] = {"JetBrains Mono", "DejaVu Sans Mono", "Cascadia Mono", "Menlo", "Consolas", NULL};
    char *f = font_pick_family(want);
    return f ? f : g_strdup("monospace");
}

void app_apply_font(App *a) {
    Font *nf = font_open(a->font_family && *a->font_family ? a->font_family : "monospace", a->font_size * 96.0 / 72.0 * a->scale, true);
    if (!nf) return;
    if (a->term_font) font_close(a->term_font);
    a->term_font = nf;
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *tb = a->tabs->pdata[i];
        if (tb->term) { tcore_lock(tb->term); vt_mark_all_dirty(tcore_vt(tb->term)); tcore_unlock(tb->term); }
    }
    app_redraw(a);
}

Tab *app_cur_tab(App *a) { return a->cur >= 0 && a->cur < (int)a->tabs->len ? a->tabs->pdata[a->cur] : NULL; }

static int tab_index(App *a, Tab *t) { for (guint i = 0; i < a->tabs->len; i++) if (a->tabs->pdata[i] == t) return (int)i; return -1; }

static void ended_prompt(App *a, Tab *t);

static void select_tab(App *a, int i) {
    if (i < 0 || i >= (int)a->tabs->len) return;
    a->cur = i;
    Tab *t = a->tabs->pdata[i];
    if (t->state == ST_ACTIVITY || t->state == ST_BELL) t->state = ST_IDLE;
    a->strip_scroll = -1;
    app_redraw(a);
    ended_prompt(a, t);
}

static void hook_wake(void *u) { (void)u; app_wake(); }
static void hook_activity(void *u) { Tab *t = u; if (t->files) files_note_activity(t->files); if (app_cur_tab(t->app) != t && t->state == ST_IDLE) t->state = ST_ACTIVITY; }
static void hook_bell(void *u) { Tab *t = u; if (app_cur_tab(t->app) != t && (t->state == ST_IDLE || t->state == ST_ACTIVITY)) t->state = ST_BELL; }
static void hook_exited(void *u, int st) {
    (void)st; Tab *t = u;
    t->state = ST_ENDED; t->asked = false;
    if (t->app->quit_on_exit) t->app->running = false;
    else ended_prompt(t->app, t);
}
static void hook_restarted(void *u) { Tab *t = u; t->state = ST_IDLE; t->asked = false; }
static void hook_cwd(void *u, const char *p) { Tab *t = u; if (t->files) files_set_cwd(t->files, p); }
static void hook_clip_set(void *u, const char *text, bool primary) { (void)u; if (primary) SDL_SetPrimarySelectionText(text); else SDL_SetClipboardText(text); }
static void hook_clip_request(void *u, bool primary) {
    Tab *t = u;
    char *s = primary ? SDL_GetPrimarySelectionText() : SDL_GetClipboardText();
    if (s && *s && t->term) tcore_paste(t->term, s);
    SDL_free(s);
}

static void tab_free(Tab *t) {
    g_free(t->title); g_free(t->color); g_free(t->control_path);
    if (t->conn) sd_conn_free(t->conn);
    g_strfreev(t->local_argv);
    g_free(t);
}

static void tab_destroy(App *a, Tab *t) {
    int idx = tab_index(a, t);
    if (t->term) { tcore_close(t->term); tcore_free(t->term); }
    if (t->files) files_free(t->files);
    if (t->control_path) sd_xfer_remove_control_path(t->control_path);
    g_ptr_array_remove_index(a->tabs, (guint)idx);
    tab_free(t);
    if (a->cur >= (int)a->tabs->len) a->cur = (int)a->tabs->len - 1;
    else if (idx < a->cur) a->cur--;
    app_redraw(a);
}

static void close_confirmed(App *a, void *user) { Tab *t = user; if (tab_index(a, t) >= 0) tab_destroy(a, t); }

void app_close_tab(App *a, Tab *t) {
    if (t->term && tcore_running(t->term)) dlg_confirm(a, "Close session", "This session is still running. Close it?", "Close", close_confirmed, t);
    else tab_destroy(a, t);
}

/* MobaXterm-style choices when a session ends (exit, logout, dropped ssh). Asked once per ending, for the visible tab. */
static void ended_choice(App *a, int i, void *user) {
    Tab *t = user;
    if (tab_index(a, t) < 0) return;
    if (i == 0) { if (t->term) tcore_restart(t->term); }
    else if (i == 1) app_dup_tab(a, t);
    else if (i == 2) tab_destroy(a, t);
    app_redraw(a);
}

static void ended_prompt(App *a, Tab *t) {
    if (t->state != ST_ENDED || t->asked || app_cur_tab(a) != t || dlg_active(a)) return;
    t->asked = true;
    static const char *const labels[] = {"Restart session", "Duplicate in a new tab", "Close tab", "Keep the tab open"};
    char msg[200];
    snprintf(msg, sizeof msg, "\"%s\" has ended. What do you want to do?", t->title);
    dlg_choice(a, "Session ended", msg, labels, 4, ended_choice, t);
}

static Tab *tab_add(App *a, TabKind kind, const char *title) {
    Tab *t = g_new0(Tab, 1);
    t->app = a; t->kind = kind; t->title = g_strdup(title); t->color = g_strdup("");
    g_ptr_array_add(a->tabs, t);
    select_tab(a, (int)a->tabs->len - 1);
    return t;
}

static bool g_fastcat = true;

static char *exe_dir(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return NULL;
    buf[n] = 0;
    return g_path_get_dirname(buf);
}

static char *find_shipped(const char *const rel[]) {
    char *dir = exe_dir();
    char *found = NULL;
    for (int i = 0; dir && rel[i] && !found; i++) {
        char *p = g_build_filename(dir, rel[i], NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) found = g_canonicalize_filename(p, NULL); 
        g_free(p);
    }
    g_free(dir);
    return found;
}

void app_fastcat_apply(App *a) {
    static char *cat_dir;
    g_free(cat_dir); cat_dir = NULL;
    tcore_cat_dir = NULL;
    if (!a->fast_cat || !g_fastcat) return;
    const char *const cands[] = {"lxcat", "../build/lxcat", NULL};
    char *lx = find_shipped(cands);
    if (!lx) return;
    char *dir = g_build_filename(g_get_user_cache_dir(), "lestrix", "bin", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *link = g_build_filename(dir, "cat", NULL);
    char *cur = g_file_read_link(link, NULL);
    if (!cur || strcmp(cur, lx) != 0) { unlink(link); if (symlink(lx, link) != 0) { g_free(dir); dir = NULL; } }
    g_free(cur); g_free(link); g_free(lx);
    cat_dir = dir;
    tcore_cat_dir = cat_dir;
}

static TermCore *make_term(App *a, Tab *t, char *const argv[], const char *cwd, bool fastcat) {
    TermHooks h = {.wake = hook_wake, .activity = hook_activity, .bell = hook_bell, .exited = hook_exited, .restarted = hook_restarted,
                   .cwd = hook_cwd, .clip_set = hook_clip_set, .clip_request = hook_clip_request};
    int cols, rows;
    float cw = (float)a->W - (a->pinned && a->dock_shown ? P(a->panel_w) : 0) - P(34), ch = (float)a->H - P(130);
    tv_fit(a->term_font, cw, ch, &cols, &rows);
    return tcore_new(argv, cwd, cols, rows, &h, t, fastcat && g_fastcat);
}

char **app_default_shell_argv(const char *shell) {
    const char *s = shell ? shell : g_getenv("SHELL");
    if (!s || !*s) s = "/bin/sh";
    char **argv = g_new0(char *, 3);
    argv[0] = g_strdup(s);
    return argv;
}

Tab *app_open_local(App *a, const char *title, char **argv, const char *cwd) {
    Tab *t = tab_add(a, TAB_LOCAL, title);
    t->term = make_term(a, t, argv, cwd, true);
    t->local_argv = g_strdupv(argv);
    return t;
}

void app_set_tab_color(App *a, Tab *t, const char *hex) {
    g_free(t->color);
    t->color = g_strdup(hex ? hex : "");
    if (t->conn) sd_store_set_color(a->store, t->conn->id, t->color);
    app_redraw(a);
}

typedef struct { SdConn *conn; } FtpOpen;

static void ftp_open_with_password(App *a, const SdConn *c, const char *password) {
    SdXfer *x = sd_xfer_new_ftp(c, password);
    Tab *t = tab_add(a, TAB_FTP, c->name);
    t->files = files_new(a, x, false);
    t->conn = sd_conn_copy(c);
    g_free(t->color);
    t->color = g_strdup(c->color);
}

static void on_ftp_password(App *a, const char *pw, void *user) {
    FtpOpen *o = user;
    ftp_open_with_password(a, o->conn, pw);
    sd_conn_free(o->conn);
    g_free(o);
}

static Tab *open_connection_copy(App *a, const SdConn *c) {
    if (g_str_equal(c->protocol, "ftp") || g_str_equal(c->protocol, "ftps")) {
        if (*c->user) {
            FtpOpen *o = g_new0(FtpOpen, 1);
            o->conn = sd_conn_copy(c);
            char *title = g_strdup_printf("Password for %s@%s", c->user, c->host);
            dlg_text(a, title, NULL, "", true, on_ftp_password, o);
            g_free(title);
        } else ftp_open_with_password(a, c, NULL);
        return NULL;
    }
    char *cp = sd_xfer_new_control_path();
    char **argv = sd_conn_argv(c, cp);
    Tab *t = tab_add(a, TAB_SSH, c->name);
    t->term = make_term(a, t, argv, NULL, false);
    g_strfreev(argv);
    t->conn = sd_conn_copy(c);
    t->control_path = cp;
    g_free(t->color);
    t->color = g_strdup(c->color);
    t->files = files_new(a, sd_xfer_new_ssh(c, cp), a->follow_default);
    return t;
}

Tab *app_open_connection(App *a, const SdConn *conn) {
    SdConn *c = sd_conn_copy(conn);
    sd_store_touch(a->store, c->id);
    app_refresh(a);
    Tab *t = open_connection_copy(a, c);
    sd_conn_free(c);
    return t;
}

void app_dup_tab(App *a, Tab *t) {
    if (t->conn) app_open_connection(a, t->conn);
    else if (t->local_argv) app_open_local(a, t->title, t->local_argv, NULL);
}

static void move_current_tab(App *a, int delta) {
    int i = a->cur, n = (int)a->tabs->len;
    if (i < 0 || i + delta < 0 || i + delta >= n) return;
    gpointer x = a->tabs->pdata[i];
    a->tabs->pdata[i] = a->tabs->pdata[i + delta];
    a->tabs->pdata[i + delta] = x;
    a->cur = i + delta;
    a->strip_scroll = -1;
}

typedef struct { char *id, *name, *group, *color, *hay, *tip; bool live; } Row;
typedef struct { char *id, *name, *dest, *age; } Recent;

static Row *rows;
static int nrows;
static Recent recents[9];
static int nrecent;
static char meta_text[96];

static void rows_clear(void) {
    for (int i = 0; i < nrows; i++) { g_free(rows[i].id); g_free(rows[i].name); g_free(rows[i].group); g_free(rows[i].color); g_free(rows[i].hay); g_free(rows[i].tip); }
    g_free(rows);
    rows = NULL; nrows = 0;
    for (int i = 0; i < nrecent; i++) { g_free(recents[i].id); g_free(recents[i].name); g_free(recents[i].dest); g_free(recents[i].age); }
    nrecent = 0;
}

static int conn_cmp(gconstpointer x, gconstpointer y) {
    const SdConn *a = *(SdConn *const *)x, *b = *(SdConn *const *)y;
    int c = g_ascii_strcasecmp(a->group, b->group);
    return c ? c : g_ascii_strcasecmp(a->name, b->name);
}

void app_refresh(App *a) {
    sd_store_reload_ssh_config(a->store, a->show_ssh_config, NULL);
    rows_clear();
    GPtrArray *all = sd_store_all(a->store);
    g_ptr_array_sort(all, conn_cmp);
    rows = g_new0(Row, all->len + 1);
    nrows = (int)all->len;
    for (guint i = 0; i < all->len; i++) {
        SdConn *c = all->pdata[i];
        Row *r = &rows[i];
        r->id = g_strdup(c->id); r->name = g_strdup(c->name); r->group = g_strdup(c->group); r->color = g_strdup(c->color);
        char *h = g_strdup_printf("%s %s %s", c->name, c->host, c->group);
        r->hay = g_utf8_strdown(h, -1);
        g_free(h);
        r->live = sd_conn_is_live(c);
        r->tip = r->live ? g_strdup_printf("ssh %s  (from ~/.ssh/config)", c->alias) : g_strdup_printf("%s://%s:%d", c->protocol, c->host, c->port);
    }
    if (all->len) snprintf(meta_text, sizeof meta_text, "%u connection%s saved", all->len, all->len == 1 ? "" : "s");
    else snprintf(meta_text, sizeof meta_text, "No connections yet");
    g_ptr_array_free(all, TRUE);
    GPtrArray *rec = sd_store_recent(a->store, 9);
    for (guint i = 0; i < rec->len && i < 9; i++) {
        SdConn *c = rec->pdata[i];
        recents[nrecent].id = g_strdup(c->id); recents[nrecent].name = g_strdup(c->name);
        recents[nrecent].dest = sd_conn_dest(c); recents[nrecent].age = relative_age(c->last_used);
        nrecent++;
    }
    g_ptr_array_free(rec, TRUE);
    app_redraw(a);
}

typedef struct { char *id, *host; int port; int ok; } Probe;

static void *probe_work(void *arg) {
    Probe *p = arg;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res = NULL;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", p->port);
    p->ok = 0;
    if (getaddrinfo(p->host, ps, &hints, &res) == 0) {
        for (struct addrinfo *ai = res; ai && !p->ok; ai = ai->ai_next) {
            int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            fcntl(fd, F_SETFL, O_NONBLOCK);
            int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
            if (rc == 0) p->ok = 1;
            else if (errno == EINPROGRESS) {
                struct pollfd pf = {fd, POLLOUT, 0};
                if (poll(&pf, 1, 3000) > 0) { int err = 0; socklen_t l = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l); p->ok = err == 0; }
            }
            close(fd);
        }
        freeaddrinfo(res);
    }
    return NULL;
}

static void probe_done(void *arg, void *r) {
    (void)r;
    Probe *p = arg;
    if (g_app) { g_hash_table_insert(g_app->up, g_strdup(p->id), GINT_TO_POINTER(p->ok ? 1 : 2)); app_redraw(g_app); }
    g_free(p->id); g_free(p->host); g_free(p);
}

static void probe_all(App *a) {
    GPtrArray *all = sd_store_all(a->store);
    for (guint i = 0; i < all->len; i++) {
        SdConn *c = all->pdata[i];
        Probe *p = g_new0(Probe, 1);
        p->id = g_strdup(c->id); p->host = g_strdup(c->host); p->port = c->port;
        jobs_run(probe_work, probe_done, p);
    }
    g_ptr_array_free(all, TRUE);
}

enum {
    A_NEW_CONN = 100, A_NEW_LOCAL, A_IMPORT_INV, A_IMPORT_SSH, A_QUIT, A_SIDEBAR, A_FILES, A_BIGGER, A_SMALLER, A_RECHECK, A_ACCENT,
    A_FASTCAT, A_ACCENT_RESET, A_HUD, A_SSHCFG, A_STARTUP, A_SPILL, A_ABOUT, A_FLEET, A_ROUND,
    A_THEME = 200, A_SB = 300, A_SHELL = 400,
    A_T_RENAME = 600, A_T_CUSTOM, A_T_DEFAULT, A_T_DUP, A_T_CLOSE, A_T_OTHERS, A_T_COLOR = 650,
    A_R_CONNECT = 700, A_R_EDIT, A_R_DUP, A_R_DELETE, A_R_NEW
};
enum { TAG_BAR = 10, TAG_TAB = 20, TAG_ROW = 30 };

static const struct { const char *label; int lines; } SB_PRESETS[] = {
    {"Off", 0}, {"1,000 lines", 1000}, {"10,000 lines (default)", 10000}, {"100,000 lines", 100000},
    {"1,000,000 lines", 1000000}, {"Unlimited (old lines move to disk)", -1},
};

static char *shells[16];
static int nshells;
static uint32_t tab_swatch[8];
static Tab *ctx_tab;

static void find_shells(void) {
    const char *names[] = {"bash", "zsh", "fish", "sh", "dash", "ksh", "tcsh", "csh", "nu", "xonsh", "elvish", "pwsh", NULL};
    for (int i = 0; names[i] && nshells < 16; i++) { char *p = g_find_program_in_path(names[i]); if (p) shells[nshells++] = p; }
}

static void open_bar_menu(App *a, int which, float x, float y) {
    UiMenuItem it[48];
    int n = 0;
#define ADD(...) it[n++] = (UiMenuItem){__VA_ARGS__}
#define SEP() ADD(NULL, NULL, 0, MI_SEP, NULL, 0)
    static char sh_labels[16][64];
    if (which == 0) {
        ADD("New connection\xe2\x80\xa6", "Ctrl+N", A_NEW_CONN, 0, NULL, 0);
        ADD("New local shell", "Ctrl+Shift+T", A_NEW_LOCAL, 0, NULL, 0);
        if (nshells) { SEP(); ADD("New local shell as\xe2\x80\xa6", NULL, 0, MI_HEADER, NULL, 0); }
        for (int i = 0; i < nshells && n < 40; i++) { { char *bn = g_path_get_basename(shells[i]); snprintf(sh_labels[i], sizeof sh_labels[i], "   %s", bn); g_free(bn); } ADD(sh_labels[i], NULL, A_SHELL + i, 0, NULL, 0); }
        SEP();
        ADD("Import Ansible inventory\xe2\x80\xa6", NULL, A_IMPORT_INV, 0, NULL, 0);
        ADD("Copy ~/.ssh/config hosts into saved connections", NULL, A_IMPORT_SSH, 0, NULL, 0);
        SEP();
        ADD("Quit", "Ctrl+Q", A_QUIT, 0, NULL, 0);
    } else if (which == 1) {
        ADD("Toggle sidebar", "Ctrl+B", A_SIDEBAR, 0, NULL, 0);
        ADD("Sessions / Files sidebar", "Ctrl+Shift+B", A_FILES, 0, NULL, 0);
        ADD("Bigger font", "Ctrl+=", A_BIGGER, 0, NULL, 0);
        ADD("Smaller font", "Ctrl+-", A_SMALLER, 0, NULL, 0);
        ADD("Re-check hosts", "F5", A_RECHECK, 0, NULL, 0);
        SEP();
        ADD("Theme", NULL, 0, MI_HEADER, NULL, 0);
        ADD("Follow fleetwm theme", NULL, A_FLEET, MI_CHECK | (a->follow_fleet ? MI_CHECKED : 0) | (a->fleet.found ? 0 : MI_DISABLED), NULL, 0);
        ADD("Rounded corners", NULL, A_ROUND, MI_CHECK | (a->rounded ? MI_CHECKED : 0), NULL, 0);
        for (int i = 0; i < UI_THEME_COUNT && n < 44; i++) ADD(UI_THEMES[i].name, NULL, A_THEME + i, MI_CHECK | (a->theme == &UI_THEMES[i] ? MI_CHECKED : 0), NULL, 0);
        ADD("Accent color\xe2\x80\xa6", NULL, A_ACCENT, 0, NULL, 0);
        ADD("Reset accent", NULL, A_ACCENT_RESET, 0, NULL, 0);
        SEP();
        ADD("Scrollback", NULL, 0, MI_HEADER, NULL, 0);
        for (unsigned i = 0; i < sizeof SB_PRESETS / sizeof *SB_PRESETS; i++) ADD(SB_PRESETS[i].label, NULL, A_SB + (int)i, MI_CHECK | (a->scrollback == SB_PRESETS[i].lines ? MI_CHECKED : 0), NULL, 0);
        ADD("Spill old scrollback to disk", NULL, A_SPILL, MI_CHECK | (a->sb_spill ? MI_CHECKED : 0), NULL, 0);
        SEP();
        ADD("Fast cat in local tabs (lxcat)", NULL, A_FASTCAT, MI_CHECK | (a->fast_cat ? MI_CHECKED : 0), NULL, 0);
        ADD("Show performance overlay", NULL, A_HUD, MI_CHECK | (a->hud ? MI_CHECKED : 0), NULL, 0);
        ADD("Show hosts from ~/.ssh/config", NULL, A_SSHCFG, MI_CHECK | (a->show_ssh_config ? MI_CHECKED : 0), NULL, 0);
        ADD("Open a local shell on startup", NULL, A_STARTUP, MI_CHECK | (a->startup_shell ? MI_CHECKED : 0), NULL, 0);
    } else {
        ADD("About Lestrix", NULL, A_ABOUT, 0, NULL, 0);
    }
    ui_menu_open(a->ui, x, y, it, n, TAG_BAR + which);
#undef ADD
#undef SEP
}

static void open_tab_menu(App *a, Tab *t, float x, float y) {
    ctx_tab = t;
    for (int i = 0; i < 8; i++) tab_swatch[i] = ui_rgba(TAB_COLORS[i][1], 255);
    UiMenuItem it[] = {
        {"Rename\xe2\x80\xa6", NULL, A_T_RENAME, 0, NULL, 0},
        {"Tab color", NULL, 0, MI_HEADER, NULL, 0},
        {NULL, NULL, A_T_COLOR, 0, tab_swatch, 8},
        {"Custom color\xe2\x80\xa6", NULL, A_T_CUSTOM, 0, NULL, 0},
        {"Default color", NULL, A_T_DEFAULT, 0, NULL, 0},
        {NULL, NULL, 0, MI_SEP, NULL, 0},
        {"Duplicate tab", "Ctrl+Shift+D", A_T_DUP, 0, NULL, 0},
        {"Close tab", "Ctrl+Shift+W", A_T_CLOSE, 0, NULL, 0},
        {"Close other tabs", NULL, A_T_OTHERS, a->tabs->len > 1 ? 0 : MI_DISABLED, NULL, 0},
    };
    ui_menu_open(a->ui, x, y, it, (int)(sizeof it / sizeof *it), TAG_TAB);
}

static void rename_cb(App *a, const char *text, void *user) {
    Tab *t = user;
    if (tab_index(a, t) < 0) return;
    g_free(t->title);
    t->title = g_strdup(text);
}

static void custom_color_cb(App *a, const char *text, void *user) {
    Tab *t = user;
    if (tab_index(a, t) >= 0) app_set_tab_color(a, t, text);
}

static void accent_cb(App *a, const char *text, void *user) { (void)user; a->follow_fleet = false; app_set_accent(a, text); }

static void inventory_cb(App *a, char **paths, int n, void *user) {
    (void)user;
    if (n < 1) return;
    GError *err = NULL;
    GPtrArray *items = sd_parse_ansible_inventory(paths[0], &err);
    if (!items) { dlg_message(a, "Import failed", err ? err->message : "unknown error"); g_clear_error(&err); }
    else { sd_store_add_imported(a->store, items); app_refresh(a); }
}

static void apply_history(App *a) {
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->term) tcore_set_history(t->term, a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill); }
    tcore_set_history_defaults(a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
}

static void do_new_local(App *a, const char *shell) {
    char **argv = app_default_shell_argv(shell);
    char *base = g_path_get_basename(argv[0]);
    app_open_local(a, shell ? base : "local", argv, NULL);
    g_free(base);
    g_strfreev(argv);
}

static void side_select(App *a, int page) {
    if (a->pinned) {
        if (a->dock_shown && a->side_page == page) a->dock_shown = false;
        else { a->side_page = page; a->dock_shown = true; }
        app_settings_save(a);
    } else if (a->side_open && a->side_page == page) a->side_open = false;
    else { a->side_page = page; a->side_open = true; }
    ui_want_frames(a->ui, 0.3);
    app_redraw(a);
}

static void run_action(App *a, int id) {
    switch (id) {
    case A_NEW_CONN: dlg_conn_editor(a, NULL, false); break;
    case A_NEW_LOCAL: do_new_local(a, NULL); break;
    case A_IMPORT_INV: dlg_pick_file(a, "Ansible inventory (INI or YAML)", false, false, g_get_home_dir(), inventory_cb, NULL); break;
    case A_IMPORT_SSH: {
        char *path = g_build_filename(g_get_home_dir(), ".ssh", "config", NULL);
        GPtrArray *items = sd_parse_ssh_config(path, false);
        g_free(path);
        if (items) { sd_store_add_imported(a->store, items); app_refresh(a); }
        break;
    }
    case A_QUIT: a->running = false; break;
    case A_SIDEBAR: if (a->lite) break; if (a->pinned) { a->dock_shown = !a->dock_shown; app_settings_save(a); } else a->side_open = !a->side_open; ui_want_frames(a->ui, 0.3); break;
    case A_FILES: if (a->lite) break; side_select(a, a->side_page == 0 ? 1 : 0); break;
    case A_BIGGER: if (a->font_size < 32) { a->font_size++; app_apply_font(a); app_settings_save(a); } break;
    case A_SMALLER: if (a->font_size > 7) { a->font_size--; app_apply_font(a); app_settings_save(a); } break;
    case A_RECHECK: g_hash_table_remove_all(a->up); probe_all(a); break;
    case A_ACCENT: dlg_color(a, "Accent color", a->accent, accent_cb, NULL); break;
    case A_ACCENT_RESET: app_set_accent(a, ""); break;
    case A_HUD: if (a->lite) break; a->hud = !a->hud; app_settings_save(a); break;
    case A_FASTCAT: a->fast_cat = !a->fast_cat; app_fastcat_apply(a); app_settings_save(a); break;
    case A_SSHCFG: a->show_ssh_config = !a->show_ssh_config; app_refresh(a); app_settings_save(a); break;
    case A_STARTUP: a->startup_shell = !a->startup_shell; app_settings_save(a); break;
    case A_SPILL: a->sb_spill = !a->sb_spill; apply_history(a); app_settings_save(a); break;
    case A_ABOUT: dlg_about(a); break;
    case A_FLEET: a->follow_fleet = !a->follow_fleet; if (a->follow_fleet) app_follow_fleetwm(a); else app_apply_theme(a, a->theme); app_settings_save(a); break;
    case A_ROUND: a->rounded = !a->rounded; a->follow_fleet = false; app_apply_theme(a, a->theme); app_settings_save(a); break;
    default:
        if (id >= A_THEME && id < A_THEME + UI_THEME_COUNT) { a->follow_fleet = false; app_apply_theme(a, &UI_THEMES[id - A_THEME]); app_settings_save(a); }
        else if (id >= A_SB && id < A_SB + 6) { a->scrollback = SB_PRESETS[id - A_SB].lines; apply_history(a); app_settings_save(a); }
        else if (id >= A_SHELL && id < A_SHELL + nshells) do_new_local(a, shells[id - A_SHELL]);
        break;
    }
}

static void run_tab_action(App *a, int id) {
    Tab *t = ctx_tab;
    if (!t || tab_index(a, t) < 0) return;
    if (id == A_T_RENAME) dlg_text(a, "Rename tab", NULL, t->title, false, rename_cb, t);
    else if (id >= A_T_COLOR && id < A_T_COLOR + 8) app_set_tab_color(a, t, TAB_COLORS[id - A_T_COLOR][1]);
    else if (id == A_T_CUSTOM) dlg_color(a, "Tab color", t->color, custom_color_cb, t);
    else if (id == A_T_DEFAULT) app_set_tab_color(a, t, "");
    else if (id == A_T_DUP) app_dup_tab(a, t);
    else if (id == A_T_CLOSE) app_close_tab(a, t);
    else if (id == A_T_OTHERS) {
        GPtrArray *others = g_ptr_array_new();
        for (guint i = 0; i < a->tabs->len; i++) if (a->tabs->pdata[i] != t) g_ptr_array_add(others, a->tabs->pdata[i]);
        for (guint i = 0; i < others->len; i++) app_close_tab(a, others->pdata[i]);
        g_ptr_array_free(others, TRUE);
    }
}

static void delete_confirmed(App *a, void *user) { sd_store_remove(a->store, user); g_free(user); app_refresh(a); }

static void run_row_action(App *a, int id) {
    SdConn *c = a->ctx_id ? sd_store_get(a->store, a->ctx_id) : NULL;
    if (id == A_R_NEW) { dlg_conn_editor(a, NULL, false); return; }
    if (!c) return;
    if (id == A_R_CONNECT) app_open_connection(a, c);
    else if (id == A_R_EDIT) dlg_conn_editor(a, c, sd_conn_is_live(c));
    else if (id == A_R_DUP && !sd_conn_is_live(c)) {
        SdConn *cp = sd_conn_copy(c);
        char *nid = g_uuid_string_random();
        sd_conn_set(&cp->id, g_strdelimit(nid, "-", 'x'));
        g_free(nid);
        char *nm = g_strconcat(c->name, " copy", NULL);
        sd_conn_set(&cp->name, nm);
        g_free(nm);
        cp->last_used = 0;
        sd_store_upsert(a->store, cp);
        sd_conn_free(cp);
        app_refresh(a);
    } else if (id == A_R_DELETE && !sd_conn_is_live(c)) {
        char *msg = g_strdup_printf("Delete \"%s\"?", c->name);
        dlg_confirm(a, "Delete connection", msg, "Delete", delete_confirmed, g_strdup(c->id));
        g_free(msg);
    }
}

typedef struct { Rect bar, rail, panel, strip, content, hud; bool panel_vis, overlay; } Lay;
static Lay lay;
static Rect term_rect;
static bool term_dragging;

static float side_t(App *a) { return a->side_anim; }

static void compute_layout(App *a) {
    float bar_h = P(30), rail_w = a->lite ? 0 : P(30);
    float hud_h = a->hud && !a->lite ? ui_line_h(a->ui) + P(10) : 0;
    float pw = P(a->panel_w);
    lay.bar = R(0, 0, (float)a->W, bar_h);
    lay.hud = R(0, a->H - hud_h, (float)a->W, hud_h);
    float top = bar_h, bottom = a->H - hud_h;
    lay.rail = R(0, top, rail_w, bottom - top);
    lay.overlay = !a->pinned;
    float docked = (a->pinned && a->dock_shown) ? pw : 0;
    float anim = side_t(a);
    lay.panel_vis = a->lite ? false : a->pinned ? a->dock_shown : anim > 0.001;
    float px = a->pinned ? rail_w : rail_w - (1.f - anim) * pw;
    lay.panel = R(px, top, pw, bottom - top);
    float cx = rail_w + docked;
    float strip_h = a->tabs->len ? P(36) : 0;
    lay.strip = R(cx, top, a->W - cx, strip_h);
    lay.content = R(cx, top + strip_h, a->W - cx, bottom - top - strip_h);
}

static void draw_menubar(App *a) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    ui_rect(u, lay.bar, c->bg0);
    ui_rect(u, R(0, lay.bar.h - 1, lay.bar.w, 1), c->border);
    static const char *names[] = {"File", "View", "Help"};
    float x = P(8);
    for (int i = 0; i < 3; i++) {
        float w = ui_text_w(u, names[i]) + P(22);
        Rect r = R(x, P(3), w, lay.bar.h - P(6));
        bool open = ui_menu_tag(u) == TAG_BAR + i;
        bool hov = ui_hover(u, r) || open;
        if (ui_menu_tag(u) >= TAG_BAR && ui_menu_tag(u) < TAG_BAR + 3 && rect_has(r, ui_mx(u), ui_my(u)) && ui_menu_tag(u) != TAG_BAR + i) { open_bar_menu(a, i, r.x, r.y + r.h + P(2)); a->menu_hover = i; hov = true; }
        if (hov) ui_rrect(u, r, c->hover, P(5));
        ui_text(u, r.x + P(11), r.y + (r.h - ui_line_h(u)) / 2, names[i], c->ink);
        if (ui_mouse_pressed(u, 1, r)) { open_bar_menu(a, i, r.x, r.y + r.h + P(2)); a->menu_hover = i; }
        x += w;
    }
}

static void draw_rail(App *a) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    ui_rect(u, lay.rail, c->bg1);
    ui_rect(u, R(lay.rail.x + lay.rail.w - 1, lay.rail.y, 1, lay.rail.h), c->border);
    float y = lay.rail.y + P(4), w = lay.rail.w;
    if (ui_icon_button(u, R(P(3), y, w - P(7), w - P(7)), IC_PIN,
                       "Pin the sidebar: docked beside the terminal. Unpinned, it slides out over the terminal without resizing it.", a->pinned ? UB_ACTIVE : 0)) {
        a->pinned = !a->pinned;
        a->dock_shown = true;
        a->side_open = false;
        a->side_anim = 0;
        app_settings_save(a);
        for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->term) { tcore_lock(t->term); vt_mark_all_dirty(tcore_vt(t->term)); tcore_unlock(t->term); } }
    }
    y += w + P(4);
    static const char *names[2] = {"SESSIONS", "FILES"};
    for (int i = 0; i < 2; i++) {
        float len = ui_text_vertical_len(u, names[i]) + P(28);
        Rect r = R(0, y, w - 1, len);
        bool shown = a->pinned ? a->dock_shown : a->side_open;
        bool active = shown && a->side_page == i;
        bool hov = ui_hover(u, r);
        if (active || hov) ui_rect(u, r, c->hover);
        if (active) ui_rect(u, R(0, y, P(2), len), c->accent);
        ui_text_vertical(u, r, names[i], active ? c->accent : hov ? c->ink : c->muted);
        if (hov) ui_set_cursor(u, 2);
        if (ui_mouse_pressed(u, 1, r)) side_select(a, i);
        y += len;
    }
}

static void draw_sessions(App *a, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    float pad = P(10), y = r.y + pad;
    float addw = P(34);
    ui_input(u, R(r.x + pad, y, r.w - 2 * pad - addw - P(6), P(32)), &a->search, "Search connections", false);
    if (ui_icon_button(u, R(r.x + r.w - pad - addw, y, addw, P(32)), IC_PLUS, "New connection (Ctrl+N)", 0)) dlg_conn_editor(a, NULL, false);
    y += P(42);
    Rect list = R(r.x, y, r.w, r.y + r.h - y);
    char needle[128];
    char *lowered = g_utf8_strdown(a->search.s, -1);
    snprintf(needle, sizeof needle, "%s", lowered);
    g_free(lowered);
    float rowh = P(30), headh = P(26);
    float ch = 0;
    const char *prev = NULL;
    for (int i = 0; i < nrows; i++) {
        if (needle[0] && !strstr(rows[i].hay, needle)) continue;
        if (!prev || !g_str_equal(prev, rows[i].group)) { ch += headh; prev = rows[i].group; }
        ch += rowh;
    }
    ui_scroll_begin(u, list, ch + P(10), &a->list_scroll);
    float yy = list.y - a->list_scroll;
    prev = NULL;
    for (int i = 0; i < nrows; i++) {
        Row *row = &rows[i];
        if (needle[0] && !strstr(row->hay, needle)) continue;
        if (!prev || !g_str_equal(prev, row->group)) {
            char *up = g_utf8_strup(row->group, -1);
            if (yy + headh > list.y && yy < list.y + list.h) ui_text(u, r.x + pad + P(4), yy + P(9), up, c->muted);
            g_free(up);
            yy += headh;
            prev = row->group;
        }
        Rect rr = R(r.x + P(4), yy, r.w - P(8), rowh);
        if (rr.y + rr.h > list.y && rr.y < list.y + list.h) {
            bool hov = ui_hover(u, rr) && rect_has(list, ui_mx(u), ui_my(u));
            bool sel = a->ctx_id && g_str_equal(a->ctx_id, row->id);
            if (sel) ui_rrect(u, rr, c->accent, P(8));
            else if (hov) ui_rrect(u, rr, c->hover, P(8));
            int st = GPOINTER_TO_INT(g_hash_table_lookup(a->up, row->id));
            uint32_t dot = st == 1 ? c->up : st == 2 ? c->down : c->border2;
            ui_rrect(u, R(rr.x + P(12), rr.y + (rowh - P(8)) / 2, P(8), P(8)), dot, P(4));
            uint32_t tc = sel ? c->bg1 : row->color[0] ? ui_rgba(row->color, 255) : c->ink;
            ui_text_fit(u, R(rr.x + P(30), rr.y, rr.w - P(38), rowh), row->name, tc, 0);
            if (hov) {
                ui_tip(u, rr, row->tip);
                ui_set_cursor(u, 2);
                if (ui_mouse_pressed(u, 1, rr)) {
                    g_free(a->ctx_id); a->ctx_id = g_strdup(row->id);
                    if (ui_clicks(u) >= 2) { SdConn *cn = sd_store_get(a->store, row->id); if (cn) { app_open_connection(a, cn); ui_scroll_end(u); return; } }
                }
                if (ui_mouse_pressed(u, 3, rr)) {
                    g_free(a->ctx_id); a->ctx_id = g_strdup(row->id);
                    SdConn *cn = sd_store_get(a->store, row->id);
                    bool live = cn && sd_conn_is_live(cn);
                    UiMenuItem it[] = {
                        {"Connect", NULL, A_R_CONNECT, 0, NULL, 0},
                        {live ? "Save a copy to edit\xe2\x80\xa6" : "Edit\xe2\x80\xa6", NULL, A_R_EDIT, 0, NULL, 0},
                        {"Duplicate", NULL, A_R_DUP, live ? MI_DISABLED : 0, NULL, 0},
                        {"Delete", NULL, A_R_DELETE, live ? MI_DISABLED : 0, NULL, 0},
                        {NULL, NULL, 0, MI_SEP, NULL, 0},
                        {"New connection\xe2\x80\xa6", NULL, A_R_NEW, 0, NULL, 0},
                    };
                    ui_menu_open(u, ui_mx(u), ui_my(u), it, 6, TAG_ROW);
                }
            }
        }
        yy += rowh;
    }
    if (!nrows) ui_text(u, r.x + pad, list.y + P(10), "No connections yet. Press + to add one.", c->muted);
    ui_scroll_end(u);
}

static void draw_files_page(App *a, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    Tab *t = app_cur_tab(a);
    if (t && t->kind == TAB_SSH && t->files) {
        char *up = g_ascii_strup(t->title, -1);
        ui_text(u, r.x + P(14), r.y + P(12), up, c->muted);
        g_free(up);
        files_draw(a, t->files, R(r.x, r.y + P(34), r.w, r.h - P(34)));
    } else {
        ui_text(u, r.x + P(14), r.y + P(12), "NO SSH SESSION", c->muted);
        ui_text(u, r.x + P(14), r.y + P(44), "The file browser appears", c->muted);
        ui_text(u, r.x + P(14), r.y + P(44) + ui_line_h(u) + P(2), "when an SSH tab is open.", c->muted);
    }
}

static void draw_panel(App *a) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    Rect p = lay.panel;
    if (lay.overlay) {
        ui_rrect(u, R(p.x + p.w, p.y, P(10), p.h), 0x30000000u, 0);
        ui_rect(u, R(p.x + p.w - 1, p.y, 1, p.h), c->border2);
    } else ui_rect(u, R(p.x + p.w - 1, p.y, 1, p.h), c->border);
    ui_rect(u, R(p.x, p.y, p.w - 1, p.h), c->bg1);
    ui_clip(u, p);
    if (a->side_page == 0) draw_sessions(a, p); else draw_files_page(a, p);
    ui_unclip(u);
    if (!lay.overlay) {
        Rect grip = R(p.x + p.w - P(3), p.y, P(6), p.h);
        static bool dragging;
        if (ui_hover(u, grip)) ui_set_cursor(u, 2);
        if (ui_mouse_pressed(u, 1, grip)) dragging = true;
        if (dragging) {
            if (ui_mouse_down(u, 1)) { int nw = (int)((ui_mx(u) - lay.rail.w) / a->scale + 0.5); a->panel_w = nw < 200 ? 200 : nw > 700 ? 700 : nw; app_redraw(a); }
            else { dragging = false; app_settings_save(a); }
        }
    }
}

static UiIcon kind_icon(TabKind k) { return k == TAB_LOCAL ? IC_TERMINAL : k == TAB_SSH ? IC_SERVER : IC_FOLDER; }

static void draw_strip(App *a) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    Rect s = lay.strip;
    if (s.h <= 0) return;
    ui_rect(u, s, c->bg0);
    ui_rect(u, R(s.x, s.y + s.h - 1, s.w, 1), c->border);
    float btn = P(30), acts_w = 4 * btn + P(10);
    Tab *ct = app_cur_tab(a);
    bool has_term = ct && ct->term;
    float x = s.x + s.w - acts_w + P(4);
    if (ui_icon_button(u, R(x, s.y + P(3), btn, btn), IC_COPY, "Copy selection (Ctrl+Shift+C)", has_term ? 0 : UB_DISABLED) && has_term) tcore_copy(ct->term);
    if (ui_icon_button(u, R(x + btn, s.y + P(3), btn, btn), IC_COPYALL, "Copy entire scrollback", has_term ? 0 : UB_DISABLED) && has_term) tcore_copy_all(ct->term);
    if (ui_icon_button(u, R(x + 2 * btn, s.y + P(3), btn, btn), IC_PASTE, "Paste (Ctrl+Shift+V)", has_term ? 0 : UB_DISABLED) && has_term) tcore_paste_request(ct->term);
    if (ui_icon_button(u, R(x + 3 * btn, s.y + P(3), btn, btn), IC_DUPLICATE, "Duplicate tab (Ctrl+Shift+D)", ct ? 0 : UB_DISABLED) && ct) app_dup_tab(a, ct);
    Rect tabs_area = R(s.x, s.y, s.w - acts_w, s.h);
    float total = 0;
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        float w = ui_text_w(u, t->title) + P(16 + 8 + 8 + 14 + 22);
        t->w = w < P(110) ? P(110) : w > P(230) ? P(230) : w;
        total += t->w;
    }
    bool overflow = total > tabs_area.w;
    float chev = overflow ? P(24) : 0;
    Rect view = R(tabs_area.x + chev, s.y, tabs_area.w - 2 * chev, s.h);
    float maxs = total > view.w ? total - view.w : 0;
    if (a->strip_scroll < 0) {
        float tx = 0;
        for (int i = 0; i < a->cur && i < (int)a->tabs->len; i++) tx += ((Tab *)a->tabs->pdata[i])->w;
        float tw = a->cur < (int)a->tabs->len ? ((Tab *)a->tabs->pdata[a->cur])->w : 0;
        float cur = a->strip_scroll == -1 ? 0 : a->strip_scroll;
        (void)cur;
        a->strip_scroll = tx + tw > view.w ? tx + tw - view.w : 0;
    }
    float wheel = ui_take_wheel(u, tabs_area);
    if (wheel != 0) a->strip_scroll -= wheel * P(48);
    if (a->strip_scroll > maxs) a->strip_scroll = maxs;
    if (a->strip_scroll < 0) a->strip_scroll = 0;
    if (overflow) {
        if (ui_icon_button(u, R(tabs_area.x, s.y + P(5), chev, s.h - P(10)), IC_CHEV_L, NULL, a->strip_scroll > 0 ? 0 : UB_DISABLED)) a->strip_scroll -= P(160);
        if (ui_icon_button(u, R(tabs_area.x + tabs_area.w - chev, s.y + P(5), chev, s.h - P(10)), IC_CHEV_R, NULL, a->strip_scroll < maxs ? 0 : UB_DISABLED)) a->strip_scroll += P(160);
    }
    ui_clip(u, view);
    float tx = view.x - a->strip_scroll;
    int close_idx = -1;
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        t->x = tx;
        Rect r = R(tx, s.y, t->w, s.h);
        tx += t->w;
        if (r.x + r.w < view.x || r.x > view.x + view.w) continue;
        bool active = (int)i == a->cur;
        bool hov = ui_hover(u, r) && rect_has(view, ui_mx(u), ui_my(u));
        if (active) { ui_rect(u, r, a->pal.bg); ui_rect(u, R(r.x, r.y + r.h - P(2), r.w, P(2)), c->accent); }
        else if (hov) ui_rect(u, r, c->hover);
        uint32_t tint = t->color[0] ? ui_rgba(t->color, 255) : (active ? c->ink : c->ink2);
        ui_icon(u, kind_icon(t->kind), R(r.x + P(10), r.y + (r.h - P(16)) / 2, P(16), P(16)), t->color[0] ? tint : c->muted);
        float lx = r.x + P(10 + 16 + 8);
        float dotw = t->state == ST_IDLE ? 0 : P(14);
        ui_text_fit(u, R(lx, r.y, r.w - P(10 + 16 + 8) - P(30) - dotw, r.h), t->title, tint, 0);
        if (t->state != ST_IDLE) {
            uint32_t dc = t->state == ST_ACTIVITY ? c->blue : t->state == ST_BELL ? c->red : c->muted;
            ui_rrect(u, R(r.x + r.w - P(30) - P(10), r.y + (r.h - P(8)) / 2, P(8), P(8)), dc, P(4));
        }
        Rect cx = R(r.x + r.w - P(26), r.y + (r.h - P(20)) / 2, P(20), P(20));
        bool chov = ui_hover(u, cx);
        if (chov) ui_rrect(u, cx, c->hover, P(4));
        ui_icon(u, IC_CLOSE, rect_inset(cx, P(3)), chov ? c->red : (active || hov ? c->ink2 : c->muted));
        if (hov) ui_set_cursor(u, 2);
        if (chov && ui_mouse_pressed(u, 1, cx)) close_idx = (int)i;
        else if (hov && ui_mouse_pressed(u, 1, r)) {
            select_tab(a, (int)i);
            a->strip_scroll = a->strip_scroll < 0 ? 0 : a->strip_scroll;
            a->drag_tab = (int)i; a->drag_dx = ui_mx(u) - r.x; a->drag_moved = false;
            if (ui_clicks(u) >= 2) dlg_text(a, "Rename tab", NULL, t->title, false, rename_cb, t);
        } else if (hov && ui_mouse_pressed(u, 2, r)) close_idx = (int)i;
        else if (hov && ui_mouse_pressed(u, 3, r)) { select_tab(a, (int)i); open_tab_menu(a, t, ui_mx(u), ui_my(u)); }
    }
    ui_unclip(u);
    if (a->drag_tab >= 0) {
        if (ui_mouse_down(u, 1) && a->drag_tab < (int)a->tabs->len) {
            float mx = ui_mx(u);
            Tab *dt = a->tabs->pdata[a->drag_tab];
            if (fabsf(mx - (dt->x + a->drag_dx)) > P(6)) a->drag_moved = true;
            if (a->drag_moved) {
                int i = a->drag_tab;
                if (i > 0) { Tab *l = a->tabs->pdata[i - 1]; if (mx < l->x + l->w / 2 + P(0)) { gpointer x2 = a->tabs->pdata[i]; a->tabs->pdata[i] = a->tabs->pdata[i - 1]; a->tabs->pdata[i - 1] = x2; a->drag_tab--; a->cur = a->drag_tab; } }
                if (i < (int)a->tabs->len - 1) { Tab *rr = a->tabs->pdata[i + 1]; if (mx > rr->x + rr->w / 2) { gpointer x2 = a->tabs->pdata[i]; a->tabs->pdata[i] = a->tabs->pdata[i + 1]; a->tabs->pdata[i + 1] = x2; a->drag_tab++; a->cur = a->drag_tab; } }
            }
        } else a->drag_tab = -1;
    }
    if (close_idx >= 0) app_close_tab(a, a->tabs->pdata[close_idx]);
}

static void draw_welcome(App *a, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    ui_rect(u, r, a->pal.bg);
    float w = fminf(P(620), r.w - P(60)), x = r.x + (r.w - w) / 2, y = r.y + fmaxf(P(40), (r.h - P(120) - (nrecent + 1) * P(40)) / 3);
    ui_text_font(u, a->big_font, FS_BOLD, x, y, "Lestrix", c->ink);
    y += font_cell_h(a->big_font) + P(2);
    ui_text(u, x, y, meta_text, c->muted);
    y += P(36);
    ui_text(u, x, y, "RECENT", c->muted);
    y += P(26);
    for (int i = 0; i <= nrecent; i++) {
        bool local = i == nrecent;
        Rect row = R(x, y, w, P(38));
        bool hov = ui_hover(u, row);
        if (hov) ui_rect(u, row, c->hover);
        ui_rect(u, R(row.x, row.y + row.h - 1, row.w, 1), c->border);
        char key[16];
        snprintf(key, sizeof key, "%d", local ? 0 : i + 1);
        Rect kb = R(row.x + P(4), row.y + (row.h - P(22)) / 2, P(24), P(22));
        ui_outline(u, kb, c->border2, P(4), 1);
        ui_text_fit(u, kb, key, c->muted, 1);
        ui_text_font(u, a->ui_font, FS_BOLD, row.x + P(42), row.y + (row.h - ui_line_h(u)) / 2, local ? "Local shell" : recents[i].name, c->ink);
        ui_text_fit(u, R(row.x + P(200), row.y, w - P(200) - P(100), row.h), local ? "this machine" : recents[i].dest, c->ink2, 0);
        if (!local) ui_text_fit(u, R(row.x + w - P(90), row.y, P(86), row.h), recents[i].age, c->muted, 2);
        if (hov) ui_set_cursor(u, 2);
        if (ui_mouse_pressed(u, 1, row)) {
            if (local) do_new_local(a, NULL);
            else { SdConn *cn = sd_store_get(a->store, recents[i].id); if (cn) { app_open_connection(a, cn); return; } }
        }
        y += P(38);
    }
    if (!nrecent) ui_text(u, x, y + P(14), "Double-click a host on the left, or press + to add one.", c->muted);
}

static void draw_content(App *a) {
    Ui *u = a->ui;
    Rect r = lay.content;
    Tab *t = app_cur_tab(a);
    if (!t) { draw_welcome(a, R(r.x, r.y, r.w, r.h)); term_rect = R(0, 0, 0, 0); return; }
    if (!t->term) {
        term_rect = R(0, 0, 0, 0);
        if (t->files) files_draw(a, t->files, r);
        return;
    }
    term_rect = r;
    int cols, rows;
    tv_fit(a->term_font, r.w, r.h, &cols, &rows);
    tcore_resize(t->term, cols, rows);
    tcore_set_focus(t->term, !dlg_active(a) && SDL_GetWindowFlags(a->win) & SDL_WINDOW_INPUT_FOCUS);
    r_flush(a->r);
    tv_draw(t->term, a->r, a->term_font, &a->pal, r.x, r.y, r.w, r.h, a->blink_on);
    if (rect_has(r, ui_mx(u), ui_my(u)) && !ui_blocked(u)) ui_set_cursor(u, 1);
}

static double g_max_fps = 60, g_io_fps = 24;
static int g_parse_threads = -1, g_compress_threads = -1, g_render_threads = -1;
static double frame_gap(bool flooding) {
    double f = g_max_fps, io = g_io_fps;
    if (flooding && io > 0 && (f <= 0 || io < f)) f = io;
    return f > 0 ? 1.0 / f : 0.0;
}
static void hud_tick(App *a);

static void draw_hud(App *a) {
    if (!a->hud) return;
    Ui *u = a->ui;
    ui_rect(u, lay.hud, a->colors.bg0);
    ui_rect(u, R(0, lay.hud.y, lay.hud.w, 1), a->colors.border);
    static const char *const tips[11] = {
        "Frames per second actually drawn. 0 when nothing changes (no wasted redraws); capped at 60 normally and at 24 while a big dump pours in, so reading output gets the CPU (see --max-fps, --io-fps).",
        "Size of the current terminal in columns x rows.",
        "Parse rate: program output the terminal engine consumed, in GB per second, over the last second.",
        "CPU used by Lestrix, all threads together. 100% = one full core; 400% = four cores busy.",
        "Hardware interrupts handled by the whole machine (from /proc/stat). Click to flip between per second and running total.",
        "Resident memory held by Lestrix right now.",
        "Threads in the Lestrix process (UI, one parser per tab, compressors, font loading) / logical CPU cores the system offers.",
        "Raw data reads: read() calls per second on the program's pty, and the average bytes per call. Bigger is better: fewer system calls per MB.",
        NULL,
        "Scrollback of the current tab: lines kept, memory they use (compressed), and how much was moved to the disk spill file.",
        "Last finished command: run time, then its output throughput in GB per second (output bytes / run time). Not measured for full-screen programs."};
    static const char *const widest[11] = {
        "fps 999", "no terminal", "parse 99.999 GB/s", "cpu 800% (99)", "irq 9999.9M/s", "memory 9999.9 MB", "99 thr/99c", "reads 9999.9k/s @9999K",
        "cache rows 100% glyphs 100% recyc 9999", "scrlbck 9999999 ln, 9999.9 MB mem, 9999.9 MB disk", "cmd 99.99 s, 99.999 GB/s"};
    static const int priority[11] = {0, 3, 2, 10, 5, 1, 8, 7, 6, 4, 9};
    float x0 = P(12), ty = lay.hud.y + (lay.hud.h - ui_line_h(u)) / 2, sepw = ui_text_mono_w(u, " \xc2\xb7 "), slot_w[11];
    bool show[11] = {false};
    float room = lay.hud.w - 2 * x0, used = 0;
    for (int i = 0; i < 11; i++) slot_w[i] = ui_text_mono_w(u, widest[i]);
    for (int k = 0; k < 11; k++) {
        int i = priority[k];
        float need = slot_w[i] + (used > 0 ? sepw : 0);
        if (used + need > room) continue;
        show[i] = true; used += need;
    }
    float tx = x0;
    bool first = true;
    for (int i = 0; i < 11; i++) {
        if (!show[i]) continue;
        if (!first) ui_text_mono(u, tx, ty, " \xc2\xb7 ", a->colors.border);
        if (!first) tx += sepw;
        first = false;
        const char *txt = a->hud_seg[i][0] ? a->hud_seg[i] : i == 9 ? "scrlbck --" : i == 10 ? "cmd --" : "";
        ui_text_mono(u, tx, ty, txt, a->colors.muted);
        Rect r = R(tx, lay.hud.y, slot_w[i], lay.hud.h);
        if (ui_hover(u, r)) ui_tip(u, r, i == 8 ? a->hud_cache_tip : i == 2 ? a->hud_parse_tip : tips[i]);
        if (i == 4 && ui_mouse_pressed(u, 1, r)) { a->hud_irq_total = !a->hud_irq_total; hud_tick(a); }
        tx += slot_w[i];
    }
    if (ui_mouse_pressed(u, 1, lay.hud)) { a->hud_irq_total = !a->hud_irq_total; hud_tick(a); }
}

static void hud_tick(App *a) {
    char before[sizeof a->hud_seg];
    memcpy(before, a->hud_seg, sizeof before);
    double now = a->now;
    long ticks = proc_cpu_ticks();
    Tab *t = app_cur_tab(a);
    uint64_t fed = t && t->term ? tcore_bytes_fed(t->term) : 0;
    double dt = a->hud_prev_time ? now - a->hud_prev_time : 0;
    double cpu = dt > 0 ? (double)(ticks - a->hud_prev_ticks) / (double)sysconf(_SC_CLK_TCK) / dt * 100 : 0;
    double rate = dt > 0 && fed >= a->hud_prev_bytes ? (double)(fed - a->hud_prev_bytes) / dt / 1048576 : 0;
    double fps = dt > 0 ? (double)(a->frames - a->hud_prev_frames) / dt : 0;
    uint64_t reads = t && t->term ? tcore_reads(t->term) : 0;
    double rd_rate = dt > 0 && reads >= a->hud_prev_reads ? (double)(reads - a->hud_prev_reads) / dt : 0;
    double rd_avg = reads > a->hud_prev_reads ? (double)(fed - a->hud_prev_bytes) / (double)(reads - a->hud_prev_reads) : 0;
    uint64_t irq = proc_interrupts();
    double irq_rate = dt > 0 && irq >= a->hud_prev_irq ? (double)(irq - a->hud_prev_irq) / dt : 0;
    a->hud_prev_time = now; a->hud_prev_ticks = ticks; a->hud_prev_bytes = fed; a->hud_prev_frames = a->frames; a->hud_prev_irq = irq; a->hud_prev_reads = reads;
    char irq_txt[32], hist[160] = "", dims[32] = "no terminal", cache[256];
    if (t && t->term) snprintf(dims, sizeof dims, "%dx%d", tcore_cols(t->term), tcore_rows(t->term));
    {
        static CacheStats prev;
        CacheStats d = {sd_cache.glyph_hit - prev.glyph_hit, sd_cache.glyph_miss - prev.glyph_miss, sd_cache.glyph_recycle - prev.glyph_recycle,
                        sd_cache.row_hit - prev.row_hit, sd_cache.row_miss - prev.row_miss, sd_cache.row_recycle - prev.row_recycle};
        prev = sd_cache;
        char gh[24], gm[24], gr[24], rh[24], rm[24], rr[24], rp[16], gp[16];
        fmt_count((double)d.glyph_hit, gh, sizeof gh); fmt_count((double)d.glyph_miss, gm, sizeof gm); fmt_count((double)d.glyph_recycle, gr, sizeof gr);
        fmt_count((double)d.row_hit, rh, sizeof rh); fmt_count((double)d.row_miss, rm, sizeof rm); fmt_count((double)d.row_recycle, rr, sizeof rr);
        uint64_t rt = d.row_hit + d.row_miss, gt = d.glyph_hit + d.glyph_miss;
        if (rt) snprintf(rp, sizeof rp, "%.0f%%", 100.0 * (double)d.row_hit / (double)rt); else snprintf(rp, sizeof rp, "--");
        if (gt) snprintf(gp, sizeof gp, "%.0f%%", 100.0 * (double)d.glyph_hit / (double)gt); else snprintf(gp, sizeof gp, "--");
        char rc[24]; fmt_count((double)(d.row_recycle + d.glyph_recycle), rc, sizeof rc);
        snprintf(cache, sizeof cache, "cache rows %s glyphs %s recyc %s", rp, gp, rc);
        snprintf(a->hud_cache_tip, sizeof a->hud_cache_tip,
                 "Cache hit rate, then recycles, per second. rows: %s hit, %s miss, %s recycled (line geometry reused instead of rebuilt). glyphs: %s hit, %s miss, %s dropped by atlas clears (letter images found on the GPU).",
                 rh, rm, rr, gh, gm, gr);
    }
    fmt_count(a->hud_irq_total ? (double)irq : irq_rate, irq_txt, sizeof irq_txt);
    if (t && t->term) {
        VtHistoryStats st;
        tcore_history_stats(t->term, &st);
        char *mem = fmt_mb((double)(st.hot_bytes + st.packed_bytes)), *disk = fmt_mb((double)st.disk_bytes);
        snprintf(hist, sizeof hist, "scrlbck %ld ln, %s mem, %s disk", st.lines, mem, disk);
        g_free(mem); g_free(disk);
    }
    char *rss = fmt_mb((double)proc_rss_kb() * 1024);
    snprintf(a->hud_seg[0], sizeof a->hud_seg[0], "fps %.0f", fps);
    snprintf(a->hud_seg[1], sizeof a->hud_seg[1], "%s", dims);
    snprintf(a->hud_seg[2], sizeof a->hud_seg[2], "parse %.3f GB/s", rate / 1024);
    snprintf(a->hud_parse_tip, sizeof a->hud_parse_tip, "Parse rate: program output the terminal engine consumed over the last second. Right now: %.0f MB/s.", rate);
    snprintf(a->hud_seg[3], sizeof a->hud_seg[3], "cpu %.0f%% (%ld)", cpu, sysconf(_SC_NPROCESSORS_ONLN));
    snprintf(a->hud_seg[4], sizeof a->hud_seg[4], "irq %s%s", irq_txt, a->hud_irq_total ? "" : "/s");
    snprintf(a->hud_seg[5], sizeof a->hud_seg[5], "memory %s", rss);
    snprintf(a->hud_seg[6], sizeof a->hud_seg[6], "%d thr/%uc", proc_threads(), (unsigned)sysconf(_SC_NPROCESSORS_ONLN));
    { char rr_[24]; fmt_count(rd_rate, rr_, sizeof rr_); snprintf(a->hud_seg[7], sizeof a->hud_seg[7], "reads %s/s @%.0fK", rr_, rd_avg / 1024); }
    snprintf(a->hud_seg[8], sizeof a->hud_seg[8], "%s", cache);
    snprintf(a->hud_seg[9], sizeof a->hud_seg[9], "%s", hist);
    g_free(rss);
    if (a->lite) {
        char title[400];
        snprintf(title, sizeof title, "Lestrix Lite  |  %.40s  |  %.40s  |  %.80s  |  glyphs %u  |  ascii %d/512%s%.60s", a->hud_seg[0], a->hud_seg[3], a->hud_seg[8],
                 a->atlas ? (unsigned)atlas_count(a->atlas) : 0u, a->atlas ? atlas_ascii_filled(a->atlas) : 0, a->hud_seg[10][0] ? "  |  " : "", a->hud_seg[10]);
        if (strcmp(title, a->title_last) != 0) { snprintf(a->title_last, sizeof a->title_last, "%s", title); SDL_SetWindowTitle(a->win, title); }
    }
    if (!a->lite && memcmp(before, a->hud_seg, sizeof before) != 0) app_redraw(a);
}

static void trim_tick(App *a) {
    uint64_t total = 0;
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->term) total += tcore_bytes_fed(t->term); }
    if (total != a->trim_prev_bytes) { a->trim_prev_bytes = total; a->trim_dirty = true; return; }
    if (!a->trim_dirty) return;
    a->trim_dirty = false;
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->term) tcore_compact(t->term); }
    malloc_trim(0);
}

static void handle_menu_results(App *a) {
    int tag = 0;
    int id = ui_menu_peek(a->ui, &tag);
    if (id < 0) return;
    if (tag >= TAG_BAR && tag < TAG_BAR + 3) { ui_menu_take(a->ui, &tag); run_action(a, id); }
    else if (tag == TAG_TAB) { ui_menu_take(a->ui, &tag); run_tab_action(a, id); }
    else if (tag == TAG_ROW) { ui_menu_take(a->ui, &tag); run_row_action(a, id); }
}

static void build_frame(App *a) {
    Ui *u = a->ui;
    ui_begin(u, a->W, a->H, a->now);
    float target = (!a->pinned && a->side_open) ? 1.f : 0.f;
    if (fabs(a->side_anim - target) > 0.002f) {
        a->side_anim += (float)((target - a->side_anim) * fminf(1.0, 0.25));
        if (fabs(a->side_anim - target) < 0.01f) a->side_anim = target;
        ui_want_frames(u, 0.05);
    } else a->side_anim = target;
    compute_layout(a);
    handle_menu_results(a);
    bool dlg = dlg_active(a);
    bool base_blocked = dlg || ui_menu_is_open(u);
    Rect pr = lay.panel;
    bool block_under = lay.overlay && lay.panel_vis && rect_has(R(pr.x, pr.y, pr.w, pr.h), ui_mx(u), ui_my(u));
    ui_set_blocked(u, base_blocked || block_under);
    a->blocked_for_term = base_blocked || block_under;
    draw_content(a);
    draw_strip(a);
    ui_set_blocked(u, base_blocked);
    draw_menubar(a);
    if (!a->lite) { draw_rail(a); if (lay.panel_vis) draw_panel(a); draw_hud(a); }
    if (dlg) { ui_set_blocked(u, ui_menu_is_open(u)); dlg_draw_top(a); }
    ui_end(u);
}

static bool shortcut(App *a, SDL_Keycode k, Uint16 m) {
    bool ctrl = m & KMOD_CTRL, shift = m & KMOD_SHIFT, alt = m & KMOD_ALT;
    int n = (int)a->tabs->len;
    if (ctrl && !shift && !alt) {
        if (k == SDLK_n) { run_action(a, A_NEW_CONN); return true; }
        if (k == SDLK_q) { a->running = false; return true; }
        if (k == SDLK_b) { run_action(a, A_SIDEBAR); return true; }
        if (k == SDLK_EQUALS || k == SDLK_PLUS || k == SDLK_KP_PLUS) { run_action(a, A_BIGGER); return true; }
        if (k == SDLK_MINUS || k == SDLK_KP_MINUS) { run_action(a, A_SMALLER); return true; }
        if (k == SDLK_PAGEDOWN || k == SDLK_TAB) { if (n) select_tab(a, (a->cur + 1) % n); return true; }
        if (k == SDLK_PAGEUP) { if (n) select_tab(a, (a->cur + n - 1) % n); return true; }
    }
    if (ctrl && shift && !alt) {
        if (k == SDLK_t) { run_action(a, A_NEW_LOCAL); return true; }
        if (k == SDLK_b) { run_action(a, A_FILES); return true; }
        if (k == SDLK_w) { Tab *t = app_cur_tab(a); if (t) app_close_tab(a, t); return true; }
        if (k == SDLK_d) { Tab *t = app_cur_tab(a); if (t) app_dup_tab(a, t); return true; }
        if (k == SDLK_TAB) { if (n) select_tab(a, (a->cur + n - 1) % n); return true; }
        if (k == SDLK_PAGEUP) { move_current_tab(a, -1); return true; }
        if (k == SDLK_PAGEDOWN) { move_current_tab(a, 1); return true; }
    }
    if (alt && !ctrl && !shift && k >= SDLK_1 && k <= SDLK_9) {
        int idx = k - SDLK_1;
        if (n) select_tab(a, idx >= 8 ? n - 1 : (idx < n ? idx : n - 1));
        return true;
    }
    if (k == SDLK_F5 && !ctrl && !alt) { run_action(a, A_RECHECK); return true; }
    if (!n && !ctrl && !alt && !dlg_active(a) && !ui_keyboard_taken(a->ui)) {
        if (k == SDLK_0) { do_new_local(a, NULL); return true; }
        if (k >= SDLK_1 && k <= SDLK_9 && k - SDLK_1 < nrecent) {
            SdConn *cn = sd_store_get(a->store, recents[k - SDLK_1].id);
            if (cn) app_open_connection(a, cn);
            return true;
        }
    }
    return false;
}

static int map_mods(Uint16 m) {
    return ((m & KMOD_SHIFT) ? TM_SHIFT : 0) | ((m & KMOD_ALT) ? TM_ALT : 0) | ((m & KMOD_CTRL) ? TM_CTRL : 0) | ((m & KMOD_GUI) ? TM_META : 0);
}

static TKey map_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: case SDLK_KP_8: return TK_UP; case SDLK_DOWN: case SDLK_KP_2: return TK_DOWN;
    case SDLK_LEFT: case SDLK_KP_4: return TK_LEFT; case SDLK_RIGHT: case SDLK_KP_6: return TK_RIGHT;
    case SDLK_HOME: return TK_HOME; case SDLK_END: return TK_END; case SDLK_INSERT: return TK_INSERT;
    case SDLK_DELETE: return TK_DELETE; case SDLK_PAGEUP: return TK_PGUP; case SDLK_PAGEDOWN: return TK_PGDN;
    case SDLK_F1: return TK_F1; case SDLK_F2: return TK_F2; case SDLK_F3: return TK_F3; case SDLK_F4: return TK_F4;
    case SDLK_F5: return TK_F5; case SDLK_F6: return TK_F6; case SDLK_F7: return TK_F7; case SDLK_F8: return TK_F8;
    case SDLK_F9: return TK_F9; case SDLK_F10: return TK_F10; case SDLK_F11: return TK_F11; case SDLK_F12: return TK_F12;
    case SDLK_RETURN: case SDLK_KP_ENTER: return TK_ENTER; case SDLK_BACKSPACE: return TK_BACKSPACE;
    case SDLK_TAB: return TK_TAB; case SDLK_ESCAPE: return TK_ESCAPE;
    default: return TK_NONE;
    }
}

static void cell_at(App *a, double x, double y, int *col, int *row) {
    *col = (int)((x * a->scale - term_rect.x - TV_PAD) / font_cell_w(a->term_font));
    *row = (int)((y * a->scale - term_rect.y - TV_PAD) / font_cell_h(a->term_font));
}

static void handle_event(App *a, SDL_Event *e) {
    ui_event(a->ui, e);
    Tab *t = app_cur_tab(a);
    TermCore *term = t ? t->term : NULL;
    bool kbd_ui = dlg_active(a) || ui_keyboard_taken(a->ui);
    switch (e->type) {
    case SDL_QUIT: a->running = false; break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_RESIZED) {
            SDL_GL_GetDrawableSize(a->win, &a->W, &a->H);
            app_redraw(a);
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e->window.event == SDL_WINDOWEVENT_FOCUS_LOST || e->window.event == SDL_WINDOWEVENT_EXPOSED) app_redraw(a);
        break;
    case SDL_DROPFILE: {
        if (t && t->files) files_drop(t->files, e->drop.file);
        SDL_free(e->drop.file);
        break;
    }
    case SDL_KEYDOWN: {
        if (e->key.repeat == 0 || true) {
            if (!dlg_active(a) && !ui_menu_is_open(a->ui)) {
                if (shortcut(a, e->key.keysym.sym, e->key.keysym.mod)) { app_redraw(a); break; }
            } else if (dlg_active(a)) break;
            if (kbd_ui || !term) break;
            SDL_Keycode k = e->key.keysym.sym;
            uint32_t cp = (k >= 32 && k < 127) ? (uint32_t)k : 0;
            if ((k == SDLK_RETURN || k == SDLK_KP_ENTER)) tcore_cmd_begin(term, now_s());
            if (tcore_key(term, map_key(k), cp, map_mods(e->key.keysym.mod))) app_redraw(a);
        }
        break;
    }
    case SDL_TEXTINPUT:
        if (!kbd_ui && term && !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT))) { tcore_text(term, e->text.text); app_redraw(a); }
        break;
    case SDL_MOUSEBUTTONDOWN: {
        bool inside = term && !a->blocked_for_term && rect_has(term_rect, (float)e->button.x * a->scale, (float)e->button.y * a->scale);
        if (inside) {
            int col, row;
            cell_at(a, e->button.x, e->button.y, &col, &row);
            int b = e->button.button == SDL_BUTTON_LEFT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
            tcore_mouse_button(term, b, true, col, row, e->button.clicks, map_mods(SDL_GetModState()));
            term_dragging = true;
            ui_release_focus(a->ui);
        }
        if (!a->pinned && a->side_open && !dlg_active(a) && !ui_menu_is_open(a->ui)) {
            float mx = (float)e->button.x * a->scale, my = (float)e->button.y * a->scale;
            if (!rect_has(lay.panel, mx, my) && !rect_has(lay.rail, mx, my) && !rect_has(lay.bar, mx, my)) { a->side_open = false; ui_want_frames(a->ui, 0.3); }
        }
        app_redraw(a);
        break;
    }
    case SDL_MOUSEBUTTONUP:
        if (term && term_dragging) {
            int col, row;
            cell_at(a, e->button.x, e->button.y, &col, &row);
            int b = e->button.button == SDL_BUTTON_LEFT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
            tcore_mouse_button(term, b, false, col, row, 1, map_mods(SDL_GetModState()));
            if (e->button.button == SDL_BUTTON_LEFT) term_dragging = false;
        }
        app_redraw(a);
        break;
    case SDL_MOUSEMOTION:
        if (term && (term_dragging || (!a->blocked_for_term && rect_has(term_rect, (float)e->motion.x * a->scale, (float)e->motion.y * a->scale)))) {
            int col, row;
            cell_at(a, e->motion.x, e->motion.y, &col, &row);
            tcore_mouse_move(term, col, row, map_mods(SDL_GetModState()));
        }
        if (term_dragging || ui_motion_matters(a->ui)) app_redraw(a);
        break;
    case SDL_MOUSEWHEEL: {
        float mx = ui_mx(a->ui) / a->scale, my = ui_my(a->ui) / a->scale;
        if (term && !a->blocked_for_term && rect_has(term_rect, mx * a->scale, my * a->scale) && !dlg_active(a)) {
            int col, row;
            cell_at(a, mx, my, &col, &row);
            tcore_wheel(term, e->wheel.preciseY, col, row, map_mods(SDL_GetModState()));
        }
        app_redraw(a);
        break;
    }
    default:
        if (e->type == EV_WAKE) atomic_store(&wake_pending, 0);
        break;
    }
}

static void *prewarm_main(void *unused) { (void)unused; font_prewarm(); return NULL; }

static void write_ppm(App *a, const char *path) {
    uint8_t *px = malloc((size_t)a->W * a->H * 4);
    glReadPixels(0, 0, a->W, a->H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", a->W, a->H);
        for (int y = a->H - 1; y >= 0; y--) for (int x = 0; x < a->W; x++) fwrite(px + ((size_t)y * a->W + x) * 4, 1, 3, fp);
        fclose(fp);
    }
    free(px);
}

static bool gl_init(App *a, GlKind kind) {
    if (a->gl) { SDL_GL_DeleteContext(a->gl); a->gl = NULL; }
    if (a->win) { SDL_DestroyWindow(a->win); a->win = NULL; }
    if (kind == GLK_CORE) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kind == GLK_ES3 ? 3 : 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    }
    int win_w = 1440, win_h = 780;
    { SDL_Rect db; if (SDL_GetDisplayBounds(0, &db) == 0) { if (win_w > db.w * 95 / 100) win_w = db.w * 95 / 100; if (win_h > db.h * 90 / 100) win_h = db.h * 90 / 100; } }
    a->win = SDL_CreateWindow("Lestrix", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (a->win) {
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)APP_ICON_RGBA, 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(a->win, ic); SDL_FreeSurface(ic); }
    }
    tmark("window created");
    a->gl = a->win ? SDL_GL_CreateContext(a->win) : NULL;
    tmark("GL context created");
    if (!a->gl || sd_gl_load(kind)) return false;
    SDL_GL_SetSwapInterval(g_max_fps > 0 ? 1 : 0);
    int ww, wh;
    SDL_GetWindowSize(a->win, &ww, &wh);
    SDL_GL_GetDrawableSize(a->win, &a->W, &a->H);
    a->scale = ww ? (float)a->W / (float)ww : 1.f;
    a->atlas = atlas_new(a->scale > 1.5f ? 4096 : 2048);
    a->r = a->atlas ? r_new(a->atlas) : NULL;
    if (!a->r) { if (a->atlas) atlas_free(a->atlas); a->atlas = NULL; return false; }
    const char *ver = (const char *)glGetString(GL_VERSION), *rend = (const char *)glGetString(GL_RENDERER);
    snprintf(a->gl_desc, sizeof a->gl_desc, "%s on %s", ver ? ver : "OpenGL", rend ? rend : "unknown GPU");
    return true;
}

typedef struct { bool local; const char *cwd, *theme, *connect; char **exec; } Startup;

typedef struct { char **steps; int n, i; double wait_until; char *shot_file; } Script;

static void script_key(const char *spec) {
    Uint16 mod = 0;
    char buf[64];
    snprintf(buf, sizeof buf, "%s", spec);
    char *name = buf, *plus;
    while ((plus = strchr(name, '+')) && plus[1]) {
        *plus = 0;
        if (!strcmp(name, "ctrl")) mod |= KMOD_CTRL; else if (!strcmp(name, "shift")) mod |= KMOD_SHIFT; else if (!strcmp(name, "alt")) mod |= KMOD_ALT;
        name = plus + 1;
    }
    SDL_Keycode k = SDL_GetKeyFromName(name);
    if (k == SDLK_UNKNOWN) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED; e.key.keysym.sym = k; e.key.keysym.mod = mod;
    SDL_PushEvent(&e);
}

static void script_mouse(App *a, int type, int button, int x, int y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    float f = 1.f / a->scale;
    (void)f;
    if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; }
    else { e.button.x = x; e.button.y = y; e.button.button = (Uint8)button; e.button.clicks = 1; e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED; }
    SDL_PushEvent(&e);
}

static bool script_run(App *a, Script *sc, const char **shot_out) {
    if (!sc->n || sc->i >= sc->n || a->now < sc->wait_until) return false;
    char *st = sc->steps[sc->i++];
    int x, y, sx, sy;
    double w;
    if (sscanf(st, "w:%lf", &w) == 1) sc->wait_until = a->now + w;
    else if (sscanf(st, "m:%d,%d", &x, &y) == 2) script_mouse(a, SDL_MOUSEMOTION, 0, x, y);
    else if (sscanf(st, "c:%d,%d", &x, &y) == 2 || sscanf(st, "d:%d,%d", &x, &y) == 2) {
        script_mouse(a, SDL_MOUSEMOTION, 0, x, y);
        script_mouse(a, SDL_MOUSEBUTTONDOWN, 1, x, y);
        script_mouse(a, SDL_MOUSEBUTTONUP, 1, x, y);
        sc->wait_until = a->now + 0.12;
    } else if (sscanf(st, "r:%d,%d", &x, &y) == 2) {
        script_mouse(a, SDL_MOUSEMOTION, 0, x, y);
        script_mouse(a, SDL_MOUSEBUTTONDOWN, 3, x, y);
        script_mouse(a, SDL_MOUSEBUTTONUP, 3, x, y);
        sc->wait_until = a->now + 0.12;
    } else if (sscanf(st, "p:%d,%d,%d,%d", &x, &y, &sx, &sy) == 4) {
        script_mouse(a, SDL_MOUSEMOTION, 0, x, y);
        script_mouse(a, SDL_MOUSEBUTTONDOWN, 1, x, y);
        script_mouse(a, SDL_MOUSEMOTION, 0, (x + sx) / 2, (y + sy) / 2);
        script_mouse(a, SDL_MOUSEMOTION, 0, sx, sy);
        script_mouse(a, SDL_MOUSEBUTTONUP, 1, sx, sy);
        sc->wait_until = a->now + 0.15;
    } else if (sscanf(st, "x:%d", &x) == 1) {
        SDL_Event we;
        SDL_zero(we);
        we.type = SDL_MOUSEWHEEL; we.wheel.y = x; we.wheel.preciseY = (float)x;
        SDL_PushEvent(&we);
        sc->wait_until = a->now + 0.1;
    } else if (!strncmp(st, "k:", 2)) { script_key(st + 2); sc->wait_until = a->now + 0.08; }
    else if (!strncmp(st, "t:", 2)) { SDL_Event e; SDL_zero(e); e.type = SDL_TEXTINPUT; snprintf(e.text.text, sizeof e.text.text, "%s", st + 2); SDL_PushEvent(&e); sc->wait_until = a->now + 0.08; }
    else if (!strncmp(st, "s:", 2)) { *shot_out = st + 2; sc->wait_until = a->now + 0.3; }
    a->dirty = true;
    return true;
}

static void jobs_wake(void) { app_wake(); }

static bool cmd_pending(App *a) {
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->term && tcore_cmd_active(t->term)) return true; }
    return false;
}

static void cmd_poll_all(App *a) {
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        double secs; uint64_t bytes;
        if (!t->term || !tcore_cmd_active(t->term) || !tcore_cmd_poll(t->term, a->now, &secs, &bytes) || !a->hud) continue;
        double gbs = secs > 0 ? (double)bytes / secs / 1073741824.0 : 0;
        char tm[24], th[24];
        if (secs < 1) snprintf(tm, sizeof tm, "%.0f ms", secs * 1000); else snprintf(tm, sizeof tm, "%.2f s", secs);
        if (gbs >= 1) snprintf(th, sizeof th, "%.2f GB/s", gbs); else if (gbs >= 0.001) snprintf(th, sizeof th, "%.3f GB/s", gbs); else snprintf(th, sizeof th, "<0.001 GB/s");
        snprintf(a->hud_seg[10], sizeof a->hud_seg[10], "cmd %s, %s", tm, th);
        if (a->lite) hud_tick(a); else app_redraw(a);
    }
}

static void pump_all(App *a) {
    if (jobs_pump()) app_redraw(a);
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (t->term && tcore_pump(t->term)) { if (i == (guint)a->cur) app_redraw(a); else app_redraw(a); }
        if (t->files) files_tick(a, t->files);
    }
}


int main(int argc, char **argv) {
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    mallopt(M_TRIM_THRESHOLD, 256 * 1024);
    signal(SIGCHLD, SIG_DFL);  /* a launcher may start us with SIGCHLD ignored; that survives exec and breaks waitpid here and in every shell child (git: "waitpid failed: No child processes") */
    Startup s = {0};
    char *flood = NULL, *run_cmd = NULL, *shot = NULL, *script_arg = NULL;
    double shot_after = 3.0;
    for (int i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "--version")) { g_print("Lestrix %s\n" APP_CREDIT, APP_VERSION); return 0; }
        if (g_str_equal(argv[i], "--benchmark")) return sd_benchmark(stdout, 8.0);
        if (g_str_equal(argv[i], "--max-fps") && i + 1 < argc) { g_max_fps = atof(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--read-delay") && i + 1 < argc) { tcore_read_delay_us = atoi(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--no-fastcat")) { g_fastcat = false; continue; }
        if (g_str_equal(argv[i], "--render-threads") && i + 1 < argc) { g_render_threads = atoi(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--io-threads") && i + 1 < argc) { tcore_io_threads = atoi(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--parse-threads") && i + 1 < argc) { g_parse_threads = atoi(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--compress-threads") && i + 1 < argc) { g_compress_threads = atoi(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--io-fps") && i + 1 < argc) { g_io_fps = atof(argv[++i]); continue; }
        if (g_str_equal(argv[i], "--help") || g_str_equal(argv[i], "-h")) {
            g_print("Usage: lestrix [--local] [--working-directory DIR] [-e COMMAND [ARGS...]]\n"
                    "  --local               open a local shell on startup\n"
                    "  --no-fast-output      keep the kernel's newline processing on the pty while cat, grep, ls, find and similar print (default: switch it off for them and let the terminal add the carriage returns; about 3x faster)\n"
                    "  --lite                terminal only: no sidebar, no stats bar; fps, cpu and cache stats go in the title bar\n"
                    "  --working-directory   start the local shell in DIR\n"
                    "  --benchmark           measure this machine's terminal throughput and exit\n"
                    "  --connect NAME        open the saved connection NAME on startup\n"
                    "  --max-fps N           cap drawing at N frames per second (default 60; 0 = no cap, vsync off)\n"
                    "  --io-fps N            cap drawing at N fps while a big dump pours in, so reading output gets the CPU\n"
                    "                        (default 24; lower is faster for output; 0 = no extra cap)\n"
                    "  --read-delay US       wait US microseconds after a tiny (under 1 KB) read of program output (default 200; 0 = off):\n"
                    "                        far fewer system calls and less CPU in a flood\n"
                    "  --render-threads N    helper threads that build the rows of a frame (default 0: measured no gain at this frame cost)\n"
                    "  --io-threads N        2: a reader and a writer thread per tab besides the parser; 0: one thread (default: 2 from 4 cores up)\n"
                    "  --parse-threads N     helper threads that build big batches of plain text in parallel (default: half the cores, up to 8; 0 = off)\n"
                    "  --compress-threads N  threads that compress old scrollback (default: a quarter of the cores, 2 to 8)\n"
                    "  --no-fastcat          do not let programs in a local tab use `lxcat` to print big files at memory speed\n"
                    "  --theme NAME          Xylonic Dark, Xylonic Light, Graphite, Nord, Gruvbox or Solarized Dark\n"
                    "  -e COMMAND ...        run COMMAND in a new tab (works as x-terminal-emulator)\n\n" APP_CREDIT);
            return 0;
        }
        if (g_str_equal(argv[i], "--local")) s.local = true;
        else if (g_str_equal(argv[i], "--lite")) { g_lite = true; s.local = true; }
        else if (g_str_equal(argv[i], "--no-fast-output")) tcore_set_fast_output(false);
        else if (g_str_equal(argv[i], "--working-directory") && i + 1 < argc) s.cwd = argv[++i];
        else if (g_str_equal(argv[i], "--theme") && i + 1 < argc) s.theme = argv[++i];
        else if (g_str_equal(argv[i], "--connect") && i + 1 < argc) s.connect = argv[++i];
        else if (g_str_equal(argv[i], "--flood") && i + 1 < argc) flood = argv[++i];
        else if (g_str_equal(argv[i], "--run") && i + 1 < argc) run_cmd = argv[++i];
        else if (g_str_equal(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (g_str_equal(argv[i], "--script") && i + 1 < argc) script_arg = argv[++i];
        else if (g_str_equal(argv[i], "--shot-after") && i + 1 < argc) shot_after = atof(argv[++i]);
        else if (g_str_equal(argv[i], "-e") || g_str_equal(argv[i], "--execute")) {
            s.exec = g_new0(char *, (gsize)(argc - i));
            for (int k = i + 1; k < argc; k++) s.exec[k - i - 1] = argv[k];
            break;
        }
    }
    for (int i = 1; i < argc; i++) if (g_str_equal(argv[i], "--lite")) g_lite = true;
    g_setenv("SDL_VIDEO_X11_WMCLASS", g_lite ? "lestrix-lite" : "lestrix", FALSE);
    g_setenv("SDL_VIDEO_WAYLAND_WMCLASS", g_lite ? "lestrix-lite" : "lestrix", FALSE);
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    tmark("main entered");
    pthread_t prewarm_thread;
    bool prewarming = pthread_create(&prewarm_thread, NULL, (void *(*)(void *))prewarm_main, NULL) == 0;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    EV_WAKE = SDL_RegisterEvents(1);
    App *a = g_new0(App, 1);
    g_app = a;
    tmark("SDL_Init done");
    {
        GlKind order[3] = {GLK_CORE, GLK_ES3, GLK_ES2};
        int n = 3, first = 0;
        const char *force = getenv("LESTRIX_GL");
        if (force) { first = !strcmp(force, "es3") ? 1 : !strcmp(force, "es2") ? 2 : 0; n = first + 1; }
        bool ok = false;
        for (int i = first; i < n && !ok; i++) ok = gl_init(a, order[i]);
        if (!ok) { fprintf(stderr, "Lestrix needs OpenGL 3.3, OpenGL ES 3.0 or OpenGL ES 2.0\n"); return 1; }
        if (getenv("LESTRIX_TIMING")) fprintf(stderr, "OpenGL: %s (%s)\n", a->gl_desc, sd_gl_kind == GLK_CORE ? "desktop core" : sd_gl_kind == GLK_ES3 ? "ES 3.0" : "ES 2.0");
    }
    tmark("GL functions loaded");
    if (prewarming) { pthread_join(prewarm_thread, NULL); prewarming = false; }
    tmark("fontconfig warmed");
    vt_set_threads(g_parse_threads, g_compress_threads);
    wp_configure(g_render_threads);
    settings_load(a);
    app_fastcat_apply(a);
    tmark("settings loaded");
    if (s.theme) a->theme = ui_theme_find(s.theme);
    if (!a->font_family || !*a->font_family) { g_free(a->font_family); a->font_family = pick_font_family(); }
    a->ui_font = font_open("sans", 13 * a->scale, false);
    a->big_font = font_open("sans", 34 * a->scale, false);
    a->term_font = font_open(a->font_family, a->font_size * 96.0 / 72.0 * a->scale, true);
    if (!a->term_font) a->term_font = font_open("monospace", a->font_size * 96.0 / 72.0 * a->scale, true);
    if (!a->ui_font || !a->term_font || !a->big_font) { fprintf(stderr, "no usable fonts found\n"); return 1; }
    tmark("fonts opened");
    a->ui_mono = font_open("monospace", 12 * a->scale, true);
    tmark("atlas and renderer ready");
    a->ui = ui_new(a->r, a->ui_font, a->ui_mono, a->scale);
    a->tabs = g_ptr_array_new();
    a->up = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    a->recent_ids = g_ptr_array_new_with_free_func(g_free);
    a->cur = -1;
    a->drag_tab = -1;
    a->dock_shown = true;
    a->running = true;
    a->dirty = true;
    a->blink_on = true;
    ui_text_init(&a->search, a->search_buf, sizeof a->search_buf);
    fleetwm_read(&a->fleet);
    if (a->follow_fleet && a->fleet.found) app_follow_fleetwm(a); else app_apply_theme(a, a->theme);
    tmark("theme applied");
    a->store = sd_store_new(NULL);
    tmark("store loaded");
    tcore_set_history_defaults(a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
    jobs_init(jobs_wake);
    find_shells();
    tmark("shells found");
    app_refresh(a);
    tmark("connections refreshed");
    probe_all(a);
    tmark("probes started");
    SDL_StartTextInput();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    if (s.exec && s.exec[0]) {
        char *base = g_path_get_basename(s.exec[0]);
        a->quit_on_exit = true;
        app_open_local(a, base, s.exec, s.cwd);
        g_free(base);
    } else if (flood || run_cmd) {
        char **sh = app_default_shell_argv(NULL);
        app_open_local(a, "local", sh, NULL);
        g_strfreev(sh);
    } else if (s.connect) {
        GPtrArray *all = sd_store_all(a->store);
        for (guint i = 0; i < all->len; i++) {
            SdConn *c = all->pdata[i];
            if (g_str_equal(c->name, s.connect) || g_str_equal(c->id, s.connect)) { app_open_connection(a, c); break; }
        }
        g_ptr_array_free(all, TRUE);
    } else if (s.local || a->startup_shell) {
        char **sh = app_default_shell_argv(NULL);
        app_open_local(a, "local", sh, s.cwd);
        g_strfreev(sh);
    }
    double t0 = now_s(), last_draw = 0;
    a->next_probe = t0 + 30; a->next_hud = t0 + 1; a->next_trim = t0 + 5;
    a->last_blink = t0;
    uint64_t last_draw_bytes = 0;
    bool flood_sent = false;
    double flood_t = 0;
    Script script = {0};
    if (script_arg) { script.steps = g_strsplit(script_arg, ";", -1); script.n = (int)g_strv_length(script.steps); }
    const char *script_shot = NULL;
    SDL_Event e;
    while (a->running) {
        a->now = now_s();
        int wait = 250;
        bool focused = (SDL_GetWindowFlags(a->win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        {
            double due = a->next_trim < a->next_probe ? a->next_trim : a->next_probe;
            if (a->hud && a->next_hud < due) due = a->next_hud;
            if (a->follow_fleet && a->next_fleet < due) due = a->next_fleet;
            if (focused && a->last_blink + 0.53 < due) due = a->last_blink + 0.53;
            double left = due - a->now;
            wait = left < 0.001 ? 1 : left > 5.0 ? 5000 : (int)(left * 1000.0) + 1;  /* idle: sleep to the next timer, not a fixed 1 s */
        }
        {
            Tab *wt = app_cur_tab(a);
            uint64_t wfed = wt && wt->term ? tcore_bytes_fed(wt->term) : 0;
            double gap = frame_gap(wfed - last_draw_bytes >= 256 * 1024);
            if (ui_animating(a->ui)) wait = 4;
            else if (a->dirty) { double left = last_draw + gap - a->now; wait = left > 0 ? (int)(left * 1000.0) + 1 : 0; }
        }
        if (flood && flood_sent && wait > 2) wait = 2;
        if (wait > 20 && cmd_pending(a)) wait = 20;
        if (SDL_WaitEventTimeout(&e, wait)) { do handle_event(a, &e); while (SDL_PollEvent(&e)); }
        a->now = now_s();
        pump_all(a);
        cmd_poll_all(a);
        while (script_run(a, &script, &script_shot)) { if (a->now < script.wait_until) break; }
        if (flood && !flood_sent && a->now - t0 > 1.0 && app_cur_tab(a)) {
            char cmd[1024];
            snprintf(cmd, sizeof cmd, "cat '%s'; echo FLOOD_DONE_$((6*7))\r", flood);
            tcore_send_str(app_cur_tab(a)->term, cmd);
            flood_sent = true; flood_t = a->now;
        }
        if (run_cmd && !flood_sent && a->now - t0 > 1.0 && app_cur_tab(a)) { tcore_send_str(app_cur_tab(a)->term, run_cmd); flood_sent = true; }
        if (flood && flood_sent && app_cur_tab(a) && tcore_screen_contains(app_cur_tab(a)->term, "FLOOD_DONE_42")) {
            printf("flood finished in %.2fs\n", a->now - flood_t);
            if (getenv("LESTRIX_THREADS")) {
                GDir *d = g_dir_open("/proc/self/task", 0, NULL);
                const char *n;
                while (d && (n = g_dir_read_name(d))) {
                    char *path = g_strdup_printf("/proc/self/task/%s/stat", n), *txt = NULL;
                    if (g_file_get_contents(path, &txt, NULL, NULL)) {
                        char *rp = strrchr(txt, ')'), *lp = strchr(txt, '(');
                        unsigned long ut = 0, st = 0;
                        if (rp && lp && sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2 && ut + st > 3) {
                            *rp = 0;
                            printf("  thread %-16s user %.2fs sys %.2fs\n", lp + 1, ut / 100.0, st / 100.0);
                        }
                    }
                    g_free(txt); g_free(path);
                }
                if (d) g_dir_close(d);
            }
            break;
        }
        if (a->follow_fleet && !a->fleet_watch) a->fleet_watch = fleetwm_watch_start();
        if (fleetwm_watch_take() || a->now >= a->next_fleet) {
            a->next_fleet = a->now + (a->fleet_watch ? 30.0 : 1.0);  /* with the watch the timer only catches a missed event */
            if (a->follow_fleet && fleetwm_stamp() != a->fleet.stamp) app_follow_fleetwm(a);
        }
        if (a->now >= a->next_hud) { a->next_hud = a->now + (focused ? 1 : 5); if (a->hud) hud_tick(a); }
        if (a->now >= a->next_trim) { a->next_trim = a->now + 5; trim_tick(a); }
        if (a->now >= a->next_probe) { a->next_probe = a->now + 30; if (SDL_GetWindowFlags(a->win) & SDL_WINDOW_INPUT_FOCUS) probe_all(a); }
        if (!focused) { if (!a->blink_on) { a->blink_on = true; app_redraw(a); } a->last_blink = a->now; }
        else if (a->now - a->last_blink > 0.53) { a->last_blink = a->now; a->blink_on = !a->blink_on; if (app_cur_tab(a) && app_cur_tab(a)->term) app_redraw(a); }
        Tab *ct = app_cur_tab(a);
        uint64_t fed = ct && ct->term ? tcore_bytes_fed(ct->term) : 0;
        double min_gap = frame_gap(fed - last_draw_bytes >= 256 * 1024);
        if ((a->dirty || ui_animating(a->ui)) && a->now - last_draw >= min_gap) {
            last_draw = a->now;
            last_draw_bytes = fed;
            a->dirty = false;
            r_begin(a->r, a->W, a->H, a->colors.bg0);
            build_frame(a);
            r_end(a->r);
            a->frames++;
            { static bool first = true; if (first) { first = false; tmark("first frame drawn"); } }
            if (script_shot) {
                write_ppm(a, script_shot);
                script_shot = NULL;
            }
            if (script.n && script.i >= script.n && a->now > script.wait_until + 0.2 && !shot) a->running = false;
            if (shot && a->now - t0 > shot_after) {
                write_ppm(a, shot);
                a->running = false;
            }
            SDL_GL_SwapWindow(a->win);
        }
    }
    app_settings_save(a);
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->term) tcore_close(t->term); }
    for (guint i = 0; i < a->tabs->len; i++) { Tab *t = a->tabs->pdata[i]; if (t->control_path) sd_xfer_remove_control_path(t->control_path); }
    jobs_shutdown();
    return 0;
}
