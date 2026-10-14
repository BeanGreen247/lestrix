/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_XFER_H
#define SD_XFER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store.h"

typedef void CURL;

struct SdEntry {
    std::string name;
    bool is_dir = false, is_link = false;
    int64_t size = 0;
    std::string modified, perms;
};

using EntryList = std::vector<SdEntry>;

EntryList sd_parse_ls(std::string_view text);
std::string sd_xfer_new_control_path();
void sd_xfer_remove_control_path(const std::string &path);

/* Remote file operations over an ssh master connection (sftp, then scp or a tar/cat stream) or over FTP/FTPS.
 * Every call that can fail takes an optional std::string *err for the reason. */
class SdXfer {
public:
    static std::unique_ptr<SdXfer> new_ssh(const SdConn &conn, const std::string &control_path);
    static std::unique_ptr<SdXfer> new_ftp(const SdConn &conn, const std::string &password);

    bool is_ftp() const { return ftp_; }
    std::string label() const;
    bool ready();
    void force_mode(const char *mode, const char *xfer_order);

    std::optional<std::string> home(std::string *err);
    std::optional<EntryList> list(const std::string &path, std::string *err);
    bool is_dir(const std::string &path);
    bool mkdir(const std::string &path, std::string *err);
    bool rename(const std::string &from, const std::string &to, std::string *err);
    bool remove(const std::string &path, bool is_dir, std::string *err);
    bool download(const std::string &remote, const std::string &local_dir, bool is_dir, std::string *err);
    bool upload(const std::string &local, const std::string &remote_dir, std::string *err);
    std::string terminal_cwd();

private:
    SdXfer(const SdConn &c) : conn_(c) {}

    SdConn conn_;
    std::string control_path_, dest_, password_;
    bool ftp_ = false;
    bool sftp_ok_ = true;
    std::vector<std::string> xfer_order_;

    std::vector<std::string> mux_args(const char *port_flag) const;
    std::string ssh_prefix() const;
    std::optional<std::string> ssh_run(const std::string &command, int timeout_s, std::string *err);
    std::optional<std::string> sftp_batch(const std::vector<std::string> &cmds, int timeout_s, bool &unavailable, std::string *err);
    std::optional<std::string> try_sftp(const std::vector<std::string> &cmds, int timeout_s, bool &used, std::string *err);
    bool scp_run(const std::vector<std::string> &tail, std::string *err);
    bool run_script(const std::string &script, std::string *err);
    bool fallback_transfer(const std::function<bool(std::string *)> &scp, const std::function<bool(std::string *)> &stream, std::string *err);

    CURL *ftp_handle(const std::string &path, bool dir);
    std::optional<EntryList> ftp_list(const std::string &path, std::string *err);
    bool ftp_quote(const std::string &dir, const std::string &cmd1, const std::string *cmd2, std::string *err);
    bool ftp_get_file(const std::string &remote, const std::string &local, std::string *err);
    bool ftp_put_file(const std::string &local, const std::string &remote, std::string *err);
    bool ftp_remove_tree(const std::string &path, bool is_dir, std::string *err);
    bool ftp_download_tree(const std::string &remote, const std::string &local_dir, bool is_dir, std::string *err);
    bool ftp_upload_tree(const std::string &local, const std::string &remote_dir, std::string *err);
};

#endif
