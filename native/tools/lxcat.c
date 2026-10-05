/* lxcat - cat for Lestrix (also installed as `cat` for local Lestrix tabs, where options it does not know are passed to the real cat): prints files through the terminal instead of through the kernel's tty layer, which caps big output at
 * about 35 MB/s. Inside a Lestrix local tab it sends the terminal the file's path (one escape sequence, carrying the tab's secret
 * from $LESTRIX_FASTCAT) and the terminal reads the file itself, in order with everything else. Anywhere else, or if the output is not
 * a terminal, it behaves like cat. Piped input is first copied to /dev/shm (RAM) and then handed over the same way.
 * usage: lxcat [--no-fast] [FILE...]   (no FILE or - reads standard input) */
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

/* Piped input, in chunks: each chunk goes to /dev/shm and is handed to the terminal as it arrives, so `tail -f x | lxcat` still shows
 * lines as they come. The terminal deletes a chunk's file when it has printed it; at most two chunks are ahead of it, which is the
 * flow control that keeps a fast producer from filling memory. */
static int stream_stdin(const char *token) {
    enum { CHUNK = 8 << 20 };
    char *buf = malloc(CHUNK);
    char names[2][64] = {"", ""};
    int rc = 0;
    long seq = 0;
    if (!buf) return 1;
    for (;;) {
        size_t have = 0;
        ssize_t n;
        while (have < CHUNK && (n = read(0, buf + have, CHUNK - have)) > 0) {
            have += (size_t)n;
            struct pollfd pf = {0, POLLIN, 0};
            if (poll(&pf, 1, 0) <= 0) break;   /* nothing more waiting: send what we have now */
        }
        if (have == 0) break;
        char *old = names[seq & 1];
        for (int w = 0; old[0] && access(old, F_OK) == 0 && w < 20000; w++) { struct timespec ts = {0, 500000}; nanosleep(&ts, NULL); }   /* wait for the terminal to consume the chunk two back */
        snprintf(old, 64, "/dev/shm/lxcat-%d-%ld", (int)getpid(), seq++);
        int fd = open(old, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || write_all(fd, buf, have)) { if (fd >= 0) close(fd); rc = 1; break; }
        close(fd);
        request(token, old, 3);   /* flag 1: the terminal deletes it afterwards */
    }
    free(buf);
    for (int k = 0; k < 2; k++) {   /* do not exit before the terminal has read the last chunks */
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
    if (as_cat) for (int k = 1; k < argc; k++) if (argv[k][0] == '-' && argv[k][1]) exec_real_cat(argv);   /* -n, -A, ...: the real cat's job */
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
    if (nfiles == 0 || use_stdin) {   /* standard input */
        if (fast && !isatty(0)) { if (stream_stdin(token)) rc = 1; }
        else if (copy_fd(0, 1)) rc = 1;
    }
    return rc;
}
