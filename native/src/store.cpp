/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "store.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

#include "json.h"
#include "util.h"

SdConn::SdConn() {
    id = lx::uuid();
    for (char &c : id) if (c == '-') c = 'x';
}

std::string SdConn::dest() const {
    if (is_live()) return alias;
    return user.empty() ? host : user + "@" + host;
}

std::vector<std::string> SdConn::argv(const char *control_path) const {
    std::vector<std::string> a{"ssh"};
    if (!is_live()) {
        a.insert(a.end(), {"-p", std::to_string(port), "-o", "StrictHostKeyChecking=accept-new"});
    }
    if (control_path) {
        a.insert(a.end(), {"-o", "ControlMaster=yes", "-o", std::string("ControlPath=") + control_path, "-o", "ControlPersist=no"});
    }
    if (!key.empty() && !is_live()) {
        a.push_back("-i");
        a.push_back(key.starts_with("~/") ? lx::path_join(lx::home_dir(), key.substr(2)) : key);
    }
    if (x11 == "untrusted") a.push_back("-X");
    else if (x11 == "trusted") a.push_back("-Y");
    if (!lx::trim(options).empty()) {
        std::vector<std::string> extra;
        if (lx::shell_parse(options, extra)) a.insert(a.end(), extra.begin(), extra.end());
    }
    bool cmd = !lx::trim(remote_command).empty();
    if (cmd) a.push_back("-t");
    a.push_back(dest());
    if (cmd) a.push_back(std::string(lx::trim(remote_command)));
    return a;
}

std::string sd_config_dir() {
    std::string o = lx::env("LESTRIX_CONFIG_DIR");
    if (!o.empty()) return o;
    return lx::path_join(lx::config_dir(), "lestrix");
}

static std::unique_ptr<SdConn> conn_from_json(const JVal *o) {
    auto c = std::make_unique<SdConn>();
    auto S = [&](std::string &field, const char *k) { if (const char *v = json_str(json_get(o, k), nullptr)) field = v; };
    S(c->id, "id"); S(c->name, "name"); S(c->group, "group"); S(c->host, "host"); S(c->user, "user"); S(c->key, "key");
    S(c->x11, "x11"); S(c->options, "options"); S(c->remote_command, "remote_command"); S(c->alias, "alias"); S(c->color, "color");
    S(c->protocol, "protocol");
    c->port = (int)json_num(json_get(o, "port"), 22);
    c->last_used = json_num(json_get(o, "last_used"), 0);
    if (c->x11 != "untrusted" && c->x11 != "trusted") c->x11 = "off";
    if (c->protocol.empty()) c->protocol = "ssh";
    return c;
}

void SdStore::load() {
    auto text = lx::read_file(path);
    if (!text) return;
    auto root = json_parse(*text);
    if (!root) return;
    const JVal *arr = json_get(root.get(), "connections");
    if (arr && arr->type == J_ARR)
        for (const auto &o : arr->items)
            if (o->type == J_OBJ) conns.push_back(conn_from_json(o.get()));
    const JVal *m = json_get(root.get(), "meta");
    if (m && m->type == J_OBJ)
        for (size_t i = 0; i < m->keys.size(); i++) {
            SdConn &c = meta[m->keys[i]];
            c.color = json_str(json_get(m->items[i].get(), "color"), "");
            c.last_used = json_num(json_get(m->items[i].get(), "last_used"), 0);
        }
}

SdStore::SdStore(const std::string &p) : path(p.empty() ? lx::path_join(sd_config_dir(), "connections.json") : p) { load(); }

bool SdStore::save() {
    std::string o = "{\n  \"version\": 1,\n  \"connections\": [";
    for (size_t i = 0; i < conns.size(); i++) {
        const SdConn &c = *conns[i];
        o += i ? ",\n    {" : "\n    {";
        auto SF = [&](const char *k, const std::string &v) { o += std::string("\"") + k + "\": "; json_append_string(o, v); o += ", "; };
        SF("id", c.id); SF("name", c.name); SF("host", c.host); SF("group", c.group);
        o += lx::strprintf("\"port\": %d, ", c.port);
        SF("user", c.user); SF("key", c.key); SF("x11", c.x11); SF("options", c.options);
        SF("remote_command", c.remote_command); SF("alias", c.alias); SF("color", c.color); SF("protocol", c.protocol);
        o += lx::strprintf("\"last_used\": %.3f}", c.last_used);
    }
    o += "\n  ],\n  \"meta\": {";
    bool first = true;
    for (const auto &[id, m] : meta) {
        o += first ? "\n    " : ",\n    ";
        first = false;
        json_append_string(o, id);
        o += ": {\"color\": ";
        json_append_string(o, m.color);
        o += lx::strprintf(", \"last_used\": %.3f}", m.last_used);
    }
    o += "\n  }\n}\n";

    lx::mkdir_p(lx::dirname(path), 0700);
    std::string tmp = path + ".tmp";
    bool ok = lx::write_file(tmp, o);
    if (ok) { chmod(tmp.c_str(), 0600); ok = rename(tmp.c_str(), path.c_str()) == 0; }
    return ok;
}

std::vector<SdConn *> SdStore::all() {
    std::vector<SdConn *> a;
    for (auto &c : conns) a.push_back(c.get());
    for (auto &c : live) a.push_back(c.get());
    return a;
}

SdConn *SdStore::get(const std::string &id) {
    for (auto &c : conns) if (c->id == id) return c.get();
    for (auto &c : live) if (c->id == id) return c.get();
    return nullptr;
}

void SdStore::upsert(const SdConn &c) {
    for (auto &e : conns)
        if (e->id == c.id) { *e = c; save(); return; }
    conns.push_back(std::make_unique<SdConn>(c));
    save();
}

void SdStore::remove(const std::string &id) {
    std::erase_if(conns, [&](const auto &c) { return c->id == id; });
    save();
}

SdConn &SdStore::meta_for(const SdConn &c) { return meta[c.id]; }

void SdStore::touch(const std::string &id) {
    SdConn *c = get(id);
    if (!c) return;
    c->last_used = lx::real_time();
    if (c->is_live()) meta_for(*c).last_used = c->last_used;
    save();
}

void SdStore::set_color(const std::string &id, const std::string &color) {
    SdConn *c = get(id);
    if (!c) return;
    c->color = color;
    if (c->is_live()) meta_for(*c).color = color;
    save();
}

std::vector<SdConn *> SdStore::recent(int limit) {
    std::vector<SdConn *> out;
    for (SdConn *c : all()) if (c->last_used > 0) out.push_back(c);
    std::stable_sort(out.begin(), out.end(), [](const SdConn *a, const SdConn *b) { return a->last_used > b->last_used; });
    if ((int)out.size() > limit) out.resize((size_t)limit);
    return out;
}

void SdStore::reload_ssh_config(bool enabled, const char *cfg) {
    live.clear();
    if (!enabled) return;
    ConnList items = sd_parse_ssh_config(cfg ? cfg : lx::path_join(lx::home_dir(), ".ssh", "config"), true);
    for (auto &c : items) {
        bool saved = std::any_of(conns.begin(), conns.end(), [&](const auto &e) { return e->name == c->name; });
        if (saved) continue;
        auto m = meta.find(c->id);
        if (m != meta.end()) { c->color = m->second.color; c->last_used = m->second.last_used; }
        live.push_back(std::move(c));
    }
}

int SdStore::add_imported(ConnList items) {
    int added = 0;
    for (auto &c : items) {
        bool dup = std::any_of(conns.begin(), conns.end(), [&](const auto &e) {
            return e->name == c->name && e->host == c->host && e->port == c->port && e->user == c->user;
        });
        if (dup) continue;
        conns.push_back(std::move(c));
        added++;
    }
    if (added) save();
    return added;
}
