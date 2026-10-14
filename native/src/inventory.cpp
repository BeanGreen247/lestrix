/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <glob.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "store.h"
#include "util.h"

namespace {

using Vars = std::map<std::string, std::string>;

std::string unquote(std::string_view s) {
    std::string_view t = lx::trim(s);
    size_t n = t.size();
    if (n >= 2 && ((t[0] == '"' && t[n - 1] == '"') || (t[0] == '\'' && t[n - 1] == '\''))) return std::string(t.substr(1, n - 2));
    return std::string(t);
}

const std::string *var(const Vars &v, const char *a, const char *b, const char *c) {
    const std::string *r = nullptr;
    for (const char *k : {a, b, c}) {
        if (!k) continue;
        auto it = v.find(k);
        r = it == v.end() ? nullptr : &it->second;
        if (r && !r->empty()) break;
    }
    return (r && !r->empty() && r->find("{{") == std::string::npos) ? r : nullptr;
}

std::unique_ptr<SdConn> conn_from_vars(const std::string &name, const std::string &group, const Vars &v) {
    auto c = std::make_unique<SdConn>();
    c->name = name;
    c->group = group.empty() ? "ungrouped" : group;
    const std::string *host = var(v, "ansible_host", "ansible_ssh_host", nullptr);
    c->host = host ? *host : name;
    const std::string *port = var(v, "ansible_port", "ansible_ssh_port", nullptr);
    if (port && atoi(port->c_str()) > 0) c->port = atoi(port->c_str());
    if (const std::string *user = var(v, "ansible_user", "ansible_ssh_user", "ansible_remote_user")) c->user = *user;
    if (const std::string *key = var(v, "ansible_ssh_private_key_file", "ansible_private_key_file", nullptr)) c->key = *key;
    if (const std::string *common = var(v, "ansible_ssh_common_args", "ansible_ssh_extra_args", nullptr)) c->options = *common;
    return c;
}

void merge_vars(Vars &into, const Vars &from) { for (const auto &[k, v] : from) into[k] = v; }

void expand_pattern(const std::string &pat, std::vector<std::string> &out) {
    size_t lb = pat.find('['), rb = lb == std::string::npos ? lb : pat.find(']', lb);
    size_t colon = rb == std::string::npos ? rb : pat.find(':', lb);
    if (lb == std::string::npos || rb == std::string::npos || colon == std::string::npos || colon > rb) { out.push_back(pat); return; }
    std::string from = pat.substr(lb + 1, colon - lb - 1), to = pat.substr(colon + 1, rb - colon - 1), head = pat.substr(0, lb);
    auto digit = [](const std::string &s) { return !s.empty() && s[0] >= '0' && s[0] <= '9'; };
    if (digit(from) && digit(to)) {
        int a = atoi(from.c_str()), b = atoi(to.c_str()), width = from[0] == '0' ? (int)from.size() : 0;
        for (int i = a; i <= b && i - a < 4096; i++) expand_pattern(head + lx::strprintf("%0*d", width, i) + pat.substr(rb + 1), out);
    } else if (from.size() == 1 && to.size() == 1) {
        for (char ch = from[0]; ch <= to[0]; ch++) expand_pattern(head + ch + pat.substr(rb + 1), out);
    } else {
        out.push_back(pat);
    }
}

std::vector<std::string> split_words(std::string_view line) {
    std::vector<std::string> w;
    size_t p = 0;
    auto sp = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    while (p < line.size()) {
        while (p < line.size() && sp(line[p])) p++;
        if (p >= line.size()) break;
        std::string cur;
        char q = 0;
        while (p < line.size() && (q || !sp(line[p]))) {
            if (q && line[p] == q) q = 0;
            else if (!q && (line[p] == '"' || line[p] == '\'')) q = line[p];
            else cur += line[p];
            p++;
        }
        w.push_back(std::move(cur));
    }
    return w;
}

ConnList parse_ini(const std::string &text) {
    ConnList out;
    std::map<std::string, Vars> group_vars;
    struct HostRec { std::string name, group; Vars vars; };
    std::vector<HostRec> hosts;
    std::string section = "ungrouped", kind = "hosts";
    for (const std::string &raw : lx::split(text, '\n')) {
        std::string line(lx::trim(raw));
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            std::string inner = line.substr(1, line.size() - 2);
            size_t colon = inner.find(':');
            if (colon != std::string::npos) { kind = inner.substr(colon + 1); inner.resize(colon); } else kind = "hosts";
            section = inner;
            continue;
        }
        if (kind == "vars") {
            size_t eq = line.find('=');
            if (eq != std::string::npos) group_vars[section][std::string(lx::trim(line.substr(0, eq)))] = unquote(line.substr(eq + 1));
        } else if (kind == "hosts") {
            std::vector<std::string> w = split_words(line);
            if (w.empty()) continue;
            Vars hv;
            for (size_t i = 1; i < w.size(); i++) {
                size_t eq = w[i].find('=');
                if (eq != std::string::npos) hv[w[i].substr(0, eq)] = w[i].substr(eq + 1);
            }
            std::vector<std::string> names;
            expand_pattern(w[0], names);
            for (std::string &n : names) hosts.push_back({std::move(n), section, hv});
        }
    }
    std::set<std::string> seen;
    for (HostRec &r : hosts) {
        if (!seen.insert(r.name).second) continue;
        Vars merged;
        if (auto it = group_vars.find("all"); it != group_vars.end()) merge_vars(merged, it->second);
        if (auto it = group_vars.find(r.group); it != group_vars.end()) merge_vars(merged, it->second);
        merge_vars(merged, r.vars);
        out.push_back(conn_from_vars(r.name, r.group, merged));
    }
    return out;
}

enum YType { Y_SCALAR, Y_MAP, Y_SEQ };
struct YNode {
    YType t;
    std::string s;
    std::vector<std::string> keys;
    std::vector<std::unique_ptr<YNode>> vals;
    explicit YNode(YType ty) : t(ty) {}
};

const YNode *yget(const YNode *m, const char *key) {
    if (!m || m->t != Y_MAP) return nullptr;
    for (size_t i = 0; i < m->keys.size(); i++) if (m->keys[i] == key) return m->vals[i].get();
    return nullptr;
}

std::unique_ptr<YNode> parse_flow(const char *&p);

void skip_sp(const char *&p) { while (*p == ' ' || *p == '\t') p++; }

std::unique_ptr<YNode> parse_flow_scalar(const char *&p) {
    skip_sp(p);
    std::string s;
    char q = 0;
    if (*p == '"' || *p == '\'') q = *p++;
    while (*p && (q ? *p != q : (*p != ',' && *p != '}' && *p != ']' && *p != ':'))) s += *p++;
    if (q && *p == q) p++;
    auto n = std::make_unique<YNode>(Y_SCALAR);
    n->s = std::string(lx::trim(s));
    return n;
}

std::unique_ptr<YNode> parse_flow_item(const char *&p) { return (*p == '{' || *p == '[') ? parse_flow(p) : parse_flow_scalar(p); }

std::unique_ptr<YNode> parse_flow(const char *&p) {
    skip_sp(p);
    if (*p == '{') {
        p++;
        auto m = std::make_unique<YNode>(Y_MAP);
        for (;;) {
            skip_sp(p);
            if (!*p || *p == '}') { if (*p) p++; break; }
            auto k = parse_flow_scalar(p);
            skip_sp(p);
            std::unique_ptr<YNode> v;
            if (*p == ':') { p++; skip_sp(p); v = parse_flow_item(p); }
            else v = std::make_unique<YNode>(Y_SCALAR);
            m->keys.push_back(k->s);
            m->vals.push_back(std::move(v));
            skip_sp(p);
            if (*p == ',') p++;
        }
        return m;
    }
    if (*p == '[') {
        p++;
        auto q = std::make_unique<YNode>(Y_SEQ);
        for (;;) {
            skip_sp(p);
            if (!*p || *p == ']') { if (*p) p++; break; }
            q->vals.push_back(parse_flow_item(p));
            skip_sp(p);
            if (*p == ',') p++;
        }
        return q;
    }
    return parse_flow_scalar(p);
}

struct YLine { int indent; std::string text; };

std::string strip_comment(const char *s) {
    std::string o;
    char q = 0;
    for (const char *p = s; *p; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) break;
        o += *p;
    }
    return std::string(lx::trim(o));
}

bool is_dash(const std::string &t) { return t.starts_with("- ") || t == "-"; }

std::unique_ptr<YNode> parse_block(const std::vector<YLine> &ls, size_t &i, int indent);

std::unique_ptr<YNode> parse_value_after_key(const std::vector<YLine> &ls, size_t &i, int indent, const char *rest) {
    if (*rest) { const char *p = rest; return parse_flow_item(p); }
    if (i < ls.size() && ls[i].indent > indent) return parse_block(ls, i, ls[i].indent);
    if (i < ls.size() && ls[i].indent == indent && ls[i].text.starts_with("- ")) return parse_block(ls, i, indent);
    return std::make_unique<YNode>(Y_SCALAR);
}

std::unique_ptr<YNode> parse_block(const std::vector<YLine> &ls, size_t &i, int indent) {
    if (is_dash(ls[i].text)) {
        auto q = std::make_unique<YNode>(Y_SEQ);
        while (i < ls.size()) {
            const YLine &l = ls[i];
            if (l.indent != indent || !is_dash(l.text)) break;
            const char *rest = l.text.c_str() + 1;
            while (*rest == ' ') rest++;
            i++;
            const char *p = rest;
            q->vals.push_back(*rest ? parse_flow_item(p) : std::make_unique<YNode>(Y_SCALAR));
        }
        return q;
    }
    auto m = std::make_unique<YNode>(Y_MAP);
    while (i < ls.size()) {
        const YLine &l = ls[i];
        if (l.indent < indent) break;
        if (l.indent > indent) { i++; continue; }
        const char *colon = strstr(l.text.c_str(), ":");
        while (colon && colon[1] && colon[1] != ' ') colon = strstr(colon + 1, ":");
        if (!colon) { i++; continue; }
        std::string key = unquote(std::string_view(l.text.data(), (size_t)(colon - l.text.c_str())));
        const char *rest = colon + 1;
        while (*rest == ' ') rest++;
        i++;
        auto v = parse_value_after_key(ls, i, indent, rest);
        m->keys.push_back(std::move(key));
        m->vals.push_back(std::move(v));
    }
    return m;
}

std::unique_ptr<YNode> yaml_parse(const std::string &text) {
    std::vector<YLine> ls;
    for (const std::string &r : lx::split(text, '\n')) {
        int ind = 0;
        while (ind < (int)r.size() && r[(size_t)ind] == ' ') ind++;
        std::string t = strip_comment(r.c_str() + ind);
        if (t.empty() || t == "---" || t == "...") continue;
        ls.push_back({ind, std::move(t)});
    }
    if (ls.empty()) return nullptr;
    size_t i = 0;
    return parse_block(ls, i, ls[0].indent);
}

void yaml_vars_into(Vars &vars, const YNode *m) {
    if (!m || m->t != Y_MAP) return;
    for (size_t i = 0; i < m->keys.size(); i++)
        if (m->vals[i]->t == Y_SCALAR) vars[m->keys[i]] = m->vals[i]->s;
}

void walk_group(const std::string &name, const YNode *node, const Vars &inherited, ConnList &out, std::set<std::string> &seen) {
    if (!node || node->t != Y_MAP) return;
    Vars vars = inherited;
    yaml_vars_into(vars, yget(node, "vars"));
    const YNode *hosts = yget(node, "hosts");
    if (hosts && hosts->t == Y_MAP) {
        for (size_t i = 0; i < hosts->keys.size(); i++) {
            std::vector<std::string> names;
            expand_pattern(hosts->keys[i], names);
            for (const std::string &n : names) {
                Vars hv = vars;
                yaml_vars_into(hv, hosts->vals[i].get());
                if (seen.insert(n).second) out.push_back(conn_from_vars(n, name == "all" ? "ungrouped" : name, hv));
            }
        }
    }
    const YNode *children = yget(node, "children");
    if (children && children->t == Y_MAP)
        for (size_t i = 0; i < children->keys.size(); i++) walk_group(children->keys[i], children->vals[i].get(), vars, out, seen);
}

ConnList parse_yaml_inventory(const std::string &text) {
    ConnList out;
    auto root = yaml_parse(text);
    std::set<std::string> seen;
    if (root && root->t == Y_MAP)
        for (size_t i = 0; i < root->keys.size(); i++) walk_group(root->keys[i], root->vals[i].get(), {}, out, seen);
    return out;
}

void config_lines(const std::string &path, int depth, std::vector<std::string> &out) {
    auto text = lx::read_file(path);
    if (!text) return;
    for (const std::string &raw : lx::split(*text, '\n')) {
        std::string t(lx::trim(raw));
        if (lx::iprefix(t, "include ") && depth < 5) {
            for (const std::string &p : lx::split_any(t.substr(8), " \t")) {
                if (p.empty()) continue;
                std::string pat = p.starts_with("~/") ? lx::path_join(lx::home_dir(), p.substr(2))
                                  : lx::is_absolute(p) ? p
                                  : lx::path_join(lx::home_dir(), ".ssh", p);
                glob_t g;
                if (glob(pat.c_str(), 0, nullptr, &g) == 0) {
                    for (size_t i = 0; i < g.gl_pathc; i++) config_lines(g.gl_pathv[i], depth + 1, out);
                    globfree(&g);
                }
            }
        } else {
            out.push_back(std::move(t));
        }
    }
}

}

std::optional<ConnList> sd_parse_ansible_inventory(const std::string &path, std::string *err) {
    auto text = lx::read_file(path);
    if (!text) { if (err) *err = "cannot read " + path; return std::nullopt; }
    bool yaml = path.ends_with(".yml") || path.ends_with(".yaml");
    if (!yaml) {
        for (const std::string &raw : lx::split(*text, '\n')) {
            std::string_view line = lx::trim(raw);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            yaml = line.back() == ':' && line.find('=') == std::string_view::npos && line[0] != '[';
            break;
        }
        if (text->starts_with("---")) yaml = true;
    }
    return yaml ? parse_yaml_inventory(*text) : parse_ini(*text);
}

ConnList sd_parse_ssh_config(const std::string &path, bool live) {
    std::vector<std::string> lines;
    config_lines(path, 0, lines);
    ConnList out;
    using Block = std::shared_ptr<Vars>;
    Block cur;
    std::vector<std::string> aliases;
    std::vector<Block> alias_opts;
    for (const std::string &line : lines) {
        if (line.empty() || line[0] == '#') continue;
        size_t sp = line.find_first_of(" \t=");
        std::string key = lx::ascii_lower(line.substr(0, sp));
        while (sp != std::string::npos && sp < line.size() && (line[sp] == ' ' || line[sp] == '\t' || line[sp] == '=')) sp++;
        std::string value = sp == std::string::npos ? std::string() : unquote(line.substr(sp));
        if (key == "host") {
            cur = std::make_shared<Vars>();
            for (const std::string &n : lx::split_any(value, " \t"))
                if (!n.empty() && n.find_first_of("*?!") == std::string::npos) { aliases.push_back(n); alias_opts.push_back(cur); }
        } else if (key == "match") {
            cur = nullptr;
        } else if (cur && !cur->contains(key)) {
            (*cur)[key] = value;
        }
    }
    for (size_t i = 0; i < aliases.size(); i++) {
        const std::string &alias = aliases[i];
        const Vars &o = *alias_opts[i];
        bool dup = false;
        for (size_t j = 0; j < i && !dup; j++) dup = aliases[j] == alias;
        if (dup) continue;
        auto c = std::make_unique<SdConn>();
        auto opt = [&](const char *k) -> const std::string * { auto it = o.find(k); return it == o.end() ? nullptr : &it->second; };
        c->name = alias;
        const std::string *v;
        c->host = (v = opt("hostname")) ? *v : alias;
        c->group = "ssh config";
        if ((v = opt("port")) && atoi(v->c_str()) > 0) c->port = atoi(v->c_str());
        if ((v = opt("user"))) c->user = *v;
        if ((v = opt("identityfile"))) c->key = *v;
        bool fwd = (v = opt("forwardx11")) && lx::iequals(*v, "yes");
        bool trusted = (v = opt("forwardx11trusted")) && lx::iequals(*v, "yes");
        c->x11 = fwd ? (trusted ? "trusted" : "untrusted") : "off";
        if (live) {
            c->alias = alias;
            c->id = "sshcfg:" + alias;
        } else if ((v = opt("proxyjump"))) {
            c->options = "-J " + *v;
        }
        out.push_back(std::move(c));
    }
    return out;
}
