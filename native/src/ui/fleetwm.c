/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "fleetwm.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

static char *user_theme_path(void) { return g_build_filename(g_get_user_config_dir(), "fleetwm", "theme.toml", NULL); }

static char *first_existing(char *const *paths) {
    for (int i = 0; paths[i]; i++) if (g_file_test(paths[i], G_FILE_TEST_IS_REGULAR)) return g_strdup(paths[i]);
    return NULL;
}

static char *theme_file(void) {
    char *u = user_theme_path();
    char *cands[] = {u, "/etc/xdg/fleetwm/theme.toml", "/usr/local/etc/xdg/fleetwm/theme.toml", NULL};
    char *r = first_existing(cands);
    g_free(u);
    return r;
}

static char *accent_file(void) {
    char *u = g_build_filename(g_get_user_config_dir(), "fleetwm", "themes", "accent.css", NULL);
    char *cands[] = {u, "/etc/xdg/fleetwm/themes/accent.css", "/usr/local/etc/xdg/fleetwm/themes/accent.css", NULL};
    char *r = first_existing(cands);
    g_free(u);
    return r;
}

static double mtime(const char *path) {
    GStatBuf st;
    return path && g_stat(path, &st) == 0 ? (double)st.st_mtime : 0;
}

double fleetwm_stamp(void) {
    char *t = theme_file(), *a = accent_file();
    double m = mtime(t), n = mtime(a);
    g_free(t); g_free(a);
    return m > n ? m : n;
}

static bool toml_get(const char *text, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p;) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = g_strndup(p, len);
        char *s = g_strstrip(line);
        if (strncmp(s, key, kl) == 0 && (s[kl] == ' ' || s[kl] == '=' || s[kl] == '\t')) {
            char *eq = strchr(s, '=');
            if (eq) {
                char *v = g_strstrip(eq + 1);
                if (*v == '"') { v++; char *q = strchr(v, '"'); if (q) *q = 0; }
                else { char *c = strchr(v, '#'); if (c) *c = 0; g_strstrip(v); }
                snprintf(out, cap, "%s", v);
                g_free(line);
                return true;
            }
        }
        g_free(line);
        p = eol ? eol + 1 : NULL;
    }
    return false;
}

void fleetwm_read(FleetCfg *o) {
    memset(o, 0, sizeof *o);
    snprintf(o->theme, sizeof o->theme, "dark");
    o->rounded = true;
    o->accent_auto = true;
    char *tf = theme_file();
    if (!tf) return;
    char *text = NULL;
    if (g_file_get_contents(tf, &text, NULL, NULL)) {
        o->found = true;
        char v[64];
        if (toml_get(text, "theme", v, sizeof v)) snprintf(o->theme, sizeof o->theme, "%s", v);
        if (toml_get(text, "corner_style", v, sizeof v)) o->rounded = strcmp(v, "sharp") != 0;
        if (toml_get(text, "accent", v, sizeof v)) {
            if (v[0] == '#' && strlen(v) == 7) { o->accent_auto = false; snprintf(o->accent, sizeof o->accent, "%s", v); }
        }
    }
    g_free(text);
    if (o->found && o->accent_auto) {
        char *af = accent_file(), *css = NULL;
        if (af && g_file_get_contents(af, &css, NULL, NULL)) {
            const char *d = strstr(css, "@define-color accent_color");
            const char *h = d ? strchr(d, '#') : NULL;
            if (h && strlen(h) >= 7) { char hex[8]; snprintf(hex, sizeof hex, "%.7s", h); snprintf(o->accent, sizeof o->accent, "%s", hex); }
        }
        g_free(css); g_free(af);
    }
    o->stamp = fleetwm_stamp();
    g_free(tf);
}

const char *fleetwm_theme_name(const char *key) {
    if (!strcmp(key, "dark")) return "fleetwm Dark";
    if (!strcmp(key, "catppuccin")) return "fleetwm Catppuccin";
    if (!strcmp(key, "dracula")) return "fleetwm Dracula";
    if (!strcmp(key, "oled_black")) return "fleetwm OLED Black";
    if (!strcmp(key, "light")) return "fleetwm Light";
    return NULL;
}
