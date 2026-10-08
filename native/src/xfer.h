/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_XFER_H
#define SD_XFER_H

#include <glib.h>

#include "store.h"

typedef struct {
    char *name;
    gboolean is_dir, is_link;
    gint64 size;
    char *modified, *perms;
} SdEntry;

typedef struct SdXfer SdXfer;

#define SD_XFER_ERROR (g_quark_from_static_string("sd-xfer"))

void sd_entry_free(SdEntry *e);
GPtrArray *sd_parse_ls(const char *text);
char *sd_xfer_new_control_path(void);
void sd_xfer_remove_control_path(const char *path);

SdXfer *sd_xfer_new_ssh(const SdConn *conn, const char *control_path);
SdXfer *sd_xfer_new_ftp(const SdConn *conn, const char *password);
void sd_xfer_free(SdXfer *x);
gboolean sd_xfer_is_ftp(const SdXfer *x);
char *sd_xfer_label(SdXfer *x);
gboolean sd_xfer_ready(SdXfer *x);
void sd_xfer_force_mode(SdXfer *x, const char *mode, const char *xfer_order);

char *sd_xfer_home(SdXfer *x, GError **err);
GPtrArray *sd_xfer_list(SdXfer *x, const char *path, GError **err);
gboolean sd_xfer_is_dir(SdXfer *x, const char *path);
gboolean sd_xfer_mkdir(SdXfer *x, const char *path, GError **err);
gboolean sd_xfer_rename(SdXfer *x, const char *from, const char *to, GError **err);
gboolean sd_xfer_remove(SdXfer *x, const char *path, gboolean is_dir, GError **err);
gboolean sd_xfer_download(SdXfer *x, const char *remote, const char *local_dir, gboolean is_dir, GError **err);
gboolean sd_xfer_upload(SdXfer *x, const char *local, const char *remote_dir, GError **err);
char *sd_xfer_terminal_cwd(SdXfer *x);

#endif
