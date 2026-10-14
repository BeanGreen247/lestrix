/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_UTIL_H
#define SD_UTIL_H

#include <cstdarg>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lx {

std::string strprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string vstrprintf(const char *fmt, va_list ap);

std::string_view trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);
std::vector<std::string> split_any(std::string_view s, std::string_view seps);
std::string join(const std::vector<std::string> &v, std::string_view sep);
bool iequals(std::string_view a, std::string_view b);
bool iprefix(std::string_view s, std::string_view prefix);
std::string ascii_lower(std::string_view s);
std::string ascii_upper(std::string_view s);
std::string utf8_lower(std::string_view s);
std::string utf8_upper(std::string_view s);
void append_utf8(std::string &out, uint32_t cp);

std::string env(const char *name);
std::string home_dir();
std::string config_dir();
std::string cache_dir();
std::string download_dir();

std::string path_join(std::string_view a, std::string_view b);
std::string path_join(std::string_view a, std::string_view b, std::string_view c);
std::string dirname(std::string_view p);
std::string basename(std::string_view p);
std::string canonicalize(std::string_view p);
bool is_absolute(std::string_view p);

bool exists(const std::string &p);
bool is_dir(const std::string &p);
std::optional<std::string> read_file(const std::string &p);
bool write_file(const std::string &p, std::string_view data);
bool mkdir_p(const std::string &p, unsigned mode = 0755);
std::string make_temp_dir(const char *prefix);
std::vector<std::string> list_dir(const std::string &p);
std::string find_program(const char *name);
std::string read_link(const std::string &p);

std::string uuid();
double real_time();
int64_t monotonic_us();

std::string shell_quote(std::string_view s);
bool shell_parse(std::string_view s, std::vector<std::string> &out);

struct RunResult {
    int status = 0;
    std::string out, err;
    bool timed_out = false;
};

/* Runs argv (searching PATH) with optional stdin text and a timeout in seconds (0 for none); false if it could not be started. */
bool run_command(const std::vector<std::string> &argv, const std::string &input, int timeout_s, RunResult &r);
/* Starts argv without waiting; returns the pid or -1. The caller reaps it. */
long spawn_async(const std::vector<std::string> &argv);

/* A tiny INI reader and writer for the settings file, [group] headers and key=value lines. */
class IniFile {
public:
    bool load(const std::string &path);
    bool save(const std::string &path) const;
    bool has(std::string_view group, std::string_view key) const;
    std::string get_string(std::string_view group, std::string_view key, std::string_view def = {}) const;
    int get_int(std::string_view group, std::string_view key, int def = 0) const;
    bool get_bool(std::string_view group, std::string_view key, bool def = false) const;
    void set_string(std::string_view group, std::string_view key, std::string_view value);
    void set_int(std::string_view group, std::string_view key, int value) { set_string(group, key, std::to_string(value)); }
    void set_bool(std::string_view group, std::string_view key, bool value) { set_string(group, key, value ? "true" : "false"); }

private:
    struct Group { std::string name; std::vector<std::pair<std::string, std::string>> keys; };
    std::vector<Group> groups_;
    const std::string *find(std::string_view group, std::string_view key) const;
};

}

#endif
