/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; const char *end; GError **err; gboolean failed; } Parser;

static void fail(Parser *ps, const char *msg) {
    if (!ps->failed) g_set_error(ps->err, g_quark_from_static_string("json"), 1, "JSON error: %s", msg);
    ps->failed = TRUE;
}

static void ws(Parser *ps) { while (ps->p < ps->end && g_ascii_isspace(*ps->p)) ps->p++; }
static JVal *value(Parser *ps, int depth);

static JVal *new_val(JType t) { JVal *v = g_new0(JVal, 1); v->type = t; return v; }

void json_free(JVal *v) {
    if (!v) return;
    g_free(v->str);
    if (v->items) { for (guint i = 0; i < v->items->len; i++) json_free(v->items->pdata[i]); g_ptr_array_free(v->items, TRUE); }
    if (v->keys) g_ptr_array_free(v->keys, TRUE);
    g_free(v);
}

static int hex4(const char *p) {
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

static char *parse_string(Parser *ps) {
    ps->p++;
    GString *s = g_string_new(NULL);
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\' && ps->p + 1 < ps->end) {
            ps->p++;
            switch (*ps->p) {
            case 'n': g_string_append_c(s, '\n'); break;
            case 't': g_string_append_c(s, '\t'); break;
            case 'r': g_string_append_c(s, '\r'); break;
            case 'b': g_string_append_c(s, '\b'); break;
            case 'f': g_string_append_c(s, '\f'); break;
            case 'u':
                if (ps->p + 4 < ps->end) {
                    int cp = hex4(ps->p + 1);
                    ps->p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && ps->p + 6 < ps->end && ps->p[1] == '\\' && ps->p[2] == 'u') {
                        int lo = hex4(ps->p + 3);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ps->p += 6; }
                    }
                    if (cp > 0) g_string_append_unichar(s, (gunichar)cp);
                }
                break;
            default: g_string_append_c(s, *ps->p); break;
            }
            ps->p++;
        } else {
            g_string_append_c(s, *ps->p++);
        }
    }
    if (ps->p >= ps->end) fail(ps, "unterminated string");
    else ps->p++;
    return g_string_free(s, FALSE);
}

static JVal *value(Parser *ps, int depth) {
    ws(ps);
    if (ps->p >= ps->end || depth > 64) { fail(ps, "unexpected end"); return NULL; }
    char c = *ps->p;
    if (c == '"') { JVal *v = new_val(J_STR); v->str = parse_string(ps); return v; }
    if (c == '{') {
        JVal *v = new_val(J_OBJ);
        v->items = g_ptr_array_new();
        v->keys = g_ptr_array_new_with_free_func(g_free);
        ps->p++;
        ws(ps);
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; return v; }
        while (!ps->failed) {
            ws(ps);
            if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected key"); break; }
            char *k = parse_string(ps);
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { g_free(k); fail(ps, "expected ':'"); break; }
            ps->p++;
            JVal *item = value(ps, depth + 1);
            if (!item) { g_free(k); break; }
            g_ptr_array_add(v->keys, k);
            g_ptr_array_add(v->items, item);
            ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == '}') { ps->p++; break; }
            fail(ps, "expected ',' or '}'");
        }
        return v;
    }
    if (c == '[') {
        JVal *v = new_val(J_ARR);
        v->items = g_ptr_array_new();
        ps->p++;
        ws(ps);
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; return v; }
        while (!ps->failed) {
            JVal *item = value(ps, depth + 1);
            if (!item) break;
            g_ptr_array_add(v->items, item);
            ws(ps);
            if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
            if (ps->p < ps->end && *ps->p == ']') { ps->p++; break; }
            fail(ps, "expected ',' or ']'");
        }
        return v;
    }
    if (!strncmp(ps->p, "true", 4)) { ps->p += 4; JVal *v = new_val(J_BOOL); v->b = TRUE; return v; }
    if (!strncmp(ps->p, "false", 5)) { ps->p += 5; return new_val(J_BOOL); }
    if (!strncmp(ps->p, "null", 4)) { ps->p += 4; return new_val(J_NULL); }
    if (c == '-' || g_ascii_isdigit(c)) {
        char *endp;
        double d = g_ascii_strtod(ps->p, &endp);
        if (endp == ps->p) { fail(ps, "bad number"); return NULL; }
        ps->p = endp;
        JVal *v = new_val(J_NUM);
        v->num = d;
        return v;
    }
    fail(ps, "unexpected character");
    return NULL;
}

JVal *json_parse(const char *text, GError **err) {
    Parser ps = {text, text + strlen(text), err, FALSE};
    JVal *v = value(&ps, 0);
    if (ps.failed) { json_free(v); return NULL; }
    return v;
}

const JVal *json_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (guint i = 0; i < obj->keys->len; i++)
        if (g_str_equal(obj->keys->pdata[i], key)) return obj->items->pdata[i];
    return NULL;
}

const char *json_str(const JVal *v, const char *def) { return v && v->type == J_STR ? v->str : def; }
double json_num(const JVal *v, double def) { return v && v->type == J_NUM ? v->num : def; }

void json_append_string(GString *out, const char *s) {
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n"); break;
        case '\r': g_string_append(out, "\\r"); break;
        case '\t': g_string_append(out, "\\t"); break;
        default:
            if (*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
            else g_string_append_c(out, (char)*p);
        }
    }
    g_string_append_c(out, '"');
}
