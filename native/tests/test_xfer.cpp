/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "../src/util.h"
#include "../src/xfer.h"

static int failures, checks, skipped;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const std::vector<std::string> OPTS = {"-o", "UserKnownHostsFile=/dev/null", "-o", "StrictHostKeyChecking=no", "-o", "LogLevel=ERROR"};

static void test_parse_ls() {
    EntryList a = sd_parse_ls(
        "sftp> ls -la\n"
        "drwxr-xr-x    ? bob      bob          4096 Oct  3 12:01 My Docs\n"
        "-rw-r--r--    1 bob      bob           120 Jan  5  2025 notes.txt\n"
        "lrwxrwxrwx    1 bob      bob             4 Jan  5 10:00 link -> docs\n"
        "drwxr-xr-x    2 bob      bob          4096 Oct  3 12:01 .\n"
        "drwxr-xr-x    9 bob      bob          4096 Oct  3 12:01 ..\n"
        "total 12\n");
    CHECK(a.size() == 3);
    const SdEntry &d = a[0], &f = a[1], &l = a[2];
    CHECK(d.name == "My Docs" && d.is_dir);
    CHECK(f.name == "notes.txt" && f.size == 120 && !f.is_dir);
    CHECK(l.name == "link" && l.is_link);
}

static bool localhost_ok() {
    std::vector<std::string> argv{"ssh"};
    argv.insert(argv.end(), OPTS.begin(), OPTS.end());
    argv.insert(argv.end(), {"-o", "BatchMode=yes", "-o", "ConnectTimeout=3", "localhost", "true"});
    lx::RunResult r;
    return lx::run_command(argv, "", 0, r) && r.status == 0;
}

static void roundtrip(const char *mode, const char *order) {
    std::string cp = sd_xfer_new_control_path();
    std::vector<std::string> argv{"ssh"};
    argv.insert(argv.end(), OPTS.begin(), OPTS.end());
    argv.insert(argv.end(), {"-N", "-o", "ControlMaster=yes", "-o", "ControlPath=" + cp, "localhost"});
    long pid = lx::spawn_async(argv);
    CHECK(pid > 0);

    SdConn conn;
    conn.host = "localhost";
    auto x = SdXfer::new_ssh(conn, cp);
    for (int i = 0; i < 60 && !x->ready(); i++) usleep(100000);
    CHECK(x->ready());
    x->force_mode(mode, order);

    std::string err;
    auto home = x->home(&err);
    CHECK(home && !home->empty() && (*home)[0] == '/');
    std::string work = *home + "/.lestrix-ctest dir";
    CHECK(x->mkdir(work, &err));
    std::string tmpd = lx::make_temp_dir("sdx-");
    std::string f = lx::path_join(tmpd, "up file.txt"), d = lx::path_join(tmpd, "updir"), di = lx::path_join(d, "inner.txt");
    lx::write_file(f, "hi");
    lx::mkdir_p(d);
    lx::write_file(di, "deep");
    CHECK(x->upload(f, work, &err));
    CHECK(x->upload(d, work, &err));
    auto l = x->list(work, &err);
    CHECK(l && l->size() == 2 && (*l)[0].name == "updir" && (*l)[1].name == "up file.txt");
    std::string out = lx::path_join(tmpd, "dl");
    lx::mkdir_p(out);
    std::string rf = work + "/up file.txt", rd = work + "/updir";
    CHECK(x->download(rf, out, false, &err));
    CHECK(x->download(rd, out, true, &err));
    auto got1 = lx::read_file(lx::path_join(out, "up file.txt")), got2 = lx::read_file(lx::path_join(out, "updir", "inner.txt"));
    CHECK(got1 && *got1 == "hi");
    CHECK(got2 && *got2 == "deep");
    std::string rn = work + "/renamed.txt";
    CHECK(x->rename(rf, rn, &err));
    CHECK(x->is_dir(work) && !x->is_dir(rn));
    CHECK(x->remove(rn, false, &err));
    CHECK(x->remove(work, true, &err));
    CHECK(!x->is_dir(work));
    if (!err.empty()) printf("  last error (%s): %s\n", mode, err.c_str());
    printf("  %-22s ok, label \"%s\"\n", order ? order : "sftp", x->label().c_str());

    kill((pid_t)pid, SIGTERM);
    waitpid((pid_t)pid, nullptr, 0);
    sd_xfer_remove_control_path(cp);
}

int main() {
    test_parse_ls();
    if (localhost_ok()) {
        roundtrip("sftp", nullptr);
        roundtrip("ssh", "scp");
        roundtrip("ssh", "ssh");
    } else {
        skipped = 1;
        printf("  (skipping ssh backends: passwordless ssh to localhost not available)\n");
    }
    printf("%d checks, %d failures%s\n", checks, failures, skipped ? ", ssh tests skipped" : "");
    return failures ? 1 : 0;
}
