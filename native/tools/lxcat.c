/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static int write_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = write(fd, p, n); if (w < 0) { if (errno == EINTR) continue; return -1; } p += w; n -= (size_t)w; }
    return 0;
}

static int copy_fd(int in, int out) {
    static char buf[1 << 20];
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -1 : 0;
        if (write_all(out, buf, (size_t)n)) return -1;
    }
}

static void request(const char *token, const char *path, unsigned flags) {
    char msg[PATH_MAX * 3 + 128];
    size_t n = (size_t)snprintf(msg, sizeof msg, "\x1b]7777;cat;%s;", token);
    for (const unsigned char *p = (const unsigned char *)path; *p && n < sizeof msg - 64; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '%' || *p == ';' || *p > 0x7e) n += (size_t)snprintf(msg + n, 4, "%%%02X", *p);
        else msg[n++] = (char)*p;
    }
    n += (size_t)snprintf(msg + n, 64, ";0;0;%u\a", flags);
    write_all(1, msg, n);
}

static const char *prog = "lxcat";

static void drop(char *name, int fd) { if (fd >= 0) close(fd); unlink(name); name[0] = 0; }

/* Moves up to `room` bytes from the pipe on stdin into fd with splice (pipe to tmpfs inside the kernel, no user buffer).
 * Returns bytes moved, 0 at EOF, -1 if stdin cannot be spliced (not a pipe), -2 on a real error. */
static ssize_t splice_chunk(int fd, size_t room) {
    size_t have = 0;
    while (have < room) {
        ssize_t n = splice(0, NULL, fd, NULL, room - have, SPLICE_F_MOVE);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return have == 0 && errno == EINVAL ? -1 : -2;
        if (n == 0) break;
        have += (size_t)n;
        struct pollfd pf = {0, POLLIN, 0};
        if (poll(&pf, 1, 0) <= 0) break;
    }
    return (ssize_t)have;
}

static int stream_stdin(const char *token) {
    enum { CHUNK = 8 << 20 };
    char *buf = NULL;
    char names[2][64] = {"", ""};
    int rc = 0;
    long seq = 0;
    int spliceable = 1;
    fcntl(0, F_SETPIPE_SZ, 4 << 20);
    for (;;) {
        char *old = names[seq & 1];
        for (int w = 0; old[0] && access(old, F_OK) == 0 && w < 20000; w++) { struct timespec ts = {0, 500000}; nanosleep(&ts, NULL); }
        snprintf(old, 64, "/dev/shm/lxcat-%d-%ld", (int)getpid(), seq);
        int fd = open(old, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) { old[0] = 0; rc = 1; break; }
        ssize_t have = 0;
        if (spliceable) {
            have = splice_chunk(fd, CHUNK);
            if (have == -1) spliceable = 0;
            else if (have < 0) { drop(old, fd); rc = 1; break; }
        }
        if (!spliceable) {
            if (!buf && !(buf = malloc(CHUNK))) { drop(old, fd); rc = 1; break; }
            ssize_t n;
            have = 0;
            while ((size_t)have < CHUNK && (n = read(0, buf + have, CHUNK - have)) > 0) {
                have += n;
                struct pollfd pf = {0, POLLIN, 0};
                if (poll(&pf, 1, 0) <= 0) break;
            }
            if (have > 0 && write_all(fd, buf, (size_t)have)) { drop(old, fd); rc = 1; break; }
        }
        if (have == 0) { drop(old, fd); break; }
        close(fd);
        seq++;
        request(token, old, 3);
    }
    free(buf);
    for (int k = 0; k < 2; k++) {
        for (int w = 0; names[k][0] && access(names[k], F_OK) == 0 && w < 20000; w++) { struct timespec ts = {0, 500000}; nanosleep(&ts, NULL); }
    }
    return rc;
}

static void exec_real_cat(char **argv) {
    argv[0] = "cat";
    execv("/usr/bin/cat", argv);
    execv("/bin/cat", argv);
    perror("cat");
    _exit(127);
}

int main(int argc, char **argv) {
    const char *slash = strrchr(argv[0], '/');
    prog = slash ? slash + 1 : argv[0];
    int as_cat = !strcmp(prog, "cat");
    if (as_cat) for (int k = 1; k < argc; k++) if (argv[k][0] == '-' && argv[k][1]) exec_real_cat(argv);
    const char *token = getenv("LESTRIX_FASTCAT");
    int fast = token && *token && isatty(1);
    int rc = 0, i = 1, nfiles = 0, use_stdin = 0;
    if (i < argc && !strcmp(argv[i], "--no-fast")) { fast = 0; i++; }
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-")) { use_stdin = 1; continue; }
        nfiles++;
        struct stat st;
        char real[PATH_MAX];
        if (fast && stat(argv[i], &st) == 0 && S_ISREG(st.st_mode) && realpath(argv[i], real)) { request(token, real, 2); continue; }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "%s: %s: %s\n", prog, argv[i], strerror(errno)); rc = 1; continue; }
        if (copy_fd(fd, 1)) rc = 1;
        close(fd);
    }
    if (nfiles == 0 || use_stdin) {
        if (fast && !isatty(0)) { if (stream_stdin(token)) rc = 1; }
        else if (copy_fd(0, 1)) rc = 1;
    }
    return rc;
}
