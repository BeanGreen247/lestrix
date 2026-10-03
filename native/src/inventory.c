/* inventory.c - Ansible inventories (INI and YAML) and ~/.ssh/config. */
#include <glob.h>
#include <string.h>

#include "store.h"

/* ---- shared helpers --------------------------------------------------------------------- */

static char *dup_strip(const char *s) { return g_strstrip(g_strdup(s)); }

static char *unquote(const char *s) {
    char *t = dup_strip(s);
    size_t n = strlen(t);
    if (n >= 2 && ((t[0] == '"' && t[n - 1] == '"') || (t[0] == '\'' && t[n - 1] == '\''))) {
        char *u = g_strndup(t + 1, n - 2);
        g_free(t);
        return u;
    }
    return t;
}

static const char *var(GHashTable *v, const char *a, const char *b, const char *c) {
    const char *r = NULL;
    if (a) r = g_hash_table_lookup(v, a);
    if ((!r || !*r) && b) r = g_hash_table_lookup(v, b);
    if ((!r || !*r) && c) r = g_hash_table_lookup(v, c);
    return (r && *r && !strstr(r, "{{")) ? r : NULL;   /* unresolved Jinja is ignored */
}

static SdConn *conn_from_vars(const char *name, const char *group, GHashTable *v) {
    SdConn *c = sd_conn_new();
    sd_conn_set(&c->name, name);
    sd_conn_set(&c->group, group && *group ? group : "ungrouped");
    const char *host = var(v, "ansible_host", "ansible_ssh_host", NULL);
    sd_conn_set(&c->host, host ? host : name);
    const char *port = var(v, "ansible_port", "ansible_ssh_port", NULL);
    if (port && atoi(port) > 0) c->port = atoi(port);
    const char *user = var(v, "ansible_user", "ansible_ssh_user", "ansible_remote_user");
    if (user) sd_conn_set(&c->user, user);
    const char *key = var(v, "ansible_ssh_private_key_file", "ansible_private_key_file", NULL);
    if (key) sd_conn_set(&c->key, key);
    const char *common = var(v, "ansible_ssh_common_args", "ansible_ssh_extra_args", NULL);
    if (common) sd_conn_set(&c->options, common);
    return c;
}

static GHashTable *new_vars(void) { return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); }

static void merge_vars(GHashTable *into, GHashTable *from) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, from);
    while (g_hash_table_iter_next(&it, &k, &v)) g_hash_table_insert(into, g_strdup(k), g_strdup(v));
}

static GHashTable *copy_vars(GHashTable *src) {
    GHashTable *d = new_vars();
    if (src) merge_vars(d, src);
    return d;
}

/* ---- host patterns: web[01:05].example.com ------------------------------------------------------ */

static void expand_pattern(const char *pat, GPtrArray *out) {
    const char *lb = strchr(pat, '['), *rb = lb ? strchr(lb, ']') : NULL, *colon = lb ? memchr(lb, ':', rb ? (size_t)(rb - lb) : 0) : NULL;
    if (!lb || !rb || !colon) { g_ptr_array_add(out, g_strdup(pat)); return; }
    char *from = g_strndup(lb + 1, (gsize)(colon - lb - 1)), *to = g_strndup(colon + 1, (gsize)(rb - colon - 1));
    char *head = g_strndup(pat, (gsize)(lb - pat));
    if (g_ascii_isdigit(*from) && g_ascii_isdigit(*to)) {
        int a = atoi(from), b = atoi(to), width = (from[0] == '0') ? (int)strlen(from) : 0;
        for (int i = a; i <= b && i - a < 4096; i++) {
            char *mid = g_strdup_printf("%0*d", width, i);
            char *s = g_strconcat(head, mid, rb + 1, NULL);
            expand_pattern(s, out);
            g_free(mid); g_free(s);
        }
    } else if (strlen(from) == 1 && strlen(to) == 1) {
        for (char ch = from[0]; ch <= to[0]; ch++) {
            char mid[2] = {ch, 0};
            char *s = g_strconcat(head, mid, rb + 1, NULL);
            expand_pattern(s, out);
            g_free(s);
        }
    } else {
        g_ptr_array_add(out, g_strdup(pat));
    }
    g_free(from); g_free(to); g_free(head);
}

/* ---- INI ------------------------------------------------------------------------------------------ */

static char **split_words(const char *line) {
    GPtrArray *w = g_ptr_array_new();
    const char *p = line;
    while (*p) {
        while (*p && g_ascii_isspace(*p)) p++;
        if (!*p) break;
        GString *cur = g_string_new(NULL);
        char q = 0;
        while (*p && (q || !g_ascii_isspace(*p))) {
            if (q && *p == q) q = 0;
            else if (!q && (*p == '"' || *p == '\'')) q = *p;
            else g_string_append_c(cur, *p);
            p++;
        }
        g_ptr_array_add(w, g_string_free(cur, FALSE));
    }
    g_ptr_array_add(w, NULL);
    return (char **)g_ptr_array_free(w, FALSE);
}

typedef struct { char *name; GHashTable *vars; } IniGroup;

static GPtrArray *parse_ini(const char *text) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)sd_conn_free);
    GHashTable *group_vars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_hash_table_destroy);
    typedef struct { char *name; char *group; GHashTable *vars; } HostRec;
    GPtrArray *hosts = g_ptr_array_new();
    char *section = g_strdup("ungrouped"), *kind = g_strdup("hosts");
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *line = dup_strip(*l);
        if (!*line || *line == '#' || *line == ';') { g_free(line); continue; }
        if (*line == '[' && line[strlen(line) - 1] == ']') {
            g_free(section); g_free(kind);
            char *inner = g_strndup(line + 1, strlen(line) - 2);
            char *colon = strchr(inner, ':');
            if (colon) { *colon = 0; kind = g_strdup(colon + 1); } else kind = g_strdup("hosts");
            section = inner;
            g_free(line);
            continue;
        }
        if (g_str_equal(kind, "vars")) {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                GHashTable *gv = g_hash_table_lookup(group_vars, section);
                if (!gv) { gv = new_vars(); g_hash_table_insert(group_vars, g_strdup(section), gv); }
                char *k = dup_strip(line), *v = unquote(eq + 1);
                g_hash_table_insert(gv, k, v);
            }
        } else if (g_str_equal(kind, "hosts")) {
            char **w = split_words(line);
            if (w[0]) {
                GHashTable *hv = new_vars();
                for (char **p = w + 1; *p; p++) {
                    char *eq = strchr(*p, '=');
                    if (eq) g_hash_table_insert(hv, g_strndup(*p, (gsize)(eq - *p)), g_strdup(eq + 1));
                }
                GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
                expand_pattern(w[0], names);
                for (guint i = 0; i < names->len; i++) {
                    HostRec *r = g_new0(HostRec, 1);
                    r->name = g_strdup(names->pdata[i]);
                    r->group = g_strdup(section);
                    r->vars = copy_vars(hv);
                    g_ptr_array_add(hosts, r);
                }
                g_ptr_array_free(names, TRUE);
                g_hash_table_destroy(hv);
            }
            g_strfreev(w);
        }
        g_free(line);
    }
    g_strfreev(lines);

    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < hosts->len; i++) {
        HostRec *r = hosts->pdata[i];
        if (!g_hash_table_contains(seen, r->name)) {
            GHashTable *merged = new_vars();
            GHashTable *all = g_hash_table_lookup(group_vars, "all"), *gv = g_hash_table_lookup(group_vars, r->group);
            if (all) merge_vars(merged, all);
            if (gv) merge_vars(merged, gv);
            merge_vars(merged, r->vars);
            SdConn *c = conn_from_vars(r->name, r->group, merged);
            g_ptr_array_add(out, c);
            g_hash_table_insert(seen, c->name, c);
            g_hash_table_destroy(merged);
        }
        g_free(r->name); g_free(r->group); g_hash_table_destroy(r->vars); g_free(r);
    }
    g_ptr_array_free(hosts, TRUE);
    g_hash_table_destroy(seen);
    g_hash_table_destroy(group_vars);
    g_free(section); g_free(kind);
    return out;
}

/* ---- YAML subset: block maps, `- ` sequences, flow {..}/[..], quoted scalars ----------------------------- */

typedef enum { Y_SCALAR, Y_MAP, Y_SEQ } YType;
typedef struct YNode { YType t; char *s; GPtrArray *keys, *vals; } YNode;

static YNode *ynew(YType t) {
    YNode *n = g_new0(YNode, 1);
    n->t = t;
    if (t != Y_SCALAR) { n->vals = g_ptr_array_new(); if (t == Y_MAP) n->keys = g_ptr_array_new_with_free_func(g_free); }
    return n;
}

static void yfree(YNode *n) {
    if (!n) return;
    g_free(n->s);
    if (n->vals) { for (guint i = 0; i < n->vals->len; i++) yfree(n->vals->pdata[i]); g_ptr_array_free(n->vals, TRUE); }
    if (n->keys) g_ptr_array_free(n->keys, TRUE);
    g_free(n);
}

static YNode *yget(const YNode *m, const char *key) {
    if (!m || m->t != Y_MAP) return NULL;
    for (guint i = 0; i < m->keys->len; i++) if (g_str_equal(m->keys->pdata[i], key)) return m->vals->pdata[i];
    return NULL;
}

static YNode *parse_flow(const char **pp);

static void skip_sp(const char **pp) { while (**pp == ' ' || **pp == '\t') (*pp)++; }

static YNode *parse_flow_scalar(const char **pp) {
    skip_sp(pp);
    GString *s = g_string_new(NULL);
    char q = 0;
    if (**pp == '"' || **pp == '\'') q = *(*pp)++;
    while (**pp && (q ? **pp != q : (**pp != ',' && **pp != '}' && **pp != ']' && **pp != ':'))) g_string_append_c(s, *(*pp)++);
    if (q && **pp == q) (*pp)++;
    YNode *n = ynew(Y_SCALAR);
    n->s = g_strstrip(g_string_free(s, FALSE));
    return n;
}

static YNode *parse_flow(const char **pp) {
    skip_sp(pp);
    if (**pp == '{') {
        (*pp)++;
        YNode *m = ynew(Y_MAP);
        for (;;) {
            skip_sp(pp);
            if (!**pp || **pp == '}') { if (**pp) (*pp)++; break; }
            YNode *k = parse_flow_scalar(pp);
            skip_sp(pp);
            YNode *v;
            if (**pp == ':') { (*pp)++; skip_sp(pp); v = (**pp == '{' || **pp == '[') ? parse_flow(pp) : parse_flow_scalar(pp); }
            else { v = ynew(Y_SCALAR); v->s = g_strdup(""); }
            g_ptr_array_add(m->keys, g_strdup(k->s));
            g_ptr_array_add(m->vals, v);
            yfree(k);
            skip_sp(pp);
            if (**pp == ',') (*pp)++;
        }
        return m;
    }
    if (**pp == '[') {
        (*pp)++;
        YNode *q = ynew(Y_SEQ);
        for (;;) {
            skip_sp(pp);
            if (!**pp || **pp == ']') { if (**pp) (*pp)++; break; }
            g_ptr_array_add(q->vals, (**pp == '{' || **pp == '[') ? parse_flow(pp) : parse_flow_scalar(pp));
            skip_sp(pp);
            if (**pp == ',') (*pp)++;
        }
        return q;
    }
    return parse_flow_scalar(pp);
}

typedef struct { int indent; char *text; } YLine;

static char *strip_comment(const char *s) {
    GString *o = g_string_new(NULL);
    char q = 0;
    for (const char *p = s; *p; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) break;
        g_string_append_c(o, *p);
    }
    return g_strstrip(g_string_free(o, FALSE));
}

static YNode *parse_block(GArray *ls, guint *i, int indent);

static YNode *parse_value_after_key(GArray *ls, guint *i, int indent, const char *rest) {
    if (*rest) {
        const char *p = rest;
        return (*rest == '{' || *rest == '[') ? parse_flow(&p) : parse_flow_scalar(&p);
    }
    if (*i < ls->len && g_array_index(ls, YLine, *i).indent > indent) return parse_block(ls, i, g_array_index(ls, YLine, *i).indent);
    if (*i < ls->len && g_array_index(ls, YLine, *i).indent == indent && g_str_has_prefix(g_array_index(ls, YLine, *i).text, "- "))
        return parse_block(ls, i, indent);   /* `key:` followed by an unindented sequence */
    YNode *n = ynew(Y_SCALAR);
    n->s = g_strdup("");
    return n;
}

static YNode *parse_block(GArray *ls, guint *i, int indent) {
    YLine *first = &g_array_index(ls, YLine, *i);
    if (g_str_has_prefix(first->text, "- ") || g_str_equal(first->text, "-")) {
        YNode *q = ynew(Y_SEQ);
        while (*i < ls->len) {
            YLine *l = &g_array_index(ls, YLine, *i);
            if (l->indent != indent || !(g_str_has_prefix(l->text, "- ") || g_str_equal(l->text, "-"))) break;
            const char *rest = l->text + 1;
            while (*rest == ' ') rest++;
            (*i)++;
            const char *p = rest;
            g_ptr_array_add(q->vals, *rest ? ((*rest == '{' || *rest == '[') ? parse_flow(&p) : parse_flow_scalar(&p)) : ynew(Y_SCALAR));
        }
        return q;
    }
    YNode *m = ynew(Y_MAP);
    while (*i < ls->len) {
        YLine *l = &g_array_index(ls, YLine, *i);
        if (l->indent < indent) break;
        if (l->indent > indent) { (*i)++; continue; }
        char *colon = strstr(l->text, ":");
        while (colon && colon[1] && colon[1] != ' ') colon = strstr(colon + 1, ":");
        if (!colon) { (*i)++; continue; }
        char *raw_key = g_strndup(l->text, (gsize)(colon - l->text));
        char *key = unquote(raw_key);
        g_free(raw_key);
        const char *rest = colon + 1;
        while (*rest == ' ') rest++;
        (*i)++;
        YNode *v = parse_value_after_key(ls, i, indent, rest);
        g_ptr_array_add(m->keys, key);
        g_ptr_array_add(m->vals, v);
    }
    return m;
}

static YNode *yaml_parse(const char *text) {
    GArray *ls = g_array_new(FALSE, FALSE, sizeof(YLine));
    char **raw = g_strsplit(text, "\n", -1);
    for (char **r = raw; *r; r++) {
        int ind = 0;
        while ((*r)[ind] == ' ') ind++;
        char *t = strip_comment(*r + ind);
        if (!*t || g_str_equal(t, "---") || g_str_equal(t, "...")) { g_free(t); continue; }
        YLine l = {ind, t};
        g_array_append_val(ls, l);
    }
    g_strfreev(raw);
    YNode *root = NULL;
    if (ls->len) { guint i = 0; root = parse_block(ls, &i, g_array_index(ls, YLine, 0).indent); }
    for (guint i = 0; i < ls->len; i++) g_free(g_array_index(ls, YLine, i).text);
    g_array_free(ls, TRUE);
    return root;
}

static void yaml_vars_into(GHashTable *vars, const YNode *m) {
    if (!m || m->t != Y_MAP) return;
    for (guint i = 0; i < m->keys->len; i++) {
        YNode *v = m->vals->pdata[i];
        if (v->t == Y_SCALAR) g_hash_table_insert(vars, g_strdup(m->keys->pdata[i]), g_strdup(v->s));
    }
}

static void walk_group(const char *name, const YNode *node, GHashTable *inherited, GPtrArray *out, GHashTable *seen) {
    if (!node || node->t != Y_MAP) return;
    GHashTable *vars = copy_vars(inherited);
    yaml_vars_into(vars, yget(node, "vars"));
    YNode *hosts = yget(node, "hosts");
    if (hosts && hosts->t == Y_MAP) {
        for (guint i = 0; i < hosts->keys->len; i++) {
            const char *hname = hosts->keys->pdata[i];
            GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
            expand_pattern(hname, names);
            for (guint k = 0; k < names->len; k++) {
                GHashTable *hv = copy_vars(vars);
                yaml_vars_into(hv, hosts->vals->pdata[i]);
                if (!g_hash_table_contains(seen, names->pdata[k])) {
                    SdConn *c = conn_from_vars(names->pdata[k], g_str_equal(name, "all") ? "ungrouped" : name, hv);
                    g_ptr_array_add(out, c);
                    g_hash_table_insert(seen, c->name, c);
                }
                g_hash_table_destroy(hv);
            }
            g_ptr_array_free(names, TRUE);
        }
    }
    YNode *children = yget(node, "children");
    if (children && children->t == Y_MAP)
        for (guint i = 0; i < children->keys->len; i++) walk_group(children->keys->pdata[i], children->vals->pdata[i], vars, out, seen);
    g_hash_table_destroy(vars);
}

static GPtrArray *parse_yaml_inventory(const char *text) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)sd_conn_free);
    YNode *root = yaml_parse(text);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal), *none = new_vars();
    if (root && root->t == Y_MAP)
        for (guint i = 0; i < root->keys->len; i++) walk_group(root->keys->pdata[i], root->vals->pdata[i], none, out, seen);
    g_hash_table_destroy(seen);
    g_hash_table_destroy(none);
    yfree(root);
    return out;
}

GPtrArray *sd_parse_ansible_inventory(const char *path, GError **err) {
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, err)) return NULL;
    gboolean yaml = g_str_has_suffix(path, ".yml") || g_str_has_suffix(path, ".yaml");
    if (!yaml) {   /* extensionless: a YAML inventory starts with a `key:` line, an INI one with [section] or a host */
        for (char *p = text; *p; ) {
            char *eol = strchr(p, '\n');
            char *line = g_strndup(p, eol ? (gsize)(eol - p) : strlen(p));
            g_strstrip(line);
            if (*line && *line != '#' && *line != ';') { yaml = line[strlen(line) - 1] == ':' && strchr(line, '=') == NULL && *line != '['; g_free(line); break; }
            g_free(line);
            if (!eol) break;
            p = eol + 1;
        }
        if (g_str_has_prefix(text, "---")) yaml = TRUE;
    }
    GPtrArray *r = yaml ? parse_yaml_inventory(text) : parse_ini(text);
    g_free(text);
    return r;
}

/* ---- ~/.ssh/config --------------------------------------------------------------------------------------- */

static void config_lines(const char *path, int depth, GPtrArray *out) {
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL)) return;
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    for (char **l = lines; *l; l++) {
        char *t = dup_strip(*l);
        if (g_ascii_strncasecmp(t, "include ", 8) == 0 && depth < 5) {
            char **pats = g_strsplit_set(t + 8, " \t", -1);
            for (char **p = pats; *p; p++) {
                if (!**p) continue;
                char *pat = g_str_has_prefix(*p, "~/") ? g_build_filename(g_get_home_dir(), *p + 2, NULL)
                            : g_path_is_absolute(*p) ? g_strdup(*p)
                            : g_build_filename(g_get_home_dir(), ".ssh", *p, NULL);
                glob_t g;
                if (glob(pat, 0, NULL, &g) == 0) {
                    for (size_t i = 0; i < g.gl_pathc; i++) config_lines(g.gl_pathv[i], depth + 1, out);
                    globfree(&g);
                }
                g_free(pat);
            }
            g_strfreev(pats);
            g_free(t);
        } else {
            g_ptr_array_add(out, t);
        }
    }
    g_strfreev(lines);
}

GPtrArray *sd_parse_ssh_config(const char *path, gboolean live) {
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    config_lines(path, 0, lines);
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)sd_conn_free);
    GHashTable *cur = NULL;
    GPtrArray *aliases = g_ptr_array_new_with_free_func(g_free), *alias_opts = g_ptr_array_new(), *blocks_opts = g_ptr_array_new();
    for (guint i = 0; i < lines->len; i++) {
        char *line = lines->pdata[i];
        if (!*line || *line == '#') continue;
        char *sp = line + strcspn(line, " \t=");
        char *raw_key = g_strndup(line, (gsize)(sp - line));
        char *key = g_ascii_strdown(raw_key, -1);
        g_free(raw_key);
        while (*sp == ' ' || *sp == '\t' || *sp == '=') sp++;
        char *value = unquote(sp);
        if (g_str_equal(key, "host")) {
            cur = new_vars();
            g_ptr_array_add(blocks_opts, cur);
            char **names = g_strsplit_set(value, " \t", -1);
            gboolean any = FALSE;
            for (char **n = names; *n; n++)
                if (**n && !strpbrk(*n, "*?!")) { g_ptr_array_add(aliases, g_strdup(*n)); g_ptr_array_add(alias_opts, cur); any = TRUE; }
            (void)any;
            g_strfreev(names);
        } else if (g_str_equal(key, "match")) {
            cur = NULL;
        } else if (cur && !g_hash_table_contains(cur, key)) {
            g_hash_table_insert(cur, g_strdup(key), g_strdup(value));
        }
        g_free(key); g_free(value);
    }
    for (guint i = 0; i < aliases->len; i++) {
        const char *alias = aliases->pdata[i];
        GHashTable *o = alias_opts->pdata[i];
        gboolean dup = FALSE;  /* the same alias in two Host lines: ssh uses the first, so list it once */
        for (guint j = 0; j < i && !dup; j++) dup = g_str_equal(aliases->pdata[j], alias);
        if (dup) continue;
        SdConn *c = sd_conn_new();
        sd_conn_set(&c->name, alias);
        const char *hn = g_hash_table_lookup(o, "hostname");
        sd_conn_set(&c->host, hn ? hn : alias);
        sd_conn_set(&c->group, "ssh config");
        const char *v;
        if ((v = g_hash_table_lookup(o, "port")) && atoi(v) > 0) c->port = atoi(v);
        if ((v = g_hash_table_lookup(o, "user"))) sd_conn_set(&c->user, v);
        if ((v = g_hash_table_lookup(o, "identityfile"))) sd_conn_set(&c->key, v);
        gboolean fwd = (v = g_hash_table_lookup(o, "forwardx11")) && g_ascii_strcasecmp(v, "yes") == 0;
        gboolean trusted = (v = g_hash_table_lookup(o, "forwardx11trusted")) && g_ascii_strcasecmp(v, "yes") == 0;
        sd_conn_set(&c->x11, fwd ? (trusted ? "trusted" : "untrusted") : "off");
        if (live) {
            sd_conn_set(&c->alias, alias);
            g_free(c->id);
            c->id = g_strdup_printf("sshcfg:%s", alias);
        } else if ((v = g_hash_table_lookup(o, "proxyjump"))) {
            char *opt = g_strdup_printf("-J %s", v);
            sd_conn_set(&c->options, opt);
            g_free(opt);
        }
        g_ptr_array_add(out, c);
    }
    for (guint i = 0; i < blocks_opts->len; i++) g_hash_table_destroy(blocks_opts->pdata[i]);
    g_ptr_array_free(blocks_opts, TRUE);
    g_ptr_array_free(aliases, TRUE);
    g_ptr_array_free(alias_opts, TRUE);
    g_ptr_array_free(lines, TRUE);
    return out;
}
