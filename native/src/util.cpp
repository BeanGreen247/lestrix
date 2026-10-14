/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "util.h"

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

namespace lx {

std::string vstrprintf(const char *fmt, va_list ap) {
    va_list cp;
    va_copy(cp, ap);
    char small[256];
    int n = vsnprintf(small, sizeof small, fmt, cp);
    va_end(cp);
    if (n < 0) return {};
    if ((size_t)n < sizeof small) return std::string(small, (size_t)n);
    std::string s((size_t)n, '\0');
    vsnprintf(s.data(), (size_t)n + 1, fmt, ap);
    return s;
}

std::string strprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = vstrprintf(fmt, ap);
    va_end(ap);
    return s;
}

static bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        size_t k = s.find(sep, at);
        if (k == std::string_view::npos) { out.emplace_back(s.substr(at)); break; }
        out.emplace_back(s.substr(at, k - at));
        at = k + 1;
    }
    return out;
}

std::vector<std::string> split_any(std::string_view s, std::string_view seps) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        size_t k = s.find_first_of(seps, at);
        if (k == std::string_view::npos) { out.emplace_back(s.substr(at)); break; }
        out.emplace_back(s.substr(at, k - at));
        at = k + 1;
    }
    return out;
}

std::string join(const std::vector<std::string> &v, std::string_view sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) { if (i) o += sep; o += v[i]; }
    return o;
}

static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (lc(a[i]) != lc(b[i])) return false;
    return true;
}

bool iprefix(std::string_view s, std::string_view p) { return s.size() >= p.size() && iequals(s.substr(0, p.size()), p); }

std::string ascii_lower(std::string_view s) { std::string o(s); for (char &c : o) c = lc(c); return o; }
std::string ascii_upper(std::string_view s) { std::string o(s); for (char &c : o) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return o; }

void append_utf8(std::string &o, uint32_t cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

static uint32_t decode_utf8(std::string_view s, size_t &i) {
    unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n == 0 || i + (size_t)n >= s.size()) { i++; return c; }
    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 0x3F);
    i += (size_t)n + 1;
    return cp;
}

static uint32_t cp_lower(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return c + 32;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (c == 0x178) return 0xFF;
        return odd_upper ? ((c & 1) ? c + 1 : c) : ((c & 1) ? c : c + 1);
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

static uint32_t cp_upper(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        return odd_upper ? ((c & 1) ? c : c - 1) : ((c & 1) ? c - 1 : c);
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

std::string utf8_lower(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) append_utf8(o, cp_lower(decode_utf8(s, i)));
    return o;
}

std::string utf8_upper(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) append_utf8(o, cp_upper(decode_utf8(s, i)));
    return o;
}

std::string env(const char *name) { const char *v = getenv(name); return v ? v : ""; }

std::string home_dir() {
    std::string h = env("HOME");
    if (!h.empty()) return h;
    if (struct passwd *pw = getpwuid(getuid())) return pw->pw_dir;
    return "/";
}

static std::string xdg(const char *var, const char *fallback) {
    std::string v = env(var);
    if (!v.empty() && v[0] == '/') return v;
    return path_join(home_dir(), fallback);
}

std::string config_dir() { return xdg("XDG_CONFIG_HOME", ".config"); }
std::string cache_dir() { return xdg("XDG_CACHE_HOME", ".cache"); }

std::string download_dir() {
    std::string home = home_dir();
    std::string rc = path_join(config_dir(), "user-dirs.dirs");
    if (auto text = read_file(rc)) {
        for (const std::string &line : split(*text, '\n')) {
            std::string_view l = trim(line);
            if (!l.starts_with("XDG_DOWNLOAD_DIR=")) continue;
            std::string_view v = trim(l.substr(17));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            if (v.starts_with("$HOME")) return home + std::string(v.substr(5));
            if (!v.empty() && v[0] == '/') return std::string(v);
        }
    }
    return home;
}

std::string path_join(std::string_view a, std::string_view b) {
    std::string o(a);
    while (o.size() > 1 && o.back() == '/') o.pop_back();
    while (!b.empty() && b.front() == '/') b.remove_prefix(1);
    if (o.empty()) return std::string(b);
    if (o.back() != '/') o += '/';
    o += b;
    return o;
}

std::string path_join(std::string_view a, std::string_view b, std::string_view c) { return path_join(path_join(a, b), c); }

std::string dirname(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    size_t k = p.rfind('/');
    if (k == std::string_view::npos) return ".";
    if (k == 0) return "/";
    while (k > 1 && p[k - 1] == '/') k--;
    return std::string(p.substr(0, k));
}

std::string basename(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    if (p == "/") return "/";
    size_t k = p.rfind('/');
    return std::string(k == std::string_view::npos ? p : p.substr(k + 1));
}

bool is_absolute(std::string_view p) { return !p.empty() && p[0] == '/'; }

std::string canonicalize(std::string_view p) {
    std::string full;
    if (is_absolute(p)) full = std::string(p);
    else { char cwd[PATH_MAX]; full = path_join(getcwd(cwd, sizeof cwd) ? cwd : "/", p); }
    std::vector<std::string> parts;
    for (const std::string &seg : split(full, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); continue; }
        parts.push_back(seg);
    }
    return "/" + join(parts, "/");
}

bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
bool is_dir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

std::optional<std::string> read_file(const std::string &p) {
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string data;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) data.append(buf, (size_t)n);
    int err = n < 0;
    close(fd);
    if (err) return std::nullopt;
    return data;
}

bool write_file(const std::string &p, std::string_view data) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return false; }
        off += (size_t)n;
    }
    return close(fd) == 0;
}

bool mkdir_p(const std::string &p, unsigned mode) {
    if (p.empty()) return false;
    if (is_dir(p)) return true;
    std::string parent = dirname(p);
    if (parent != p && parent != "." && !mkdir_p(parent, mode)) return false;
    return mkdir(p.c_str(), mode) == 0 || errno == EEXIST;
}

std::string make_temp_dir(const char *prefix) {
    std::string t = env("TMPDIR");
    if (t.empty()) t = "/tmp";
    std::string tmpl = path_join(t, std::string(prefix) + "XXXXXX");
    std::vector<char> b(tmpl.begin(), tmpl.end());
    b.push_back('\0');
    return mkdtemp(b.data()) ? std::string(b.data()) : std::string();
}

std::vector<std::string> list_dir(const std::string &p) {
    std::vector<std::string> out;
    if (DIR *d = opendir(p.c_str())) {
        while (struct dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            out.emplace_back(e->d_name);
        }
        closedir(d);
    }
    return out;
}

std::string find_program(const char *name) {
    if (strchr(name, '/')) return access(name, X_OK) == 0 ? name : "";
    for (const std::string &dir : split(env("PATH"), ':')) {
        if (dir.empty()) continue;
        std::string full = path_join(dir, name);
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(full.c_str(), X_OK) == 0) return full;
    }
    return "";
}

std::string read_link(const std::string &p) {
    char buf[PATH_MAX];
    ssize_t n = readlink(p.c_str(), buf, sizeof buf - 1);
    return n < 0 ? std::string() : std::string(buf, (size_t)n);
}

std::string uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x4000;
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    return strprintf("%08x-%04x-%04x-%04x-%012llx", (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xFFFF), (unsigned)(a & 0xFFFF),
                     (unsigned)(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFull));
}

double real_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int64_t monotonic_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

std::string shell_quote(std::string_view s) {
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += '\'';
    return o;
}

bool shell_parse(std::string_view s, std::vector<std::string> &out) {
    out.clear();
    std::string cur;
    bool have = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (is_space(c)) { if (have) { out.push_back(cur); cur.clear(); have = false; } continue; }
        have = true;
        if (c == '\'') {
            size_t k = s.find('\'', i + 1);
            if (k == std::string_view::npos) return false;
            cur.append(s.substr(i + 1, k - i - 1));
            i = k;
        } else if (c == '"') {
            i++;
            for (;; i++) {
                if (i >= s.size()) return false;
                if (s[i] == '"') break;
                if (s[i] == '\\' && i + 1 < s.size() && strchr("$`\"\\\n", s[i + 1])) { if (s[i + 1] != '\n') cur += s[i + 1]; i++; }
                else cur += s[i];
            }
        } else if (c == '\\') {
            if (i + 1 >= s.size()) return false;
            if (s[i + 1] != '\n') cur += s[i + 1];
            i++;
        } else cur += c;
    }
    if (have) out.push_back(cur);
    return !out.empty();
}

static std::vector<char *> c_argv(const std::vector<std::string> &argv) {
    std::vector<char *> av;
    for (const std::string &a : argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    return av;
}

static void child_exec(std::vector<char *> &av, int in_fd, int out_fd, int err_fd, int fail_fd) {
    if (in_fd >= 0) dup2(in_fd, 0);
    if (out_fd >= 0) dup2(out_fd, 1);
    if (err_fd >= 0) dup2(err_fd, 2);
    int keep = fail_fd;
    for (int fd = 3; fd < 1024; fd++) if (fd != keep) close(fd);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    signal(SIGPIPE, SIG_DFL);
    execvp(av[0], av.data());
    if (fail_fd >= 0) { ssize_t w = write(fail_fd, "x", 1); (void)w; }
    _exit(127);
}

long spawn_async(const std::vector<std::string> &argv) {
    std::vector<char *> av = c_argv(argv);
    pid_t pid = fork();
    if (pid == 0) child_exec(av, -1, -1, -1, -1);
    return pid;
}

static void read_available(int fd, std::string &dst, bool &open) {
    char buf[8192];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) dst.append(buf, (size_t)n);
    else if (n == 0 || (errno != EAGAIN && errno != EINTR)) open = false;
}

bool run_command(const std::vector<std::string> &argv, const std::string &input, int timeout_s, RunResult &r) {
    r = RunResult();
    int pin[2], pout[2], perr[2], pexec[2];
    if (pipe2(pin, O_CLOEXEC) || pipe2(pout, O_CLOEXEC) || pipe2(perr, O_CLOEXEC) || pipe2(pexec, O_CLOEXEC)) return false;
    std::vector<char *> av = c_argv(argv);
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        close(pexec[0]);
        child_exec(av, pin[0], pout[1], perr[1], pexec[1]);
    }
    close(pin[0]); close(pout[1]); close(perr[1]); close(pexec[1]);
    /* the pipe is close-on-exec, so it reads empty when the program started and one byte when exec failed */
    char dummy;
    ssize_t failed = read(pexec[0], &dummy, 1);
    close(pexec[0]);
    if (failed == 1) {
        close(pin[1]); close(pout[0]); close(perr[0]);
        waitpid(pid, nullptr, 0);
        return false;
    }
    int in_fd = pin[1], out_fd = pout[0], err_fd = perr[0];
    fcntl(out_fd, F_SETFL, O_NONBLOCK);
    fcntl(err_fd, F_SETFL, O_NONBLOCK);
    size_t in_off = 0;
    if (input.empty()) { close(in_fd); in_fd = -1; } else fcntl(in_fd, F_SETFL, O_NONBLOCK);
    bool out_open = true, err_open = true;
    int64_t deadline = timeout_s > 0 ? monotonic_us() + (int64_t)timeout_s * 1000000 : 0;
    signal(SIGPIPE, SIG_IGN);
    while (out_open || err_open) {
        struct pollfd fds[3];
        int n = 0;
        if (out_open) fds[n++] = {out_fd, POLLIN, 0};
        if (err_open) fds[n++] = {err_fd, POLLIN, 0};
        int in_idx = -1;
        if (in_fd >= 0) { in_idx = n; fds[n++] = {in_fd, POLLOUT, 0}; }
        int wait_ms = 1000;
        if (deadline) {
            int64_t left = (deadline - monotonic_us()) / 1000;
            if (left <= 0) { r.timed_out = true; kill(pid, SIGKILL); break; }
            if (left < wait_ms) wait_ms = (int)left;
        }
        int pr = poll(fds, (nfds_t)n, wait_ms);
        if (pr < 0 && errno != EINTR) break;
        int k = 0;
        if (out_open) { if (fds[k].revents) read_available(out_fd, r.out, out_open); k++; }
        if (err_open) { if (fds[k].revents) read_available(err_fd, r.err, err_open); k++; }
        if (in_idx >= 0 && fds[in_idx].revents) {
            ssize_t w = write(in_fd, input.data() + in_off, input.size() - in_off);
            if (w > 0) in_off += (size_t)w;
            if (w < 0 && errno != EAGAIN && errno != EINTR) in_off = input.size();
            if (in_off >= input.size()) { close(in_fd); in_fd = -1; }
        }
    }
    if (in_fd >= 0) close(in_fd);
    close(out_fd);
    close(err_fd);
    int st = 0;
    waitpid(pid, &st, 0);
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    return true;
}

static std::string ini_escape(std::string_view v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        char c = v[i];
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";
        else if (c == ' ' && i == 0) o += "\\s";
        else o += c;
    }
    return o;
}

static std::string ini_unescape(std::string_view v) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[++i];
            o += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n == 's' ? ' ' : n;
        } else o += v[i];
    }
    return o;
}

bool IniFile::load(const std::string &path) {
    auto text = read_file(path);
    if (!text) return false;
    groups_.clear();
    Group *cur = nullptr;
    for (const std::string &raw : split(*text, '\n')) {
        std::string_view l = trim(raw);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        if (l[0] == '[' && l.back() == ']') { groups_.push_back({std::string(l.substr(1, l.size() - 2)), {}}); cur = &groups_.back(); continue; }
        size_t eq = l.find('=');
        if (!cur || eq == std::string_view::npos) continue;
        cur->keys.emplace_back(std::string(trim(l.substr(0, eq))), ini_unescape(trim(l.substr(eq + 1))));
    }
    return true;
}

bool IniFile::save(const std::string &path) const {
    std::string o;
    for (const Group &g : groups_) {
        o += "[" + g.name + "]\n";
        for (const auto &kv : g.keys) o += kv.first + "=" + ini_escape(kv.second) + "\n";
        o += "\n";
    }
    mkdir_p(dirname(path), 0700);
    return write_file(path, o);
}

const std::string *IniFile::find(std::string_view group, std::string_view key) const {
    for (const Group &g : groups_) {
        if (g.name != group) continue;
        for (const auto &kv : g.keys) if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

bool IniFile::has(std::string_view group, std::string_view key) const { return find(group, key) != nullptr; }

std::string IniFile::get_string(std::string_view group, std::string_view key, std::string_view def) const {
    const std::string *v = find(group, key);
    return v ? *v : std::string(def);
}

int IniFile::get_int(std::string_view group, std::string_view key, int def) const {
    const std::string *v = find(group, key);
    if (!v) return def;
    char *end;
    long n = strtol(v->c_str(), &end, 10);
    return end == v->c_str() ? def : (int)n;
}

bool IniFile::get_bool(std::string_view group, std::string_view key, bool def) const {
    const std::string *v = find(group, key);
    if (!v) return def;
    return *v == "true" || *v == "1";
}

void IniFile::set_string(std::string_view group, std::string_view key, std::string_view value) {
    for (Group &g : groups_) {
        if (g.name != group) continue;
        for (auto &kv : g.keys) if (kv.first == key) { kv.second = std::string(value); return; }
        g.keys.emplace_back(std::string(key), std::string(value));
        return;
    }
    groups_.push_back({std::string(group), {{std::string(key), std::string(value)}}});
}

}
