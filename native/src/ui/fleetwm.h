/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_FLEETWM_H
#define SD_FLEETWM_H
#include <stdbool.h>

typedef struct {
    bool found;
    char theme[64];
    bool rounded;
    bool accent_auto;
    char accent[16];
    double stamp;
} FleetCfg;

void fleetwm_read(FleetCfg *out);
double fleetwm_stamp(void);
bool fleetwm_watch_start(void);  /* inotify thread; false: keep polling */
bool fleetwm_watch_take(void);   /* true once after the config directories changed */
const char *fleetwm_theme_name(const char *key);
#endif
