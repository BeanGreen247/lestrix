/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_JOBS_H
#define SD_JOBS_H

#include <functional>

/* A small pool for blocking work (ssh, sftp, ftp, port probes). `work` runs on a worker thread, `done` runs on the UI thread
 * from jobs_pump() once the work has finished. Share data between the two through the captures. */
void jobs_init(void (*wake)());
void jobs_shutdown();
void jobs_run(std::function<void()> work, std::function<void()> done);
bool jobs_pump();
int jobs_pending();
#endif
