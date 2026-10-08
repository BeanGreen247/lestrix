/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "tcore.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/prctl.h>
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
    atomic_int stop, act_flag, eof_flag, running_flag, wake_pending;
    atomic_int ui_waiting;
    atomic_ullong bytes_fed;
    struct { bool on, saw_child, alt; double t0, t_last, t_back; uint64_t fed0, fed_last; } cmd;
    struct { bool active, suspect, th_started; struct termios saved; pid_t fg, last_checked; double release_at; pthread_t th; atomic_int restored, cancel; int pidfd; } fo;
    char fastcat_token[33];
    atomic_ullong reads;
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
    atomic_fetch_add_explicit(&t->ui_waiting, 1, memory_order_relaxed);
    pthread_mutex_lock(&t->lock);
    atomic_fetch_sub_explicit(&t->ui_waiting, 1, memory_order_relaxed);
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
const char *tcore_cat_dir = NULL;

uint64_t tcore_reads(const TermCore *t) { return atomic_load((atomic_ullong *)&t->reads); }
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
    TermCore *t = data;
    struct pollfd p = {t->fo.pidfd, POLLIN, 0};
    while (!atomic_load(&t->stop) && !atomic_load(&t->fo.cancel))
        if (poll(&p, 1, 100) > 0) break;
    fo_restore_kernel(t);
    atomic_store(&t->fo.restored, 1);
    return NULL;
}

static void fo_join(TermCore *t) {
    if (!t->fo.th_started) return;
    pthread_join(t->fo.th, NULL);
    t->fo.th_started = false;
    if (t->fo.pidfd >= 0) { close(t->fo.pidfd); t->fo.pidfd = -1; }
}

static void fo_shutdown(TermCore *t) {
    atomic_store(&t->fo.cancel, 1);
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
    atomic_store(&t->fo.cancel, 0);
    atomic_store(&t->fo.restored, 0);
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
        if (atomic_load(&t->fo.restored)) {
            fo_join(t);
            t->fo.active = false;
            t->fo.release_at = now + 0.25;
            atomic_store(&t->fo.restored, 0);
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
    uint64_t fed = atomic_load(&t->bytes_fed);
    t->cmd.on = true; t->cmd.saw_child = false; t->cmd.alt = false; t->cmd.t_back = 0;
    t->cmd.t0 = t->cmd.t_last = now; t->cmd.fed0 = t->cmd.fed_last = fed;
}

bool tcore_cmd_active(const TermCore *t) { return t->cmd.on || t->fo.active || t->fo.release_at > 0; }

bool tcore_cmd_poll(TermCore *t, double now, double *secs, uint64_t *bytes) {
    fo_step(t, now);
    if (!t->cmd.on) return false;
    uint64_t fed = atomic_load(&t->bytes_fed);
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

uint64_t tcore_bytes_fed(const TermCore *t) { return atomic_load((atomic_ullong *)&t->bytes_fed); }
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
    if (!len || !atomic_load(&t->running_flag)) return;
    pthread_mutex_lock(&t->outlock);
    if (t->outhead + t->outlen + len > t->outcap) {
        if (t->outhead) { memmove(t->outq, t->outq + t->outhead, t->outlen); t->outhead = 0; }
        if (t->outlen + len > t->outcap) {
            size_t nc = t->outcap ? t->outcap * 2 : 1024;
            while (nc < t->outlen + len) nc *= 2;
            uint8_t *nb = realloc(t->outq, nc);
            if (nb) { t->outq = nb; t->outcap = nc; }
        }
    }
    if (t->outlen + len <= t->outcap) { memcpy(t->outq + t->outhead + t->outlen, data, len); t->outlen += len; }
    pthread_mutex_unlock(&t->outlock);
    wake_worker(t);
}

void tcore_send_str(TermCore *t, const char *s) { tcore_send(t, s, strlen(s)); }

static void vt_write_cb(const uint8_t *d, size_t n, void *user) { tcore_send(user, (const char *)d, n); }

static void vt_event_cb(VtEvent ev, const char *text, void *user) {
    TermCore *t = user;
    PendingEv *pe = calloc(1, sizeof *pe);
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

static void feed_sliced(TermCore *t, const uint8_t *p, size_t n, bool notify) {
    const size_t SLICE = notify ? 512 * 1024 : 64 * 1024;
    while (n) {
        size_t k = n < SLICE ? n : SLICE;
        LOCK(t);
        vt_feed(t->vt, p, k);
        UNLOCK(t);
        p += k; n -= k;
        if (notify) { atomic_fetch_add(&t->bytes_fed, k); atomic_store(&t->act_flag, 1); wake_ui(t); }
        for (int spin = 0; atomic_load_explicit(&t->ui_waiting, memory_order_relaxed) > 0 && spin < 2000; spin++) sched_yield();
    }
}

static void stream_file(TermCore *t, const char *path, uint64_t off, uint64_t len, unsigned flags) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return; }
    enum { CH = 1 << 20 };
    uint8_t *in = malloc(CH);
    if (in) {
        uint64_t done = 0;
        if (flags & 2) { LOCK(t); vt_set_stream_nl(t->vt, true); UNLOCK(t); }
        while (!atomic_load(&t->stop) && (len == 0 || done < len)) {
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
    TermCore *t = data;
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    while (!atomic_load(&t->stop)) {
        struct pollfd fds[2] = {{t->pty.fd, POLLIN, 0}, {t->wake[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
        if (atomic_load(&t->stop)) break;
        if (fds[1].revents & POLLIN) { char junk[64]; ssize_t r = read(t->wake[0], junk, sizeof junk); (void)r; }
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        for (;;) {
            pthread_mutex_lock(&t->iom);
            while (t->io_count == 8 && !atomic_load(&t->stop)) pthread_cond_wait(&t->io_space, &t->iom);
            int slot = (t->io_head + t->io_count) % 8;
            bool stop = atomic_load(&t->stop);
            pthread_mutex_unlock(&t->iom);
            if (stop) return NULL;
            ssize_t n = read(t->pty.fd, t->iob[slot].data, 65536);
            if (n > 0) {
                pthread_mutex_lock(&t->iom);
                t->iob[slot].n = (size_t)n; t->io_count++;
                pthread_cond_signal(&t->io_data);
                pthread_mutex_unlock(&t->iom);
                atomic_fetch_add_explicit(&t->reads, 1, memory_order_relaxed);
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
    TermCore *t = data;
    while (!atomic_load(&t->stop)) {
        pthread_mutex_lock(&t->iom);
        pthread_mutex_lock(&t->outlock);
        bool empty = t->outlen == 0;
        pthread_mutex_unlock(&t->outlock);
        if (empty && !atomic_load(&t->stop)) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 200 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&t->out_cv, &t->iom, &ts);
        }
        pthread_mutex_unlock(&t->iom);
        if (atomic_load(&t->stop)) break;
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
        while (t->io_count == 0 && !t->io_eof && !atomic_load(&t->stop)) pthread_cond_wait(&t->io_data, &t->iom);
        if (atomic_load(&t->stop)) { pthread_mutex_unlock(&t->iom); return; }
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
            atomic_fetch_add(&t->bytes_fed, unsaid);
            atomic_store(&t->act_flag, 1);
            wake_ui(t);
            unsaid = 0; wt0 = wt1;
        }
    }
    if (unsaid) { atomic_fetch_add(&t->bytes_fed, unsaid); atomic_store(&t->act_flag, 1); wake_ui(t); }
    atomic_store(&t->eof_flag, 1);
    wake_ui(t);
    while (!atomic_load(&t->stop)) {
        int st = 0;
        pid_t r = waitpid(t->pty.pid, &st, WNOHANG);
        if (r == t->pty.pid || (r < 0 && errno != EINTR)) {
            t->exit_status = r == t->pty.pid ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)) : 0;
            atomic_store(&t->act_flag, 1);
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
    TermCore *t = data;
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    if (t->io_split) { parser_main(t); return NULL; }

    uint8_t *buf = malloc(65536);
    bool out_pending = false;
    while (!atomic_load(&t->stop)) {
        struct pollfd fds[2] = {{t->pty.fd, POLLIN | (out_pending ? POLLOUT : 0), 0}, {t->wake[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
        if (atomic_load(&t->stop)) break;
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
                            atomic_fetch_add(&t->bytes_fed, unsaid);
                            atomic_store(&t->act_flag, 1);
                            wake_ui(t);
                            unsaid = 0; wt0 = wt1;
                        }
                    }
                    atomic_fetch_add_explicit(&t->reads, 1, memory_order_relaxed);
                    if (total >= (1u << 20)) break;
                    if (tcore_read_delay_us > 0 && n < 1024) { struct timespec ts = {0, (long)tcore_read_delay_us * 1000}; nanosleep(&ts, NULL); }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
                eof = true;
                break;
            }
            if (unsaid || total) {
                atomic_fetch_add(&t->bytes_fed, unsaid);
                atomic_store(&t->act_flag, 1);
                wake_ui(t);
            }
            flush_out(t, &out_pending);
            if (eof) {
                atomic_store(&t->eof_flag, 1);
                wake_ui(t);
                while (!atomic_load(&t->stop)) {
                    int st = 0;
                    pid_t r = waitpid(t->pty.pid, &st, WNOHANG);
                    if (r == t->pty.pid || (r < 0 && errno != EINTR)) {
                        t->exit_status = r == t->pty.pid ? (WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)) : 0;
                        atomic_store(&t->act_flag, 1);
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
    char env_fc[64], env_path[4096], env_fn[4200];
    char *extra[4] = {NULL, NULL, NULL, NULL};
    if (t->fastcat_token[0]) {
        snprintf(env_fc, sizeof env_fc, "LESTRIX_FASTCAT=%s", t->fastcat_token);
        extra[0] = env_fc;
        if (tcore_cat_dir && *tcore_cat_dir) {
            const char *cur = getenv("PATH");
            snprintf(env_path, sizeof env_path, "PATH=%s:%s", tcore_cat_dir, cur && *cur ? cur : "/usr/local/bin:/usr/bin:/bin");
            extra[1] = env_path;
            if (!strchr(tcore_cat_dir, '\'')) { snprintf(env_fn, sizeof env_fn, "BASH_FUNC_cat%%%%=() {  '%s/cat' \"$@\"\n}", tcore_cat_dir); extra[2] = env_fn; }
        }
    }
    if (sd_pty_spawn(&t->pty, t->argv, t->cwd, extra, cols, rows) != 0) return false;
    t->running = true;
    atomic_store(&t->running_flag, 1);
    t->eof = t->child_done = false;
    atomic_store(&t->stop, 0);
    atomic_store(&t->eof_flag, 0);
    if (pipe(t->wake) != 0) { t->wake[0] = t->wake[1] = -1; }
    else for (int i = 0; i < 2; i++) { fcntl(t->wake[i], F_SETFL, O_NONBLOCK); fcntl(t->wake[i], F_SETFD, FD_CLOEXEC); }
    int want_split = tcore_io_threads;
    if (want_split < 0) want_split = sysconf(_SC_NPROCESSORS_ONLN) >= 4 ? 2 : 0;
    t->io_split = false;
    if (want_split >= 2) {
        bool ok = true;
        for (int k = 0; k < 8; k++) if (!t->iob[k].data) { t->iob[k].data = malloc(65536); if (!t->iob[k].data) ok = false; }
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
    atomic_store(&t->stop, 1);
    wake_worker(t);
    pthread_join(t->worker, NULL);
    t->worker_started = false;
    if (t->reader_started) { pthread_join(t->reader, NULL); t->reader_started = false; }
    if (t->writer_started) { pthread_join(t->writer, NULL); t->writer_started = false; }
    t->io_split = false;
    for (int i = 0; i < 2; i++) if (t->wake[i] >= 0) { close(t->wake[i]); t->wake[i] = -1; }
}

TermCore *tcore_new(char *const argv[], const char *cwd, int cols, int rows, const TermHooks *hooks, void *user, bool fastcat) {
    TermCore *t = calloc(1, sizeof *t);
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
    t->argv = calloc((size_t)n + 1, sizeof *t->argv);
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
        atomic_store(&t->running_flag, 0);
        sd_pty_hangup(&t->pty);
        join_worker(t);
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
    free(t);
}

static void finish(TermCore *t) {
    if (!t->eof || !t->child_done || !t->running) return;
    join_worker(t);
    t->running = false;
    atomic_store(&t->running_flag, 0);
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
    if (atomic_compare_exchange_strong(&t->act_flag, &exp, 0)) {
        if (t->offset == 0) t->sel_on = false;
        changed = true;
        if (t->hooks.activity) t->hooks.activity(t->user);
    }
    exp = 1;
    if (atomic_compare_exchange_strong(&t->eof_flag, &exp, 0)) { t->eof = true; changed = true; }
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
    if (*len + 5 > *cap) { *cap = *cap ? *cap * 2 : 256; *out = realloc(*out, *cap); }
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
    char *g = malloc(n + 1);
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
