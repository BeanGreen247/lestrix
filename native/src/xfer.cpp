/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "xfer.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "util.h"

namespace {

void set_err(std::string *err, std::string msg) { if (err) *err = std::move(msg); }

bool icontains(const std::string &hay, const char *needle) { return strcasestr(hay.c_str(), needle) != nullptr; }

std::string sftp_quote(std::string_view p) {
    std::string s = "\"";
    for (char c : p) { if (c == '"' || c == '\\') s += '\\'; s += c; }
    return s + '"';
}

std::string scp_quote(std::string_view p) {
    std::string s;
    for (char c : p) {
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum && !strchr("_./~+:@%-", c)) s += '\\';
        s += c;
    }
    return s;
}

std::string join_path(const std::string &a, const std::string &b) {
    if (a == "/") return "/" + b;
    return a.ends_with("/") ? a + b : a + "/" + b;
}

std::string error_text(const std::string &err, const char *fallback) {
    std::string_view t = lx::trim(err);
    return t.empty() ? std::string(fallback) : std::string(t);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

/* One `ls -l` line: perms links owner group size month day time name. */
bool parse_ls_line(std::string_view l, SdEntry &e) {
    size_t p = 0, n = l.size();
    auto skip_ws = [&]() { size_t s = p; while (p < n && is_space(l[p])) p++; return p > s; };
    auto token = [&](bool (*ok)(char)) { size_t s = p; while (p < n && ok(l[p])) p++; return l.substr(s, p - s); };
    auto nonspace = [](char c) { return !is_space(c); };
    auto digit = [](char c) { return is_digit(c); };
    auto word = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    auto timechar = [](char c) { return is_digit(c) || c == ':'; };
    if (n < 11 || !strchr("-dlbcps", l[0])) return false;
    for (size_t i = 1; i < 10; i++) if (!strchr("rwxsStT-", l[i])) return false;
    p = 10;
    if (p < n && strchr(".+@", l[p])) p++;
    size_t perms_end = p;
    if (!skip_ws()) return false;
    if (p < n && l[p] == '?') p++;
    else if (token(digit).empty()) return false;
    if (!skip_ws() || token(nonspace).empty() || !skip_ws() || token(nonspace).empty() || !skip_ws()) return false;
    std::string_view size = token(digit);
    if (size.empty() || !skip_ws()) return false;
    size_t date_start = p;
    if (token(word).size() != 3 || !skip_ws() || token(digit).empty() || !skip_ws() || token(timechar).empty()) return false;
    size_t date_end = p;
    if (!skip_ws() || p >= n) return false;
    e.perms = std::string(l.substr(0, perms_end));
    e.modified = std::string(l.substr(date_start, date_end - date_start));
    e.size = strtoll(std::string(size).c_str(), nullptr, 10);
    e.name = std::string(l.substr(p));
    return true;
}

size_t curl_to_string(void *p, size_t sz, size_t n, void *ud) { static_cast<std::string *>(ud)->append(static_cast<const char *>(p), sz * n); return sz * n; }
size_t curl_to_file(void *p, size_t sz, size_t n, void *ud) { return fwrite(p, sz, n, static_cast<FILE *>(ud)); }
size_t curl_from_file(void *p, size_t sz, size_t n, void *ud) { return fread(p, sz, n, static_cast<FILE *>(ud)); }

bool ftp_perform(CURL *c, std::string *err) {
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) { set_err(err, curl_easy_strerror(rc)); return false; }
    return true;
}

}

EntryList sd_parse_ls(std::string_view text) {
    EntryList out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        std::string_view line = lx::trim(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        SdEntry e;
        if (!parse_ls_line(line, e)) continue;
        e.is_link = e.perms[0] == 'l';
        if (e.is_link) { size_t arrow = e.name.find(" -> "); if (arrow != std::string::npos) e.name.resize(arrow); }
        size_t cr = e.name.find('\r');
        if (cr != std::string::npos) e.name.resize(cr);
        if (e.name == "." || e.name == "..") continue;
        e.is_dir = e.perms[0] == 'd';
        out.push_back(std::move(e));
    }
    return out;
}

std::string sd_xfer_new_control_path() {
    std::string dir = lx::make_temp_dir("sd-");
    if (dir.empty() || dir.size() > 60) {
        std::string t = "/tmp/sd-XXXXXX";
        dir = mkdtemp(t.data()) ? t : std::string();
    }
    if (dir.empty()) return {};
    chmod(dir.c_str(), 0700);
    return lx::path_join(dir, "m");
}

void sd_xfer_remove_control_path(const std::string &path) {
    if (path.empty()) return;
    unlink(path.c_str());
    rmdir(lx::dirname(path).c_str());
}

std::unique_ptr<SdXfer> SdXfer::new_ssh(const SdConn &conn, const std::string &control_path) {
    std::unique_ptr<SdXfer> x(new SdXfer(conn));
    x->control_path_ = control_path;
    x->dest_ = conn.dest();
    x->xfer_order_ = {"scp", "ssh"};
    return x;
}

std::unique_ptr<SdXfer> SdXfer::new_ftp(const SdConn &conn, const std::string &password) {
    std::unique_ptr<SdXfer> x(new SdXfer(conn));
    if (x->conn_.port <= 0 || x->conn_.port == 22) x->conn_.port = 21;
    x->ftp_ = true;
    x->password_ = password;
    return x;
}

void SdXfer::force_mode(const char *mode, const char *order) {
    if (mode) sftp_ok_ = !strcmp(mode, "sftp");
    if (order) xfer_order_ = lx::split(order, ',');
}

std::string SdXfer::label() const {
    if (ftp_) return conn_.protocol == "ftps" ? "FTPS" : "FTP";
    return sftp_ok_ ? "SFTP" : "ssh (no SFTP on this server)";
}

std::vector<std::string> SdXfer::mux_args(const char *port_flag) const {
    std::vector<std::string> a{"-o", "ControlPath=" + control_path_, "-o", "ControlMaster=no", "-o", "BatchMode=yes"};
    if (!conn_.is_live()) { a.push_back(port_flag); a.push_back(std::to_string(conn_.port)); }
    return a;
}

bool SdXfer::ready() {
    if (ftp_) return true;
    if (!lx::exists(control_path_)) return false;
    lx::RunResult r;
    return lx::run_command({"ssh", "-S", control_path_, "-O", "check", dest_}, "", 5, r) && r.status == 0;
}

std::optional<std::string> SdXfer::ssh_run(const std::string &command, int timeout_s, std::string *err) {
    std::vector<std::string> argv{"ssh"};
    for (std::string &m : mux_args("-p")) argv.push_back(std::move(m));
    argv.push_back(dest_);
    argv.push_back(command);
    lx::RunResult r;
    if (!lx::run_command(argv, "", timeout_s, r)) { set_err(err, "could not run ssh"); return std::nullopt; }
    if (r.timed_out) { set_err(err, "timed out"); return std::nullopt; }
    if (r.status != 0) { set_err(err, error_text(r.err, "command failed")); return std::nullopt; }
    return std::move(r.out);
}

std::optional<std::string> SdXfer::sftp_batch(const std::vector<std::string> &cmds, int timeout_s, bool &unavailable, std::string *err) {
    std::vector<std::string> argv{"sftp"};
    for (std::string &m : mux_args("-P")) argv.push_back(std::move(m));
    argv.insert(argv.end(), {"-b", "-", dest_});
    lx::RunResult r;
    unavailable = false;
    if (!lx::run_command(argv, lx::join(cmds, "\n") + "\n", timeout_s, r)) {
        unavailable = true;
        set_err(err, "the sftp command is not installed");
        return std::nullopt;
    }
    if (r.timed_out) { set_err(err, "timed out"); return std::nullopt; }
    if (r.status != 0) {
        if (icontains(r.err, "subsystem")) { unavailable = true; set_err(err, "SFTP is not enabled on this server"); }
        else set_err(err, error_text(r.err, "sftp failed"));
        return std::nullopt;
    }
    return std::move(r.out);
}

std::optional<std::string> SdXfer::try_sftp(const std::vector<std::string> &cmds, int timeout_s, bool &used, std::string *err) {
    used = false;
    if (!sftp_ok_) return std::nullopt;
    bool unavailable;
    auto out = sftp_batch(cmds, timeout_s, unavailable, err);
    if (unavailable) { sftp_ok_ = false; if (err) err->clear(); return std::nullopt; }
    used = true;
    return out;
}

CURL *SdXfer::ftp_handle(const std::string &path, bool dir) {
    CURL *c = curl_easy_init();
    char *enc = curl_easy_escape(c, path.c_str(), 0);
    std::string clean;
    for (const char *p = enc; *p; p++) {
        if (!strncmp(p, "%2F", 3)) { clean += '/'; p += 2; } else clean += *p;
    }
    const char *scheme = conn_.protocol == "ftps" ? "ftps" : "ftp";
    std::string url = lx::strprintf("%s://%s:%d%s%s", scheme, conn_.host.c_str(), conn_.port, clean.starts_with("/") ? "" : "/", clean.c_str());
    if (dir && !url.ends_with("/")) url += '/';
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERNAME, conn_.user.empty() ? "anonymous" : conn_.user.c_str());
    curl_easy_setopt(c, CURLOPT_PASSWORD, ftp_ && !password_.empty() ? password_.c_str() : "anonymous@");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FTP_FILEMETHOD, CURLFTPMETHOD_NOCWD);
    if (conn_.protocol == "ftps") curl_easy_setopt(c, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_free(enc);
    return c;
}

std::optional<EntryList> SdXfer::ftp_list(const std::string &path, std::string *err) {
    CURL *c = ftp_handle(path, true);
    std::string buf;
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_to_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    if (!ftp_perform(c, err)) return std::nullopt;
    return sd_parse_ls(buf);
}

bool SdXfer::ftp_quote(const std::string &dir, const std::string &cmd1, const std::string *cmd2, std::string *err) {
    CURL *c = ftp_handle(dir, true);
    struct curl_slist *q = curl_slist_append(nullptr, cmd1.c_str());
    if (cmd2) q = curl_slist_append(q, cmd2->c_str());
    curl_easy_setopt(c, CURLOPT_QUOTE, q);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    bool ok = ftp_perform(c, err);
    curl_slist_free_all(q);
    return ok;
}

bool SdXfer::ftp_get_file(const std::string &remote, const std::string &local, std::string *err) {
    FILE *f = fopen(local.c_str(), "wb");
    if (!f) { set_err(err, "cannot write " + local); return false; }
    CURL *c = ftp_handle(remote, false);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_to_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    bool ok = ftp_perform(c, err);
    fclose(f);
    if (!ok) unlink(local.c_str());
    return ok;
}

bool SdXfer::ftp_put_file(const std::string &local, const std::string &remote, std::string *err) {
    FILE *f = fopen(local.c_str(), "rb");
    if (!f) { set_err(err, "cannot read " + local); return false; }
    struct stat st;
    fstat(fileno(f), &st);
    CURL *c = ftp_handle(remote, false);
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READFUNCTION, curl_from_file);
    curl_easy_setopt(c, CURLOPT_READDATA, f);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
    curl_easy_setopt(c, CURLOPT_FTP_CREATE_MISSING_DIRS, 1L);
    bool ok = ftp_perform(c, err);
    fclose(f);
    return ok;
}

bool SdXfer::ftp_remove_tree(const std::string &path, bool is_dir, std::string *err) {
    std::string dir = lx::dirname(path);
    if (is_dir) {
        auto kids = ftp_list(path, err);
        if (!kids) return false;
        for (const SdEntry &e : *kids)
            if (!ftp_remove_tree(join_path(path, e.name), e.is_dir, err)) return false;
    }
    return ftp_quote(dir, std::string(is_dir ? "RMD " : "DELE ") + path, nullptr, err);
}

bool SdXfer::ftp_download_tree(const std::string &remote, const std::string &local_dir, bool is_dir, std::string *err) {
    std::string target = lx::path_join(local_dir, lx::basename(remote));
    if (!is_dir) return ftp_get_file(remote, target, err);
    lx::mkdir_p(target, 0755);
    auto kids = ftp_list(remote, err);
    if (!kids) return false;
    for (const SdEntry &e : *kids)
        if (!ftp_download_tree(join_path(remote, e.name), target, e.is_dir, err)) return false;
    return true;
}

bool SdXfer::ftp_upload_tree(const std::string &local, const std::string &remote_dir, std::string *err) {
    std::string target = join_path(remote_dir, lx::basename(local));
    if (!lx::is_dir(local)) return ftp_put_file(local, target, err);
    std::string ignored;
    ftp_quote(remote_dir, "MKD " + target, nullptr, &ignored);
    for (const std::string &n : lx::list_dir(local))
        if (!ftp_upload_tree(lx::path_join(local, n), target, err)) return false;
    return true;
}

std::optional<std::string> SdXfer::home(std::string *err) {
    if (ftp_) return "/";
    bool used;
    auto out = try_sftp({"pwd"}, 20, used, err);
    if (!used) {
        auto o = ssh_run("printf %s \"$HOME\"", 10, err);
        if (o && o->empty()) return "/";
        return o;
    }
    if (!out) return std::nullopt;
    size_t p = out->find("Remote working directory:");
    if (p == std::string::npos) { set_err(err, "could not read the remote home directory"); return std::nullopt; }
    return std::string(lx::trim(std::string_view(*out).substr(p + 25)));
}

std::optional<EntryList> SdXfer::list(const std::string &path, std::string *err) {
    std::optional<EntryList> r;
    if (ftp_) r = ftp_list(path, err);
    else {
        bool used;
        auto out = try_sftp({"cd " + sftp_quote(path), "ls -la"}, 60, used, err);
        if (!used) out = ssh_run("cd -- " + lx::shell_quote(path) + " && LC_ALL=C ls -la", 60, err);
        if (out) r = sd_parse_ls(*out);
    }
    if (r) {
        std::vector<std::pair<std::string, size_t>> keys;
        for (size_t i = 0; i < r->size(); i++) keys.emplace_back(lx::utf8_lower((*r)[i].name), i);
        std::vector<size_t> idx(r->size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            const SdEntry &x = (*r)[a], &y = (*r)[b];
            bool dx = x.is_dir || x.is_link, dy = y.is_dir || y.is_link;
            if (dx != dy) return dx;
            return keys[a].first < keys[b].first;
        });
        EntryList sorted;
        sorted.reserve(r->size());
        for (size_t i : idx) sorted.push_back(std::move((*r)[i]));
        *r = std::move(sorted);
    }
    return r;
}

bool SdXfer::is_dir(const std::string &path) {
    std::string ignored;
    if (ftp_) return ftp_list(path, &ignored).has_value();
    bool used;
    auto out = try_sftp({"cd " + sftp_quote(path)}, 20, used, &ignored);
    if (used) return out.has_value();
    return ssh_run("test -d " + lx::shell_quote(path), 20, &ignored).has_value();
}

bool SdXfer::mkdir(const std::string &path, std::string *err) {
    if (ftp_) return ftp_quote(lx::dirname(path), "MKD " + path, nullptr, err);
    bool used;
    auto out = try_sftp({"mkdir " + sftp_quote(path)}, 30, used, err);
    if (used) return out.has_value();
    return ssh_run("mkdir -- " + lx::shell_quote(path), 30, err).has_value();
}

bool SdXfer::rename(const std::string &from, const std::string &to, std::string *err) {
    if (ftp_) {
        std::string b = "RNTO " + to;
        return ftp_quote(lx::dirname(from), "RNFR " + from, &b, err);
    }
    bool used;
    auto out = try_sftp({"rename " + sftp_quote(from) + " " + sftp_quote(to)}, 30, used, err);
    if (used) return out.has_value();
    return ssh_run("mv -- " + lx::shell_quote(from) + " " + lx::shell_quote(to), 30, err).has_value();
}

bool SdXfer::remove(const std::string &path, bool is_dir, std::string *err) {
    if (ftp_) return ftp_remove_tree(path, is_dir, err);
    if (is_dir) return ssh_run("rm -rf -- " + lx::shell_quote(path), 600, err).has_value();
    bool used;
    auto out = try_sftp({"rm " + sftp_quote(path)}, 60, used, err);
    if (used) return out.has_value();
    return ssh_run("rm -- " + lx::shell_quote(path), 60, err).has_value();
}

bool SdXfer::run_script(const std::string &script, std::string *err) {
    bool bash = !lx::find_program("bash").empty();
    lx::RunResult r;
    bool ok = lx::run_command({bash ? "bash" : "sh", "-c", std::string(bash ? "set -o pipefail; " : "") + script}, "", 0, r) && r.status == 0;
    if (!ok) set_err(err, error_text(r.err, "transfer failed"));
    return ok;
}

std::string SdXfer::ssh_prefix() const {
    std::vector<std::string> a{"ssh"};
    for (const std::string &m : mux_args("-p")) a.push_back(m);
    a.push_back(dest_);
    std::string s;
    for (size_t i = 0; i < a.size(); i++) { if (i) s += ' '; s += lx::shell_quote(a[i]); }
    return s;
}

bool SdXfer::scp_run(const std::vector<std::string> &tail, std::string *err) {
    for (int legacy = 1; legacy >= 0; legacy--) {
        std::vector<std::string> argv{"scp"};
        for (std::string &m : mux_args("-P")) argv.push_back(std::move(m));
        if (legacy) argv.push_back("-O");
        argv.insert(argv.end(), tail.begin(), tail.end());
        lx::RunResult r;
        if (!lx::run_command(argv, "", 0, r)) { set_err(err, "the scp command is not installed"); return false; }
        bool ok = r.status == 0;
        bool retry = legacy && !ok && (icontains(r.err, "unknown option") || icontains(r.err, "illegal option") || icontains(r.err, "usage:"));
        if (!ok && !retry) set_err(err, error_text(r.err, "scp failed"));
        if (ok) return true;
        if (!retry) return false;
    }
    return false;
}

bool SdXfer::fallback_transfer(const std::function<bool(std::string *)> &scp, const std::function<bool(std::string *)> &stream, std::string *err) {
    std::string errors;
    for (const std::string &m : xfer_order_) {
        std::string e;
        if (m == "scp" ? scp(&e) : stream(&e)) return true;
        errors += (errors.empty() ? "" : "; ") + m + ": " + (e.empty() ? "failed" : e);
    }
    set_err(err, errors);
    return false;
}

bool SdXfer::download(const std::string &remote, const std::string &local_dir, bool is_dir, std::string *err) {
    if (ftp_) return ftp_download_tree(remote, local_dir, is_dir, err);
    bool used;
    auto out = try_sftp({std::string("get ") + (is_dir ? "-r " : "") + sftp_quote(remote) + " " + sftp_quote(local_dir)}, 0, used, err);
    if (used) return out.has_value();
    auto scp = [&](std::string *e) {
        std::string src = dest_ + ":" + scp_quote(remote), dst = local_dir + "/";
        return scp_run(is_dir ? std::vector<std::string>{"-r", src, dst} : std::vector<std::string>{src, dst}, e);
    };
    auto stream = [&](std::string *e) {
        std::string ssh = ssh_prefix(), parent = lx::dirname(remote), name = lx::basename(remote), script;
        if (is_dir) {
            std::string remote_cmd = "tar -C " + lx::shell_quote(parent) + " -cf - " + lx::shell_quote(name);
            script = ssh + " " + lx::shell_quote(remote_cmd) + " | tar -C " + lx::shell_quote(local_dir) + " -xf -";
        } else {
            std::string remote_cmd = "cat -- " + lx::shell_quote(remote);
            script = ssh + " " + lx::shell_quote(remote_cmd) + " > " + lx::shell_quote(lx::path_join(local_dir, name));
        }
        return run_script(script, e);
    };
    return fallback_transfer(scp, stream, err);
}

bool SdXfer::upload(const std::string &local, const std::string &remote_dir, std::string *err) {
    if (ftp_) return ftp_upload_tree(local, remote_dir, err);
    bool dir = lx::is_dir(local);
    bool used;
    auto out = try_sftp({std::string("put ") + (dir ? "-r " : "") + sftp_quote(local) + " " + sftp_quote(remote_dir)}, 0, used, err);
    if (used) return out.has_value();
    auto scp = [&](std::string *e) {
        std::string rd = remote_dir.ends_with("/") ? remote_dir : remote_dir + "/";
        std::string dst = dest_ + ":" + scp_quote(rd);
        return scp_run(dir ? std::vector<std::string>{"-r", local, dst} : std::vector<std::string>{local, dst}, e);
    };
    auto stream = [&](std::string *e) {
        std::string ssh = ssh_prefix(), name = lx::basename(local), parent = lx::dirname(local), script;
        if (dir) {
            std::string rc = "tar -C " + lx::shell_quote(remote_dir) + " -xf -";
            script = "tar -C " + lx::shell_quote(parent) + " -cf - " + lx::shell_quote(name) + " | " + ssh + " " + lx::shell_quote(rc);
        } else {
            std::string rc = "cat > " + lx::shell_quote(join_path(remote_dir, name));
            script = ssh + " " + lx::shell_quote(rc) + " < " + lx::shell_quote(local);
        }
        return run_script(script, e);
    };
    return fallback_transfer(scp, stream, err);
}

std::string SdXfer::terminal_cwd() {
    if (ftp_) return {};
    const char *script = "p=$(ps -o pid=,tty= --ppid $PPID 2>/dev/null | awk '$2!=\"?\"{print $1; exit}'); "
                         "[ -n \"$p\" ] && readlink /proc/$p/cwd";
    auto o = ssh_run(script, 5, nullptr);
    return o ? std::string(lx::trim(*o)) : std::string();
}
