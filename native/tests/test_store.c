/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/json.h"
#include "../src/store.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; g_print("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static GPtrArray *parse_text(const char *name, const char *text) {
    char *dir = g_dir_make_tmp("sdtest-XXXXXX", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_file_set_contents(path, text, -1, NULL);
    GError *err = NULL;
    GPtrArray *r = sd_parse_ansible_inventory(path, &err);
    g_free(path); g_free(dir);
    return r;
}

static SdConn *find(GPtrArray *a, const char *name) {
    for (guint i = 0; a && i < a->len; i++) if (g_str_equal(((SdConn *)a->pdata[i])->name, name)) return a->pdata[i];
    return NULL;
}

static void test_json(void) {
    JVal *v = json_parse("{\"a\": [1, 2.5, \"x\\n\\u00e9\\ud83d\\ude00\"], \"b\": {\"c\": true}, \"d\": null}", NULL);
    CHECK(v && json_get(v, "a")->items->len == 3);
    CHECK(g_str_equal(json_str(json_get(v, "a")->items->pdata[2], ""), "x\n\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(json_get(json_get(v, "b"), "c")->b);
    json_free(v);
    GError *err = NULL;
    CHECK(json_parse("{\"a\": ", &err) == NULL && err);
    g_clear_error(&err);
    GString *s = g_string_new(NULL);
    json_append_string(s, "a\"b\\c\n");
    CHECK(g_str_equal(s->str, "\"a\\\"b\\\\c\\n\""));
    g_string_free(s, TRUE);
}

static void test_ssh_argv(void) {
    SdConn *c = sd_conn_new();
    sd_conn_set(&c->host, "h"); sd_conn_set(&c->user, "u"); sd_conn_set(&c->key, "/k"); sd_conn_set(&c->x11, "trusted");
    sd_conn_set(&c->options, "-J jump -o 'A=b c'"); sd_conn_set(&c->remote_command, "fish -l");
    c->port = 2222;
    char **a = sd_conn_argv(c, "/tmp/sd/m");
    char *joined = g_strjoinv("|", a);
    CHECK(strstr(joined, "-p|2222"));
    CHECK(strstr(joined, "ControlPath=/tmp/sd/m"));
    CHECK(strstr(joined, "-i|/k") && strstr(joined, "-Y") && strstr(joined, "-J|jump"));
    CHECK(strstr(joined, "A=b c"));
    CHECK(g_str_has_suffix(joined, "-t|u@h|fish -l"));
    g_free(joined); g_strfreev(a);
    sd_conn_set(&c->alias, "box");
    a = sd_conn_argv(c, NULL);
    joined = g_strjoinv("|", a);
    CHECK(!strstr(joined, "-p|") && strstr(joined, "|box|"));
    g_free(joined); g_strfreev(a);
    sd_conn_free(c);
}

static void test_store_roundtrip(void) {
    char *dir = g_dir_make_tmp("sdtest-XXXXXX", NULL);
    char *path = g_build_filename(dir, "c.json", NULL);
    SdStore *s = sd_store_new(path);
    SdConn *c = sd_conn_new();
    sd_conn_set(&c->name, "web \"1\""); sd_conn_set(&c->host, "10.0.0.1"); c->port = 2200;
    sd_store_upsert(s, c);
    GStatBuf st;
    g_stat(path, &st);
    CHECK((st.st_mode & 0777) == 0600);
    SdStore *s2 = sd_store_new(path);
    SdConn *back = sd_store_get(s2, c->id);
    CHECK(back && g_str_equal(back->name, "web \"1\"") && back->port == 2200);
    GPtrArray *items = g_ptr_array_new();
    SdConn *dup = sd_conn_copy(c);
    g_free(dup->id); dup->id = g_strdup("other");
    g_ptr_array_add(items, dup);
    CHECK(sd_store_add_imported(s2, items) == 0);
    sd_store_touch(s2, c->id);
    GPtrArray *recent = sd_store_recent(s2, 9);
    CHECK(recent->len == 1);
    g_ptr_array_free(recent, TRUE);
    sd_store_remove(s2, c->id);
    SdStore *s3 = sd_store_new(path);
    CHECK(s3->conns->len == 0);
    sd_conn_free(c);
    sd_store_free(s); sd_store_free(s2); sd_store_free(s3);
    g_free(path); g_free(dir);
}

static void test_python_store_compat(void) {
    char *dir = g_dir_make_tmp("sdtest-XXXXXX", NULL);
    char *path = g_build_filename(dir, "c.json", NULL);
    g_file_set_contents(path, "{\"version\": 1, \"connections\": [{\"name\": \"a\", \"host\": \"h\", \"group\": \"G\", \"port\": 22,"
        " \"user\": \"\", \"key\": \"\", \"x11\": \"off\", \"options\": \"\", \"remote_command\": \"\", \"alias\": \"\","
        " \"color\": \"#ff0000\", \"last_used\": 12.5, \"id\": \"abc\"}], \"meta\": {\"sshcfg:box\": {\"color\": \"#00ff00\", \"last_used\": 3}}}", -1, NULL);
    SdStore *s = sd_store_new(path);
    SdConn *c = sd_store_get(s, "abc");
    CHECK(c && g_str_equal(c->color, "#ff0000") && c->last_used == 12.5 && g_str_equal(c->protocol, "ssh"));
    sd_store_free(s);
    g_free(path); g_free(dir);
}

static void test_ansible_ini(void) {
    GPtrArray *a = parse_text("hosts.ini",
        "# comment\n"
        "[web]\n"
        "w[01:03].example.com ansible_user=bob\n"
        "w1 ansible_host=10.0.0.1 ansible_port=2200 ansible_ssh_private_key_file=~/.ssh/k\n"
        "[web:vars]\n"
        "ansible_user=deploy\n"
        "ansible_ssh_common_args='-o ProxyJump=bastion'\n"
        "[db]\n"
        "d1 ansible_host=\"10.1.1.1\"\n"
        "[all:vars]\n"
        "ansible_port=2022\n"
        "[db:children]\n"
        "x\n");
    CHECK(a && a->len == 5);
    SdConn *w2 = find(a, "w02.example.com");
    CHECK(w2 && g_str_equal(w2->user, "bob") && w2->port == 2022 && g_str_equal(w2->group, "web"));
    CHECK(find(a, "w03.example.com"));
    SdConn *w1 = find(a, "w1");
    CHECK(w1 && g_str_equal(w1->host, "10.0.0.1") && w1->port == 2200 && g_str_equal(w1->user, "deploy"));
    CHECK(w1 && strstr(w1->options, "ProxyJump=bastion") && g_str_equal(w1->key, "~/.ssh/k"));
    SdConn *d1 = find(a, "d1");
    CHECK(d1 && g_str_equal(d1->host, "10.1.1.1") && g_str_equal(d1->group, "db"));
    g_ptr_array_free(a, TRUE);
}

static void test_ansible_yaml(void) {
    GPtrArray *a = parse_text("inv.yml",
        "---\n"
        "all:\n"
        "  vars:\n"
        "    ansible_user: ops   # default user\n"
        "  hosts:\n"
        "    solo:\n"
        "      ansible_host: 192.0.2.9\n"
        "  children:\n"
        "    webservers:\n"
        "      hosts:\n"
        "        web1:\n"
        "          ansible_host: 192.0.2.10\n"
        "          ansible_port: 2222\n"
        "        web2: {ansible_host: 192.0.2.11, ansible_user: root}\n"
        "      vars:\n"
        "        ansible_ssh_private_key_file: ~/.ssh/web\n"
        "    dbs:\n"
        "      hosts:\n"
        "        db[1:2]:\n");
    CHECK(a && a->len == 5);
    SdConn *solo = find(a, "solo");
    CHECK(solo && g_str_equal(solo->host, "192.0.2.9") && g_str_equal(solo->user, "ops") && g_str_equal(solo->group, "ungrouped"));
    SdConn *w1 = find(a, "web1");
    CHECK(w1 && w1->port == 2222 && g_str_equal(w1->key, "~/.ssh/web") && g_str_equal(w1->group, "webservers"));
    SdConn *w2 = find(a, "web2");
    CHECK(w2 && g_str_equal(w2->user, "root") && g_str_equal(w2->host, "192.0.2.11"));
    CHECK(find(a, "db1") && find(a, "db2"));
    g_ptr_array_free(a, TRUE);
}

static void test_ssh_config(void) {
    char *dir = g_dir_make_tmp("sdtest-XXXXXX", NULL);
    char *inc = g_build_filename(dir, "extra", NULL), *cfg = g_build_filename(dir, "config", NULL);
    g_file_set_contents(inc, "Host jump\n  HostName 9.9.9.9\n  User ops\n", -1, NULL);
    char *body = g_strdup_printf("Include %s\nHost *\n  ServerAliveInterval 5\nHost box other\n  HostName 1.2.3.4\n  ForwardX11 yes\n  ProxyJump jump\n  Port 2200\n", inc);
    g_file_set_contents(cfg, body, -1, NULL);
    GPtrArray *live = sd_parse_ssh_config(cfg, TRUE);
    CHECK(live->len == 3 && find(live, "jump") && find(live, "other"));
    SdConn *box = find(live, "box");
    CHECK(box && sd_conn_is_live(box) && box->port == 2200 && g_str_equal(box->x11, "untrusted") && g_str_equal(box->id, "sshcfg:box"));
    CHECK(g_str_equal(box->options, ""));
    GPtrArray *copy = sd_parse_ssh_config(cfg, FALSE);
    CHECK(g_str_equal(find(copy, "box")->options, "-J jump") && !sd_conn_is_live(find(copy, "box")));
    g_ptr_array_free(live, TRUE); g_ptr_array_free(copy, TRUE);
    g_free(inc); g_free(cfg); g_free(body); g_free(dir);
}

int main(void) {
    test_json(); test_ssh_argv(); test_store_roundtrip(); test_python_store_compat();
    test_ansible_ini(); test_ansible_yaml(); test_ssh_config();
    g_print("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
