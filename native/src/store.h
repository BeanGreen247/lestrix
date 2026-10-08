/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_STORE_H
#define SD_STORE_H

#include <glib.h>

typedef struct {
    char *id, *name, *group, *host, *user, *key;
    char *x11;
    char *options;
    char *remote_command;
    char *alias;
    char *color;
    char *protocol;
    int port;
    double last_used;
} SdConn;

typedef struct {
    char *path;
    GPtrArray *conns;
    GPtrArray *live;
    GHashTable *meta;
} SdStore;

SdConn *sd_conn_new(void);
SdConn *sd_conn_copy(const SdConn *c);
void sd_conn_free(SdConn *c);
void sd_conn_set(char **field, const char *value);
gboolean sd_conn_is_live(const SdConn *c);
char *sd_conn_dest(const SdConn *c);
char **sd_conn_argv(const SdConn *c, const char *control_path);

char *sd_config_dir(void);
SdStore *sd_store_new(const char *path);
void sd_store_free(SdStore *s);
gboolean sd_store_save(SdStore *s);
GPtrArray *sd_store_all(SdStore *s);
SdConn *sd_store_get(SdStore *s, const char *id);
void sd_store_upsert(SdStore *s, const SdConn *c);
void sd_store_remove(SdStore *s, const char *id);
void sd_store_touch(SdStore *s, const char *id);
void sd_store_set_color(SdStore *s, const char *id, const char *color);
GPtrArray *sd_store_recent(SdStore *s, int limit);
void sd_store_reload_ssh_config(SdStore *s, gboolean enabled, const char *path);
int sd_store_add_imported(SdStore *s, GPtrArray *items);

GPtrArray *sd_parse_ansible_inventory(const char *path, GError **err);
GPtrArray *sd_parse_ssh_config(const char *path, gboolean live);

#endif
