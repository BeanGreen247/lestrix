/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_STORE_H
#define SD_STORE_H

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct SdConn {
    std::string id, name, group = "Ungrouped", host, user, key;
    std::string x11 = "off";
    std::string options;
    std::string remote_command;
    std::string alias;
    std::string color;
    std::string protocol = "ssh";
    int port = 22;
    double last_used = 0;

    SdConn();
    bool is_live() const { return !alias.empty(); }
    std::string dest() const;
    std::vector<std::string> argv(const char *control_path) const;
};

using ConnList = std::vector<std::unique_ptr<SdConn>>;

class SdStore {
public:
    explicit SdStore(const std::string &path = {});

    std::string path;
    ConnList conns;
    ConnList live;
    std::map<std::string, SdConn> meta;

    bool save();
    std::vector<SdConn *> all();
    SdConn *get(const std::string &id);
    void upsert(const SdConn &c);
    void remove(const std::string &id);
    void touch(const std::string &id);
    void set_color(const std::string &id, const std::string &color);
    std::vector<SdConn *> recent(int limit);
    void reload_ssh_config(bool enabled, const char *path = nullptr);
    int add_imported(ConnList items);

private:
    void load();
    SdConn &meta_for(const SdConn &c);
};

std::string sd_config_dir();

std::optional<ConnList> sd_parse_ansible_inventory(const std::string &path, std::string *err = nullptr);
ConnList sd_parse_ssh_config(const std::string &path, bool live);

#endif
