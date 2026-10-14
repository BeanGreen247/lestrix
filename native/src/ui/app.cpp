/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <math.h>
#include <malloc.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../bench.h"
#include "app.h"
#include "appicon.h"
#include "fleetwm.h"
#include "gl.h"
#include "jobs.h"
#include "workpool.h"

#define P(v) S(a->ui, (v))


App *g_app;
static Uint32 EV_WAKE;
static std::atomic<int> wake_pending;

void app_wake(void) {
    int exp = 0;
    if (wake_pending.compare_exchange_strong(exp, 1)) {
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

static std::string relative_age(double ts) {
    double secs = lx::real_time() - ts;
    if (secs < 90) return "just now";
    if (secs < 5400) return lx::strprintf("%dm ago", (int)(secs / 60));
    if (secs < 129600) return lx::strprintf("%dh ago", (int)(secs / 3600));
    return lx::strprintf("%dd ago", (int)(secs / 86400));
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

static std::string fmt_mb(double bytes) { return bytes >= 1048576 ? lx::strprintf("%.1f MB", bytes / 1048576) : lx::strprintf("%.0f KB", bytes / 1024); }

static bool g_lite;

static void settings_load(App *a) {
    a->settings_path = lx::path_join(sd_config_dir(), "settings.ini");
    a->settings.load(a->settings_path);
    const lx::IniFile &s = a->settings;
    a->theme = ui_theme_find(s.get_string("ui", "theme").c_str());
    a->accent = s.get_string("ui", "accent");
    a->font_size = s.get_int("ui", "font_size", 11);
    if (a->font_size < 7 || a->font_size > 32) a->font_size = 11;
    a->font_family = s.get_string("ui", "font_family");
    a->show_ssh_config = s.get_bool("ui", "show_ssh_config", true);
    a->startup_shell = s.get_bool("ui", "startup_shell", false);
    a->follow_default = s.get_bool("ui", "follow_path", true);
    a->scrollback = s.get_int("ui", "scrollback", 10000);
    a->sb_ram_mb = (size_t)s.get_int("ui", "scrollback_ram_mb", 32);
    a->sb_disk_mb = (size_t)s.get_int("ui", "scrollback_disk_mb", 4096);
    a->sb_spill = s.get_bool("ui", "scrollback_spill", true);
    a->fast_cat = s.get_bool("ui", "fast_cat", true);
    a->hud = s.get_bool("ui", "performance_overlay", true);
    a->follow_fleet = s.get_bool("ui", "follow_fleetwm", true);
    a->rounded = s.get_bool("ui", "rounded_corners", true);
    a->pinned = s.get_bool("ui", "sidebar_pinned", true);
    if (s.has("ui", "sidebar_shown")) a->dock_shown = s.get_bool("ui", "sidebar_shown", true);
    if (s.has("ui", "sidebar_page")) a->side_page = s.get_int("ui", "sidebar_page", 0);
    a->panel_w = s.get_int("ui", "panel_width", 300);
    if (a->panel_w < 200 || a->panel_w > 700) a->panel_w = 300;
    if (g_lite) { a->lite = true; a->hud = true; a->pinned = false; a->dock_shown = false; a->side_open = false; }
}

void app_settings_save(App *a) {
    lx::IniFile &s = a->settings;
    s.set_string("ui", "theme", a->theme->name);
    s.set_string("ui", "accent", a->accent);
    s.set_int("ui", "font_size", a->font_size);
    s.set_bool("ui", "show_ssh_config", a->show_ssh_config);
    s.set_bool("ui", "startup_shell", a->startup_shell);
    bool follow = a->follow_default;
    for (auto &t : a->tabs) if (t->kind == TAB_SSH && t->files) follow = t->files->follow();
    a->follow_default = follow;
    s.set_bool("ui", "follow_path", a->follow_default);
    s.set_int("ui", "scrollback", a->scrollback);
    s.set_int("ui", "scrollback_ram_mb", (int)a->sb_ram_mb);
    s.set_int("ui", "scrollback_disk_mb", (int)a->sb_disk_mb);
    s.set_bool("ui", "scrollback_spill", a->sb_spill);
    if (!a->lite) s.set_bool("ui", "performance_overlay", a->hud);
    if (!a->lite) s.set_bool("ui", "sidebar_pinned", a->pinned);
    s.set_bool("ui", "fast_cat", a->fast_cat);
    if (!a->lite) s.set_bool("ui", "sidebar_shown", a->dock_shown);
    if (!a->lite) s.set_int("ui", "sidebar_page", a->side_page);
    s.set_bool("ui", "follow_fleetwm", a->follow_fleet);
    s.set_bool("ui", "rounded_corners", a->rounded);
    s.set_int("ui", "panel_width", a->panel_w);
    s.save(a->settings_path);
}

static void mark_terms_dirty(App *a) {
    for (auto &tb : a->tabs)
        if (tb->term) { tcore_lock(tb->term); vt_mark_all_dirty(tcore_vt(tb->term)); tcore_unlock(tb->term); }
}

void app_apply_theme(App *a, const UiTheme *t) {
    a->theme = t;
    const char *acc = a->follow_fleet && a->fleet.found && !a->fleet.accent.empty() ? a->fleet.accent.c_str() : (!a->accent.empty() ? a->accent.c_str() : t->accent);
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
    mark_terms_dirty(a);
    app_redraw(a);
}

void app_set_accent(App *a, const std::string &hex) {
    a->accent = hex;
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

static std::string pick_font_family() {
    const char *want[] = {"JetBrains Mono", "DejaVu Sans Mono", "Cascadia Mono", "Menlo", "Consolas", nullptr};
    char *f = font_pick_family(want);
    std::string out = f ? f : "monospace";
    free(f);
    return out;
}

void app_apply_font(App *a) {
    Font *nf = font_open(a->font_family.empty() ? "monospace" : a->font_family.c_str(), a->font_size * 96.0 / 72.0 * a->scale, true);
    if (!nf) return;
    if (a->term_font) font_close(a->term_font);
    a->term_font = nf;
    mark_terms_dirty(a);
    app_redraw(a);
}

Tab *app_cur_tab(App *a) { return a->cur >= 0 && a->cur < (int)a->tabs.size() ? a->tabs[(size_t)a->cur].get() : nullptr; }

static int tab_index(App *a, const Tab *t) {
    for (size_t i = 0; i < a->tabs.size(); i++) if (a->tabs[i].get() == t) return (int)i;
    return -1;
}

static void ended_prompt(App *a, Tab *t);

static void select_tab(App *a, int i) {
    if (i < 0 || i >= (int)a->tabs.size()) return;
    a->cur = i;
    Tab *t = a->tabs[(size_t)i].get();
    if (t->state == ST_ACTIVITY || t->state == ST_BELL) t->state = ST_IDLE;
    a->strip_scroll = -1;
    app_redraw(a);
    ended_prompt(a, t);
}

static void hook_wake(void *) { app_wake(); }
static void hook_activity(void *u) { Tab *t = static_cast<Tab *>(u); if (t->files) t->files->note_activity(); if (app_cur_tab(t->app) != t && t->state == ST_IDLE) t->state = ST_ACTIVITY; }
static void hook_bell(void *u) { Tab *t = static_cast<Tab *>(u); if (app_cur_tab(t->app) != t && (t->state == ST_IDLE || t->state == ST_ACTIVITY)) t->state = ST_BELL; }
static void hook_exited(void *u, int) {
    Tab *t = static_cast<Tab *>(u);
    t->state = ST_ENDED; t->asked = false;
    if (t->app->quit_on_exit) t->app->running = false;
    else ended_prompt(t->app, t);
}
static void hook_restarted(void *u) { Tab *t = static_cast<Tab *>(u); t->state = ST_IDLE; t->asked = false; }
static void hook_cwd(void *u, const char *p) { Tab *t = static_cast<Tab *>(u); if (t->files) t->files->set_cwd(p); }
static void hook_clip_set(void *, const char *text, bool primary) { if (primary) SDL_SetPrimarySelectionText(text); else SDL_SetClipboardText(text); }
static void hook_clip_request(void *u, bool primary) {
    Tab *t = static_cast<Tab *>(u);
    char *s = primary ? SDL_GetPrimarySelectionText() : SDL_GetClipboardText();
    if (s && *s && t->term) tcore_paste(t->term, s);
    SDL_free(s);
}

Tab::~Tab() {
    if (term) { tcore_close(term); tcore_free(term); }
    if (files) files->close();
    if (!control_path.empty()) sd_xfer_remove_control_path(control_path);
}

static void tab_destroy(App *a, Tab *t) {
    int idx = tab_index(a, t);
    if (idx < 0) return;
    a->tabs.erase(a->tabs.begin() + idx);
    if (a->cur >= (int)a->tabs.size()) a->cur = (int)a->tabs.size() - 1;
    else if (idx < a->cur) a->cur--;
    app_redraw(a);
}

void app_close_tab(App *a, Tab *t) {
    if (t->term && tcore_running(t->term))
        dlg_confirm(a, "Close session", "This session is still running. Close it?", "Close", [a, t] { if (tab_index(a, t) >= 0) tab_destroy(a, t); });
    else tab_destroy(a, t);
}

/* MobaXterm-style choices when a session ends (exit, logout, dropped ssh). Asked once per ending, for the visible tab. */
static void ended_prompt(App *a, Tab *t) {
    if (t->state != ST_ENDED || t->asked || app_cur_tab(a) != t || dlg_active(a)) return;
    t->asked = true;
    dlg_choice(a, "Session ended", "\"" + t->title + "\" has ended. What do you want to do?",
               {"Restart session", "Duplicate in a new tab", "Close tab", "Keep the tab open"}, [a, t](int i) {
                   if (tab_index(a, t) < 0) return;
                   if (i == 0) { if (t->term) tcore_restart(t->term); }
                   else if (i == 1) app_dup_tab(a, t);
                   else if (i == 2) tab_destroy(a, t);
                   app_redraw(a);
               });
}

static Tab *tab_add(App *a, TabKind kind, const std::string &title) {
    auto t = std::make_unique<Tab>();
    t->app = a; t->kind = kind; t->title = title;
    Tab *raw = t.get();
    a->tabs.push_back(std::move(t));
    select_tab(a, (int)a->tabs.size() - 1);
    return raw;
}

static bool g_fastcat = true;

void app_fastcat_apply(App *a) { tcore_set_native_cat(a->fast_cat && g_fastcat); }

static TermCore *make_term(App *a, Tab *t, const std::vector<std::string> &args, const char *cwd, bool fastcat) {
    TermHooks h{};
    h.wake = hook_wake; h.activity = hook_activity; h.bell = hook_bell; h.exited = hook_exited; h.restarted = hook_restarted;
    h.cwd = hook_cwd; h.clip_set = hook_clip_set; h.clip_request = hook_clip_request;
    int cols, rows;
    float cw = (float)a->W - (a->pinned && a->dock_shown ? P(a->panel_w) : 0) - P(34), ch = (float)a->H - P(130);
    tv_fit(a->term_font, cw, ch, &cols, &rows);
    std::vector<char *> argv;
    for (const std::string &s : args) argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);
    return tcore_new(argv.data(), cwd, cols, rows, &h, t, fastcat && g_fastcat);
}

std::vector<std::string> app_default_shell_argv(const std::string &shell) {
    std::string s = shell.empty() ? lx::env("SHELL") : shell;
    return {s.empty() ? "/bin/sh" : s};
}

Tab *app_open_local(App *a, const std::string &title, const std::vector<std::string> &argv, const char *cwd) {
    Tab *t = tab_add(a, TAB_LOCAL, title);
    t->term = make_term(a, t, argv, cwd, true);
    t->local_argv = argv;
    return t;
}

void app_set_tab_color(App *a, Tab *t, const std::string &hex) {
    t->color = hex;
    if (t->conn) a->store->set_color(t->conn->id, t->color);
    app_redraw(a);
}

static void ftp_open_with_password(App *a, const SdConn &c, const std::string &password) {
    Tab *t = tab_add(a, TAB_FTP, c.name);
    t->files = FilesPane::create(a, SdXfer::new_ftp(c, password), false);
    t->conn = std::make_unique<SdConn>(c);
    t->color = c.color;
}

static Tab *open_connection_copy(App *a, const SdConn &c) {
    if (c.protocol == "ftp" || c.protocol == "ftps") {
        if (!c.user.empty()) dlg_text(a, "Password for " + c.user + "@" + c.host, "", "", true, [a, c](const std::string &pw) { ftp_open_with_password(a, c, pw); });
        else ftp_open_with_password(a, c, "");
        return nullptr;
    }
    std::string cp = sd_xfer_new_control_path();
    Tab *t = tab_add(a, TAB_SSH, c.name);
    t->term = make_term(a, t, c.argv(cp.c_str()), nullptr, false);
    t->conn = std::make_unique<SdConn>(c);
    t->control_path = cp;
    t->color = c.color;
    t->files = FilesPane::create(a, SdXfer::new_ssh(c, cp), a->follow_default);
    return t;
}

Tab *app_open_connection(App *a, const SdConn &conn) {
    SdConn c = conn;
    a->store->touch(c.id);
    app_refresh(a);
    return open_connection_copy(a, c);
}

void app_dup_tab(App *a, Tab *t) {
    if (t->conn) app_open_connection(a, *t->conn);
    else if (!t->local_argv.empty()) app_open_local(a, t->title, t->local_argv, nullptr);
}

static void move_current_tab(App *a, int delta) {
    int i = a->cur, n = (int)a->tabs.size();
    if (i < 0 || i + delta < 0 || i + delta >= n) return;
    std::swap(a->tabs[(size_t)i], a->tabs[(size_t)(i + delta)]);
    a->cur = i + delta;
    a->strip_scroll = -1;
}

struct Row {
    std::string id, name, group, color, hay, tip;
    bool live;
};
struct Recent {
    std::string id, name, dest, age;
};

static std::vector<Row> rows;
static std::vector<Recent> recents;
static char meta_text[96];

void app_refresh(App *a) {
    a->store->reload_ssh_config(a->show_ssh_config);
    std::vector<SdConn *> all = a->store->all();
    std::stable_sort(all.begin(), all.end(), [](const SdConn *x, const SdConn *y) {
        int c = strcasecmp(x->group.c_str(), y->group.c_str());
        return c ? c < 0 : strcasecmp(x->name.c_str(), y->name.c_str()) < 0;
    });
    rows.clear();
    for (const SdConn *c : all) {
        Row r;
        r.id = c->id; r.name = c->name; r.group = c->group; r.color = c->color;
        r.hay = lx::utf8_lower(c->name + " " + c->host + " " + c->group);
        r.live = c->is_live();
        r.tip = r.live ? "ssh " + c->alias + "  (from ~/.ssh/config)" : lx::strprintf("%s://%s:%d", c->protocol.c_str(), c->host.c_str(), c->port);
        rows.push_back(std::move(r));
    }
    if (!all.empty()) snprintf(meta_text, sizeof meta_text, "%zu connection%s saved", all.size(), all.size() == 1 ? "" : "s");
    else snprintf(meta_text, sizeof meta_text, "No connections yet");
    recents.clear();
    for (const SdConn *c : a->store->recent(9)) recents.push_back({c->id, c->name, c->dest(), relative_age(c->last_used)});
    app_redraw(a);
}

/* Is the host's port reachable? Runs on a worker thread; the result is applied on the UI thread. */
static bool probe_port(const std::string &host, int port) {
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    bool ok = false;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) ok = true;
        else if (errno == EINPROGRESS) {
            struct pollfd pf = {fd, POLLOUT, 0};
            if (poll(&pf, 1, 3000) > 0) { int err = 0; socklen_t l = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l); ok = err == 0; }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return ok;
}

static void probe_all(App *a) {
    for (const SdConn *c : a->store->all()) {
        auto ok = std::make_shared<bool>(false);
        jobs_run([ok, host = c->host, port = c->port] { *ok = probe_port(host, port); },
                 [ok, id = c->id] {
                     if (g_app) { g_app->up[id] = *ok ? 1 : 2; app_redraw(g_app); }
                 });
    }
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


static std::vector<std::string> shells;
static uint32_t tab_swatch[8];
static Tab *ctx_tab;

static void find_shells() {
    for (const char *n : {"bash", "zsh", "fish", "sh", "dash", "ksh", "tcsh", "csh", "nu", "xonsh", "elvish", "pwsh"}) {
        std::string p = lx::find_program(n);
        if (!p.empty() && shells.size() < 16) shells.push_back(p);
    }
}

static void open_bar_menu(App *a, int which, float x, float y) {
    UiMenuItem it[48];
    int n = 0;
    auto add = [&](const char *label, const char *hint, int id, int flags) { it[n++] = UiMenuItem{label, hint, id, flags, nullptr, 0}; };
    auto sep = [&] { add(nullptr, nullptr, 0, MI_SEP); };
    static char sh_labels[16][64];
    if (which == 0) {
        add("New connection\xe2\x80\xa6", "Ctrl+N", A_NEW_CONN, 0);
        add("New local shell", "Ctrl+Shift+T", A_NEW_LOCAL, 0);
        if (!shells.empty()) { sep(); add("New local shell as\xe2\x80\xa6", nullptr, 0, MI_HEADER); }
        for (size_t i = 0; i < shells.size() && n < 40; i++) {
            snprintf(sh_labels[i], sizeof sh_labels[i], "   %s", lx::basename(shells[i]).c_str());
            add(sh_labels[i], nullptr, A_SHELL + (int)i, 0);
        }
        sep();
        add("Import Ansible inventory\xe2\x80\xa6", nullptr, A_IMPORT_INV, 0);
        add("Copy ~/.ssh/config hosts into saved connections", nullptr, A_IMPORT_SSH, 0);
        sep();
        add("Quit", "Ctrl+Q", A_QUIT, 0);
    } else if (which == 1) {
        add("Toggle sidebar", "Ctrl+B", A_SIDEBAR, 0);
        add("Sessions / Files sidebar", "Ctrl+Shift+B", A_FILES, 0);
        add("Bigger font", "Ctrl+=", A_BIGGER, 0);
        add("Smaller font", "Ctrl+-", A_SMALLER, 0);
        add("Re-check hosts", "F5", A_RECHECK, 0);
        sep();
        add("Theme", nullptr, 0, MI_HEADER);
        add("Follow fleetwm theme", nullptr, A_FLEET, MI_CHECK | (a->follow_fleet ? MI_CHECKED : 0) | (a->fleet.found ? 0 : MI_DISABLED));
        add("Rounded corners", nullptr, A_ROUND, MI_CHECK | (a->rounded ? MI_CHECKED : 0));
        for (int i = 0; i < UI_THEME_COUNT && n < 44; i++) add(UI_THEMES[i].name, nullptr, A_THEME + i, MI_CHECK | (a->theme == &UI_THEMES[i] ? MI_CHECKED : 0));
        add("Accent color\xe2\x80\xa6", nullptr, A_ACCENT, 0);
        add("Reset accent", nullptr, A_ACCENT_RESET, 0);
        sep();
        add("Scrollback", nullptr, 0, MI_HEADER);
        for (size_t i = 0; i < std::size(SB_PRESETS); i++) add(SB_PRESETS[i].label, nullptr, A_SB + (int)i, MI_CHECK | (a->scrollback == SB_PRESETS[i].lines ? MI_CHECKED : 0));
        add("Spill old scrollback to disk", nullptr, A_SPILL, MI_CHECK | (a->sb_spill ? MI_CHECKED : 0));
        sep();
        add("Fast cat in local tabs", nullptr, A_FASTCAT, MI_CHECK | (a->fast_cat ? MI_CHECKED : 0));
        add("Show performance overlay", nullptr, A_HUD, MI_CHECK | (a->hud ? MI_CHECKED : 0));
        add("Show hosts from ~/.ssh/config", nullptr, A_SSHCFG, MI_CHECK | (a->show_ssh_config ? MI_CHECKED : 0));
        add("Open a local shell on startup", nullptr, A_STARTUP, MI_CHECK | (a->startup_shell ? MI_CHECKED : 0));
    } else {
        add("About Lestrix", nullptr, A_ABOUT, 0);
    }
    ui_menu_open(a->ui, x, y, it, n, TAG_BAR + which);
}

static void open_tab_menu(App *a, Tab *t, float x, float y) {
    ctx_tab = t;
    for (int i = 0; i < 8; i++) tab_swatch[i] = ui_rgba(TAB_COLORS[i][1], 255);
    UiMenuItem it[] = {
        {"Rename\xe2\x80\xa6", nullptr, A_T_RENAME, 0, nullptr, 0},
        {"Tab color", nullptr, 0, MI_HEADER, nullptr, 0},
        {nullptr, nullptr, A_T_COLOR, 0, tab_swatch, 8},
        {"Custom color\xe2\x80\xa6", nullptr, A_T_CUSTOM, 0, nullptr, 0},
        {"Default color", nullptr, A_T_DEFAULT, 0, nullptr, 0},
        {nullptr, nullptr, 0, MI_SEP, nullptr, 0},
        {"Duplicate tab", "Ctrl+Shift+D", A_T_DUP, 0, nullptr, 0},
        {"Close tab", "Ctrl+Shift+W", A_T_CLOSE, 0, nullptr, 0},
        {"Close other tabs", nullptr, A_T_OTHERS, a->tabs.size() > 1 ? 0 : MI_DISABLED, nullptr, 0},
    };
    ui_menu_open(a->ui, x, y, it, (int)(sizeof it / sizeof *it), TAG_TAB);
}

static void rename_tab_dialog(App *a, Tab *t) {
    dlg_text(a, "Rename tab", "", t->title, false, [a, t](const std::string &text) { if (tab_index(a, t) >= 0) t->title = text; });
}

static void apply_history(App *a) {
    for (auto &t : a->tabs) if (t->term) tcore_set_history(t->term, a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
    tcore_set_history_defaults(a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
}

static void do_new_local(App *a, const std::string &shell) {
    std::vector<std::string> argv = app_default_shell_argv(shell);
    app_open_local(a, shell.empty() ? "local" : lx::basename(argv[0]), argv, nullptr);
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
    case A_NEW_CONN: dlg_conn_editor(a, nullptr, false); break;
    case A_NEW_LOCAL: do_new_local(a, ""); break;
    case A_IMPORT_INV:
        dlg_pick_file(a, "Ansible inventory (INI or YAML)", false, false, lx::home_dir(), [a](const std::vector<std::string> &paths) {
            if (paths.empty()) return;
            std::string err;
            auto items = sd_parse_ansible_inventory(paths[0], &err);
            if (!items) { dlg_message(a, "Import failed", err.empty() ? "unknown error" : err); return; }
            a->store->add_imported(std::move(*items));
            app_refresh(a);
        });
        break;
    case A_IMPORT_SSH:
        a->store->add_imported(sd_parse_ssh_config(lx::path_join(lx::home_dir(), ".ssh", "config"), false));
        app_refresh(a);
        break;
    case A_QUIT: a->running = false; break;
    case A_SIDEBAR: if (a->lite) break; if (a->pinned) { a->dock_shown = !a->dock_shown; app_settings_save(a); } else a->side_open = !a->side_open; ui_want_frames(a->ui, 0.3); break;
    case A_FILES: if (a->lite) break; side_select(a, a->side_page == 0 ? 1 : 0); break;
    case A_BIGGER: if (a->font_size < 32) { a->font_size++; app_apply_font(a); app_settings_save(a); } break;
    case A_SMALLER: if (a->font_size > 7) { a->font_size--; app_apply_font(a); app_settings_save(a); } break;
    case A_RECHECK: a->up.clear(); probe_all(a); break;
    case A_ACCENT: dlg_color(a, "Accent color", a->accent, [a](const std::string &text) { a->follow_fleet = false; app_set_accent(a, text); }); break;
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
        else if (id >= A_SHELL && id < A_SHELL + (int)shells.size()) do_new_local(a, shells[(size_t)(id - A_SHELL)]);
        break;
    }
}

static void run_tab_action(App *a, int id) {
    Tab *t = ctx_tab;
    if (!t || tab_index(a, t) < 0) return;
    if (id == A_T_RENAME) rename_tab_dialog(a, t);
    else if (id >= A_T_COLOR && id < A_T_COLOR + 8) app_set_tab_color(a, t, TAB_COLORS[id - A_T_COLOR][1]);
    else if (id == A_T_CUSTOM) dlg_color(a, "Tab color", t->color, [a, t](const std::string &text) { if (tab_index(a, t) >= 0) app_set_tab_color(a, t, text); });
    else if (id == A_T_DEFAULT) app_set_tab_color(a, t, "");
    else if (id == A_T_DUP) app_dup_tab(a, t);
    else if (id == A_T_CLOSE) app_close_tab(a, t);
    else if (id == A_T_OTHERS) {
        std::vector<Tab *> others;
        for (auto &o : a->tabs) if (o.get() != t) others.push_back(o.get());
        for (Tab *o : others) app_close_tab(a, o);
    }
}

static void run_row_action(App *a, int id) {
    SdConn *c = a->ctx_id.empty() ? nullptr : a->store->get(a->ctx_id);
    if (id == A_R_NEW) { dlg_conn_editor(a, nullptr, false); return; }
    if (!c) return;
    if (id == A_R_CONNECT) app_open_connection(a, *c);
    else if (id == A_R_EDIT) dlg_conn_editor(a, c, c->is_live());
    else if (id == A_R_DUP && !c->is_live()) {
        SdConn cp = *c;
        cp.id = SdConn().id;
        cp.name = c->name + " copy";
        cp.last_used = 0;
        a->store->upsert(cp);
        app_refresh(a);
    } else if (id == A_R_DELETE && !c->is_live()) {
        dlg_confirm(a, "Delete connection", "Delete \"" + c->name + "\"?", "Delete", [a, cid = c->id] { a->store->remove(cid); app_refresh(a); });
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
    float strip_h = a->tabs.size() ? P(36) : 0;
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
        mark_terms_dirty(a);
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
    if (ui_icon_button(u, R(r.x + r.w - pad - addw, y, addw, P(32)), IC_PLUS, "New connection (Ctrl+N)", 0)) dlg_conn_editor(a, nullptr, false);
    y += P(42);
    Rect list = R(r.x, y, r.w, r.y + r.h - y);
    std::string needle = lx::utf8_lower(a->search.s);
    float rowh = P(30), headh = P(26);
    float ch = 0;
    const std::string *prev = nullptr;
    for (const Row &row : rows) {
        if (!needle.empty() && row.hay.find(needle) == std::string::npos) continue;
        if (!prev || *prev != row.group) { ch += headh; prev = &row.group; }
        ch += rowh;
    }
    ui_scroll_begin(u, list, ch + P(10), &a->list_scroll);
    float yy = list.y - a->list_scroll;
    prev = nullptr;
    for (const Row &row : rows) {
        if (!needle.empty() && row.hay.find(needle) == std::string::npos) continue;
        if (!prev || *prev != row.group) {
            if (yy + headh > list.y && yy < list.y + list.h) ui_text(u, r.x + pad + P(4), yy + P(9), lx::utf8_upper(row.group).c_str(), c->muted);
            yy += headh;
            prev = &row.group;
        }
        Rect rr = R(r.x + P(4), yy, r.w - P(8), rowh);
        if (rr.y + rr.h > list.y && rr.y < list.y + list.h) {
            bool hov = ui_hover(u, rr) && rect_has(list, ui_mx(u), ui_my(u));
            bool sel = a->ctx_id == row.id;
            if (sel) ui_rrect(u, rr, c->accent, P(8));
            else if (hov) ui_rrect(u, rr, c->hover, P(8));
            auto up_it = a->up.find(row.id);
            int st = up_it == a->up.end() ? 0 : up_it->second;
            uint32_t dot = st == 1 ? c->up : st == 2 ? c->down : c->border2;
            ui_rrect(u, R(rr.x + P(12), rr.y + (rowh - P(8)) / 2, P(8), P(8)), dot, P(4));
            uint32_t tc = sel ? c->bg1 : !row.color.empty() ? ui_rgba(row.color.c_str(), 255) : c->ink;
            ui_text_fit(u, R(rr.x + P(30), rr.y, rr.w - P(38), rowh), row.name.c_str(), tc, 0);
            if (hov) {
                ui_tip(u, rr, row.tip.c_str());
                ui_set_cursor(u, 2);
                if (ui_mouse_pressed(u, 1, rr)) {
                    a->ctx_id = row.id;
                    if (ui_clicks(u) >= 2) { SdConn *cn = a->store->get(row.id); if (cn) { app_open_connection(a, *cn); ui_scroll_end(u); return; } }
                }
                if (ui_mouse_pressed(u, 3, rr)) {
                    a->ctx_id = row.id;
                    SdConn *cn = a->store->get(row.id);
                    bool live = cn && cn->is_live();
                    UiMenuItem it[] = {
                        {"Connect", nullptr, A_R_CONNECT, 0, nullptr, 0},
                        {live ? "Save a copy to edit\xe2\x80\xa6" : "Edit\xe2\x80\xa6", nullptr, A_R_EDIT, 0, nullptr, 0},
                        {"Duplicate", nullptr, A_R_DUP, live ? MI_DISABLED : 0, nullptr, 0},
                        {"Delete", nullptr, A_R_DELETE, live ? MI_DISABLED : 0, nullptr, 0},
                        {nullptr, nullptr, 0, MI_SEP, nullptr, 0},
                        {"New connection\xe2\x80\xa6", nullptr, A_R_NEW, 0, nullptr, 0},
                    };
                    ui_menu_open(u, ui_mx(u), ui_my(u), it, 6, TAG_ROW);
                }
            }
        }
        yy += rowh;
    }
    if (rows.empty()) ui_text(u, r.x + pad, list.y + P(10), "No connections yet. Press + to add one.", c->muted);
    ui_scroll_end(u);
}

static void draw_files_page(App *a, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    Tab *t = app_cur_tab(a);
    if (t && t->kind == TAB_SSH && t->files) {
        ui_text(u, r.x + P(14), r.y + P(12), lx::ascii_upper(t->title).c_str(), c->muted);
        t->files->draw(a, R(r.x, r.y + P(34), r.w, r.h - P(34)));
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
    for (auto &tp : a->tabs) {
        Tab *t = tp.get();
        float w = ui_text_w(u, t->title.c_str()) + P(16 + 8 + 8 + 14 + 22);
        t->w = w < P(110) ? P(110) : w > P(230) ? P(230) : w;
        total += t->w;
    }
    bool overflow = total > tabs_area.w;
    float chev = overflow ? P(24) : 0;
    Rect view = R(tabs_area.x + chev, s.y, tabs_area.w - 2 * chev, s.h);
    float maxs = total > view.w ? total - view.w : 0;
    if (a->strip_scroll < 0) {
        float tx = 0;
        for (int i = 0; i < a->cur && i < (int)a->tabs.size(); i++) tx += a->tabs[(size_t)i]->w;
        float tw = a->cur < (int)a->tabs.size() ? a->tabs[(size_t)a->cur]->w : 0;
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
    for (size_t i = 0; i < a->tabs.size(); i++) {
        Tab *t = a->tabs[i].get();
        t->x = tx;
        Rect r = R(tx, s.y, t->w, s.h);
        tx += t->w;
        if (r.x + r.w < view.x || r.x > view.x + view.w) continue;
        bool active = (int)i == a->cur;
        bool hov = ui_hover(u, r) && rect_has(view, ui_mx(u), ui_my(u));
        /* A coloured tab gets the colour as its background, with dark or light text picked for contrast, so it reads on any colour. */
        bool coloured = !t->color.empty();
        uint32_t tint = active ? c->ink : c->ink2, icon_col = c->muted, close_col = active || hov ? c->ink2 : c->muted;
        if (coloured) {
            uint32_t base = ui_rgba(t->color.c_str(), 255);
            uint32_t fill = active ? base : ui_mix(c->bg0, base, hov ? 0.85 : 0.65);
            double lum = (0.299 * (fill & 255) + 0.587 * ((fill >> 8) & 255) + 0.114 * ((fill >> 16) & 255)) / 255.0;
            uint32_t fg = lum > 0.55 ? RGBA(16, 18, 20, 255) : RGBA(255, 255, 255, 255);
            ui_rrect(u, R(r.x + P(1), r.y + P(3), r.w - P(2), r.h - P(3)), fill, P(6));
            if (active) ui_rect(u, R(r.x + P(1), r.y + r.h - P(2), r.w - P(2), P(2)), fg);
            tint = icon_col = close_col = fg;
        } else {
            if (active) { ui_rect(u, r, a->pal.bg); ui_rect(u, R(r.x, r.y + r.h - P(2), r.w, P(2)), c->accent); }
            else if (hov) ui_rect(u, r, c->hover);
        }
        ui_icon(u, kind_icon(t->kind), R(r.x + P(10), r.y + (r.h - P(16)) / 2, P(16), P(16)), coloured ? icon_col : c->muted);
        float lx = r.x + P(10 + 16 + 8);
        float dotw = t->state == ST_IDLE ? 0 : P(14);
        ui_text_fit(u, R(lx, r.y, r.w - P(10 + 16 + 8) - P(30) - dotw, r.h), t->title.c_str(), tint, 0);
        if (t->state != ST_IDLE) {
            uint32_t dc = coloured ? tint : t->state == ST_ACTIVITY ? c->blue : t->state == ST_BELL ? c->red : c->muted;
            ui_rrect(u, R(r.x + r.w - P(30) - P(10), r.y + (r.h - P(8)) / 2, P(8), P(8)), dc, P(4));
        }
        Rect cx = R(r.x + r.w - P(26), r.y + (r.h - P(20)) / 2, P(20), P(20));
        bool chov = ui_hover(u, cx);
        if (chov) ui_rrect(u, cx, coloured ? 0x40000000u : c->hover, P(4));
        ui_icon(u, IC_CLOSE, rect_inset(cx, P(3)), chov && !coloured ? c->red : close_col);
        if (hov) ui_set_cursor(u, 2);
        if (chov && ui_mouse_pressed(u, 1, cx)) close_idx = (int)i;
        else if (hov && ui_mouse_pressed(u, 1, r)) {
            select_tab(a, (int)i);
            a->strip_scroll = a->strip_scroll < 0 ? 0 : a->strip_scroll;
            a->drag_tab = (int)i; a->drag_dx = ui_mx(u) - r.x; a->drag_moved = false;
            if (ui_clicks(u) >= 2) rename_tab_dialog(a, t);
        } else if (hov && ui_mouse_pressed(u, 2, r)) close_idx = (int)i;
        else if (hov && ui_mouse_pressed(u, 3, r)) { select_tab(a, (int)i); open_tab_menu(a, t, ui_mx(u), ui_my(u)); }
    }
    ui_unclip(u);
    if (a->drag_tab >= 0) {
        if (ui_mouse_down(u, 1) && a->drag_tab < (int)a->tabs.size()) {
            float mx = ui_mx(u);
            Tab *dt = a->tabs[(size_t)(a->drag_tab)].get();
            if (fabsf(mx - (dt->x + a->drag_dx)) > P(6)) a->drag_moved = true;
            if (a->drag_moved) {
                int i = a->drag_tab;
                if (i > 0) { Tab *l = a->tabs[(size_t)(i - 1)].get(); if (mx < l->x + l->w / 2 + P(0)) { std::swap(a->tabs[(size_t)i], a->tabs[(size_t)(i - 1)]); a->drag_tab--; a->cur = a->drag_tab; } }
                if (i < (int)a->tabs.size() - 1) { Tab *rr = a->tabs[(size_t)(i + 1)].get(); if (mx > rr->x + rr->w / 2) { std::swap(a->tabs[(size_t)i], a->tabs[(size_t)(i + 1)]); a->drag_tab++; a->cur = a->drag_tab; } }
            }
        } else a->drag_tab = -1;
    }
    if (close_idx >= 0) app_close_tab(a, a->tabs[(size_t)(close_idx)].get());
}

static void draw_welcome(App *a, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    ui_rect(u, r, a->pal.bg);
    float w = fminf(P(620), r.w - P(60)), x = r.x + (r.w - w) / 2, y = r.y + fmaxf(P(40), (r.h - P(120) - ((float)recents.size() + 1) * P(40)) / 3);
    ui_text_font(u, a->big_font, FS_BOLD, x, y, "Lestrix", c->ink);
    y += font_cell_h(a->big_font) + P(2);
    ui_text(u, x, y, meta_text, c->muted);
    y += P(36);
    ui_text(u, x, y, "RECENT", c->muted);
    y += P(26);
    int nrecent = (int)recents.size();
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
        ui_text_font(u, a->ui_font, FS_BOLD, row.x + P(42), row.y + (row.h - ui_line_h(u)) / 2, local ? "Local shell" : recents[(size_t)i].name.c_str(), c->ink);
        ui_text_fit(u, R(row.x + P(200), row.y, w - P(200) - P(100), row.h), local ? "this machine" : recents[(size_t)i].dest.c_str(), c->ink2, 0);
        if (!local) ui_text_fit(u, R(row.x + w - P(90), row.y, P(86), row.h), recents[(size_t)i].age.c_str(), c->muted, 2);
        if (hov) ui_set_cursor(u, 2);
        if (ui_mouse_pressed(u, 1, row)) {
            if (local) do_new_local(a, "");
            else { SdConn *cn = a->store->get(recents[(size_t)i].id); if (cn) { app_open_connection(a, *cn); return; } }
        }
        y += P(38);
    }
    if (recents.empty()) ui_text(u, x, y + P(14), "Double-click a host on the left, or press + to add one.", c->muted);
}

static void draw_content(App *a) {
    Ui *u = a->ui;
    Rect r = lay.content;
    Tab *t = app_cur_tab(a);
    if (!t) { draw_welcome(a, R(r.x, r.y, r.w, r.h)); term_rect = R(0, 0, 0, 0); return; }
    if (!t->term) {
        term_rect = R(0, 0, 0, 0);
        if (t->files) t->files->draw(a, r);
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
        snprintf(hist, sizeof hist, "scrlbck %ld ln, %s mem, %s disk", st.lines, fmt_mb((double)(st.hot_bytes + st.packed_bytes)).c_str(), fmt_mb((double)st.disk_bytes).c_str());
    }
    std::string rss = fmt_mb((double)proc_rss_kb() * 1024);
    snprintf(a->hud_seg[0], sizeof a->hud_seg[0], "fps %.0f", fps);
    snprintf(a->hud_seg[1], sizeof a->hud_seg[1], "%s", dims);
    snprintf(a->hud_seg[2], sizeof a->hud_seg[2], "parse %.3f GB/s", rate / 1024);
    snprintf(a->hud_parse_tip, sizeof a->hud_parse_tip, "Parse rate: program output the terminal engine consumed over the last second. Right now: %.0f MB/s.", rate);
    snprintf(a->hud_seg[3], sizeof a->hud_seg[3], "cpu %.0f%% (%ld)", cpu, sysconf(_SC_NPROCESSORS_ONLN));
    snprintf(a->hud_seg[4], sizeof a->hud_seg[4], "irq %s%s", irq_txt, a->hud_irq_total ? "" : "/s");
    snprintf(a->hud_seg[5], sizeof a->hud_seg[5], "memory %s", rss.c_str());
    snprintf(a->hud_seg[6], sizeof a->hud_seg[6], "%d thr/%uc", proc_threads(), (unsigned)sysconf(_SC_NPROCESSORS_ONLN));
    { char rr_[24]; fmt_count(rd_rate, rr_, sizeof rr_); snprintf(a->hud_seg[7], sizeof a->hud_seg[7], "reads %s/s @%.0fK", rr_, rd_avg / 1024); }
    snprintf(a->hud_seg[8], sizeof a->hud_seg[8], "%s", cache);
    snprintf(a->hud_seg[9], sizeof a->hud_seg[9], "%s", hist);
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
    for (auto &t : a->tabs) if (t->term) total += tcore_bytes_fed(t->term);
    if (total != a->trim_prev_bytes) { a->trim_prev_bytes = total; a->trim_dirty = true; return; }
    if (!a->trim_dirty) return;
    a->trim_dirty = false;
    for (auto &t : a->tabs) if (t->term) tcore_compact(t->term);
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
    int n = (int)a->tabs.size();
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
        if (k == SDLK_0) { do_new_local(a, ""); return true; }
        if (k >= SDLK_1 && k <= SDLK_9 && k - SDLK_1 < (int)recents.size()) {
            SdConn *cn = a->store->get(recents[(size_t)(k - SDLK_1)].id);
            if (cn) app_open_connection(a, *cn);
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
        if (t && t->files) t->files->drop(e->drop.file);
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
        if (e->type == EV_WAKE) wake_pending.store(0);
        break;
    }
}


static void write_ppm(App *a, const char *path) {
    uint8_t *px = static_cast<uint8_t*>(malloc((size_t)a->W * a->H * 4));
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

struct Startup {
    bool local = false;
    const char *cwd = nullptr, *theme = nullptr, *connect = nullptr;
    std::vector<std::string> exec;
};

/* The --script test driver: steps like "w:0.5" (wait), "c:x,y" (click), "k:escape", "t:text", "s:file.ppm" (screenshot). */
struct Script {
    std::vector<std::string> steps;
    size_t i = 0;
    double wait_until = 0;
};

static void script_key(const std::string &spec) {
    Uint16 mod = 0;
    std::string name = spec;
    size_t plus;
    while ((plus = name.find('+')) != std::string::npos && plus + 1 < name.size()) {
        std::string m = name.substr(0, plus);
        if (m == "ctrl") mod |= KMOD_CTRL; else if (m == "shift") mod |= KMOD_SHIFT; else if (m == "alt") mod |= KMOD_ALT;
        name.erase(0, plus + 1);
    }
    SDL_Keycode k = SDL_GetKeyFromName(name.c_str());
    if (k == SDLK_UNKNOWN) return;
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN; e.key.state = SDL_PRESSED; e.key.keysym.sym = k; e.key.keysym.mod = mod;
    SDL_PushEvent(&e);
}

static void script_mouse(int type, int button, int x, int y) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; }
    else { e.button.x = x; e.button.y = y; e.button.button = (Uint8)button; e.button.clicks = 1; e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED; }
    SDL_PushEvent(&e);
}

static bool script_run(App *a, Script &sc, std::string &shot_out) {
    if (sc.steps.empty() || sc.i >= sc.steps.size() || a->now < sc.wait_until) return false;
    const std::string &st = sc.steps[sc.i++];
    int x, y, sx, sy;
    double w;
    if (sscanf(st.c_str(), "w:%lf", &w) == 1) sc.wait_until = a->now + w;
    else if (sscanf(st.c_str(), "m:%d,%d", &x, &y) == 2) script_mouse(SDL_MOUSEMOTION, 0, x, y);
    else if (sscanf(st.c_str(), "c:%d,%d", &x, &y) == 2 || sscanf(st.c_str(), "d:%d,%d", &x, &y) == 2) {
        script_mouse(SDL_MOUSEMOTION, 0, x, y);
        script_mouse(SDL_MOUSEBUTTONDOWN, 1, x, y);
        script_mouse(SDL_MOUSEBUTTONUP, 1, x, y);
        sc.wait_until = a->now + 0.12;
    } else if (sscanf(st.c_str(), "r:%d,%d", &x, &y) == 2) {
        script_mouse(SDL_MOUSEMOTION, 0, x, y);
        script_mouse(SDL_MOUSEBUTTONDOWN, 3, x, y);
        script_mouse(SDL_MOUSEBUTTONUP, 3, x, y);
        sc.wait_until = a->now + 0.12;
    } else if (sscanf(st.c_str(), "p:%d,%d,%d,%d", &x, &y, &sx, &sy) == 4) {
        script_mouse(SDL_MOUSEMOTION, 0, x, y);
        script_mouse(SDL_MOUSEBUTTONDOWN, 1, x, y);
        script_mouse(SDL_MOUSEMOTION, 0, (x + sx) / 2, (y + sy) / 2);
        script_mouse(SDL_MOUSEMOTION, 0, sx, sy);
        script_mouse(SDL_MOUSEBUTTONUP, 1, sx, sy);
        sc.wait_until = a->now + 0.15;
    } else if (sscanf(st.c_str(), "x:%d", &x) == 1) {
        SDL_Event we;
        SDL_zero(we);
        we.type = SDL_MOUSEWHEEL; we.wheel.y = x; we.wheel.preciseY = (float)x;
        SDL_PushEvent(&we);
        sc.wait_until = a->now + 0.1;
    } else if (st.starts_with("k:")) { script_key(st.substr(2)); sc.wait_until = a->now + 0.08; }
    else if (st.starts_with("t:")) { SDL_Event e; SDL_zero(e); e.type = SDL_TEXTINPUT; snprintf(e.text.text, sizeof e.text.text, "%s", st.c_str() + 2); SDL_PushEvent(&e); sc.wait_until = a->now + 0.08; }
    else if (st.starts_with("s:")) { shot_out = st.substr(2); sc.wait_until = a->now + 0.3; }
    a->dirty = true;
    return true;
}

static bool cmd_pending(App *a) {
    for (auto &t : a->tabs) if (t->term && tcore_cmd_active(t->term)) return true;
    return false;
}

static void cmd_poll_all(App *a) {
    for (auto &t : a->tabs) {
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
    for (size_t i = 0; i < a->tabs.size(); i++) {
        Tab *t = a->tabs[i].get();
        if (t->term && tcore_pump(t->term)) app_redraw(a);
        if (t->files) t->files->tick(a);
    }
}

static void print_task_cpu() {
    for (const std::string &n : lx::list_dir("/proc/self/task")) {
        auto txt = lx::read_file("/proc/self/task/" + n + "/stat");
        if (!txt) continue;
        size_t rp = txt->rfind(')'), lp = txt->find('(');
        unsigned long ut = 0, st = 0;
        if (rp != std::string::npos && lp != std::string::npos && sscanf(txt->c_str() + rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2 && ut + st > 3)
            printf("  thread %-16s user %.2fs sys %.2fs\n", txt->substr(lp + 1, rp - lp - 1).c_str(), (double)ut / 100.0, (double)st / 100.0);
    }
}

static const char *HELP_TEXT =
    "Usage: lestrix [--local] [--working-directory DIR] [-e COMMAND [ARGS...]]\n"
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
    "  --no-fastcat          do not take over `cat FILE` in a local tab to print big files at memory speed\n"
    "  --theme NAME          Xylonic Dark, Xylonic Light, Graphite, Nord, Gruvbox or Solarized Dark\n"
    "  -e COMMAND ...        run COMMAND in a new tab (works as x-terminal-emulator)\n\n" APP_CREDIT;

int main(int argc, char **argv) {
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    mallopt(M_TRIM_THRESHOLD, 256 * 1024);
    signal(SIGCHLD, SIG_DFL);  /* a launcher may start us with SIGCHLD ignored; that survives exec and breaks waitpid here and in every shell child (git: "waitpid failed: No child processes") */
    Startup s;
    const char *flood = nullptr, *run_cmd = nullptr, *shot = nullptr, *script_arg = nullptr;
    double shot_after = 3.0;
    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        bool more = i + 1 < argc;
        if (arg == "--version") { printf("Lestrix %s\n" APP_CREDIT, APP_VERSION); return 0; }
        if (arg == "--benchmark") return sd_benchmark(stdout, 8.0);
        if (arg == "--max-fps" && more) { g_max_fps = atof(argv[++i]); continue; }
        if (arg == "--read-delay" && more) { tcore_read_delay_us = atoi(argv[++i]); continue; }
        if (arg == "--no-fastcat") { g_fastcat = false; continue; }
        if (arg == "--render-threads" && more) { g_render_threads = atoi(argv[++i]); continue; }
        if (arg == "--io-threads" && more) { tcore_io_threads = atoi(argv[++i]); continue; }
        if (arg == "--parse-threads" && more) { g_parse_threads = atoi(argv[++i]); continue; }
        if (arg == "--compress-threads" && more) { g_compress_threads = atoi(argv[++i]); continue; }
        if (arg == "--io-fps" && more) { g_io_fps = atof(argv[++i]); continue; }
        if (arg == "--help" || arg == "-h") { fputs(HELP_TEXT, stdout); return 0; }
        if (arg == "--local") s.local = true;
        else if (arg == "--lite") { g_lite = true; s.local = true; }
        else if (arg == "--no-fast-output") tcore_set_fast_output(false);
        else if (arg == "--working-directory" && more) s.cwd = argv[++i];
        else if (arg == "--theme" && more) s.theme = argv[++i];
        else if (arg == "--connect" && more) s.connect = argv[++i];
        else if (arg == "--flood" && more) flood = argv[++i];
        else if (arg == "--run" && more) run_cmd = argv[++i];
        else if (arg == "--shot" && more) shot = argv[++i];
        else if (arg == "--script" && more) script_arg = argv[++i];
        else if (arg == "--shot-after" && more) shot_after = atof(argv[++i]);
        else if (arg == "-e" || arg == "--execute") {
            for (int k = i + 1; k < argc; k++) s.exec.push_back(argv[k]);
            break;
        }
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--lite")) g_lite = true;
    setenv("SDL_VIDEO_X11_WMCLASS", g_lite ? "lestrix-lite" : "lestrix", 0);
    setenv("SDL_VIDEO_WAYLAND_WMCLASS", g_lite ? "lestrix-lite" : "lestrix", 0);
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    tmark("main entered");
    std::thread prewarm(font_prewarm);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); prewarm.join(); return 1; }
    EV_WAKE = SDL_RegisterEvents(1);
    App *a = new App();
    g_app = a;
    tmark("SDL_Init done");
    {
        GlKind order[3] = {GLK_CORE, GLK_ES3, GLK_ES2};
        int n = 3, first = 0;
        const char *force = getenv("LESTRIX_GL");
        if (force) { first = !strcmp(force, "es3") ? 1 : !strcmp(force, "es2") ? 2 : 0; n = first + 1; }
        bool ok = false;
        for (int i = first; i < n && !ok; i++) ok = gl_init(a, order[i]);
        if (!ok) { fprintf(stderr, "Lestrix needs OpenGL 3.3, OpenGL ES 3.0 or OpenGL ES 2.0\n"); prewarm.join(); return 1; }
        if (getenv("LESTRIX_TIMING")) fprintf(stderr, "OpenGL: %s (%s)\n", a->gl_desc, sd_gl_kind == GLK_CORE ? "desktop core" : sd_gl_kind == GLK_ES3 ? "ES 3.0" : "ES 2.0");
    }
    tmark("GL functions loaded");
    prewarm.join();
    tmark("fontconfig warmed");
    vt_set_threads(g_parse_threads, g_compress_threads);
    wp_configure(g_render_threads);
    settings_load(a);
    app_fastcat_apply(a);
    tmark("settings loaded");
    if (s.theme) a->theme = ui_theme_find(s.theme);
    if (a->font_family.empty()) a->font_family = pick_font_family();
    a->ui_font = font_open("sans", 13 * a->scale, false);
    a->big_font = font_open("sans", 34 * a->scale, false);
    a->term_font = font_open(a->font_family.c_str(), a->font_size * 96.0 / 72.0 * a->scale, true);
    if (!a->term_font) a->term_font = font_open("monospace", a->font_size * 96.0 / 72.0 * a->scale, true);
    if (!a->ui_font || !a->term_font || !a->big_font) { fprintf(stderr, "no usable fonts found\n"); return 1; }
    tmark("fonts opened");
    a->ui_mono = font_open("monospace", 12 * a->scale, true);
    tmark("atlas and renderer ready");
    a->ui = ui_new(a->r, a->ui_font, a->ui_mono, a->scale);
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
    a->store = std::make_unique<SdStore>();
    tmark("store loaded");
    tcore_set_history_defaults(a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
    jobs_init(app_wake);
    find_shells();
    tmark("shells found");
    app_refresh(a);
    tmark("connections refreshed");
    probe_all(a);
    tmark("probes started");
    SDL_StartTextInput();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    if (!s.exec.empty()) {
        a->quit_on_exit = true;
        app_open_local(a, lx::basename(s.exec[0]), s.exec, s.cwd);
    } else if (flood || run_cmd) {
        app_open_local(a, "local", app_default_shell_argv(""), nullptr);
    } else if (s.connect) {
        for (SdConn *c : a->store->all())
            if (c->name == s.connect || c->id == s.connect) { app_open_connection(a, *c); break; }
    } else if (s.local || a->startup_shell) {
        app_open_local(a, "local", app_default_shell_argv(""), s.cwd);
    }
    double t0 = now_s(), last_draw = 0;
    a->next_probe = t0 + 30; a->next_hud = t0 + 1; a->next_trim = t0 + 5;
    a->last_blink = t0;
    uint64_t last_draw_bytes = 0;
    bool flood_sent = false;
    double flood_t = 0;
    Script script;
    if (script_arg) script.steps = lx::split(script_arg, ';');
    std::string script_shot;
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
        {
            std::string shot_name;
            while (script_run(a, script, shot_name)) {
                if (!shot_name.empty()) script_shot = std::move(shot_name);
                if (a->now < script.wait_until) break;
            }
        }
        if (flood && !flood_sent && a->now - t0 > 1.0 && app_cur_tab(a)) {
            tcore_send_str(app_cur_tab(a)->term, lx::strprintf("cat '%s'; echo FLOOD_DONE_$((6*7))\r", flood).c_str());
            flood_sent = true; flood_t = a->now;
        }
        if (run_cmd && !flood_sent && a->now - t0 > 1.0 && app_cur_tab(a)) { tcore_send_str(app_cur_tab(a)->term, run_cmd); flood_sent = true; }
        if (flood && flood_sent && app_cur_tab(a) && tcore_screen_contains(app_cur_tab(a)->term, "FLOOD_DONE_42")) {
            printf("flood finished in %.2fs\n", a->now - flood_t);
            if (getenv("LESTRIX_THREADS")) print_task_cpu();
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
            if (!script_shot.empty()) {
                write_ppm(a, script_shot.c_str());
                script_shot.clear();
            }
            if (!script.steps.empty() && script.i >= script.steps.size() && a->now > script.wait_until + 0.2 && !shot) a->running = false;
            if (shot && a->now - t0 > shot_after) {
                write_ppm(a, shot);
                a->running = false;
            }
            SDL_GL_SwapWindow(a->win);
        }
    }
    app_settings_save(a);
    for (auto &t : a->tabs) if (t->term) tcore_close(t->term);
    for (auto &t : a->tabs) if (!t->control_path.empty()) sd_xfer_remove_control_path(t->control_path);
    jobs_shutdown();
    return 0;
}
