/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "fleetwm.h"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <thread>

#include "../util.h"

namespace {

std::string first_regular(std::initializer_list<std::string> paths) {
    for (const std::string &p : paths) {
        struct stat st;
        if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) return p;
    }
    return {};
}

std::string theme_file() {
    return first_regular({lx::path_join(lx::config_dir(), "fleetwm", "theme.toml"), "/etc/xdg/fleetwm/theme.toml",
                          "/usr/local/etc/xdg/fleetwm/theme.toml"});
}

std::string accent_file() {
    return first_regular({lx::path_join(lx::path_join(lx::config_dir(), "fleetwm"), "themes", "accent.css"),
                          "/etc/xdg/fleetwm/themes/accent.css", "/usr/local/etc/xdg/fleetwm/themes/accent.css"});
}

double mtime(const std::string &path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0 ? (double)st.st_mtime : 0;
}

bool toml_get(const std::string &text, std::string_view key, std::string &out) {
    for (const std::string &raw : lx::split(text, '\n')) {
        std::string_view s = lx::trim(raw);
        if (!s.starts_with(key) || s.size() == key.size()) continue;
        char after = s[key.size()];
        if (after != ' ' && after != '=' && after != '\t') continue;
        size_t eq = s.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view v = lx::trim(s.substr(eq + 1));
        if (!v.empty() && v[0] == '"') {
            v.remove_prefix(1);
            size_t q = v.find('"');
            if (q != std::string_view::npos) v = v.substr(0, q);
        } else {
            size_t c = v.find('#');
            if (c != std::string_view::npos) v = lx::trim(v.substr(0, c));
        }
        out = std::string(v);
        return true;
    }
    return false;
}

}

double fleetwm_stamp() { return std::max(mtime(theme_file()), mtime(accent_file())); }

void fleetwm_read(FleetCfg *o) {
    *o = FleetCfg();
    std::string tf = theme_file();
    if (tf.empty()) return;
    if (auto text = lx::read_file(tf)) {
        o->found = true;
        std::string v;
        if (toml_get(*text, "theme", v)) o->theme = v.substr(0, 63);
        if (toml_get(*text, "corner_style", v)) o->rounded = v != "sharp";
        if (toml_get(*text, "accent", v) && v.size() == 7 && v[0] == '#') { o->accent_auto = false; o->accent = v; }
    }
    if (o->found && o->accent_auto) {
        std::string af = accent_file();
        if (auto css = af.empty() ? std::nullopt : lx::read_file(af)) {
            size_t d = css->find("@define-color accent_color");
            size_t h = d == std::string::npos ? d : css->find('#', d);
            if (h != std::string::npos && css->size() - h >= 7) o->accent = css->substr(h, 7);
        }
    }
    o->stamp = fleetwm_stamp();
}

const char *fleetwm_theme_name(const std::string &key) {
    if (key == "dark") return "fleetwm Dark";
    if (key == "catppuccin") return "fleetwm Catppuccin";
    if (key == "dracula") return "fleetwm Dracula";
    if (key == "oled_black") return "fleetwm OLED Black";
    if (key == "light") return "fleetwm Light";
    return nullptr;
}

/*
 * Theme changes arrive through inotify on the Fleetwm config directories instead of a one second stat() poll: the poll woke
 * an idle terminal every second and made four stat() calls each time. The thread only flags the change and wakes the UI;
 * the UI still compares the file stamps, so a spurious event costs one stat pass.
 */
extern void app_wake(void);
static std::atomic<int> fleet_dirty;
static int fleet_fd = -1;
static std::vector<std::string> fleet_dirs;

static void fleet_add_watches() {
    for (const std::string &d : fleet_dirs)
        inotify_add_watch(fleet_fd, d.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
}

static void fleet_watch_main() {
    char buf[4096];
    while (read(fleet_fd, buf, sizeof buf) > 0) {
        fleet_add_watches();  /* a directory may have been created by the event; adding an existing watch is a no-op */
        fleet_dirty.store(1);
        app_wake();
    }
}

bool fleetwm_watch_start() {
    if (fleet_fd >= 0) return true;
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return false;
    std::string cfg = lx::config_dir();
    std::string mine = lx::path_join(cfg, "fleetwm");
    fleet_dirs = {mine, lx::path_join(mine, "themes"), "/etc/xdg/fleetwm", "/etc/xdg/fleetwm/themes", "/usr/local/etc/xdg/fleetwm",
                  "/usr/local/etc/xdg/fleetwm/themes"};
    fleet_fd = fd;
    fleet_add_watches();
    /* the user's fleetwm directory may not exist yet: watch the config directory for its creation */
    inotify_add_watch(fd, cfg.c_str(), IN_CREATE | IN_MOVED_TO);
    try {
        std::thread(fleet_watch_main).detach();
    } catch (...) {
        close(fd);
        fleet_fd = -1;
        return false;
    }
    return true;
}

bool fleetwm_watch_take() { return fleet_dirty.exchange(0) != 0; }
