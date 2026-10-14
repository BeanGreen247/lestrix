/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_FLEETWM_H
#define SD_FLEETWM_H

#include <string>

struct FleetCfg {
    bool found = false;
    std::string theme = "dark";
    bool rounded = true;
    bool accent_auto = true;
    std::string accent;
    double stamp = 0;
};

void fleetwm_read(FleetCfg *out);
double fleetwm_stamp();
bool fleetwm_watch_start();  /* inotify thread; false: keep polling */
bool fleetwm_watch_take();   /* true once after the config directories changed */
const char *fleetwm_theme_name(const std::string &key);
#endif
