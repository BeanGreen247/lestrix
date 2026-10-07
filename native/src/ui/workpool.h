/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_WORKPOOL_H
#define SD_WORKPOOL_H

void wp_configure(int workers);
int wp_workers(void);
void wp_run(void (*fn)(void *arg, int index), void *arg, int n);

#endif
