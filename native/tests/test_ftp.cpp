/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../src/util.h"
#include "../src/xfer.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    std::string root = lx::make_temp_dir("sdftp-");
    int fds[2];
    if (pipe(fds)) return 1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]); close(fds[1]);
        execlp("python3", "python3", "tests/ftp_server.py", root.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[32] = {0};
    ssize_t n = read(fds[0], buf, sizeof buf - 1);
    if (n <= 0) { printf("  (skipping ftp: python3 not available)\n"); waitpid(pid, nullptr, 0); return 0; }
    int port = atoi(buf);
    CHECK(port > 0);

    SdConn c;
    c.host = "127.0.0.1";
    c.user = "tester";
    c.protocol = "ftp";
    c.port = port;
    auto x = SdXfer::new_ftp(c, "secret");
    std::string err;

    std::string tmpd = lx::make_temp_dir("sdftpc-");
    std::string f = lx::path_join(tmpd, "up file.txt"), d = lx::path_join(tmpd, "updir"), di = lx::path_join(d, "inner.txt");
    lx::write_file(f, "hi ftp");
    lx::mkdir_p(d);
    lx::write_file(di, "deep ftp");

    auto home = x->home(&err);
    CHECK(home && *home == "/");
    CHECK(x->mkdir("/work", &err));
    CHECK(x->upload(f, "/work", &err));
    CHECK(x->upload(d, "/work", &err));
    auto l = x->list("/work", &err);
    CHECK(l && l->size() == 2 && (*l)[0].name == "updir" && (*l)[0].is_dir);
    CHECK(l && (*l)[1].name == "up file.txt" && (*l)[1].size == 6);

    std::string out = lx::path_join(tmpd, "dl");
    lx::mkdir_p(out);
    CHECK(x->download("/work/up file.txt", out, false, &err));
    CHECK(x->download("/work/updir", out, true, &err));
    auto got1 = lx::read_file(lx::path_join(out, "up file.txt")), got2 = lx::read_file(lx::path_join(out, "updir", "inner.txt"));
    CHECK(got1 && *got1 == "hi ftp");
    CHECK(got2 && *got2 == "deep ftp");

    CHECK(x->rename("/work/up file.txt", "/work/renamed.txt", &err));
    CHECK(x->is_dir("/work") && lx::exists(lx::path_join(root, "work", "renamed.txt")));
    CHECK(x->remove("/work", true, &err));
    CHECK(!lx::exists(lx::path_join(root, "work")));
    CHECK(x->label() == "FTP");
    if (!err.empty()) printf("  last error: %s\n", err.c_str());

    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
