/* store.h - saved connections, JSON persistence, live ~/.ssh/config hosts, importers. */
#ifndef SD_STORE_H
#define SD_STORE_H

#include <glib.h>

typedef struct {
    char *id, *name, *group, *host, *user, *key;
    char *x11;              /* "off" | "untrusted" | "trusted" */
    char *options;          /* extra ssh arguments, shell-style */
    char *remote_command;   /* shell/command to start on the host */
    char *alias;            /* ~/.ssh/config host: connect with `ssh <alias>` */
    char *color;            /* tab colour "#rrggbb" or "" */
    char *protocol;         /* "ssh" (terminal + SFTP) | "ftp" | "ftps" (file browser only) */
    int port;
    double last_used;
} SdConn;

typedef struct {
    char *path;
    GPtrArray *conns;       /* SdConn*, saved */
    GPtrArray *live;        /* SdConn*, read from ~/.ssh/config, never saved */
    GHashTable *meta;       /* id -> SdConn* holding only color/last_used for live hosts */
} SdStore;

SdConn *sd_conn_new(void);
SdConn *sd_conn_copy(const SdConn *c);
void sd_conn_free(SdConn *c);
void sd_conn_set(char **field, const char *value);
gboolean sd_conn_is_live(const SdConn *c);
char *sd_conn_dest(const SdConn *c);                    /* user@host, host, or the alias */
/* ssh command line; control_path (may be NULL) makes this ssh the multiplexing master */
char **sd_conn_argv(const SdConn *c, const char *control_path);

char *sd_config_dir(void);
SdStore *sd_store_new(const char *path);                /* NULL = default location */
void sd_store_free(SdStore *s);
gboolean sd_store_save(SdStore *s);
GPtrArray *sd_store_all(SdStore *s);                    /* saved + live, borrowed pointers; g_ptr_array_free(x, TRUE) */
SdConn *sd_store_get(SdStore *s, const char *id);       /* borrowed */
void sd_store_upsert(SdStore *s, const SdConn *c);      /* copies */
void sd_store_remove(SdStore *s, const char *id);
void sd_store_touch(SdStore *s, const char *id);
void sd_store_set_color(SdStore *s, const char *id, const char *color);
GPtrArray *sd_store_recent(SdStore *s, int limit);      /* borrowed, newest first */
void sd_store_reload_ssh_config(SdStore *s, gboolean enabled, const char *path);
int sd_store_add_imported(SdStore *s, GPtrArray *items); /* takes ownership of items; returns number added */

/* importers (return SdConn* arrays; NULL + err on failure) */
GPtrArray *sd_parse_ansible_inventory(const char *path, GError **err);   /* INI or YAML */
GPtrArray *sd_parse_ssh_config(const char *path, gboolean live);

#endif
