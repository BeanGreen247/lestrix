#define _GNU_SOURCE
#include <stdbool.h>
#include "pty.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern char **environ;

/* The environment is built before fork(): other threads may be running, and the child must not
 * touch the allocator or other locks between fork and exec. */
static char **build_env(char *const extra[]) {
    size_t n = 0;
    while (environ && environ[n]) n++;
    size_t ne = 0;
    while (extra && extra[ne]) ne++;
    char **env = malloc((n + ne + 4) * sizeof *env);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const char *e = environ[i];
        if (!strncmp(e, "TERM=", 5) || !strncmp(e, "COLORTERM=", 10)) continue;
        bool shadowed = false;   /* an extra variable with the same name replaces the inherited one (getenv would find the first) */
        for (size_t x = 0; extra && extra[x] && !shadowed; x++) {
            const char *eq = strchr(extra[x], '=');
            if (eq && !strncmp(e, extra[x], (size_t)(eq - extra[x] + 1))) shadowed = true;
        }
        if (shadowed) continue;
        env[k++] = strdup(e);
    }
    env[k++] = strdup("TERM=xterm-256color");
    env[k++] = strdup("COLORTERM=truecolor");
    for (size_t i = 0; i < ne; i++) env[k++] = strdup(extra[i]);
    env[k] = NULL;
    return env;
}

int sd_pty_spawn(SdPty *p, char *const argv[], const char *cwd, char *const extra_env[], int cols, int rows) {
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    char **env = build_env(extra_env);
    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        for (char **e = env; *e; e++) free(*e);
        free(env);
        return -1;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        if (cwd && chdir(cwd) != 0) { /* fall back to the inherited directory */ }
        execvpe(argv[0], argv, env);
        _exit(127);
    }
    for (char **e = env; *e; e++) free(*e);
    free(env);
    int fl = fcntl(master, F_GETFL);
    fcntl(master, F_SETFL, fl | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    p->fd = master;
    p->pid = pid;
    return 0;
}

void sd_pty_resize(SdPty *p, int cols, int rows) {
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    if (p->fd >= 0) ioctl(p->fd, TIOCSWINSZ, &ws);
}

ssize_t sd_pty_write(SdPty *p, const void *buf, size_t len) {
    if (p->fd < 0) return -1;
    return write(p->fd, buf, len);
}

void sd_pty_hangup(SdPty *p) {
    if (p->pid > 0) kill(-p->pid, SIGHUP);
    if (p->pid > 0) kill(p->pid, SIGHUP);
}

void sd_pty_close(SdPty *p) {
    if (p->fd >= 0) close(p->fd);
    p->fd = -1;
}
