/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <strings.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "app.h"

#define P(v) S(a->ui, (v))

void dlg_push(App *a, std::unique_ptr<Dialog> d) {
    if (a->dialogs.size() < 4) a->dialogs.push_back(std::move(d));
    app_redraw(a);
}

bool dlg_active(const App *a) { return !a->dialogs.empty(); }

void dlg_draw_top(App *a) {
    if (a->dialogs.empty()) return;
    Dialog *d = a->dialogs.back().get();
    d->draw(a);
    if (!d->done) return;
    /* the draw may have pushed another dialog, so find this one rather than assuming it is still on top */
    auto it = std::find_if(a->dialogs.begin(), a->dialogs.end(), [d](const auto &p) { return p.get() == d; });
    if (it != a->dialogs.end()) a->dialogs.erase(it);
    ui_release_focus(a->ui);
    app_redraw(a);
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

namespace {

bool esc(App *a) { return ui_key(a->ui, SDLK_ESCAPE, 0); }

class TextDlg : public Dialog {
public:
    TextDlg(std::string title, std::string label, const std::string &initial, bool password, TextCb cb)
        : title_(std::move(title)), label_(std::move(label)), password_(password), cb_(std::move(cb)) {
        ui_text_init(&in_, buf_, sizeof buf_);
        ui_text_set(&in_, initial.c_str());
    }
    void draw(App *a) override {
        ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
        Rect c = dlg_frame(a, title_.c_str(), 400, label_.empty() ? 150 : 168);
        if (!focused_) { ui_input_focus(a->ui, &in_); focused_ = true; }
        float y = c.y;
        if (!label_.empty()) { ui_text(a->ui, c.x, y, label_.c_str(), ui_colors(a->ui)->muted); y += P(24); }
        bool enter = ui_input(a->ui, R(c.x, y, c.w, P(32)), &in_, nullptr, password_);
        float by = c.y + c.h - P(32);
        bool ok = ui_button(a->ui, R(c.x + c.w - P(90), by, P(90), P(32)), "OK", UB_PRIMARY);
        bool cancel = ui_button(a->ui, R(c.x + c.w - P(190), by, P(90), P(32)), "Cancel", 0);
        if ((ok || enter) && in_.len) { done = true; cb_(in_.s); }
        else if (cancel || esc(a)) done = true;
    }

private:
    std::string title_, label_;
    char buf_[256];
    UiText in_;
    bool password_, focused_ = false;
    TextCb cb_;
};

void wrap_text(App *a, Rect r, const std::string &text, uint32_t col) {
    const char *s = text.c_str();
    char line[256];
    float y = r.y, lh = ui_line_h(a->ui) + P(3);
    const char *p = s;
    while (*p) {
        int n = 0;
        const char *last_space = nullptr;
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

class ConfirmDlg : public Dialog {
public:
    ConfirmDlg(std::string title, std::string msg, std::string ok, ConfirmCb cb)
        : title_(std::move(title)), msg_(std::move(msg)), ok_(std::move(ok)), cb_(std::move(cb)) {}
    void draw(App *a) override {
        ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
        Rect r = dlg_frame(a, title_.c_str(), 420, 190);
        wrap_text(a, R(r.x, r.y, r.w, r.h - P(40)), msg_, ui_colors(a->ui)->ink2);
        float by = r.y + r.h - P(32);
        bool ok = ui_button(a->ui, R(r.x + r.w - P(110), by, P(110), P(32)), ok_.c_str(), UB_PRIMARY);
        bool cancel = ui_button(a->ui, R(r.x + r.w - P(210), by, P(90), P(32)), "Cancel", 0);
        if (ok) { done = true; cb_(); }
        else if (cancel || esc(a)) done = true;
    }

private:
    std::string title_, msg_, ok_;
    ConfirmCb cb_;
};

class ChoiceDlg : public Dialog {
public:
    ChoiceDlg(std::string title, std::string msg, std::vector<std::string> labels, ChoiceCb cb)
        : title_(std::move(title)), msg_(std::move(msg)), labels_(std::move(labels)), cb_(std::move(cb)) {
        if (labels_.size() > 5) labels_.resize(5);
    }
    void draw(App *a) override {
        ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
        int n = (int)labels_.size();
        Rect r = dlg_frame(a, title_.c_str(), 440, 130 + 40 * (float)n);
        wrap_text(a, R(r.x, r.y, r.w, P(60)), msg_, ui_colors(a->ui)->ink2);
        float y = r.y + P(66);
        for (int i = 0; i < n; i++, y += P(40))
            if (ui_button(a->ui, R(r.x, y, r.w, P(32)), labels_[(size_t)i].c_str(), i == 0 ? UB_PRIMARY : 0)) { done = true; cb_(i); return; }
        if (esc(a)) done = true;
    }

private:
    std::string title_, msg_;
    std::vector<std::string> labels_;
    ChoiceCb cb_;
};

class MsgDlg : public Dialog {
public:
    MsgDlg(std::string title, std::string msg) : title_(std::move(title)), msg_(std::move(msg)) {}
    void draw(App *a) override {
        ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
        /* long messages get a wider, taller box so the text never runs into the button */
        bool wide = msg_.size() > 160;
        float w = wide ? 640 : 440;
        size_t per_line = wide ? 80 : 52, rows = 1;
        for (size_t i = 0, run = 0; i < msg_.size(); i++) { if (msg_[i] == '\n' || ++run > per_line) { rows++; run = 0; } }
        float h = std::max(200.f, 120.f + 25.f * (float)rows);
        Rect r = dlg_frame(a, title_.c_str(), w, h);
        wrap_text(a, R(r.x, r.y, r.w, r.h - P(40)), msg_, ui_colors(a->ui)->ink2);
        if (ui_button(a->ui, R(r.x + r.w - P(90), r.y + r.h - P(32), P(90), P(32)), "OK", UB_PRIMARY) || esc(a) || ui_key(a->ui, SDLK_RETURN, 0)) done = true;
    }

private:
    std::string title_, msg_;
};

const char *const SWATCHES[] = {"#ef5350", "#ff9800", "#ffca28", "#66bb6a", "#26a69a", "#26c6da", "#42a5f5", "#7e57c2", "#ba68c8", "#ec407a"};

class ColorDlg : public Dialog {
public:
    ColorDlg(std::string title, const std::string &initial, TextCb cb) : title_(std::move(title)), cb_(std::move(cb)) {
        ui_text_init(&in_, buf_, sizeof buf_);
        ui_text_set(&in_, initial.empty() ? "#00bcd4" : initial.c_str());
    }
    void draw(App *a) override {
        ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
        Rect r = dlg_frame(a, title_.c_str(), 360, 230);
        if (!focused_) { ui_input_focus(a->ui, &in_); focused_ = true; }
        for (int i = 0; i < 10; i++) {
            Rect s = R(r.x + i * P(32), r.y, P(26), P(26));
            ui_rrect(a->ui, s, ui_rgba(SWATCHES[i], 255), P(13));
            if (ui_mouse_pressed(a->ui, 1, s)) ui_text_set(&in_, SWATCHES[i]);
            if (ui_hover(a->ui, s)) ui_outline(a->ui, rect_inset(s, -P(2)), ui_colors(a->ui)->ink, P(15), 2);
        }
        ui_text(a->ui, r.x, r.y + P(42), "Hex colour", ui_colors(a->ui)->muted);
        bool enter = ui_input(a->ui, R(r.x, r.y + P(64), r.w - P(50), P(32)), &in_, "#rrggbb", false);
        unsigned rr, gg, bb;
        bool valid = in_.s[0] == '#' && strlen(in_.s) == 7 && sscanf(in_.s + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3;
        ui_rrect(a->ui, R(r.x + r.w - P(38), r.y + P(64), P(38), P(32)), valid ? ui_rgba(in_.s, 255) : ui_colors(a->ui)->bg2, P(5));
        float by = r.y + r.h - P(32);
        bool ok = ui_button(a->ui, R(r.x + r.w - P(90), by, P(90), P(32)), "OK", valid ? UB_PRIMARY : UB_DISABLED);
        bool cancel = ui_button(a->ui, R(r.x + r.w - P(190), by, P(90), P(32)), "Cancel", 0);
        if ((ok || enter) && valid) { done = true; cb_(in_.s); }
        else if (cancel || esc(a)) done = true;
    }

private:
    std::string title_;
    char buf_[16];
    UiText in_;
    bool focused_ = false;
    TextCb cb_;
};

struct PEntry {
    std::string name;
    bool dir;
};

class PickDlg : public Dialog {
public:
    PickDlg(std::string title, bool folder, bool multi, const std::string &start, PickCb cb)
        : title_(std::move(title)), folder_(folder), multi_(multi), cb_(std::move(cb)) {
        ui_text_init(&path_, buf_, sizeof buf_);
        load(start.empty() ? lx::home_dir() : start);
    }
    void draw(App *a) override;

private:
    void load(const std::string &dir);
    void finish(bool ok);

    std::string title_;
    bool folder_, multi_, hidden_ = false;
    std::string dir_;
    std::vector<PEntry> e_;
    std::vector<char> sel_;
    int last_click_ = -1;
    char buf_[1024];
    UiText path_;
    float scroll_ = 0;
    PickCb cb_;
};

void PickDlg::load(const std::string &dir) {
    dir_ = lx::canonicalize(dir);
    e_.clear();
    for (const std::string &nm : lx::list_dir(dir_)) {
        if (!hidden_ && nm[0] == '.') continue;
        bool isdir = lx::is_dir(lx::path_join(dir_, nm));
        if (folder_ && !isdir) continue;
        e_.push_back({nm, isdir});
    }
    std::sort(e_.begin(), e_.end(), [](const PEntry &x, const PEntry &y) {
        if (x.dir != y.dir) return x.dir;
        return strcasecmp(x.name.c_str(), y.name.c_str()) < 0;
    });
    sel_.assign(e_.size() + 1, 0);
    scroll_ = 0;
    last_click_ = -1;
    ui_text_set(&path_, dir_.c_str());
}

void PickDlg::finish(bool ok) {
    done = true;
    if (!ok) { cb_({}); return; }
    std::vector<std::string> paths;
    for (size_t i = 0; i < e_.size(); i++) if (sel_[i]) paths.push_back(lx::path_join(dir_, e_[i].name));
    if (paths.empty() && folder_) paths.push_back(dir_);
    cb_(paths);
}

void PickDlg::draw(App *a) {
    const UiColors *c = ui_colors(a->ui);
    ui_set_blocked(a->ui, ui_menu_is_open(a->ui));
    Rect r = dlg_frame(a, title_.c_str(), 640, 520);
    float y = r.y;
    if (ui_icon_button(a->ui, R(r.x, y, P(32), P(32)), IC_UP, "Parent folder", 0)) load(lx::dirname(dir_));
    if (ui_icon_button(a->ui, R(r.x + P(36), y, P(32), P(32)), IC_HOME, "Home", 0)) load(lx::home_dir());
    bool enter = ui_input(a->ui, R(r.x + P(76), y, r.w - P(76) - P(120), P(32)), &path_, nullptr, false);
    if (enter) {
        std::string typed = path_.s;
        if (lx::is_dir(typed)) load(typed);
        else if (!folder_ && lx::exists(typed)) { done = true; cb_({typed}); return; }
    }
    bool before = hidden_;
    ui_checkbox(a->ui, R(r.x + r.w - P(112), y, P(112), P(32)), "Hidden", &hidden_);
    if (before != hidden_) load(dir_);
    y += P(42);
    float lh = P(28), listh = r.h - P(42) - P(50);
    Rect list = R(r.x, y, r.w, listh);
    int n = (int)e_.size();
    ui_rrect(a->ui, list, c->bg0, P(5));
    ui_outline(a->ui, list, c->border, P(5), 1);
    ui_scroll_begin(a->ui, rect_inset(list, 1), (float)n * lh, &scroll_);
    for (int i = 0; i < n; i++) {
        Rect row = R(list.x + 1, list.y + 1 + (float)i * lh - scroll_, list.w - 2, lh);
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        bool hov = ui_hover(a->ui, row) && rect_has(list, ui_mx(a->ui), ui_my(a->ui));
        bool sel = sel_[(size_t)i];
        if (sel) ui_rrect(a->ui, row, c->accent, P(8));
        else if (hov) ui_rrect(a->ui, row, c->hover, P(8));
        const PEntry &pe = e_[(size_t)i];
        ui_icon(a->ui, pe.dir ? IC_FOLDER : IC_FILE, R(row.x + P(8), row.y + (lh - P(16)) / 2, P(16), P(16)), sel ? c->bg1 : (pe.dir ? c->accent : c->ink2));
        ui_text_fit(a->ui, R(row.x + P(32), row.y, row.w - P(40), row.h), pe.name.c_str(), sel ? c->bg1 : c->ink, 0);
        if (hov && ui_mouse_pressed(a->ui, 1, row)) {
            int mods = SDL_GetModState();
            if (multi_ && (mods & KMOD_CTRL)) sel_[(size_t)i] = !sel_[(size_t)i];
            else if (multi_ && (mods & KMOD_SHIFT) && last_click_ >= 0) {
                int lo = std::min(last_click_, i), hi = std::max(last_click_, i);
                for (int k = lo; k <= hi; k++) sel_[(size_t)k] = 1;
            } else { std::fill(sel_.begin(), sel_.end(), 0); sel_[(size_t)i] = 1; }
            last_click_ = i;
            if (ui_clicks(a->ui) >= 2) {
                if (pe.dir) { load(lx::path_join(dir_, pe.name)); break; }
                finish(true);
                break;
            }
        }
    }
    ui_scroll_end(a->ui);
    float by = r.y + r.h - P(32);
    bool any = folder_ || std::find(sel_.begin(), sel_.end(), 1) != sel_.end();
    bool ok = ui_button(a->ui, R(r.x + r.w - P(130), by, P(130), P(32)), folder_ ? "Choose folder" : "Open", any ? UB_PRIMARY : UB_DISABLED);
    bool cancel = ui_button(a->ui, R(r.x + r.w - P(230), by, P(90), P(32)), "Cancel", 0);
    if (ok) finish(true);
    else if (cancel || esc(a)) finish(false);
}

constexpr int NF = 8;
const char *const PROTOS[] = {"ssh", "ftp", "ftps"};
const char *const PROTO_L[] = {"SSH (terminal + SFTP)", "FTP (file browser)", "FTPS (file browser)"};
const char *const X11S[] = {"off", "untrusted", "trusted"};
const char *const X11_L[] = {"Off", "X11 forwarding (-X)", "Trusted X11 forwarding (-Y)"};
enum { F_NAME, F_GROUP, F_HOST, F_USER, F_KEY, F_REMOTE, F_OPTS };

template <size_t N> int index_of(const char *const (&list)[N], const std::string &v) {
    for (size_t i = 0; i < N; i++) if (v == list[i]) return (int)i;
    return 0;
}

class ConnDlg : public Dialog {
public:
    ConnDlg(const SdConn *conn, bool copy_of_live) : conn_(conn ? *conn : SdConn()), is_new_(!conn || copy_of_live) {
        if (conn && copy_of_live) {
            conn_.id = SdConn().id;
            conn_.alias.clear();
            conn_.group = "Saved";
        }
        for (int i = 0; i < NF; i++) ui_text_init(&f_[i], b_[i], sizeof b_[i]);
        ui_text_set(&f_[F_NAME], conn_.name.c_str()); ui_text_set(&f_[F_GROUP], conn_.group.c_str()); ui_text_set(&f_[F_HOST], conn_.host.c_str());
        ui_text_set(&f_[F_USER], conn_.user.c_str()); ui_text_set(&f_[F_KEY], conn_.key.c_str()); ui_text_set(&f_[F_REMOTE], conn_.remote_command.c_str());
        ui_text_set(&f_[F_OPTS], conn_.options.c_str());
        ui_text_init(&port_, port_b_, sizeof port_b_);
        ui_text_set(&port_, std::to_string(conn_.port).c_str());
        proto_ = index_of(PROTOS, conn_.protocol);
        x11_ = index_of(X11S, conn_.x11);
    }
    void draw(App *a) override;

private:
    SdConn conn_;
    bool is_new_;
    char b_[NF][320];
    char port_b_[12];
    UiText f_[NF], port_;
    int proto_ = 0, x11_ = 0;
    bool focused_ = false, error_ = false;
};

void ConnDlg::draw(App *a) {
    const UiColors *col = ui_colors(a->ui);
    bool menu = ui_menu_is_open(a->ui);
    ui_set_blocked(a->ui, menu);
    int tag;
    int chosen = ui_menu_take(a->ui, &tag);
    if (chosen >= 0 && tag == 71) proto_ = chosen;
    if (chosen >= 0 && tag == 72) x11_ = chosen;
    Rect r = dlg_frame(a, is_new_ ? "New connection" : "Edit connection", 520, 560);
    if (!focused_) { ui_input_focus(a->ui, &f_[F_NAME]); focused_ = true; }
    float lw = P(104), rowh = P(44), y = r.y;
    struct Row { const char *label; int kind; int idx; const char *hint; };
    const Row rows[] = {
        {"Name", 0, F_NAME, nullptr}, {"Group", 0, F_GROUP, nullptr}, {"Type", 1, 0, nullptr}, {"Host", 0, F_HOST, nullptr}, {"Port", 2, 0, nullptr}, {"User", 0, F_USER, nullptr},
        {"Private key", 3, F_KEY, "optional - defaults to ssh-agent and ~/.ssh keys"}, {"X11", 4, 0, nullptr},
        {"Remote shell", 0, F_REMOTE, "empty = the account's login shell (e.g. fish -l)"}, {"Extra ssh args", 0, F_OPTS, "e.g. -J jumphost -L 8080:localhost:80"},
    };
    UiText *order[NF + 1];
    int no = 0;
    auto choice_button = [&](Rect fr, const char *label, const char *const *labels, int current, int tag_id) {
        if (ui_button(a->ui, fr, label, 0)) {
            UiMenuItem it[3];
            for (int k = 0; k < 3; k++) it[k] = UiMenuItem{labels[k], nullptr, k, MI_CHECK | (k == current ? MI_CHECKED : 0), nullptr, 0};
            ui_menu_open(a->ui, fr.x, fr.y + fr.h + P(2), it, 3, tag_id);
        }
        ui_icon(a->ui, IC_CHEV_D, R(fr.x + fr.w - P(24), fr.y + (fr.h - P(14)) / 2, P(14), P(14)), col->ink2);
    };
    for (const Row &row : rows) {
        ui_text(a->ui, r.x, y + (rowh - P(10) - ui_line_h(a->ui)) / 2, row.label, col->muted);
        Rect fr = R(r.x + lw, y, r.w - lw, rowh - P(10));
        switch (row.kind) {
        case 0: ui_input(a->ui, fr, &f_[row.idx], row.hint, false); order[no++] = &f_[row.idx]; break;
        case 1: choice_button(fr, PROTO_L[proto_], PROTO_L, proto_, 71); break;
        case 2: ui_input(a->ui, R(fr.x, fr.y, P(110), fr.h), &port_, nullptr, false); order[no++] = &port_; break;
        case 3:
            ui_input(a->ui, R(fr.x, fr.y, fr.w - P(96), fr.h), &f_[F_KEY], row.hint, false);
            order[no++] = &f_[F_KEY];
            if (ui_button(a->ui, R(fr.x + fr.w - P(88), fr.y, P(88), fr.h), "Browse\xe2\x80\xa6", 0)) {
                UiText *key = &f_[F_KEY];
                dlg_pick_file(a, "Private key", false, false, lx::path_join(lx::home_dir(), ".ssh"), [key](const std::vector<std::string> &paths) {
                    if (!paths.empty()) ui_text_set(key, paths[0].c_str());
                });
            }
            break;
        default: choice_button(fr, X11_L[x11_], X11_L, x11_, 72); break;
        }
        y += rowh;
    }
    if (error_) ui_text(a->ui, r.x, y - P(4), "Name and host are required.", col->red);
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
        std::string name(lx::trim(f_[F_NAME].s)), host(lx::trim(f_[F_HOST].s));
        if (name.empty() || host.empty()) { error_ = true; return; }
        conn_.name = name;
        conn_.host = host;
        conn_.group = f_[F_GROUP].s[0] ? f_[F_GROUP].s : "Ungrouped";
        int port = atoi(port_.s);
        conn_.port = port < 1 ? 22 : port > 65535 ? 65535 : port;
        conn_.user = f_[F_USER].s;
        conn_.key = f_[F_KEY].s;
        conn_.options = f_[F_OPTS].s;
        conn_.remote_command = f_[F_REMOTE].s;
        conn_.protocol = PROTOS[proto_];
        conn_.x11 = X11S[x11_];
        a->store->upsert(conn_);
        app_refresh(a);
        done = true;
    } else if (cancel || (esc(a) && !menu)) done = true;
}

}

void dlg_text(App *a, const std::string &title, const std::string &label, const std::string &initial, bool password, TextCb cb) {
    dlg_push(a, std::make_unique<TextDlg>(title, label, initial, password, std::move(cb)));
}

void dlg_confirm(App *a, const std::string &title, const std::string &msg, const std::string &ok, ConfirmCb cb) {
    dlg_push(a, std::make_unique<ConfirmDlg>(title, msg, ok, std::move(cb)));
}

void dlg_choice(App *a, const std::string &title, const std::string &msg, const std::vector<std::string> &labels, ChoiceCb cb) {
    dlg_push(a, std::make_unique<ChoiceDlg>(title, msg, labels, std::move(cb)));
}

void dlg_message(App *a, const std::string &title, const std::string &msg) { dlg_push(a, std::make_unique<MsgDlg>(title, msg)); }

void dlg_about(App *a) {
    dlg_message(a, "About Lestrix",
                std::string("Lestrix " APP_VERSION "\nSSH sessions, SFTP/FTP and local shells in one window.\n\nMIT licence.\n\nRenderer: ") + a->gl_desc);
}

void dlg_color(App *a, const std::string &title, const std::string &initial, TextCb cb) {
    dlg_push(a, std::make_unique<ColorDlg>(title, initial, std::move(cb)));
}

void dlg_pick_file(App *a, const std::string &title, bool folder, bool multi, const std::string &start, PickCb cb) {
    dlg_push(a, std::make_unique<PickDlg>(title, folder, multi, start, std::move(cb)));
}

void dlg_conn_editor(App *a, const SdConn *conn, bool copy_of_live) { dlg_push(a, std::make_unique<ConnDlg>(conn, copy_of_live)); }
