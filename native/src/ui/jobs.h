/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_JOBS_H
#define SD_JOBS_H
#include <stdbool.h>

typedef void *(*JobWork)(void *arg);
typedef void (*JobDone)(void *arg, void *result);

void jobs_init(void (*wake)(void));
void jobs_shutdown(void);
void jobs_run(JobWork work, JobDone done, void *arg);
bool jobs_pump(void);
int jobs_pending(void);
#endif
