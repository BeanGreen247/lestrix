/* xfer.h - remote file operations.
 *
 * SSH sessions ride on the terminal's own connection (OpenSSH ControlMaster), trying in order:
 *   1. sftp          browsing and transfers over the sftp subsystem
 *   2. ssh exec      if sftp is missing locally or disabled on the server: ls/mv/rm/mkdir
 *   3. scp, then ssh transfers when sftp is unavailable: legacy scp, else cat/tar streamed over ssh
 * FTP and FTPS sessions use libcurl. All calls block; run them off the UI thread. */
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
GPtrArray *sd_parse_ls(const char *text);                 /* sftp / ls -l / FTP LIST output -> SdEntry* */
char *sd_xfer_new_control_path(void);                      /* short socket path in a private /tmp dir */
void sd_xfer_remove_control_path(const char *path);

SdXfer *sd_xfer_new_ssh(const SdConn *conn, const char *control_path);
SdXfer *sd_xfer_new_ftp(const SdConn *conn, const char *password);
void sd_xfer_free(SdXfer *x);
gboolean sd_xfer_is_ftp(const SdXfer *x);
char *sd_xfer_label(SdXfer *x);                            /* "SFTP", "ssh (no SFTP on this server)", "FTP", ... */
gboolean sd_xfer_ready(SdXfer *x);                         /* ssh: master socket is up; ftp: always */
void sd_xfer_force_mode(SdXfer *x, const char *mode, const char *xfer_order); /* tests: "sftp"|"ssh", "scp,ssh" */

char *sd_xfer_home(SdXfer *x, GError **err);
GPtrArray *sd_xfer_list(SdXfer *x, const char *path, GError **err);
gboolean sd_xfer_is_dir(SdXfer *x, const char *path);
gboolean sd_xfer_mkdir(SdXfer *x, const char *path, GError **err);
gboolean sd_xfer_rename(SdXfer *x, const char *from, const char *to, GError **err);
gboolean sd_xfer_remove(SdXfer *x, const char *path, gboolean is_dir, GError **err);
gboolean sd_xfer_download(SdXfer *x, const char *remote, const char *local_dir, gboolean is_dir, GError **err);
gboolean sd_xfer_upload(SdXfer *x, const char *local, const char *remote_dir, GError **err);
char *sd_xfer_terminal_cwd(SdXfer *x);                     /* the shell's directory on a Linux server, or NULL */

#endif
