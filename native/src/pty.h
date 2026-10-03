/* pty.h - run a command on a pseudo-terminal. */
#ifndef SD_PTY_H
#define SD_PTY_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;      /* master side, non-blocking */
    pid_t pid;
} SdPty;

/* argv is NULL-terminated; cwd and extra_env (NULL-terminated "K=V" array) may be NULL.
 * Returns 0 on success, -1 on failure (errno set). */
int sd_pty_spawn(SdPty *p, char *const argv[], const char *cwd, char *const extra_env[], int cols, int rows);
void sd_pty_resize(SdPty *p, int cols, int rows);
ssize_t sd_pty_write(SdPty *p, const void *buf, size_t len);
void sd_pty_hangup(SdPty *p);   /* SIGHUP the child's process group */
void sd_pty_close(SdPty *p);

#endif
