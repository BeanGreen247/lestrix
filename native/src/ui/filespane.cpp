/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <cstring>
#include <strings.h>

#include "app.h"
#include "jobs.h"

#define P(v) S(a->ui, (v))

namespace {

std::string fmt_size(int64_t n) {
    double v = (double)n;
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    return i == 0 ? lx::strprintf("%d B", (int)v) : lx::strprintf("%.1f %s", v, u[i]);
}

std::string join(const std::string &dir, const std::string &name) {
    if (dir == "/") return "/" + name;
    return dir.ends_with("/") ? dir + name : dir + "/" + name;
}

enum TaskKind { K_LIST, K_READY, K_CWD, K_MKDIR, K_RENAME, K_DELETE, K_DOWNLOAD, K_UPLOAD };

struct Item {
    std::string path;
    bool is_dir = false;
};

enum { M_DOWNLOAD = 1, M_UPLOAD, M_RENAME, M_DELETE, M_MKDIR, M_REFRESH, M_COPYPATH };
constexpr int MENU_TAG = 900;

}

/* One unit of background work. `work` fills the result fields on a worker thread; finish_task reads them on the UI thread. */
struct FilesPane::Task {
    int kind;
    std::string a, b;
    std::vector<Item> items;
    std::string err, out_path;
    EntryList out_entries;
    bool ok = false;
    std::shared_ptr<FilesPane> pane;
};

std::shared_ptr<FilesPane> FilesPane::create(App *a, std::unique_ptr<SdXfer> x, bool follow) {
    std::shared_ptr<FilesPane> f(new FilesPane(a, std::move(x), follow));
    if (f->ftp_) { f->ready_ = true; f->navigate_to(nullptr); }
    return f;
}

FilesPane::FilesPane(App *a, std::unique_ptr<SdXfer> x, bool follow) : app_(a), xfer_(std::move(x)) {
    ftp_ = xfer_->is_ftp();
    follow_ = follow && !ftp_;
    ui_text_init(&pathin_, pathbuf_, sizeof pathbuf_);
    status_ = "Waiting for the connection\xe2\x80\xa6";
}

FilesPane::~FilesPane() = default;

void FilesPane::close() { alive_ = false; }

void FilesPane::set_status(const std::string &s) {
    status_ = s;
    app_redraw(app_);
}

void FilesPane::run_task(std::shared_ptr<Task> t) {
    t->pane = shared_from_this();
    auto work = [t] {
        FilesPane &f = *t->pane;
        SdXfer *x = f.xfer_.get();
        std::string *err = &t->err;
        switch (t->kind) {
        case K_READY: t->ok = x->ready(); break;
        case K_CWD: t->out_path = x->terminal_cwd(); t->ok = true; break;
        case K_LIST: {
            std::string path;
            if (!t->a.empty()) path = t->a;
            else if (auto home = x->home(err)) path = *home;
            else break;
            auto e = x->list(path, err);
            if (!e) break;
            t->out_path = path;
            t->out_entries = std::move(*e);
            t->ok = true;
            break;
        }
        case K_MKDIR: t->ok = x->mkdir(t->a, err); break;
        case K_RENAME: t->ok = x->rename(t->a, t->b, err); break;
        case K_DELETE:
            t->ok = true;
            for (size_t i = 0; i < t->items.size() && t->ok; i++) t->ok = x->remove(t->items[i].path, t->items[i].is_dir, err);
            break;
        case K_DOWNLOAD:
            t->ok = true;
            for (size_t i = 0; i < t->items.size() && t->ok; i++) t->ok = x->download(t->items[i].path, t->b, t->items[i].is_dir, err);
            break;
        case K_UPLOAD:
            t->ok = true;
            for (size_t i = 0; i < t->items.size() && t->ok; i++) t->ok = x->upload(t->items[i].path, t->a, err);
            break;
        }
    };
    jobs_run(work, [t] { t->pane->finish_task(*t); });
}

void FilesPane::finish_task(Task &t) {
    if (!alive_) return;
    auto failed = [&] { set_status("\xe2\x9a\xa0 " + (t.err.empty() ? std::string("failed") : t.err)); };
    switch (t.kind) {
    case K_READY:
        if (t.ok && !ready_) { ready_ = true; set_status("Connected"); navigate_to(nullptr); }
        break;
    case K_CWD:
        polling_ = false;
        if (!t.out_path.empty() && !path_.empty() && t.out_path != path_) navigate_to(&t.out_path);
        break;
    case K_LIST:
        if (!t.ok) { failed(); break; }
        path_ = t.out_path;
        ui_text_set(&pathin_, path_.c_str());
        ent_ = std::move(t.out_entries);
        sel_.assign(ent_.size() + 1, 0);
        last_click_ = -1;
        scroll_ = 0;
        sort_entries();
        set_status(lx::strprintf("%d item(s) \xc2\xb7 %s", (int)ent_.size(), xfer_->label().c_str()));
        if (!pending_cwd_.empty() && pending_cwd_ != path_ && follow_) {
            std::string next = std::move(pending_cwd_);
            pending_cwd_.clear();
            navigate_to(&next);
        }
        break;
    case K_MKDIR: case K_RENAME: case K_DELETE:
        if (!t.ok) failed();
        else navigate_to(&path_);
        break;
    case K_DOWNLOAD:
        if (!t.ok) failed();
        else set_status("Downloaded to " + t.b);
        break;
    case K_UPLOAD:
        if (!t.ok) failed();
        else { set_status("Upload complete"); navigate_to(&path_); }
        break;
    }
    app_redraw(app_);
}

void FilesPane::sort_entries() {
    int col = sort_col_;
    bool desc = sort_desc_;
    std::stable_sort(ent_.begin(), ent_.end(), [col, desc](const SdEntry &a, const SdEntry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        int r;
        if (col == 1) r = a.size < b.size ? -1 : a.size > b.size;
        else if (col == 2) r = a.modified.compare(b.modified);
        else r = strcasecmp(a.name.c_str(), b.name.c_str());
        return desc ? r > 0 : r < 0;
    });
}

void FilesPane::navigate_to(const std::string *path) {
    if (!ready_) return;
    auto t = std::make_shared<Task>();
    t->kind = K_LIST;
    if (path) t->a = *path;
    run_task(t);
}

void FilesPane::set_cwd(const std::string &path) {
    osc7_seen_ = true;
    if (follow_ && path != path_) {
        pending_cwd_ = path;
        if (ready_) navigate_to(&path);
    }
}

void FilesPane::tick(App *a) {
    if (!ready_ && a->now >= next_ready_) {
        next_ready_ = a->now + 1.0;
        auto t = std::make_shared<Task>();
        t->kind = K_READY;
        run_task(t);
    }
    bool visible = a->now - drawn_at_ < 0.6;
    if (ready_ && !polling_ && !osc7_seen_ && !ftp_ && follow_ && a->now >= next_poll_) {
        next_poll_ = a->now + 2.0;
        if (visible && (active_ || a->now - last_poll_ >= 10.0)) {
            active_ = false;
            last_poll_ = a->now;
            polling_ = true;
            auto t = std::make_shared<Task>();
            t->kind = K_CWD;
            run_task(t);
        }
    }
}

std::vector<int> FilesPane::selected() const {
    std::vector<int> out;
    for (size_t i = 0; i < ent_.size(); i++) if (sel_[i]) out.push_back((int)i);
    return out;
}

void FilesPane::start_download(const std::vector<int> &entries, const std::string &dest) {
    auto t = std::make_shared<Task>();
    t->kind = K_DOWNLOAD;
    t->b = dest;
    for (int i : entries) t->items.push_back({join(path_, ent_[(size_t)i].name), ent_[(size_t)i].is_dir});
    set_status(lx::strprintf("Downloading %d item(s)\xe2\x80\xa6", (int)entries.size()));
    run_task(t);
}

void FilesPane::start_upload(const std::vector<std::string> &paths) {
    if (!ready_ || path_.empty() || paths.empty()) return;
    auto t = std::make_shared<Task>();
    t->kind = K_UPLOAD;
    t->a = path_;
    for (const std::string &p : paths) t->items.push_back({p, false});
    set_status(lx::strprintf("Uploading %d item(s)\xe2\x80\xa6", (int)paths.size()));
    run_task(t);
}

void FilesPane::drop(const std::string &path) { start_upload({path}); }

void FilesPane::act_download(App *a) {
    if (selected().empty()) return;
    std::weak_ptr<FilesPane> weak = shared_from_this();
    dlg_pick_file(a, "Download to folder", true, false, lx::download_dir(), [weak](const std::vector<std::string> &paths) {
        auto f = weak.lock();
        if (!f || !f->alive_ || paths.empty()) return;
        std::vector<int> sel = f->selected();
        if (!sel.empty()) f->start_download(sel, paths[0]);
    });
}

void FilesPane::act_delete(App *a) {
    std::vector<int> sel = selected();
    if (sel.empty()) return;
    std::vector<Item> items;
    std::string names;
    for (size_t k = 0; k < sel.size(); k++) {
        const SdEntry &e = ent_[(size_t)sel[k]];
        items.push_back({join(path_, e.name), e.is_dir});
        if (k < 5) names += (k ? ", " : "") + e.name;
    }
    std::string msg = lx::strprintf("Delete %d item(s) on the server?\n", (int)sel.size()) + names;
    std::weak_ptr<FilesPane> weak = shared_from_this();
    dlg_confirm(a, "Delete", msg, "Delete", [weak, items = std::move(items)] {
        auto f = weak.lock();
        if (!f || !f->alive_ || items.empty()) return;
        auto t = std::make_shared<Task>();
        t->kind = K_DELETE;
        t->items = items;
        f->run_task(t);
    });
}

void FilesPane::act_rename(App *a) {
    std::vector<int> sel = selected();
    if (sel.size() != 1) return;
    std::string old = ent_[(size_t)sel[0]].name;
    std::weak_ptr<FilesPane> weak = shared_from_this();
    dlg_text(a, "Rename", "", old, false, [weak, old](const std::string &text) {
        auto f = weak.lock();
        if (!f || !f->alive_ || f->path_.empty() || text == old) return;
        auto t = std::make_shared<Task>();
        t->kind = K_RENAME;
        t->a = join(f->path_, old);
        t->b = join(f->path_, text);
        f->run_task(t);
    });
}

void FilesPane::activate(int i) {
    if (path_.empty()) return;
    const SdEntry &e = ent_[(size_t)i];
    std::string target = join(path_, e.name);
    if (e.is_dir || e.is_link) navigate_to(&target);
    else start_download({i}, lx::download_dir());
}

void FilesPane::draw(App *a, Rect r) {
    Ui *u = a->ui;
    const UiColors *c = ui_colors(u);
    drawn_at_ = a->now;
    std::weak_ptr<FilesPane> weak = shared_from_this();
    auto mkdir_dialog = [&] {
        dlg_text(a, "New folder", "", "", false, [weak](const std::string &text) {
            auto f = weak.lock();
            if (!f || !f->alive_ || f->path_.empty()) return;
            auto t = std::make_shared<Task>();
            t->kind = K_MKDIR;
            t->a = join(f->path_, text);
            f->run_task(t);
        });
    };
    auto upload_dialog = [&] {
        dlg_pick_file(a, "Upload files", false, true, lx::home_dir(), [weak](const std::vector<std::string> &paths) {
            if (auto f = weak.lock(); f && f->alive_) f->start_upload(paths);
        });
    };
    int tag;
    int chosen = ui_menu_take(u, &tag);
    if (chosen >= 0 && tag == MENU_TAG) {
        switch (chosen) {
        case M_DOWNLOAD: act_download(a); break;
        case M_UPLOAD: upload_dialog(); break;
        case M_RENAME: act_rename(a); break;
        case M_DELETE: act_delete(a); break;
        case M_MKDIR: mkdir_dialog(); break;
        case M_REFRESH: if (!path_.empty()) navigate_to(&path_); break;
        case M_COPYPATH: {
            std::vector<int> sel = selected();
            if (sel.size() == 1 && !path_.empty()) SDL_SetClipboardText(join(path_, ent_[(size_t)sel[0]].name).c_str());
            break;
        }
        }
    }
    ui_rect(u, r, c->bg1);
    float pad = P(8), y = r.y + pad, bh = P(30);
    if (ui_icon_button(u, R(r.x + pad, y, bh, bh), IC_UP, "Parent folder", 0) && !path_.empty() && path_ != "/") {
        std::string up = lx::dirname(path_);
        navigate_to(&up);
    }
    if (ui_icon_button(u, R(r.x + pad + bh + P(2), y, bh, bh), IC_HOME, "Home", 0)) navigate_to(nullptr);
    if (ui_icon_button(u, R(r.x + pad + 2 * (bh + P(2)), y, bh, bh), IC_REFRESH, "Refresh", 0) && !path_.empty()) navigate_to(&path_);
    float right = r.x + r.w - pad - 3 * (bh + P(2));
    float px = r.x + pad + 3 * (bh + P(2)) + P(4);
    if (ui_input(u, R(px, y, right - px, bh), &pathin_, nullptr, false)) {
        std::string typed = pathin_.s[0] ? pathin_.s : "/";
        navigate_to(&typed);
    }
    if (ui_icon_button(u, R(right + P(2), y, bh, bh), IC_NEWFOLDER, "New folder", 0)) mkdir_dialog();
    if (ui_icon_button(u, R(right + P(2) + bh + P(2), y, bh, bh), IC_UPLOAD, "Upload files", 0)) upload_dialog();
    if (ui_icon_button(u, R(right + P(2) + 2 * (bh + P(2)), y, bh, bh), IC_DOWNLOAD, "Download selected", 0)) act_download(a);
    y += bh + P(6);
    if (!ftp_) {
        bool v = follow_;
        if (ui_checkbox(u, R(r.x + pad, y, r.w - 2 * pad, P(24)), "Follow terminal path", &v)) { follow_ = v; if (v) next_poll_ = 0; app_settings_save(a); }
        y += P(28);
    }
    float nameW = r.w - 2 * pad - P(86) - P(130);
    Rect hdr = R(r.x + pad, y, r.w - 2 * pad, P(24));
    ui_rect(u, hdr, c->bg2);
    const char *hn[3] = {"Name", "Size", "Modified"};
    float hx[3] = {hdr.x + P(8), hdr.x + nameW, hdr.x + nameW + P(86)};
    float hw[3] = {nameW - P(8), P(80), P(124)};
    for (int i = 0; i < 3; i++) {
        Rect hr = R(i == 0 ? hdr.x : hx[i] - P(6), hdr.y, i == 0 ? nameW : hw[i] + P(6), hdr.h);
        if (ui_hover(u, hr) && ui_mouse_pressed(u, 1, hr)) {
            if (sort_col_ == i) sort_desc_ = !sort_desc_; else { sort_col_ = i; sort_desc_ = false; }
            std::vector<std::string> keep;
            for (int k : selected()) keep.push_back(ent_[(size_t)k].name);
            sort_entries();
            for (size_t k = 0; k < ent_.size(); k++) sel_[k] = std::find(keep.begin(), keep.end(), ent_[k].name) != keep.end();
        }
        char lab[40];
        snprintf(lab, sizeof lab, "%s%s", hn[i], sort_col_ == i ? (sort_desc_ ? "  \xe2\x96\xbe" : "  \xe2\x96\xb4") : "");
        ui_text_fit(u, R(hx[i], hdr.y, hw[i], hdr.h), lab, c->muted, i == 1 ? 2 : 0);
    }
    y += hdr.h;
    Rect list = R(r.x + pad, y, r.w - 2 * pad, r.y + r.h - y - P(28));
    float lh = P(26);
    bool in_list = ui_hover(u, list);
    int n = (int)ent_.size();
    ui_scroll_begin(u, list, (float)n * lh, &scroll_);
    int clicked_row = -1;
    for (int i = 0; i < n; i++) {
        Rect row = R(list.x, list.y + (float)i * lh - scroll_, list.w, lh);
        if (row.y + row.h < list.y || row.y > list.y + list.h) continue;
        const SdEntry &e = ent_[(size_t)i];
        bool hov = in_list && ui_hover(u, row);
        bool sel = sel_[(size_t)i];
        uint32_t tx = sel ? c->bg1 : c->ink, tm = sel ? c->bg1 : c->muted;
        if (sel) ui_rrect(u, row, c->accent, P(8));
        else if (hov) ui_rrect(u, row, c->hover, P(8));
        ui_icon(u, e.is_dir ? IC_FOLDER : e.is_link ? IC_LINK : IC_FILE, R(row.x + P(6), row.y + (lh - P(16)) / 2, P(16), P(16)), sel ? c->bg1 : (e.is_dir ? c->accent : c->ink2));
        ui_text_fit(u, R(row.x + P(28), row.y, nameW - P(28), lh), e.name.c_str(), tx, 0);
        if (!e.is_dir) ui_text_fit(u, R(hx[1], row.y, hw[1], lh), fmt_size(e.size).c_str(), tm, 2);
        if (!e.modified.empty()) ui_text_fit(u, R(hx[2], row.y, hw[2], lh), e.modified.c_str(), tm, 0);
        if (hov && (ui_mouse_pressed(u, 1, row) || ui_mouse_pressed(u, 3, row))) {
            bool right_click = ui_mouse_pressed(u, 3, row);
            int mods = SDL_GetModState();
            if (right_click && sel) {}
            else if (mods & KMOD_CTRL) sel_[(size_t)i] = !sel_[(size_t)i];
            else if ((mods & KMOD_SHIFT) && last_click_ >= 0) {
                int lo = std::min(last_click_, i), hi = std::max(last_click_, i);
                for (int k = lo; k <= hi; k++) sel_[(size_t)k] = 1;
            } else { std::fill(sel_.begin(), sel_.end(), 0); sel_[(size_t)i] = 1; }
            last_click_ = i;
            clicked_row = right_click ? -2 - i : i;
            if (!right_click && ui_clicks(u) >= 2) { activate(i); break; }
        }
    }
    ui_scroll_end(u);
    if (in_list && clicked_row <= -2) {
        size_t count = selected().size();
        bool one = count == 1, some = count > 0;
        UiMenuItem it[] = {
            {"Download\xe2\x80\xa6", nullptr, M_DOWNLOAD, some ? 0 : MI_DISABLED, nullptr, 0},
            {"Upload\xe2\x80\xa6", nullptr, M_UPLOAD, 0, nullptr, 0},
            {nullptr, nullptr, 0, MI_SEP, nullptr, 0},
            {"Rename\xe2\x80\xa6", nullptr, M_RENAME, one ? 0 : MI_DISABLED, nullptr, 0},
            {"Delete\xe2\x80\xa6", nullptr, M_DELETE, some ? 0 : MI_DISABLED, nullptr, 0},
            {"New folder\xe2\x80\xa6", nullptr, M_MKDIR, 0, nullptr, 0},
            {nullptr, nullptr, 0, MI_SEP, nullptr, 0},
            {"Copy path", nullptr, M_COPYPATH, one ? 0 : MI_DISABLED, nullptr, 0},
            {"Refresh", nullptr, M_REFRESH, 0, nullptr, 0},
        };
        ui_menu_open(u, ui_mx(u), ui_my(u), it, (int)(sizeof it / sizeof *it), MENU_TAG);
    } else if (in_list && ui_mouse_pressed(u, 3, list) && clicked_row == -1) {
        UiMenuItem it[] = {
            {"Upload\xe2\x80\xa6", nullptr, M_UPLOAD, 0, nullptr, 0}, {"New folder\xe2\x80\xa6", nullptr, M_MKDIR, 0, nullptr, 0}, {"Refresh", nullptr, M_REFRESH, 0, nullptr, 0},
        };
        ui_menu_open(u, ui_mx(u), ui_my(u), it, 3, MENU_TAG);
    }
    ui_text_fit(u, R(r.x + pad, r.y + r.h - P(26), r.w - 2 * pad, P(24)), status_.c_str(), c->muted, 0);
}
