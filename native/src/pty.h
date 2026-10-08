/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_PTY_H
#define SD_PTY_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;
    pid_t pid;
} SdPty;

int sd_pty_spawn(SdPty *p, char *const argv[], const char *cwd, char *const extra_env[], int cols, int rows);
void sd_pty_resize(SdPty *p, int cols, int rows);
ssize_t sd_pty_write(SdPty *p, const void *buf, size_t len);
void sd_pty_hangup(SdPty *p);
void sd_pty_close(SdPty *p);

#endif
