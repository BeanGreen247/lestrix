/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "../src/json.h"
#include "../src/store.h"
#include "../src/util.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static ConnList parse_text(const char *name, const char *text) {
    std::string dir = lx::make_temp_dir("sdtest-");
    std::string path = lx::path_join(dir, name);
    lx::write_file(path, text);
    auto r = sd_parse_ansible_inventory(path);
    return r ? std::move(*r) : ConnList();
}

static SdConn *find(const ConnList &a, const char *name) {
    for (const auto &c : a) if (c->name == name) return c.get();
    return nullptr;
}

static bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

static void test_json() {
    auto v = json_parse("{\"a\": [1, 2.5, \"x\\n\\u00e9\\ud83d\\ude00\"], \"b\": {\"c\": true}, \"d\": null}");
    CHECK(v && json_get(v.get(), "a")->items.size() == 3);
    CHECK(std::string(json_str(json_get(v.get(), "a")->items[2].get(), "")) == "x\n\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(json_get(json_get(v.get(), "b"), "c")->b);
    std::string err;
    CHECK(json_parse("{\"a\": ", &err) == nullptr && !err.empty());
    std::string s;
    json_append_string(s, "a\"b\\c\n");
    CHECK(s == "\"a\\\"b\\\\c\\n\"");
}

static void test_ssh_argv() {
    SdConn c;
    c.host = "h"; c.user = "u"; c.key = "/k"; c.x11 = "trusted";
    c.options = "-J jump -o 'A=b c'"; c.remote_command = "fish -l";
    c.port = 2222;
    std::string joined = lx::join(c.argv("/tmp/sd/m"), "|");
    CHECK(contains(joined, "-p|2222"));
    CHECK(contains(joined, "ControlPath=/tmp/sd/m"));
    CHECK(contains(joined, "-i|/k") && contains(joined, "-Y") && contains(joined, "-J|jump"));
    CHECK(contains(joined, "A=b c"));
    CHECK(joined.ends_with("-t|u@h|fish -l"));
    c.alias = "box";
    joined = lx::join(c.argv(nullptr), "|");
    CHECK(!contains(joined, "-p|") && contains(joined, "|box|"));
}

static void test_store_roundtrip() {
    std::string dir = lx::make_temp_dir("sdtest-");
    std::string path = lx::path_join(dir, "c.json");
    SdStore s(path);
    SdConn c;
    c.name = "web \"1\""; c.host = "10.0.0.1"; c.port = 2200;
    s.upsert(c);
    struct stat st;
    stat(path.c_str(), &st);
    CHECK((st.st_mode & 0777) == 0600);
    SdStore s2(path);
    SdConn *back = s2.get(c.id);
    CHECK(back && back->name == "web \"1\"" && back->port == 2200);
    ConnList items;
    items.push_back(std::make_unique<SdConn>(c));
    items[0]->id = "other";
    CHECK(s2.add_imported(std::move(items)) == 0);
    s2.touch(c.id);
    CHECK(s2.recent(9).size() == 1);
    s2.remove(c.id);
    SdStore s3(path);
    CHECK(s3.conns.empty());
}

static void test_python_store_compat() {
    std::string dir = lx::make_temp_dir("sdtest-");
    std::string path = lx::path_join(dir, "c.json");
    lx::write_file(path, "{\"version\": 1, \"connections\": [{\"name\": \"a\", \"host\": \"h\", \"group\": \"G\", \"port\": 22,"
        " \"user\": \"\", \"key\": \"\", \"x11\": \"off\", \"options\": \"\", \"remote_command\": \"\", \"alias\": \"\","
        " \"color\": \"#ff0000\", \"last_used\": 12.5, \"id\": \"abc\"}], \"meta\": {\"sshcfg:box\": {\"color\": \"#00ff00\", \"last_used\": 3}}}");
    SdStore s(path);
    SdConn *c = s.get("abc");
    CHECK(c && c->color == "#ff0000" && c->last_used == 12.5 && c->protocol == "ssh");
}

static void test_ansible_ini() {
    ConnList a = parse_text("hosts.ini",
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
    CHECK(a.size() == 5);
    SdConn *w2 = find(a, "w02.example.com");
    CHECK(w2 && w2->user == "bob" && w2->port == 2022 && w2->group == "web");
    CHECK(find(a, "w03.example.com"));
    SdConn *w1 = find(a, "w1");
    CHECK(w1 && w1->host == "10.0.0.1" && w1->port == 2200 && w1->user == "deploy");
    CHECK(w1 && contains(w1->options, "ProxyJump=bastion") && w1->key == "~/.ssh/k");
    SdConn *d1 = find(a, "d1");
    CHECK(d1 && d1->host == "10.1.1.1" && d1->group == "db");
}

static void test_ansible_yaml() {
    ConnList a = parse_text("inv.yml",
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
    CHECK(a.size() == 5);
    SdConn *solo = find(a, "solo");
    CHECK(solo && solo->host == "192.0.2.9" && solo->user == "ops" && solo->group == "ungrouped");
    SdConn *w1 = find(a, "web1");
    CHECK(w1 && w1->port == 2222 && w1->key == "~/.ssh/web" && w1->group == "webservers");
    SdConn *w2 = find(a, "web2");
    CHECK(w2 && w2->user == "root" && w2->host == "192.0.2.11");
    CHECK(find(a, "db1") && find(a, "db2"));
}

static void test_ssh_config() {
    std::string dir = lx::make_temp_dir("sdtest-");
    std::string inc = lx::path_join(dir, "extra"), cfg = lx::path_join(dir, "config");
    lx::write_file(inc, "Host jump\n  HostName 9.9.9.9\n  User ops\n");
    lx::write_file(cfg, "Include " + inc + "\nHost *\n  ServerAliveInterval 5\nHost box other\n  HostName 1.2.3.4\n  ForwardX11 yes\n  ProxyJump jump\n  Port 2200\n");
    ConnList live = sd_parse_ssh_config(cfg, true);
    CHECK(live.size() == 3 && find(live, "jump") && find(live, "other"));
    SdConn *box = find(live, "box");
    CHECK(box && box->is_live() && box->port == 2200 && box->x11 == "untrusted" && box->id == "sshcfg:box");
    CHECK(box->options.empty());
    ConnList copy = sd_parse_ssh_config(cfg, false);
    CHECK(find(copy, "box")->options == "-J jump" && !find(copy, "box")->is_live());
}

int main() {
    test_json(); test_ssh_argv(); test_store_roundtrip(); test_python_store_compat();
    test_ansible_ini(); test_ansible_yaml(); test_ssh_config();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
