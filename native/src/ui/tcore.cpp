/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "tcore.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <atomic>
#include <new>
#include <dirent.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include "../pty.h"

typedef struct PendingEv { VtEvent ev; char *text; struct PendingEv *next; } PendingEv;

static int default_scrollback = 10000;
static size_t default_ram_mb = 32, default_disk_mb = 4096;
static bool default_spill = true;

struct TermCore {
    Vt *vt;
    SdPty pty;
    pthread_mutex_t lock;
    pthread_t worker;
    bool worker_started;
    bool io_split, reader_started, writer_started;
    pthread_t reader, writer;
    pthread_mutex_t iom; pthread_cond_t io_data, io_space, out_cv;
    struct { uint8_t *data; size_t n; } iob[8];
    int io_head, io_count;
    bool io_eof;
    int wake[2];
    std::atomic<int> stop, act_flag, eof_flag, running_flag, wake_pending;
    std::atomic<int> ui_waiting;
    std::atomic<unsigned long long> bytes_fed;
    struct { bool on, saw_child, alt; double t0, t_last, t_back; uint64_t fed0, fed_last; } cmd;
    struct { bool active, suspect, th_started; struct termios saved; pid_t fg, last_checked; double release_at; pthread_t th; std::atomic<int> restored, cancel; int pidfd; } fo;
    char fastcat_token[33];
    struct { std::atomic<int> busy; pthread_t th; bool started; pid_t last; std::atomic<int> pipe_fd, stream_done; } nc;
    std::atomic<unsigned> intr_seq;  /* counts Ctrl-C keystrokes sent to the program, so a take-over can tell it was interrupted */
    std::atomic<unsigned long long> reads;
    bool running, eof, child_done;
    int exit_status;
    pthread_mutex_t outlock, evlock;
    uint8_t *outq; size_t outhead, outlen, outcap;
    PendingEv *ev_head, *ev_tail;
    char **argv;
    char *cwd, *title;
    int cols, rows;
    int offset;
    double scroll_acc;
    bool sel_on, dragging;
    int sel_a_idx, sel_a_col, sel_b_idx, sel_b_col;
    int mouse_btn;
    bool focused;
    TermHooks hooks;
    void *user;
};

static inline void lock_counted(TermCore *t) {
    t->ui_waiting.fetch_add(1, std::memory_order_relaxed);
    pthread_mutex_lock(&t->lock);
    t->ui_waiting.fetch_sub(1, std::memory_order_relaxed);
}
#define LOCK(t) lock_counted(t)
#define UNLOCK(t) pthread_mutex_unlock(&(t)->lock)

void tcore_set_history_defaults(int lines, size_t ram_mb, size_t disk_mb, bool spill) {
    default_scrollback = lines;
    if (ram_mb) default_ram_mb = ram_mb;
    if (disk_mb) default_disk_mb = disk_mb;
    default_spill = spill;
}

Vt *tcore_vt(TermCore *t) { return t->vt; }
void tcore_lock(TermCore *t) { LOCK(t); }
void tcore_unlock(TermCore *t) { UNLOCK(t); }
int tcore_cols(const TermCore *t) { return t->cols; }
int tcore_rows(const TermCore *t) { return t->rows; }
bool tcore_running(const TermCore *t) { return t->running; }
int tcore_read_delay_us = 200;
int tcore_io_threads = -1;
static std::atomic<bool> g_native_cat{true};
static std::atomic<unsigned> g_native_cat_count{0};
void tcore_set_native_cat(bool on) { g_native_cat.store(on); }
unsigned tcore_native_cat_count() { return g_native_cat_count.load(); }

uint64_t tcore_reads(const TermCore *t) { return t->reads.load(); }
static bool g_fast_output = true;

void tcore_set_fast_output(bool on) { g_fast_output = on; }

static bool fo_comm_ok(pid_t pgrp) {
    static const char *const ok[] = {"cat", "ls", "grep", "egrep", "fgrep", "rg", "find", "fd", "seq", "tail", "head", "tree", "du", "df", "ps", "lsblk", "wc", "sort",
                                     "uniq", "cut", "tr", "sed", "awk", "gawk", "mawk", "base64", "od", "hexdump", "xxd", "dd", "zcat", "bzcat", "xzcat", "strings",
                                     "nl", "tac", "rev", "fold", "yes", "make", "gcc", "cc", "g++", "cargo", "tar", "unzip", "rsync", "zgrep",
                                     "diff", "cmp", "comm", "join", "paste", "expand", "unexpand", "pr", "column", "jq", "curl", "wget", "printenv", "env", "dmesg",
                                     "lscpu", "lsusb", "lspci", "lsmod", "lsof", "ss", "ip", "free", "dig", "host", "nslookup", "last", "who", "stat", "realpath",
                                     "md5sum", "sha1sum", "sha256sum", "sha512sum", "b2sum", "cksum", "shuf", "fmt", "iconv", "zstdcat", "lz4cat", "pigz", "gzip"};
    char path[64], comm[32];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pgrp);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, comm, sizeof comm - 1);
    close(fd);
    if (n <= 0) return false;
    comm[n] = 0;
    char *nl = strchr(comm, '\n');
    if (nl) *nl = 0;
    for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) if (!strcmp(comm, ok[i])) return true;
    return false;
}

static void fo_restore_kernel(TermCore *t) {
    struct termios cur;
    if (t->pty.fd >= 0 && tcgetattr(t->pty.fd, &cur) == 0) {
        cur.c_oflag = (cur.c_oflag & ~(OPOST | ONLCR)) | (t->fo.saved.c_oflag & (OPOST | ONLCR));
        tcsetattr(t->pty.fd, TCSANOW, &cur);
    }
}

static void *fo_watch_main(void *data) {
    TermCore *t = static_cast<TermCore*>(data);
    struct pollfd p = {t->fo.pidfd, POLLIN, 0};
    while (!t->stop.load() && !t->fo.cancel.load())
        if (poll(&p, 1, 100) > 0) break;
    fo_restore_kernel(t);
    t->fo.restored.store(1);
    return NULL;
}

static void fo_join(TermCore *t) {
    if (!t->fo.th_started) return;
    pthread_join(t->fo.th, NULL);
    t->fo.th_started = false;
    if (t->fo.pidfd >= 0) { close(t->fo.pidfd); t->fo.pidfd = -1; }
}

static void fo_shutdown(TermCore *t) {
    t->fo.cancel.store(1);
    fo_join(t);
    t->fo.active = false;
}

static bool fo_activate(TermCore *t, pid_t fg, const struct termios *cur, bool already_off) {
    int pfd = (int)syscall(SYS_pidfd_open, fg, 0);
    if (pfd < 0) return false;
    t->fo.saved = *cur;
    t->fo.saved.c_oflag |= OPOST | ONLCR;
    t->fo.fg = fg;
    t->fo.release_at = 0;
    t->fo.pidfd = pfd;
    t->fo.cancel.store(0);
    t->fo.restored.store(0);
    LOCK(t);
    vt_set_force_nl(t->vt, true);
    UNLOCK(t);
    if (!already_off) {
        struct termios off = *cur;
        off.c_oflag &= ~(OPOST | ONLCR);
        if (tcsetattr(t->pty.fd, TCSANOW, &off) != 0) { close(pfd); t->fo.pidfd = -1; return false; }
    }
    if (pthread_create(&t->fo.th, NULL, fo_watch_main, t) != 0) { fo_restore_kernel(t); close(pfd); t->fo.pidfd = -1; return false; }
    t->fo.th_started = true;
    t->fo.active = true;
    t->fo.suspect = true;
    return true;
}

static void fo_step(TermCore *t, double now) {
    if (!g_fast_output || t->pty.fd < 0) return;
    if (t->fo.active) {
        if (t->fo.restored.load()) {
            fo_join(t);
            t->fo.active = false;
            t->fo.release_at = now + 0.25;
            t->fo.restored.store(0);
        }
        return;
    }
    pid_t fg = tcgetpgrp(t->pty.fd);
    if (t->fo.release_at > 0 && now >= t->fo.release_at) {
        LOCK(t);
        vt_set_force_nl(t->vt, false);
        UNLOCK(t);
        t->fo.release_at = 0;
    }
    if (fg <= 0) return;
    struct termios cur;
    if (t->fo.suspect && t->fo.release_at == 0 && tcgetattr(t->pty.fd, &cur) == 0 && (cur.c_oflag & (OPOST | ONLCR)) != (OPOST | ONLCR)) {
        bool shell = fg == t->pty.pid;
        if (getenv("LX_FODEBUG")) fprintf(stderr, "fastout: kernel newline processing found off (fg %s)\n", shell ? "shell" : "child");
        if (!shell && fo_comm_ok(fg)) { fo_activate(t, fg, &cur, true); return; }
        if (shell || !fo_comm_ok(fg)) {
            cur.c_oflag |= OPOST | ONLCR;
            tcsetattr(t->pty.fd, TCSANOW, &cur);
            return;
        }
    }
    if (fg == t->pty.pid) { t->fo.last_checked = 0; return; }
    if (fg == t->fo.last_checked) return;
    t->fo.last_checked = fg;
    if (!fo_comm_ok(fg)) return;
    if (tcgetattr(t->pty.fd, &cur) != 0 || (cur.c_oflag & (OPOST | ONLCR)) != (OPOST | ONLCR)) return;
    fo_activate(t, fg, &cur, false);
}

void tcore_cmd_begin(TermCore *t, double now) {
    if (t->cmd.on || t->pty.fd < 0 || (tcore_modes(t) & VT_M_ALT_SCREEN) || tcgetpgrp(t->pty.fd) != t->pty.pid) return;
    uint64_t fed = t->bytes_fed.load();
    t->cmd.on = true; t->cmd.saw_child = false; t->cmd.alt = false; t->cmd.t_back = 0;
    t->cmd.t0 = t->cmd.t_last = now; t->cmd.fed0 = t->cmd.fed_last = fed;
}

bool tcore_cmd_active(const TermCore *t) { return t->cmd.on || t->fo.active || t->fo.release_at > 0; }

bool tcore_cmd_poll(TermCore *t, double now, double *secs, uint64_t *bytes) {
    fo_step(t, now);
    if (!t->cmd.on) return false;
    uint64_t fed = t->bytes_fed.load();
    if (fed != t->cmd.fed_last) { t->cmd.fed_last = fed; t->cmd.t_last = now; }
    if (tcore_modes(t) & VT_M_ALT_SCREEN) t->cmd.alt = true;
    pid_t fg = t->pty.fd >= 0 ? tcgetpgrp(t->pty.fd) : -1;
    if (fg != t->pty.pid && fg > 0) t->cmd.saw_child = true;
    if (fg < 0) { t->cmd.on = false; return false; }
    if (fg != t->pty.pid) { t->cmd.t_back = 0; return false; }
    double end;
    if (t->cmd.saw_child) {
        if (t->cmd.t_back == 0) t->cmd.t_back = now;
        if (now - t->cmd.t_last < 0.03) return false;
        end = t->cmd.t_back > t->cmd.t_last ? t->cmd.t_back : t->cmd.t_last;
    } else {
        if (now - t->cmd.t_last < 0.05 || now - t->cmd.t0 < 0.05) return false;
        end = t->cmd.t_last;
    }
    t->cmd.on = false;
    if (t->cmd.alt) return false;
    *secs = end - t->cmd.t0;
    *bytes = fed - t->cmd.fed0;
    return true;
}

uint64_t tcore_bytes_fed(const TermCore *t) { return t->bytes_fed.load(); }
const char *tcore_title(const TermCore *t) { return t->title; }
bool tcore_focused(const TermCore *t) { return t->focused; }
int tcore_offset(const TermCore *t) { return t->offset; }

uint32_t tcore_modes(TermCore *t) {
    LOCK(t);
    uint32_t m = vt_modes(t->vt);
    UNLOCK(t);
    return m;
}

static void wake_ui(TermCore *t) {
    if (t->hooks.wake) t->hooks.wake(t->user);
}

static void wake_worker(TermCore *t) {
    if (t->wake[1] >= 0) { char c = 1; ssize_t r = write(t->wake[1], &c, 1); (void)r; }
    if (t->io_split) {
        pthread_mutex_lock(&t->iom);
        pthread_cond_broadcast(&t->out_cv); pthread_cond_broadcast(&t->io_data); pthread_cond_broadcast(&t->io_space);
        pthread_mutex_unlock(&t->iom);
    }
}

void tcore_send(TermCore *t, const char *data, size_t len) {
    if (!len || !t->running_flag.load()) return;
    if (memchr(data, 3, len)) t->intr_seq.fetch_add(1);
    pthread_mutex_lock(&t->outlock);
    if (t->outhead + t->outlen + len > t->outcap) {
        if (t->outhead) { memmove(t->outq, t->outq + t->outhead, t->outlen); t->outhead = 0; }
        if (t->outlen + len > t->outcap) {
            size_t nc = t->outcap ? t->outcap * 2 : 1024;
            while (nc < t->outlen + len) nc *= 2;
            uint8_t *nb = static_cast<uint8_t*>(realloc(t->outq, nc));
            if (nb) { t->outq = nb; t->outcap = nc; }
        }
    }
    if (t->outlen + len <= t->outcap) { memcpy(t->outq + t->outhead + t->outlen, data, len); t->outlen += len; }
    pthread_mutex_unlock(&t->outlock);
    wake_worker(t);
}

void tcore_send_str(TermCore *t, const char *s) { tcore_send(t, s, strlen(s)); }

static void vt_write_cb(const uint8_t *d, size_t n, void *user) { tcore_send(static_cast<TermCore*>(user), (const char *)d, n); }

static void vt_event_cb(VtEvent ev, const char *text, void *user) {
    TermCore *t = static_cast<TermCore*>(user);
    PendingEv *pe = static_cast<PendingEv*>(calloc(1, sizeof *pe));
    if (!pe) return;
    pe->ev = ev;
    pe->text = text ? strdup(text) : NULL;
    pthread_mutex_lock(&t->evlock);
    if (t->ev_tail) t->ev_tail->next = pe; else t->ev_head = pe;
    t->ev_tail = pe;
    pthread_mutex_unlock(&t->evlock);
    wake_ui(t);
}

static void cache_free_cb(void *cache, void *user) { (void)user; free(cache); }

static void term_print(TermCore *t, const char *s) {
    LOCK(t);
    vt_feed(t->vt, (const uint8_t *)s, strlen(s));
    UNLOCK(t);
}

static void flush_out(TermCore *t, bool *still_pending) {
    pthread_mutex_lock(&t->outlock);
    while (t->outlen) {
        ssize_t n = sd_pty_write(&t->pty, t->outq + t->outhead, t->outlen);
        if (n > 0) { t->outhead += (size_t)n; t->outlen -= (size_t)n; if (!t->outlen) t->outhead = 0; }
        else if (errno == EINTR) continue;
        else if (errno == EAGAIN) break;
        else { t->outlen = 0; t->outhead = 0; break; }
    }
    *still_pending = t->outlen > 0;
    pthread_mutex_unlock(&t->outlock);
}


/*
 * Built-in fast `cat`. A tty moves about 30 MB/s, so `cat bigfile` and `producer | cat` are limited by the kernel, not by the
 * terminal. When the foreground job of a local tab ends in a plain `cat` that writes to this terminal, the terminal takes the
 * rest of the input over and reads it itself, then lets cat and the shell carry on. Both forms keep the shell stopped for the
 * whole hand-over so it cannot print its next prompt in the middle, and write one in-band request (the same escape request the
 * terminal already understands) into the pty after everything cat had written, which is what keeps the order exact.
 *
 *   cat FILE        stop cat for an instant, read how far into the file it is, move its file offset to the end (pidfd_getfd
 *                   gives the same open file), let it go: it finishes the chunk it holds and exits. The request names the
 *                   file and the offset; the terminal streams the rest.
 *   producer | cat  ptrace cat and run it until it is about to call read(0) (so it holds no data and has written all of it),
 *                   park it there, and let the terminal read the pipe itself (pidfd_getfd again) until end of file; then
 *                   detach cat: its read sees end of file and it exits.
 *
 * Anything unusual (options, several files, a redirect, a small file, no permission for pidfd_getfd or ptrace, cat not a direct
 * child of the shell) is declined before anything is stopped, so those run through the pty as before. Ctrl-C ends the job as it
 * would with a plain cat.
 */
enum { NC_MIN_FILE = 1 << 20, NC_MIN_REST = 256 << 10, NC_FLAG_NL = 2, NC_FLAG_PIPE = 4 };

typedef struct { TermCore *t; pid_t cat; bool pipe; char path[4096]; int fdnum; unsigned intr0; } NcJob;

static bool nc_read(const char *path, char *buf, size_t cap, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return false;
    buf[n] = 0;
    *len = (size_t)n;
    return true;
}

/* state letter, parent and process group of a process from /proc/PID/stat, and optionally its exit code (zombies) */
static bool nc_stat(pid_t pid, char *state, pid_t *ppid, pid_t *pgrp, long *exit_code) {
    char path[48], buf[1024];
    size_t n;
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    if (!nc_read(path, buf, sizeof buf, &n)) return false;
    char *rp = strrchr(buf, ')');
    if (!rp) return false;
    int pp = 0, pg = 0;
    if (sscanf(rp + 2, "%c %d %d", state, &pp, &pg) != 3) return false;
    if (ppid) *ppid = pp;
    if (pgrp) *pgrp = pg;
    if (exit_code) {
        *exit_code = 0;
        const char *f = rp + 2;
        for (int field = 3; field < 52 && f; field++) { f = strchr(f, ' '); if (f) f++; }
        if (f) *exit_code = strtol(f, NULL, 10);
    }
    return true;
}

static bool nc_wait_stopped(pid_t pid) {
    for (int i = 0; i < 100; i++) {
        char st; pid_t pp;
        if (nc_stat(pid, &st, &pp, NULL, NULL) && (st == 'T' || st == 't')) return true;
        struct timespec ts = {0, 500000};
        nanosleep(&ts, NULL);
    }
    return false;
}

/* Is `pid` a plain cat of ours, writing to this terminal? Fills the job for the file form (`cat FILE`) or the pipe form (`cat`, `cat -`). */
static bool nc_examine(TermCore *t, pid_t pid, const struct stat *slave, NcJob *j) {
    char path[96], buf[8192], comm[32];
    size_t n;
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    if (!nc_read(path, comm, sizeof comm, &n) || strcmp(comm, "cat\n") != 0) return false;
    char st; pid_t pp;
    if (!nc_stat(pid, &st, &pp, NULL, NULL) || pp != t->pty.pid) return false;
    struct stat so;
    snprintf(path, sizeof path, "/proc/%d/fd/1", (int)pid);
    if (stat(path, &so) != 0 || !S_ISCHR(so.st_mode) || so.st_rdev != slave->st_rdev) return false;
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    if (!nc_read(path, buf, sizeof buf, &n)) return false;
    const char *argv0 = buf, *arg1 = argv0 + strlen(argv0) + 1;
    j->t = t; j->cat = pid; j->intr0 = t->intr_seq.load();
    if ((size_t)(arg1 - buf) >= n) {  /* no argument: reads its standard input */
        j->pipe = true;
    } else {
        const char *end = arg1 + strlen(arg1) + 1;
        if ((size_t)(end - buf) != n || !*arg1) return false;
        j->pipe = !strcmp(arg1, "-");
        if (!j->pipe && arg1[0] == '-') return false;
        if (!j->pipe) {
            struct stat sf, sn;
            char full[8192 + 128];
            if (arg1[0] == '/') snprintf(full, sizeof full, "%s", arg1);
            else {
                char cwd[4096];
                snprintf(path, sizeof path, "/proc/%d/cwd", (int)pid);
                ssize_t l = readlink(path, cwd, sizeof cwd - 1);
                if (l <= 0) return false;
                cwd[l] = 0;
                snprintf(full, sizeof full, "%s/%s", cwd, arg1);
            }
            if (!realpath(full, j->path) || stat(j->path, &sf) != 0 || !S_ISREG(sf.st_mode) || sf.st_size < NC_MIN_FILE) return false;
            snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
            DIR *d = opendir(path);
            if (!d) return false;
            j->fdnum = -1;
            while (struct dirent *e = readdir(d)) {
                int num = atoi(e->d_name);
                if (num < 3) continue;
                char fp[96];
                snprintf(fp, sizeof fp, "/proc/%d/fd/%d", (int)pid, num);
                if (stat(fp, &sn) == 0 && sn.st_dev == sf.st_dev && sn.st_ino == sf.st_ino) { j->fdnum = num; break; }
            }
            closedir(d);
            return j->fdnum >= 0;
        }
    }
    struct stat si;
    snprintf(path, sizeof path, "/proc/%d/fd/0", (int)pid);
    if (stat(path, &si) != 0 || !S_ISFIFO(si.st_mode)) return false;
    j->fdnum = 0;
    return true;
}

/* The cat that matters is the last stage of the job, not necessarily its process group leader: look through the group. */
static bool nc_find(TermCore *t, pid_t fg, NcJob *j) {
    char slave[128];
    struct stat ss;
    if (ptsname_r(t->pty.fd, slave, sizeof slave) != 0 || stat(slave, &ss) != 0) return false;
    if (nc_examine(t, fg, &ss, j)) return true;
    DIR *d = opendir("/proc");
    if (!d) return false;
    bool found = false;
    while (!found) {
        struct dirent *e = readdir(d);
        if (!e) break;
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == fg) continue;
        char st; pid_t pp, pg;
        if (!nc_stat(pid, &st, &pp, &pg, NULL) || pg != fg || pp != t->pty.pid) continue;
        found = nc_examine(t, pid, &ss, j);
    }
    closedir(d);
    return found;
}

static void nc_request(TermCore *t, int slave_fd, const char *path, uint64_t off, unsigned flags) {
    char msg[4096 * 3 + 160];
    size_t n = (size_t)snprintf(msg, sizeof msg, "\x1b]7777;cat;%s;", t->fastcat_token);
    for (const unsigned char *p = (const unsigned char *)path; *p && n < sizeof msg - 80; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '%' || *p == ';' || *p > 0x7e) n += (size_t)snprintf(msg + n, 4, "%%%02X", *p);
        else msg[n++] = (char)*p;
    }
    n += (size_t)snprintf(msg + n, 80, ";%llu;0;%u\a", (unsigned long long)off, flags);
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(slave_fd, msg + done, n - done);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;
        done += (size_t)w;
    }
}

static void nc_file_job(NcJob *j, int pidfd, int dupfd, int sfd) {
    TermCore *t = j->t;
    pid_t shell = t->pty.pid;
    bool shell_stopped = false, cat_stopped = false, handed_over = false;
    kill(shell, SIGSTOP);
    shell_stopped = nc_wait_stopped(shell);
    if (shell_stopped) {
        syscall(SYS_pidfd_send_signal, pidfd, SIGSTOP, NULL, 0);
        cat_stopped = nc_wait_stopped(j->cat);
    }
    off_t pos = -1;
    struct stat st = {};
    if (cat_stopped) {
        pos = lseek(dupfd, 0, SEEK_CUR);
        if (fstat(dupfd, &st) == 0 && pos >= 0 && st.st_size - pos >= NC_MIN_REST && lseek(dupfd, 0, SEEK_END) >= 0) handed_over = true;
    }
    if (cat_stopped) syscall(SYS_pidfd_send_signal, pidfd, SIGCONT, NULL, 0);
    if (handed_over) {
        struct pollfd p = {pidfd, POLLIN, 0};
        bool gone = false;
        while (!t->stop.load() && !(gone = poll(&p, 1, 50) > 0)) {}
        if (gone) {
            char state; pid_t pp; long code = 0;
            bool signaled = nc_stat(j->cat, &state, &pp, NULL, &code) && (code & 0x7f) != 0;  /* Ctrl-C: do not finish the file */
            if (!signaled && t->intr_seq.load() == j->intr0) {
                nc_request(t, sfd, j->path, (uint64_t)pos, NC_FLAG_NL);
                g_native_cat_count.fetch_add(1);
            }
        }
    }
    kill(shell, SIGCONT);
}

/* PTRACE_GET_SYSCALL_INFO (Linux 5.3): is the tracee stopped at the entry of read(0, ...)? */
static bool nc_at_read_entry(pid_t pid) {
    unsigned char info[128];
    memset(info, 0, sizeof info);
    long r = ptrace((enum __ptrace_request)0x420e, pid, (void *)sizeof info, info);
    if (r <= 0) return false;
    uint64_t nr, arg0;
    memcpy(&nr, info + 24, 8);
    memcpy(&arg0, info + 32, 8);
    return info[0] == 1 && nr == (uint64_t)SYS_read && arg0 == 0;
}

static void nc_pipe_job(NcJob *j, int pidfd, int dupfd, int sfd) {
    TermCore *t = j->t;
    pid_t shell = t->pty.pid, cat = j->cat;
    if (ptrace(PTRACE_SEIZE, cat, 0, (void *)(long)PTRACE_O_TRACESYSGOOD) != 0) return;
    bool parked = false, dead = false;
    int sig = 0, st = 0;
    ptrace(PTRACE_INTERRUPT, cat, 0, 0);
    if (waitpid(cat, &st, __WALL) != cat || !WIFSTOPPED(st)) { dead = !WIFSTOPPED(st); }
    double give_up = 0;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    give_up = (double)t0.tv_sec + (double)t0.tv_nsec * 1e-9 + 3.0;
    while (!dead && !parked && !t->stop.load()) {
        if (nc_at_read_entry(cat)) { parked = true; break; }
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        if ((double)t1.tv_sec + (double)t1.tv_nsec * 1e-9 > give_up) break;
        ptrace(PTRACE_SYSCALL, cat, 0, (void *)(long)sig);
        sig = 0;
        if (waitpid(cat, &st, __WALL) != cat || WIFEXITED(st) || WIFSIGNALED(st)) { dead = true; break; }
        if (WIFSTOPPED(st)) {
            int s = WSTOPSIG(st);
            if (s != (SIGTRAP | 0x80) && s != SIGTRAP && (st >> 16) == 0) sig = s;  /* a real signal (Ctrl-C): hand it on when cat resumes */
        }
    }
    if (parked && t->intr_seq.load() == j->intr0) {
        kill(shell, SIGSTOP);
        bool shell_stopped = nc_wait_stopped(shell);
        if (shell_stopped) {
            t->nc.pipe_fd.store(dupfd);
            t->nc.stream_done.store(0);
            nc_request(t, sfd, "/", 0, NC_FLAG_NL | NC_FLAG_PIPE);
            while (!t->stop.load() && !t->nc.stream_done.load()) { struct timespec ts = {0, 2000000}; nanosleep(&ts, NULL); }
            if (t->nc.stream_done.load()) g_native_cat_count.fetch_add(1);
            t->nc.pipe_fd.store(-1);
        }
        sig = 0;
        for (;;) {  /* a signal that reached cat while it was parked has to be passed on, or Ctrl-C would be swallowed */
            pid_t w = waitpid(cat, &st, WNOHANG | __WALL);
            if (w != cat || !WIFSTOPPED(st)) break;
            int s = WSTOPSIG(st);
            if (s != (SIGTRAP | 0x80) && s != SIGTRAP && (st >> 16) == 0) { sig = s; ptrace(PTRACE_SYSCALL, cat, 0, (void *)(long)sig); sig = 0; }
            else break;
        }
        ptrace(PTRACE_DETACH, cat, 0, (void *)(long)sig);
        struct pollfd p = {pidfd, POLLIN, 0};
        while (!t->stop.load() && poll(&p, 1, 50) <= 0) {}
        kill(shell, SIGCONT);
        (void)dupfd;
        return;
    }
    if (!dead) {
        if (sig) ptrace(PTRACE_SYSCALL, cat, 0, (void *)(long)sig);
        ptrace(PTRACE_DETACH, cat, 0, (void *)(long)sig);
    }
}

static void *nc_main(void *arg) {
    NcJob *j = static_cast<NcJob*>(arg);
    TermCore *t = j->t;
    prctl(PR_SET_NAME, "lx-cat", 0, 0, 0);
    int pidfd = (int)syscall(SYS_pidfd_open, j->cat, 0);
    int dupfd = pidfd >= 0 ? (int)syscall(SYS_pidfd_getfd, pidfd, j->fdnum, 0) : -1;
    char slave[128];
    int sfd = dupfd >= 0 && ptsname_r(t->pty.fd, slave, sizeof slave) == 0 ? open(slave, O_WRONLY | O_NOCTTY | O_CLOEXEC) : -1;
    if (sfd >= 0) {
        if (j->pipe) nc_pipe_job(j, pidfd, dupfd, sfd);
        else nc_file_job(j, pidfd, dupfd, sfd);
    }
    if (sfd >= 0) close(sfd);
    if (dupfd >= 0) close(dupfd);
    if (pidfd >= 0) close(pidfd);
    free(j);
    t->nc.busy.store(0);
    return NULL;
}

static void nc_join(TermCore *t) {
    if (!t->nc.started) return;
    pthread_join(t->nc.th, NULL);
    t->nc.started = false;
}

/* Called by the reader after a sizeable read: is the foreground job one that ends in a plain `cat`? */
static void nc_probe(TermCore *t) {
    if (!t->fastcat_token[0] || !g_native_cat.load() || t->nc.busy.load() || t->pty.fd < 0) return;
    pid_t fg = tcgetpgrp(t->pty.fd);
    if (fg <= 0 || fg == t->pty.pid) { t->nc.last = 0; return; }
    if (fg == t->nc.last) return;
    t->nc.last = fg;
    NcJob *j = static_cast<NcJob*>(calloc(1, sizeof *j));
    if (!j) return;
    if (!nc_find(t, fg, j)) { free(j); return; }
    nc_join(t);
    t->nc.busy.store(1);
    if (pthread_create(&t->nc.th, NULL, nc_main, j) != 0) { t->nc.busy.store(0); free(j); return; }
    t->nc.started = true;
}

static void feed_sliced(TermCore *t, const uint8_t *p, size_t n, bool notify) {
    const size_t SLICE = notify ? 512 * 1024 : 64 * 1024;
    while (n) {
        size_t k = n < SLICE ? n : SLICE;
        LOCK(t);
        vt_feed(t->vt, p, k);
        UNLOCK(t);
        p += k; n -= k;
        if (notify) { t->bytes_fed.fetch_add(k); t->act_flag.store(1); wake_ui(t); }
        for (int spin = 0; t->ui_waiting.load(std::memory_order_relaxed) > 0 && spin < 2000; spin++) sched_yield();
    }
}

/* The pipe form of the built-in cat: read the pipe cat was parked on until every writer has closed it. */
static void stream_pipe(TermCore *t) {
    int fd = t->nc.pipe_fd.load();
    if (fd >= 0) {
        enum { CH = 1 << 20 };
        uint8_t *in = static_cast<uint8_t*>(malloc(CH));
        if (in) {
            const unsigned intr0 = t->intr_seq.load();
            LOCK(t); vt_set_stream_nl(t->vt, true); UNLOCK(t);
            while (!t->stop.load() && t->intr_seq.load() == intr0) {
                struct pollfd p = {fd, POLLIN, 0};
                int pr = poll(&p, 1, 50);
                if (pr == 0) continue;
                if (pr < 0 && errno == EINTR) continue;
                ssize_t n = read(fd, in, CH);
                if (n > 0) feed_sliced(t, in, (size_t)n, true);
                else if (n == 0 || (errno != EINTR && errno != EAGAIN)) break;
            }
            LOCK(t); vt_set_stream_nl(t->vt, false); UNLOCK(t);
            free(in);
        }
    }
    t->nc.stream_done.store(1);
}

static void stream_file(TermCore *t, const char *path, uint64_t off, uint64_t len, unsigned flags) {
    if (flags & NC_FLAG_PIPE) { stream_pipe(t); return; }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return; }
    enum { CH = 1 << 20 };
    uint8_t *in = static_cast<uint8_t*>(malloc(CH));
    if (in) {
        uint64_t done = 0;
        const unsigned intr0 = t->intr_seq.load();
        if (flags & 2) { LOCK(t); vt_set_stream_nl(t->vt, true); UNLOCK(t); }
        while (!t->stop.load() && t->intr_seq.load() == intr0 && (len == 0 || done < len)) {
            size_t want = len ? (len - done < CH ? (size_t)(len - done) : (size_t)CH) : (size_t)CH;
            ssize_t n = pread(fd, in, want, (off_t)(off + done));
            if (n <= 0) break;
            feed_sliced(t, in, (size_t)n, true);
            done += (uint64_t)n;
        }
        if (flags & 2) { LOCK(t); vt_set_stream_nl(t->vt, false); UNLOCK(t); }
    }
    free(in);
    if ((flags & 1) && !strncmp(path, "/dev/shm/lxcat-", 15) && st.st_uid == getuid()) unlink(path);
    close(fd);
}

static void feed_output(TermCore *t, const uint8_t *p, size_t n) {
    while (n) {
        char path[4097]; uint64_t off, len; unsigned flags;
        LOCK(t);
        size_t used = vt_feed_stream(t->vt, p, n);
        bool req = vt_take_stream(t->vt, path, sizeof path, &off, &len, &flags);
        UNLOCK(t);
        p += used; n -= used;
        if (req) stream_file(t, path, off, len, flags);
        else if (!used) break;
    }
}

static void *reader_main(void *data) {
    prctl(PR_SET_NAME, "lx-reader", 0, 0, 0);
    TermCore *t = static_cast<TermCore*>(data);
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    while (!t->stop.load()) {
        struct pollfd fds[2] = {{t->pty.fd, POLLIN, 0}, {t->wake[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
        if (t->stop.load()) break;
        if (fds[1].revents & POLLIN) { char junk[64]; ssize_t r = read(t->wake[0], junk, sizeof junk); (void)r; }
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        for (;;) {
            pthread_mutex_lock(&t->iom);
            while (t->io_count == 8 && !t->stop.load()) pthread_cond_wait(&t->io_space, &t->iom);
            int slot = (t->io_head + t->io_count) % 8;
            bool stop = t->stop.load();
            pthread_mutex_unlock(&t->iom);
            if (stop) return NULL;
            ssize_t n = read(t->pty.fd, t->iob[slot].data, 65536);
            if (n > 0) {
                pthread_mutex_lock(&t->iom);
                t->iob[slot].n = (size_t)n; t->io_count++;
                pthread_cond_signal(&t->io_data);
                pthread_mutex_unlock(&t->iom);
                t->reads.fetch_add(1, std::memory_order_relaxed);
                if (n >= 4096) nc_probe(t);
                if (tcore_read_delay_us > 0 && n < 1024) { struct timespec ts = {0, (long)tcore_read_delay_us * 1000}; nanosleep(&ts, NULL); }
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
            pthread_mutex_lock(&t->iom);
            t->io_eof = true;
            pthread_cond_broadcast(&t->io_data);
            pthread_mutex_unlock(&t->iom);
            return NULL;
        }
    }
    return NULL;
}

static void *writer_main(void *data) {
    prctl(PR_SET_NAME, "lx-writer", 0, 0, 0);
    TermCore *t = static_cast<TermCore*>(data);
    while (!t->stop.load()) {
        pthread_mutex_lock(&t->iom);
        pthread_mutex_lock(&t->outlock);
        bool empty = t->outlen == 0;
        pthread_mutex_unlock(&t->outlock);
        if (empty && !t->stop.load()) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 200 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&t->out_cv, &t->iom, &ts);
        }
        pthread_mutex_unlock(&t->iom);
        if (t->stop.load()) break;
        bool pending = false;
        flush_out(t, &pending);
        if (pending) { struct pollfd pf = {t->pty.fd, POLLOUT, 0}; poll(&pf, 1, 20); }
    }
    return NULL;
}

static void parser_main(TermCore *t) {
    struct timespec wt0;
    clock_gettime(CLOCK_MONOTONIC, &wt0);
    size_t unsaid = 0;
    for (;;) {
        pthread_mutex_lock(&t->iom);
        while (t->io_count == 0 && !t->io_eof && !t->stop.load()) pthread_cond_wait(&t->io_data, &t->iom);
        if (t->stop.load()) { pthread_mutex_unlock(&t->iom); return; }
        if (t->io_count == 0) { pthread_mutex_unlock(&t->iom); break; }
        int slot = t->io_head;
        pthread_mutex_unlock(&t->iom);
        if (t->fastcat_token[0]) feed_output(t, t->iob[slot].data, t->iob[slot].n);
        else feed_sliced(t, t->iob[slot].data, t->iob[slot].n, false);
        unsaid += t->iob[slot].n;
        pthread_mutex_lock(&t->iom);
        t->io_head = (t->io_head + 1) % 8; t->io_count--;
        pthread_cond_signal(&t->io_space);
        bool idle = t->io_count == 0;
        pthread_mutex_unlock(&t->iom);
        struct timespec wt1;
        clock_gettime(CLOCK_MONOTONIC, &wt1);
        if (idle || (wt1.tv_sec - wt0.tv_sec) * 1000000000L + (wt1.tv_nsec - wt0.tv_nsec) > 200000L) {
            t->bytes_fed.fetch_add(unsaid);
            t->act_flag.store(1);
            wake_ui(t);
            unsaid = 0; wt0 = wt1;
        }
    }
    if (unsaid) { t->bytes_fed.fetch_add(unsaid); t->act_flag.store(1); wake_ui(t); }
    t->eof_flag.store(1);
    wake_ui(t);
    while (!t->stop.load()) {
        int st = 0;
        pid_t r = waitpid(t->pty.pid, &st, WNOHANG);
        if (r == t->pty.pid || (r < 0 && errno != EINTR)) {
            t->exit_status = r == t->pty.pid ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)) : 0;
            t->act_flag.store(1);
            t->child_done = true;
            wake_ui(t);
            break;
        }
        struct timespec ts = {0, 20 * 1000000L};
        nanosleep(&ts, NULL);
    }
}

static void *worker_main(void *data) {
    prctl(PR_SET_NAME, "lx-io", 0, 0, 0);
    TermCore *t = static_cast<TermCore*>(data);
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    if (t->io_split) { parser_main(t); return NULL; }

    uint8_t *buf = static_cast<uint8_t*>(malloc(65536));
    bool out_pending = false;
    while (!t->stop.load()) {
        struct pollfd fds[2] = {{t->pty.fd, (short)(POLLIN | (out_pending ? POLLOUT : 0)), 0}, {t->wake[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
        if (t->stop.load()) break;
        if (fds[1].revents & POLLIN) { char junk[64]; ssize_t r = read(t->wake[0], junk, sizeof junk); (void)r; }
        flush_out(t, &out_pending);
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            size_t total = 0, unsaid = 0;
            bool eof = false;
            struct timespec wt0;
            clock_gettime(CLOCK_MONOTONIC, &wt0);
            for (;;) {
                ssize_t n = read(t->pty.fd, buf, 65536);
                if (n > 0) {
                    if (t->fastcat_token[0]) feed_output(t, buf, (size_t)n);
                    else feed_sliced(t, buf, (size_t)n, false);
                    total += (size_t)n;
                    unsaid += (size_t)n;
                    {
                        struct timespec wt1;
                        clock_gettime(CLOCK_MONOTONIC, &wt1);
                        if ((wt1.tv_sec - wt0.tv_sec) * 1000000000L + (wt1.tv_nsec - wt0.tv_nsec) > 200000L) {
                            t->bytes_fed.fetch_add(unsaid);
                            t->act_flag.store(1);
                            wake_ui(t);
                            unsaid = 0; wt0 = wt1;
                        }
                    }
                    t->reads.fetch_add(1, std::memory_order_relaxed);
                    if (n >= 4096) nc_probe(t);
                    if (total >= (1u << 20)) break;
                    if (tcore_read_delay_us > 0 && n < 1024) { struct timespec ts = {0, (long)tcore_read_delay_us * 1000}; nanosleep(&ts, NULL); }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                eof = true;
                break;
            }
            if (unsaid || total) {
                t->bytes_fed.fetch_add(unsaid);
                t->act_flag.store(1);
                wake_ui(t);
            }
            flush_out(t, &out_pending);
            if (eof) {
                t->eof_flag.store(1);
                wake_ui(t);
                while (!t->stop.load()) {
                    int st = 0;
                    pid_t r = waitpid(t->pty.pid, &st, WNOHANG);
                    if (r == t->pty.pid || (r < 0 && errno != EINTR)) {
                        t->exit_status = r == t->pty.pid ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)) : 0;
                        t->act_flag.store(1);
                        t->child_done = true;
                        wake_ui(t);
                        break;
                    }
                    poll(&fds[1], 1, 50);
                    if (fds[1].revents & POLLIN) { char junk[64]; ssize_t rr = read(t->wake[0], junk, sizeof junk); (void)rr; }
                }
                break;
            }
        }
    }
    free(buf);
    return NULL;
}

static bool spawn(TermCore *t) {
    LOCK(t);
    int cols = t->cols, rows = t->rows;
    UNLOCK(t);
    char *extra[1] = {NULL};
    if (sd_pty_spawn(&t->pty, t->argv, t->cwd, extra, cols, rows) != 0) return false;
    t->running = true;
    t->running_flag.store(1);
    t->eof = t->child_done = false;
    t->stop.store(0);
    t->eof_flag.store(0);
    if (pipe(t->wake) != 0) { t->wake[0] = t->wake[1] = -1; }
    else for (int i = 0; i < 2; i++) { fcntl(t->wake[i], F_SETFL, O_NONBLOCK); fcntl(t->wake[i], F_SETFD, FD_CLOEXEC); }
    int want_split = tcore_io_threads;
    if (want_split < 0) want_split = sysconf(_SC_NPROCESSORS_ONLN) >= 4 ? 2 : 0;
    t->io_split = false;
    if (want_split >= 2) {
        bool ok = true;
        for (int k = 0; k < 8; k++) if (!t->iob[k].data) { t->iob[k].data = static_cast<uint8_t*>(malloc(65536)); if (!t->iob[k].data) ok = false; }
        if (ok) {
            t->io_head = t->io_count = 0; t->io_eof = false;
            t->io_split = true;
            t->reader_started = pthread_create(&t->reader, NULL, reader_main, t) == 0;
            t->writer_started = pthread_create(&t->writer, NULL, writer_main, t) == 0;
            if (!t->reader_started) t->io_split = false;
        }
    }
    t->worker_started = pthread_create(&t->worker, NULL, worker_main, t) == 0;
    return true;
}

static void join_worker(TermCore *t) {
    if (!t->worker_started) return;
    t->stop.store(1);
    wake_worker(t);
    pthread_join(t->worker, NULL);
    t->worker_started = false;
    if (t->reader_started) { pthread_join(t->reader, NULL); t->reader_started = false; }
    if (t->writer_started) { pthread_join(t->writer, NULL); t->writer_started = false; }
    t->io_split = false;
    for (int i = 0; i < 2; i++) if (t->wake[i] >= 0) { close(t->wake[i]); t->wake[i] = -1; }
}

TermCore *tcore_new(char *const argv[], const char *cwd, int cols, int rows, const TermHooks *hooks, void *user, bool fastcat) {
    TermCore *t = new (std::nothrow) TermCore();
    if (!t) return NULL;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&t->lock, &at);
    pthread_mutexattr_destroy(&at);
    pthread_mutex_init(&t->outlock, NULL);
    pthread_mutex_init(&t->iom, NULL); pthread_cond_init(&t->io_data, NULL); pthread_cond_init(&t->io_space, NULL); pthread_cond_init(&t->out_cv, NULL);
    pthread_mutex_init(&t->evlock, NULL);
    if (hooks) t->hooks = *hooks;
    t->user = user;
    t->cols = cols > 0 ? cols : 80;
    t->rows = rows > 0 ? rows : 24;
    t->mouse_btn = -1;
    t->wake[0] = t->wake[1] = -1;
    t->pty.fd = -1;
    t->fo.pidfd = -1;
    int n = 0;
    while (argv && argv[n]) n++;
    t->argv = static_cast<char**>(calloc((size_t)n + 1, sizeof *t->argv));
    for (int i = 0; i < n; i++) t->argv[i] = strdup(argv[i]);
    t->cwd = cwd ? strdup(cwd) : NULL;
    t->vt = vt_new(t->cols, t->rows, default_scrollback);
    LOCK(t);
    vt_set_history(t->vt, default_scrollback, default_ram_mb << 20, default_disk_mb << 20, default_spill);
    UNLOCK(t);
    vt_set_callbacks(t->vt, vt_write_cb, vt_event_cb, cache_free_cb, t);
    if (fastcat) {
        uint8_t rnd[16];
        if (getrandom(rnd, sizeof rnd, 0) == (ssize_t)sizeof rnd) {
            for (int i = 0; i < 16; i++) snprintf(t->fastcat_token + 2 * i, 3, "%02x", rnd[i]);
            vt_set_stream_token(t->vt, t->fastcat_token);
        }
    }
    if (!spawn(t)) term_print(t, "failed to start the session\r\n");
    return t;
}

void tcore_close(TermCore *t) {
    fo_shutdown(t);
    if (t->running || t->worker_started) {
        t->running_flag.store(0);
        sd_pty_hangup(&t->pty);
        join_worker(t);
        nc_join(t);
        t->running = false;
        sd_pty_close(&t->pty);
    }
}

void tcore_free(TermCore *t) {
    if (!t) return;
    tcore_close(t);
    vt_free(t->vt);
    for (PendingEv *e = t->ev_head, *n; e; e = n) { n = e->next; free(e->text); free(e); }
    for (int i = 0; t->argv && t->argv[i]; i++) free(t->argv[i]);
    free(t->argv); free(t->cwd); free(t->title); free(t->outq);
    for (int k = 0; k < 8; k++) free(t->iob[k].data);
    pthread_mutex_destroy(&t->lock); pthread_mutex_destroy(&t->outlock); pthread_mutex_destroy(&t->evlock);
    delete t;
}

static void finish(TermCore *t) {
    if (!t->eof || !t->child_done || !t->running) return;
    join_worker(t);
    t->running = false;
    t->running_flag.store(0);
    sd_pty_close(&t->pty);
    char msg[160];
    snprintf(msg, sizeof msg, "\r\n\x1b[2m[session ended, exit code %d - press Enter to reconnect]\x1b[0m\r\n", t->exit_status);
    term_print(t, msg);
    if (t->hooks.exited) t->hooks.exited(t->user, t->exit_status);
}

bool tcore_pump(TermCore *t) {
    bool changed = false;
    pthread_mutex_lock(&t->evlock);
    PendingEv *evs = t->ev_head;
    t->ev_head = t->ev_tail = NULL;
    pthread_mutex_unlock(&t->evlock);
    while (evs) {
        PendingEv *e = evs;
        evs = e->next;
        if (e->ev == VT_EV_TITLE) { free(t->title); t->title = e->text ? strdup(e->text) : NULL; if (t->hooks.title) t->hooks.title(t->user, e->text); }
        else if (e->ev == VT_EV_CWD) { if (t->hooks.cwd) t->hooks.cwd(t->user, e->text); }
        else if (t->hooks.bell) t->hooks.bell(t->user);
        free(e->text);
        free(e);
    }
    int exp = 1;
    if (t->act_flag.compare_exchange_strong(exp, 0)) {
        if (t->offset == 0) t->sel_on = false;
        changed = true;
        if (t->hooks.activity) t->hooks.activity(t->user);
    }
    exp = 1;
    if (t->eof_flag.compare_exchange_strong(exp, 0)) { t->eof = true; changed = true; }
    if (t->eof && t->child_done) { finish(t); changed = true; }
    return changed;
}

void tcore_resize(TermCore *t, int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;
    LOCK(t);
    if (cols != t->cols || rows != t->rows) {
        t->cols = cols; t->rows = rows;
        vt_resize(t->vt, cols, rows);
        if (t->running) sd_pty_resize(&t->pty, cols, rows);
        t->sel_on = false;
        int max = vt_history_count(t->vt);
        if (t->offset > max) t->offset = max;
    }
    UNLOCK(t);
}

void tcore_set_focus(TermCore *t, bool focused) {
    if (t->focused == focused) return;
    t->focused = focused;
    if (tcore_modes(t) & VT_M_FOCUS_EVENTS) tcore_send_str(t, focused ? "\x1b[I" : "\x1b[O");
}

void tcore_scroll(TermCore *t, int delta) {
    LOCK(t);
    int max = vt_history_count(t->vt);
    UNLOCK(t);
    int o = t->offset + delta;
    t->offset = o < 0 ? 0 : o > max ? max : o;
}

void tcore_scroll_to_live(TermCore *t) { t->offset = 0; }

void tcore_set_history(TermCore *t, int lines, size_t ram_mb, size_t disk_mb, bool spill) {
    LOCK(t);
    vt_set_history(t->vt, lines, ram_mb << 20, disk_mb << 20, spill);
    int max = vt_history_count(t->vt);
    if (t->offset > max) t->offset = max;
    UNLOCK(t);
}

void tcore_history_stats(TermCore *t, VtHistoryStats *out) {
    LOCK(t);
    vt_history_stats(t->vt, out);
    UNLOCK(t);
}

size_t tcore_compact(TermCore *t) {
    LOCK(t);
    size_t freed = vt_compact(t->vt);
    UNLOCK(t);
    return freed;
}

bool tcore_has_selection(TermCore *t) { return t->sel_on; }
void tcore_clear_selection(TermCore *t) { t->sel_on = false; t->dragging = false; }

bool tcore_selection(TermCore *t, int *a_idx, int *a_col, int *b_idx, int *b_col) {
    if (!t->sel_on) return false;
    int ai = t->sel_a_idx, ac = t->sel_a_col, bi = t->sel_b_idx, bc = t->sel_b_col;
    if (ai > bi || (ai == bi && ac > bc)) { int x = ai; ai = bi; bi = x; x = ac; ac = bc; bc = x; }
    *a_idx = ai; *a_col = ac; *b_idx = bi; *b_col = bc;
    return true;
}

static void put_cell(char **out, size_t *len, size_t *cap, uint32_t cp) {
    char u[4];
    int n = cp < 0x80 ? (u[0] = (char)cp, 1) : cp < 0x800 ? (u[0] = (char)(0xC0 | cp >> 6), u[1] = (char)(0x80 | (cp & 63)), 2)
          : cp < 0x10000 ? (u[0] = (char)(0xE0 | cp >> 12), u[1] = (char)(0x80 | ((cp >> 6) & 63)), u[2] = (char)(0x80 | (cp & 63)), 3)
          : (u[0] = (char)(0xF0 | cp >> 18), u[1] = (char)(0x80 | ((cp >> 12) & 63)), u[2] = (char)(0x80 | ((cp >> 6) & 63)), u[3] = (char)(0x80 | (cp & 63)), 4);
    if (*len + 5 > *cap) { *cap = *cap ? *cap * 2 : 256; *out = static_cast<char*>(realloc(*out, *cap)); }
    memcpy(*out + *len, u, (size_t)n);
    *len += (size_t)n;
}

static char *lines_text(TermCore *t, int first, int last, int lo, int hi) {
    char *out = NULL;
    size_t len = 0, cap = 0;
    for (int idx = first; idx <= last; idx++) {
        int llen = 0;
        VtCell *l = vt_line(t->vt, idx, &llen);
        size_t start = len;
        if (l) {
            int a = idx == first ? lo : 0, b = idx == last ? hi : t->cols - 1;
            for (int x = a; x <= b && x < t->cols; x++) {
                if (x >= llen) { put_cell(&out, &len, &cap, ' '); continue; }
                if (VT_CELL_FLAGS(l[x]) & VT_F_TAIL) continue;
                uint32_t ch = VT_CELL_CH(l[x]);
                put_cell(&out, &len, &cap, ch ? ch : ' ');
                uint32_t comb = vt_comb_char(t->vt, VT_CELL_COMB(l[x]));
                if (comb) put_cell(&out, &len, &cap, comb);
            }
        }
        while (len > start && out[len - 1] == ' ') len--;
        if (idx < last) put_cell(&out, &len, &cap, '\n');
    }
    if (!out) return strdup("");
    out[len] = 0;
    return out;
}

char *tcore_selection_text(TermCore *t) {
    int ai, ac, bi, bc;
    if (!tcore_selection(t, &ai, &ac, &bi, &bc)) return NULL;
    LOCK(t);
    char *r = lines_text(t, ai, bi, ac, bc);
    UNLOCK(t);
    return r;
}

void tcore_copy(TermCore *t) {
    char *s = tcore_selection_text(t);
    if (s && *s && t->hooks.clip_set) t->hooks.clip_set(t->user, s, false);
    free(s);
}

void tcore_copy_all(TermCore *t) {
    LOCK(t);
    int hist = vt_history_count(t->vt);
    char *s = lines_text(t, -hist, t->rows - 1, 0, t->cols - 1);
    UNLOCK(t);
    size_t n = strlen(s);
    while (n && s[n - 1] == '\n') s[--n] = 0;
    if (n && t->hooks.clip_set) t->hooks.clip_set(t->user, s, false);
    free(s);
}

void tcore_paste_request(TermCore *t) { if (t->hooks.clip_request) t->hooks.clip_request(t->user, false); }

void tcore_paste(TermCore *t, const char *text) {
    if (!text || !*text) return;
    size_t n = strlen(text);
    char *g = static_cast<char*>(malloc(n + 1));
    size_t m = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        g[m++] = *p == '\n' ? '\r' : *p;
    }
    g[m] = 0;
    if (tcore_modes(t) & VT_M_BRACKETED_PASTE) {
        char *e;
        while ((e = strstr(g, "\x1b[201~"))) { memmove(e, e + 6, strlen(e + 6) + 1); m -= 6; }
        tcore_send(t, "\x1b[200~", 6);
        tcore_send(t, g, m);
        tcore_send(t, "\x1b[201~", 6);
    } else {
        tcore_send(t, g, m);
    }
    free(g);
}

bool tcore_screen_contains(TermCore *t, const char *needle) {
    char row[1024];
    LOCK(t);
    for (int y = 0; y < t->rows; y++) {
        int llen = 0;
        VtCell *l = vt_line(t->vt, y, &llen);
        int n = 0;
        for (int x = 0; l && x < t->cols && n < 1000; x++) row[n++] = (x < llen && VT_CELL_CH(l[x]) && VT_CELL_CH(l[x]) < 0x7f) ? (char)VT_CELL_CH(l[x]) : ' ';
        row[n] = 0;
        if (strstr(row, needle)) { UNLOCK(t); return true; }
    }
    UNLOCK(t);
    return false;
}

static int mod_param(int m) { return 1 + ((m & TM_SHIFT) ? 1 : 0) + ((m & TM_ALT) ? 2 : 0) + ((m & TM_CTRL) ? 4 : 0); }

static bool special_key(TermCore *t, TKey k, int mods) {
    char b[32];
    int m = mod_param(mods);
    bool app = (tcore_modes(t) & VT_M_APP_CURSOR) != 0;
    const char *final = NULL;
    switch (k) {
    case TK_UP: final = "A"; break; case TK_DOWN: final = "B"; break; case TK_RIGHT: final = "C"; break;
    case TK_LEFT: final = "D"; break; case TK_HOME: final = "H"; break; case TK_END: final = "F"; break;
    default: break;
    }
    if (final) {
        if (m > 1) snprintf(b, sizeof b, "\x1b[1;%d%s", m, final); else snprintf(b, sizeof b, app ? "\x1bO%s" : "\x1b[%s", final);
        tcore_send_str(t, b);
        return true;
    }
    int tilde = 0;
    switch (k) {
    case TK_INSERT: tilde = 2; break; case TK_DELETE: tilde = 3; break; case TK_PGUP: tilde = 5; break; case TK_PGDN: tilde = 6; break;
    case TK_F5: tilde = 15; break; case TK_F6: tilde = 17; break; case TK_F7: tilde = 18; break; case TK_F8: tilde = 19; break;
    case TK_F9: tilde = 20; break; case TK_F10: tilde = 21; break; case TK_F11: tilde = 23; break; case TK_F12: tilde = 24; break;
    default: break;
    }
    if (tilde) {
        if (m > 1) snprintf(b, sizeof b, "\x1b[%d;%d~", tilde, m); else snprintf(b, sizeof b, "\x1b[%d~", tilde);
        tcore_send_str(t, b);
        return true;
    }
    if (k >= TK_F1 && k <= TK_F4) {
        char c = (char)('P' + (k - TK_F1));
        if (m > 1) snprintf(b, sizeof b, "\x1b[1;%d%c", m, c); else snprintf(b, sizeof b, "\x1bO%c", c);
        tcore_send_str(t, b);
        return true;
    }
    return false;
}

static void utf8_encode(uint32_t cp, char *b, int *n) {
    if (cp < 0x80) { b[0] = (char)cp; *n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); *n = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); *n = 3; }
    else { b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); *n = 4; }
}

static void reconnect(TermCore *t) {
    LOCK(t);
    vt_reset(t->vt);
    vt_clear_history(t->vt);
    t->offset = 0;
    UNLOCK(t);
    if (spawn(t) && t->hooks.restarted) t->hooks.restarted(t->user);
}

void tcore_restart(TermCore *t) { if (t && !t->running) reconnect(t); }

bool tcore_key(TermCore *t, TKey key, uint32_t cp, int mods) {
    bool ctrl = mods & TM_CTRL, shift = mods & TM_SHIFT, alt = mods & TM_ALT;
    if (!t->running) {
        if (key == TK_ENTER) reconnect(t);
        return true;
    }
    if (ctrl && shift && (cp == 'c' || cp == 'C')) { tcore_copy(t); return true; }
    if ((ctrl && shift && (cp == 'v' || cp == 'V')) || (shift && key == TK_INSERT)) { tcore_paste_request(t); return true; }
    if (shift && !ctrl && (key == TK_PGUP || key == TK_PGDN)) { tcore_scroll(t, key == TK_PGUP ? t->rows - 1 : -(t->rows - 1)); return true; }
    if (t->offset) t->offset = 0;
    if (key == TK_ENTER) { tcore_send_str(t, alt ? "\x1b\r" : "\r"); return true; }
    if (key == TK_BACKSPACE) { tcore_send_str(t, alt ? "\x1b\x7f" : ctrl ? "\x08" : "\x7f"); return true; }
    if (key == TK_TAB) { tcore_send_str(t, shift ? "\x1b[Z" : "\t"); return true; }
    if (key == TK_ESCAPE) { tcore_send_str(t, "\x1b"); return true; }
    if (key != TK_NONE && special_key(t, key, mods)) return true;
    if (ctrl && !alt && cp) {
        char c;
        if (cp >= 'a' && cp <= 'z') c = (char)(cp - 'a' + 1);
        else if (cp >= 'A' && cp <= 'Z') c = (char)(cp - 'A' + 1);
        else if (cp == ' ' || cp == '@' || cp == '2') c = 0;
        else if (cp == '[') c = 0x1b; else if (cp == '\\') c = 0x1c; else if (cp == ']') c = 0x1d;
        else if (cp == '^' || cp == '6') c = 0x1e; else if (cp == '_' || cp == '/') c = 0x1f;
        else return false;
        tcore_send(t, &c, 1);
        return true;
    }
    if (alt && !ctrl && cp >= 0x20) {
        char b[8] = {0x1b};
        int n;
        utf8_encode(cp, b + 1, &n);
        tcore_send(t, b, (size_t)n + 1);
        return true;
    }
    return false;
}

void tcore_text(TermCore *t, const char *utf8) {
    if (!t->running || !utf8 || !*utf8) return;
    if (t->offset) t->offset = 0;
    tcore_send(t, utf8, strlen(utf8));
}

static bool mouse_reporting(TermCore *t, int mods) {
    return (tcore_modes(t) & (VT_M_MOUSE_BTN | VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY)) && !(mods & TM_SHIFT);
}

static void report_mouse(TermCore *t, int button, int col, int row, bool press, bool motion, int mods) {
    int code = button + (motion ? 32 : 0) + ((mods & TM_SHIFT) ? 4 : 0) + ((mods & TM_ALT) ? 8 : 0) + ((mods & TM_CTRL) ? 16 : 0);
    char b[64];
    if (tcore_modes(t) & VT_M_MOUSE_SGR) {
        snprintf(b, sizeof b, "\x1b[<%d;%d;%d%c", code, col + 1, row + 1, press ? 'M' : 'm');
        tcore_send_str(t, b);
    } else if (col < 223 && row < 223) {
        if (!press && button < 64) code = 3 + (code & ~3);
        b[0] = 0x1b; b[1] = '['; b[2] = 'M';
        b[3] = (char)(32 + code); b[4] = (char)(33 + col); b[5] = (char)(33 + row);
        tcore_send(t, b, 6);
    }
}

static bool is_word(VtCell *l, int len, int x) { if (x >= len) return false; uint32_t c = VT_CELL_CH(l[x]); return c > 0x20 && c != 0x7f; }

void tcore_mouse_button(TermCore *t, int button, bool press, int col, int row, int clicks, int mods) {
    if (col < 0) { col = 0; }
    if (col >= t->cols) { col = t->cols - 1; }
    if (row < 0) { row = 0; }
    if (row >= t->rows) { row = t->rows - 1; }
    if (press) {
        if (mouse_reporting(t, mods) && button >= 1 && button <= 3) {
            t->mouse_btn = button - 1;
            report_mouse(t, t->mouse_btn, col, row, true, false, mods);
            return;
        }
        if (button == 1) {
            int idx = row - t->offset;
            t->sel_a_idx = t->sel_b_idx = idx;
            t->sel_a_col = t->sel_b_col = col;
            t->dragging = true;
            t->sel_on = false;
            LOCK(t);
            int llen = 0;
            VtCell *l = vt_line(t->vt, idx, &llen);
            if (clicks == 2 && l) {
                int lo = col, hi = col;
                while (lo > 0 && is_word(l, llen, lo - 1)) lo--;
                while (hi < t->cols - 1 && is_word(l, llen, hi + 1)) hi++;
                t->sel_a_col = lo; t->sel_b_col = hi; t->sel_on = true;
            }
            UNLOCK(t);
            if (clicks >= 3) { t->sel_a_col = 0; t->sel_b_col = t->cols - 1; t->sel_on = true; }
        } else if (button == 2) {
            if (t->hooks.clip_request) t->hooks.clip_request(t->user, true);
        } else if (button == 3 && !mouse_reporting(t, mods)) {
            if (t->sel_on) tcore_copy(t); else tcore_paste_request(t);
        }
    } else {
        if (t->mouse_btn >= 0) { report_mouse(t, t->mouse_btn, col, row, false, false, mods); t->mouse_btn = -1; return; }
        if (t->dragging) {
            t->dragging = false;
            char *s = tcore_selection_text(t);
            if (s && *s && t->hooks.clip_set) t->hooks.clip_set(t->user, s, true);
            free(s);
        }
    }
}

void tcore_mouse_move(TermCore *t, int col, int row, int mods) {
    if (col < 0) { col = 0; }
    if (col >= t->cols) { col = t->cols - 1; }
    if (row < 0) { row = 0; }
    if (row >= t->rows) { row = t->rows - 1; }
    uint32_t m = tcore_modes(t);
    if (t->mouse_btn >= 0 && (m & (VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY))) { report_mouse(t, t->mouse_btn, col, row, true, true, mods); return; }
    if ((m & VT_M_MOUSE_ANY) && !(mods & TM_SHIFT)) { report_mouse(t, 3, col, row, true, true, mods); return; }
    if (t->dragging) {
        t->sel_b_idx = row - t->offset;
        t->sel_b_col = col;
        t->sel_on = true;
    }
}

void tcore_wheel(TermCore *t, double dy, int col, int row, int mods) {
    t->scroll_acc += dy;
    int steps = (int)t->scroll_acc;
    if (!steps) return;
    t->scroll_acc -= steps;
    int n = steps < 0 ? -steps : steps;
    bool up = steps > 0;
    uint32_t m = tcore_modes(t);
    if (mouse_reporting(t, mods)) {
        for (int i = 0; i < n; i++) report_mouse(t, up ? 64 : 65, col < 0 ? 0 : col, row < 0 ? 0 : row, true, false, mods);
    } else if (m & VT_M_ALT_SCREEN) {
        const char *k = (m & VT_M_APP_CURSOR) ? (up ? "\x1bOA" : "\x1bOB") : (up ? "\x1b[A" : "\x1b[B");
        for (int i = 0; i < n * 3; i++) tcore_send_str(t, k);
    } else {
        tcore_scroll(t, up ? 3 * n : -3 * n);
    }
}
