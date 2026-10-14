/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "json.h"

#include <charconv>
#include <cstdio>

#include "util.h"

namespace {

struct Parser {
    const char *p, *end;
    std::string *err;
    bool failed = false;

    void fail(const char *msg) {
        if (!failed && err) *err = std::string("JSON error: ") + msg;
        failed = true;
    }
    bool more() const { return p < end; }
    void ws() { while (p < end && (*p == ' ' || (*p >= '\t' && *p <= '\r'))) p++; }
    bool lit(const char *w, size_t n) {
        if ((size_t)(end - p) >= n && !std::char_traits<char>::compare(p, w, n)) { p += n; return true; }
        return false;
    }
};

int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i];
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
        else return -1;
    }
    return v;
}

std::string parse_string(Parser &ps) {
    ps.p++;
    std::string s;
    while (ps.more() && *ps.p != '"') {
        if (*ps.p == '\\' && ps.p + 1 < ps.end) {
            ps.p++;
            switch (*ps.p) {
            case 'n': s += '\n'; break;
            case 't': s += '\t'; break;
            case 'r': s += '\r'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'u':
                if (ps.p + 4 < ps.end) {
                    int cp = hex4(ps.p + 1);
                    ps.p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && ps.p + 6 < ps.end && ps.p[1] == '\\' && ps.p[2] == 'u') {
                        int lo = hex4(ps.p + 3);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ps.p += 6; }
                    }
                    if (cp > 0) lx::append_utf8(s, (uint32_t)cp);
                }
                break;
            default: s += *ps.p; break;
            }
            ps.p++;
        } else {
            s += *ps.p++;
        }
    }
    if (!ps.more()) ps.fail("unterminated string");
    else ps.p++;
    return s;
}

std::unique_ptr<JVal> make(JType t) { auto v = std::make_unique<JVal>(); v->type = t; return v; }

std::unique_ptr<JVal> value(Parser &ps, int depth) {
    ps.ws();
    if (!ps.more() || depth > 64) { ps.fail("unexpected end"); return nullptr; }
    char c = *ps.p;
    if (c == '"') { auto v = make(J_STR); v->str = parse_string(ps); return v; }
    if (c == '{') {
        auto v = make(J_OBJ);
        ps.p++;
        ps.ws();
        if (ps.more() && *ps.p == '}') { ps.p++; return v; }
        while (!ps.failed) {
            ps.ws();
            if (!ps.more() || *ps.p != '"') { ps.fail("expected key"); break; }
            std::string k = parse_string(ps);
            ps.ws();
            if (!ps.more() || *ps.p != ':') { ps.fail("expected ':'"); break; }
            ps.p++;
            auto item = value(ps, depth + 1);
            if (!item) break;
            v->keys.push_back(std::move(k));
            v->items.push_back(std::move(item));
            ps.ws();
            if (ps.more() && *ps.p == ',') { ps.p++; continue; }
            if (ps.more() && *ps.p == '}') { ps.p++; break; }
            ps.fail("expected ',' or '}'");
        }
        return v;
    }
    if (c == '[') {
        auto v = make(J_ARR);
        ps.p++;
        ps.ws();
        if (ps.more() && *ps.p == ']') { ps.p++; return v; }
        while (!ps.failed) {
            auto item = value(ps, depth + 1);
            if (!item) break;
            v->items.push_back(std::move(item));
            ps.ws();
            if (ps.more() && *ps.p == ',') { ps.p++; continue; }
            if (ps.more() && *ps.p == ']') { ps.p++; break; }
            ps.fail("expected ',' or ']'");
        }
        return v;
    }
    if (ps.lit("true", 4)) { auto v = make(J_BOOL); v->b = true; return v; }
    if (ps.lit("false", 5)) return make(J_BOOL);
    if (ps.lit("null", 4)) return make(J_NULL);
    if (c == '-' || (c >= '0' && c <= '9')) {
        double d = 0;
        auto r = std::from_chars(ps.p, ps.end, d);
        if (r.ec != std::errc()) { ps.fail("bad number"); return nullptr; }
        ps.p = r.ptr;
        auto v = make(J_NUM);
        v->num = d;
        return v;
    }
    ps.fail("unexpected character");
    return nullptr;
}

}

std::unique_ptr<JVal> json_parse(std::string_view text, std::string *err) {
    Parser ps{text.data(), text.data() + text.size(), err};
    auto v = value(ps, 0);
    if (ps.failed) return nullptr;
    return v;
}

const JVal *json_get(const JVal *obj, std::string_view key) {
    if (!obj || obj->type != J_OBJ) return nullptr;
    for (size_t i = 0; i < obj->keys.size(); i++)
        if (obj->keys[i] == key) return obj->items[i].get();
    return nullptr;
}

const char *json_str(const JVal *v, const char *def) { return v && v->type == J_STR ? v->str.c_str() : def; }
double json_num(const JVal *v, double def) { return v && v->type == J_NUM ? v->num : def; }

void json_append_string(std::string &out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
            else out += (char)c;
        }
    }
    out += '"';
}
