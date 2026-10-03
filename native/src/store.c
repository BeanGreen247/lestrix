#include "store.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "json.h"

void sd_conn_set(char **field, const char *value) {
    g_free(*field);
    *field = g_strdup(value ? value : "");
}

SdConn *sd_conn_new(void) {
    SdConn *c = g_new0(SdConn, 1);
    char *id = g_uuid_string_random();
    c->id = g_strdelimit(id, "-", 'x');
    c->group = g_strdup("Ungrouped");
    c->name = g_strdup(""); c->host = g_strdup(""); c->user = g_strdup(""); c->key = g_strdup("");
    c->x11 = g_strdup("off"); c->options = g_strdup(""); c->remote_command = g_strdup("");
    c->alias = g_strdup(""); c->color = g_strdup(""); c->protocol = g_strdup("ssh");
    c->port = 22;
    return c;
}

SdConn *sd_conn_copy(const SdConn *o) {
    SdConn *c = g_new0(SdConn, 1);
    c->id = g_strdup(o->id); c->name = g_strdup(o->name); c->group = g_strdup(o->group); c->host = g_strdup(o->host);
    c->user = g_strdup(o->user); c->key = g_strdup(o->key); c->x11 = g_strdup(o->x11); c->options = g_strdup(o->options);
    c->remote_command = g_strdup(o->remote_command); c->alias = g_strdup(o->alias); c->color = g_strdup(o->color);
    c->protocol = g_strdup(o->protocol); c->port = o->port; c->last_used = o->last_used;
    return c;
}

void sd_conn_free(SdConn *c) {
    if (!c) return;
    g_free(c->id); g_free(c->name); g_free(c->group); g_free(c->host); g_free(c->user); g_free(c->key); g_free(c->x11);
    g_free(c->options); g_free(c->remote_command); g_free(c->alias); g_free(c->color); g_free(c->protocol);
    g_free(c);
}

gboolean sd_conn_is_live(const SdConn *c) { return c->alias && *c->alias; }

char *sd_conn_dest(const SdConn *c) {
    if (sd_conn_is_live(c)) return g_strdup(c->alias);
    return *c->user ? g_strdup_printf("%s@%s", c->user, c->host) : g_strdup(c->host);
}

char **sd_conn_argv(const SdConn *c, const char *control_path) {
    GPtrArray *a = g_ptr_array_new();
    g_ptr_array_add(a, g_strdup("ssh"));
    if (!sd_conn_is_live(c)) {
        g_ptr_array_add(a, g_strdup("-p"));
        g_ptr_array_add(a, g_strdup_printf("%d", c->port));
        g_ptr_array_add(a, g_strdup("-o"));
        g_ptr_array_add(a, g_strdup("StrictHostKeyChecking=accept-new"));
    }
    if (control_path) {
        g_ptr_array_add(a, g_strdup("-o")); g_ptr_array_add(a, g_strdup("ControlMaster=yes"));
        g_ptr_array_add(a, g_strdup("-o")); g_ptr_array_add(a, g_strdup_printf("ControlPath=%s", control_path));
        g_ptr_array_add(a, g_strdup("-o")); g_ptr_array_add(a, g_strdup("ControlPersist=no"));
    }
    if (*c->key && !sd_conn_is_live(c)) {
        char *k = g_str_has_prefix(c->key, "~/") ? g_build_filename(g_get_home_dir(), c->key + 2, NULL) : g_strdup(c->key);
        g_ptr_array_add(a, g_strdup("-i"));
        g_ptr_array_add(a, k);
    }
    if (g_str_equal(c->x11, "untrusted")) g_ptr_array_add(a, g_strdup("-X"));
    else if (g_str_equal(c->x11, "trusted")) g_ptr_array_add(a, g_strdup("-Y"));
    if (*g_strstrip(c->options)) {
        char **extra = NULL;
        if (g_shell_parse_argv(c->options, NULL, &extra, NULL)) {
            for (char **e = extra; *e; e++) g_ptr_array_add(a, g_strdup(*e));
            g_strfreev(extra);
        }
    }
    gboolean cmd = *g_strstrip(c->remote_command);
    if (cmd) g_ptr_array_add(a, g_strdup("-t")); /* force a tty so interactive shells and tmux work */
    g_ptr_array_add(a, sd_conn_dest(c));
    if (cmd) g_ptr_array_add(a, g_strdup(c->remote_command));
    g_ptr_array_add(a, NULL);
    return (char **)g_ptr_array_free(a, FALSE);
}

/* ---- persistence ------------------------------------------------------------------------ */

char *sd_config_dir(void) {
    const char *o = g_getenv("LESTRIX_CONFIG_DIR");
    if (o && *o) return g_strdup(o);
    return g_build_filename(g_get_user_config_dir(), "lestrix", NULL);
}

static SdConn *conn_from_json(const JVal *o) {
    SdConn *c = sd_conn_new();
#define S(field, key) do { const char *v = json_str(json_get(o, key), NULL); if (v) sd_conn_set(&c->field, v); } while (0)
    S(id, "id"); S(name, "name"); S(group, "group"); S(host, "host"); S(user, "user"); S(key, "key");
    S(x11, "x11"); S(options, "options"); S(remote_command, "remote_command"); S(alias, "alias"); S(color, "color");
    S(protocol, "protocol");
#undef S
    c->port = (int)json_num(json_get(o, "port"), 22);
    c->last_used = json_num(json_get(o, "last_used"), 0);
    if (!g_str_equal(c->x11, "untrusted") && !g_str_equal(c->x11, "trusted")) sd_conn_set(&c->x11, "off");
    if (!*c->protocol) sd_conn_set(&c->protocol, "ssh");
    return c;
}

static void load(SdStore *s) {
    char *text = NULL;
    if (!g_file_get_contents(s->path, &text, NULL, NULL)) return;
    JVal *root = json_parse(text, NULL);
    g_free(text);
    if (!root) return;
    const JVal *arr = json_get(root, "connections");
    if (arr && arr->type == J_ARR)
        for (guint i = 0; i < arr->items->len; i++) {
            const JVal *o = arr->items->pdata[i];
            if (o->type == J_OBJ) g_ptr_array_add(s->conns, conn_from_json(o));
        }
    const JVal *meta = json_get(root, "meta");
    if (meta && meta->type == J_OBJ)
        for (guint i = 0; i < meta->keys->len; i++) {
            const JVal *m = meta->items->pdata[i];
            SdConn *c = sd_conn_new();
            sd_conn_set(&c->color, json_str(json_get(m, "color"), ""));
            c->last_used = json_num(json_get(m, "last_used"), 0);
            g_hash_table_insert(s->meta, g_strdup(meta->keys->pdata[i]), c);
        }
    json_free(root);
}

SdStore *sd_store_new(const char *path) {
    SdStore *s = g_new0(SdStore, 1);
    if (path) s->path = g_strdup(path);
    else { char *d = sd_config_dir(); s->path = g_build_filename(d, "connections.json", NULL); g_free(d); }
    s->conns = g_ptr_array_new_with_free_func((GDestroyNotify)sd_conn_free);
    s->live = g_ptr_array_new_with_free_func((GDestroyNotify)sd_conn_free);
    s->meta = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)sd_conn_free);
    load(s);
    return s;
}

void sd_store_free(SdStore *s) {
    if (!s) return;
    g_ptr_array_free(s->conns, TRUE);
    g_ptr_array_free(s->live, TRUE);
    g_hash_table_destroy(s->meta);
    g_free(s->path);
    g_free(s);
}

gboolean sd_store_save(SdStore *s) {
    GString *o = g_string_new("{\n  \"version\": 1,\n  \"connections\": [");
    for (guint i = 0; i < s->conns->len; i++) {
        SdConn *c = s->conns->pdata[i];
        g_string_append(o, i ? ",\n    {" : "\n    {");
#define SF(k, v) do { g_string_append(o, "\"" k "\": "); json_append_string(o, v); g_string_append(o, ", "); } while (0)
        SF("id", c->id); SF("name", c->name); SF("host", c->host); SF("group", c->group);
        g_string_append_printf(o, "\"port\": %d, ", c->port);
        SF("user", c->user); SF("key", c->key); SF("x11", c->x11); SF("options", c->options);
        SF("remote_command", c->remote_command); SF("alias", c->alias); SF("color", c->color); SF("protocol", c->protocol);
#undef SF
        g_string_append_printf(o, "\"last_used\": %.3f}", c->last_used);
    }
    g_string_append(o, "\n  ],\n  \"meta\": {");
    GHashTableIter it;
    gpointer k, v;
    gboolean first = TRUE;
    g_hash_table_iter_init(&it, s->meta);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        SdConn *m = v;
        g_string_append(o, first ? "\n    " : ",\n    ");
        first = FALSE;
        json_append_string(o, k);
        g_string_append(o, ": {\"color\": ");
        json_append_string(o, m->color);
        g_string_append_printf(o, ", \"last_used\": %.3f}", m->last_used);
    }
    g_string_append(o, "\n  }\n}\n");

    char *dir = g_path_get_dirname(s->path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    char *tmp = g_strconcat(s->path, ".tmp", NULL);
    gboolean ok = g_file_set_contents(tmp, o->str, (gssize)o->len, NULL);
    if (ok) { chmod(tmp, 0600); ok = rename(tmp, s->path) == 0; }
    g_free(tmp);
    g_string_free(o, TRUE);
    return ok;
}

GPtrArray *sd_store_all(SdStore *s) {
    GPtrArray *a = g_ptr_array_new();
    for (guint i = 0; i < s->conns->len; i++) g_ptr_array_add(a, s->conns->pdata[i]);
    for (guint i = 0; i < s->live->len; i++) g_ptr_array_add(a, s->live->pdata[i]);
    return a;
}

SdConn *sd_store_get(SdStore *s, const char *id) {
    for (guint i = 0; id && i < s->conns->len; i++) if (g_str_equal(((SdConn *)s->conns->pdata[i])->id, id)) return s->conns->pdata[i];
    for (guint i = 0; id && i < s->live->len; i++) if (g_str_equal(((SdConn *)s->live->pdata[i])->id, id)) return s->live->pdata[i];
    return NULL;
}

void sd_store_upsert(SdStore *s, const SdConn *c) {
    for (guint i = 0; i < s->conns->len; i++) {
        if (g_str_equal(((SdConn *)s->conns->pdata[i])->id, c->id)) {
            sd_conn_free(s->conns->pdata[i]);
            s->conns->pdata[i] = sd_conn_copy(c);
            sd_store_save(s);
            return;
        }
    }
    g_ptr_array_add(s->conns, sd_conn_copy(c));
    sd_store_save(s);
}

void sd_store_remove(SdStore *s, const char *id) {
    for (guint i = 0; i < s->conns->len; i++)
        if (g_str_equal(((SdConn *)s->conns->pdata[i])->id, id)) { g_ptr_array_remove_index(s->conns, i); break; }
    sd_store_save(s);
}

static SdConn *meta_for(SdStore *s, SdConn *c) {
    SdConn *m = g_hash_table_lookup(s->meta, c->id);
    if (!m) { m = sd_conn_new(); g_hash_table_insert(s->meta, g_strdup(c->id), m); }
    return m;
}

void sd_store_touch(SdStore *s, const char *id) {
    SdConn *c = sd_store_get(s, id);
    if (!c) return;
    c->last_used = (double)g_get_real_time() / 1e6;
    if (sd_conn_is_live(c)) meta_for(s, c)->last_used = c->last_used;
    sd_store_save(s);
}

void sd_store_set_color(SdStore *s, const char *id, const char *color) {
    SdConn *c = sd_store_get(s, id);
    if (!c) return;
    sd_conn_set(&c->color, color);
    if (sd_conn_is_live(c)) sd_conn_set(&meta_for(s, c)->color, color);
    sd_store_save(s);
}

static int by_recent(gconstpointer a, gconstpointer b) {
    double x = (*(SdConn *const *)a)->last_used, y = (*(SdConn *const *)b)->last_used;
    return x < y ? 1 : x > y ? -1 : 0;
}

GPtrArray *sd_store_recent(SdStore *s, int limit) {
    GPtrArray *all = sd_store_all(s), *out = g_ptr_array_new();
    for (guint i = 0; i < all->len; i++) if (((SdConn *)all->pdata[i])->last_used > 0) g_ptr_array_add(out, all->pdata[i]);
    g_ptr_array_free(all, TRUE);
    g_ptr_array_sort(out, by_recent);
    if ((int)out->len > limit) g_ptr_array_set_size(out, (guint)limit);
    return out;
}

void sd_store_reload_ssh_config(SdStore *s, gboolean enabled, const char *path) {
    g_ptr_array_set_size(s->live, 0);
    if (!enabled) return;
    char *def = g_build_filename(g_get_home_dir(), ".ssh", "config", NULL);
    GPtrArray *items = sd_parse_ssh_config(path ? path : def, TRUE);
    g_free(def);
    if (!items) return;
    for (guint i = 0; i < items->len; i++) {
        SdConn *c = items->pdata[i];
        gboolean saved = FALSE;
        for (guint j = 0; j < s->conns->len; j++) if (g_str_equal(((SdConn *)s->conns->pdata[j])->name, c->name)) { saved = TRUE; break; }
        if (saved) { sd_conn_free(c); continue; }   /* a saved copy wins */
        SdConn *m = g_hash_table_lookup(s->meta, c->id);
        if (m) { sd_conn_set(&c->color, m->color); c->last_used = m->last_used; }
        g_ptr_array_add(s->live, c);
    }
    g_free(g_ptr_array_free(items, FALSE));
}

int sd_store_add_imported(SdStore *s, GPtrArray *items) {
    int added = 0;
    for (guint i = 0; i < items->len; i++) {
        SdConn *c = items->pdata[i];
        gboolean dup = FALSE;
        for (guint j = 0; j < s->conns->len && !dup; j++) {
            SdConn *e = s->conns->pdata[j];
            dup = g_str_equal(e->name, c->name) && g_str_equal(e->host, c->host) && e->port == c->port && g_str_equal(e->user, c->user);
        }
        if (dup) sd_conn_free(c);
        else { g_ptr_array_add(s->conns, c); added++; }
    }
    g_free(g_ptr_array_free(items, FALSE));
    if (added) sd_store_save(s);
    return added;
}
