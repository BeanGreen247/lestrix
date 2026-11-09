/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* The scrollback find bar and clickable links (OSC 8 hyperlinks and plain URLs). */

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"

#define P(v) S(a->ui, (v))

/* ---------------------------------------------------------------- find bar */

struct Find {
    bool open = false, focus_pending = false, cs = false, word = false, want_jump = false;
    UiText in{};
    char buf[128] = "";
    Finder f;
    TermCore *term = nullptr;
    std::string q;
    bool q_cs = false, q_word = false;
    int cur = -1;
    uint64_t fed = 0;
    double rescan_at = 0;
    Rect bar{};
};

bool find_is_open(const App *a) { return a->find && a->find->open; }

bool find_hit(const App *a, float x, float y) {
    if (!find_is_open(a)) return false;
    const Rect &b = a->find->bar;
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void find_open(App *a) {
    if (!a->find) {
        a->find = new Find;
        ui_text_init(&a->find->in, a->find->buf, (int)sizeof a->find->buf);
    }
    Find *F = a->find;
    if (!F->open) {
        F->open = true;
        F->q = "\x01";   /* never equal to the box: the first frame starts a search */
        F->cur = -1;
        F->want_jump = true;
        F->term = nullptr;
        Tab *t = app_cur_tab(a);
        if (t && t->term && tcore_has_selection(t->term)) {
            char *sel = tcore_selection_text(t->term);
            if (sel) {
                char *nl = strpbrk(sel, "\r\n");
                if (nl) *nl = 0;
                if (strlen(sel) < sizeof F->buf) ui_text_set(&F->in, sel);
                free(sel);
            }
        }
    }
    F->focus_pending = true;
    app_redraw(a);
}

void find_close(App *a) {
    Find *F = a->find;
    if (!F || !F->open) return;
    F->open = false;
    F->f = Finder();
    F->cur = -1;
    F->term = nullptr;
    ui_release_focus(a->ui);
    app_redraw(a);
}

/* Brings match `cur` into view without moving the screen when it is already visible. */
static void jump_to(Find *F) {
    const std::vector<FindMatch> &m = F->f.matches();
    if (F->cur < 0 || F->cur >= (int)m.size() || !F->term) return;
    int idx = m[(size_t)F->cur].idx, rows = tcore_rows(F->term), off = tcore_offset(F->term);
    if (idx >= -off && idx <= rows - 1 - off) return;
    tcore_scroll(F->term, (idx >= 0 ? 0 : rows / 3 - idx) - off);
}

/* dir > 0: the next older match (Enter), dir < 0: the next newer one (Shift+Enter). */
static void navigate(App *a, int dir) {
    Find *F = a->find;
    int n = (int)F->f.matches().size();
    if (!n) return;
    F->cur = F->cur < 0 ? (dir > 0 ? 0 : n - 1) : (F->cur + dir + n) % n;
    jump_to(F);
    app_redraw(a);
}

bool find_key(App *a, SDL_Keycode k, Uint16 mods) {
    if (!find_is_open(a)) return false;
    if (k == SDLK_ESCAPE) { find_close(a); return true; }
    if (k == SDLK_F3) { navigate(a, (mods & KMOD_SHIFT) ? -1 : 1); return true; }
    return false;
}

void find_frame(App *a, Tab *t, Rect r) {
    Find *F = a->find;
    if (!F || !F->open || !t->term) return;
    Ui *u = a->ui;
    const UiColors *c = &a->colors;
    bool changed = F->q != F->buf || F->cs != F->q_cs || F->word != F->q_word || F->term != t->term;
    uint64_t fed = tcore_bytes_fed(t->term);
    bool rescan = !changed && F->f.active() && F->f.done() && fed != F->fed && a->now >= F->rescan_at;
    if (changed || rescan) {
        F->f.start(F->buf, F->cs, F->word);
        F->q = F->buf; F->q_cs = F->cs; F->q_word = F->word;
        F->term = t->term;
        F->fed = fed;
        F->rescan_at = a->now + 0.4;
        F->cur = -1;
        if (changed) F->want_jump = true;
    }
    if (!F->f.done()) {
        tcore_lock(t->term);
        F->f.step(tcore_vt(t->term), 3000);
        tcore_unlock(t->term);
        ui_want_frames(u, 0.03);
        app_redraw(a);
    }
    if (F->want_jump) {
        if (!F->f.matches().empty()) { F->cur = 0; jump_to(F); F->want_jump = false; }
        else if (F->f.done()) F->want_jump = false;
    }

    float w = fminf(P(480), r.w - P(20)), h = P(36);
    Rect bar = R(r.x + r.w - w - P(14), r.y + P(8), w, h);
    F->bar = bar;
    ui_rrect(u, bar, c->bg1, P(9));
    ui_outline(u, bar, c->border2, P(9), 1);
    float by = bar.y + P(5), bh = h - P(10), x = bar.x + bar.w - P(6);
    Rect close_r = R(x - P(26), by, P(26), bh); x = close_r.x;
    Rect newer_r = R(x - P(26), by, P(26), bh); x = newer_r.x;
    Rect older_r = R(x - P(26), by, P(26), bh); x = older_r.x - P(4);
    Rect word_r = R(x - P(28), by, P(28), bh); x = word_r.x;
    Rect case_r = R(x - P(32), by, P(32), bh); x = case_r.x - P(6);
    Rect count_r = R(x - P(92), by, P(92), bh); x = count_r.x - P(4);
    Rect in_r = R(bar.x + P(6), by, x - bar.x - P(6), bh);
    if (F->focus_pending) { ui_input_focus(u, &F->in); F->focus_pending = false; }
    if (ui_input(u, in_r, &F->in, "Find in scrollback", false)) navigate(a, (SDL_GetModState() & KMOD_SHIFT) ? -1 : 1);

    const std::vector<FindMatch> &m = F->f.matches();
    char cnt[64] = "";
    uint32_t cc = c->muted;
    if (F->f.active()) {
        if (m.empty()) { snprintf(cnt, sizeof cnt, "%s", F->f.done() ? "No results" : "Searching\xe2\x80\xa6"); if (F->f.done()) cc = c->red; }
        else {
            const char *more = F->f.truncated() ? "+" : F->f.done() ? "" : "\xe2\x80\xa6";
            if (F->cur >= 0) snprintf(cnt, sizeof cnt, "%d of %zu%s", F->cur + 1, m.size(), more);
            else snprintf(cnt, sizeof cnt, "%zu found%s", m.size(), more);
        }
    }
    ui_text_fit(u, count_r, cnt, cc, 2);
    if (ui_button(u, case_r, "Aa", UB_FLAT | (F->cs ? UB_ACTIVE : 0))) F->cs = !F->cs;
    ui_tip(u, case_r, "Match case");
    if (ui_button(u, word_r, "W", UB_FLAT | (F->word ? UB_ACTIVE : 0))) F->word = !F->word;
    ui_tip(u, word_r, "Whole word");
    if (ui_icon_button(u, older_r, IC_UP, "Older match (Enter)", m.empty() ? UB_DISABLED : 0)) navigate(a, 1);
    if (ui_icon_button(u, newer_r, IC_CHEV_D, "Newer match (Shift+Enter)", m.empty() ? UB_DISABLED : 0)) navigate(a, -1);
    if (ui_icon_button(u, close_r, IC_CLOSE, "Close (Esc)", 0)) find_close(a);
}

/* ------------------------------------------------------------------- links */

struct Hover {
    Pane *pane = nullptr;
    int idx = 0, c0 = 0, c1 = 0;
    std::string url;
    uint64_t fed = 0;
};
static Hover H;
static TvOverlay g_ov;

/* The link under cell (col, row) of pane p: an OSC 8 link when the cell has one, else a URL written in the text. */
static bool link_lookup(Pane *p, int col, int row, bool ctrl, Hover *out) {
    if (!p || !p->term || col < 0 || row < 0) return false;
    TermCore *t = p->term;
    if (!ctrl && (tcore_modes(t) & (VT_M_MOUSE_BTN | VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY))) return false;   /* a program that wants the mouse keeps it unless Ctrl is held */
    Vt *vt = tcore_vt(t);
    int off = tcore_offset(t);
    bool found = false;
    tcore_lock(t);
    if (col < tcore_cols(t) && row < tcore_rows(t)) {
        int idx = row - off, len = 0;
        VtCell *l = vt_line(vt, idx, &len);
        if (l && col < len) {
            uint32_t lid = vt_style(vt, VT_CELL_STYLE(l[col]))->link;
            const char *uri = lid ? vt_link(vt, lid) : nullptr;
            if (uri) {
                int c0 = col, c1 = col;
                while (c0 > 0 && vt_style(vt, VT_CELL_STYLE(l[c0 - 1]))->link == lid) c0--;
                while (c1 + 1 < len && vt_style(vt, VT_CELL_STYLE(l[c1 + 1]))->link == lid) c1++;
                out->idx = idx; out->c0 = c0; out->c1 = c1; out->url = uri;
                found = true;
            } else {
                int c0, c1;
                std::string url;
                if (scan_row_url(l, len, col, &c0, &c1, &url)) { out->idx = idx; out->c0 = c0; out->c1 = c1; out->url = url; found = true; }
            }
        }
    }
    out->fed = tcore_bytes_fed(t);
    tcore_unlock(t);
    return found;
}

bool links_hover(App *, Pane *p, int col, int row, bool ctrl) {
    Hover n;
    if (!link_lookup(p, col, row, ctrl, &n)) n = Hover();
    else n.pane = p;
    bool changed = n.pane != H.pane || n.idx != H.idx || n.c0 != H.c0 || n.c1 != H.c1 || n.url != H.url;
    H = n;
    return changed;
}

void links_clear(App *) { H = Hover(); }

/* Opens url with xdg-open, detached, no shell: the address is one argv entry, so nothing in it can run as a command. */
static void open_url(const char *url) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execlp("xdg-open", "xdg-open", url, (char *)nullptr);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

bool links_open_at(App *, Pane *p, int col, int row) {
    Hover n;
    if (!link_lookup(p, col, row, true, &n) || !scan_url_allowed(n.url.c_str())) return false;
    open_url(n.url.c_str());
    return true;
}

void links_draw_tip(App *a, Pane *p) {
    if (H.pane != p) return;
    Ui *u = a->ui;
    ui_set_cursor(u, 2);
    std::string text = H.url;
    if (scan_url_allowed(H.url.c_str())) text += "   Ctrl+click to open";
    float tw = ui_text_w(u, text.c_str()) + P(16), maxw = p->r.w - P(16);
    if (tw > maxw) tw = maxw;
    float th = ui_line_h(u) + P(8);
    Rect r = R(p->r.x + P(8), p->r.y + p->r.h - th - P(8), tw, th);
    ui_rrect(u, r, a->colors.bg1, P(6));
    ui_outline(u, r, a->colors.border2, P(6), 1);
    ui_text_fit(u, R(r.x + P(8), r.y, r.w - P(16), r.h), text.c_str(), a->colors.ink, 0);
}

const TvOverlay *extras_overlay(App *a, Pane *p) {
    Find *F = a->find;
    bool fo = F && F->open && F->term == p->term && F->f.active() && !F->f.matches().empty();
    if (H.pane == p && tcore_bytes_fed(p->term) != H.fed) H = Hover();   /* the text moved under the pointer: wait for the next mouse move */
    bool lo = H.pane == p;
    if (!fo && !lo) return nullptr;
    g_ov = TvOverlay{};
    g_ov.current = -1;
    if (fo) { g_ov.matches = F->f.matches().data(); g_ov.nmatches = (int)F->f.matches().size(); g_ov.current = F->cur; }
    if (lo) { g_ov.link = true; g_ov.link_idx = H.idx; g_ov.link_c0 = H.c0; g_ov.link_c1 = H.c1; }
    return &g_ov;
}
