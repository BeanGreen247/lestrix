/* files.h - remote file browser panel (SFTP / ssh fallbacks / FTP). */
#ifndef SD_FILES_H
#define SD_FILES_H

#include <gtk/gtk.h>

#include "xfer.h"

typedef struct SdFiles SdFiles;

/* takes ownership of xfer; follow = initial state of "Follow terminal path" (ssh only) */
SdFiles *sd_files_new(SdXfer *xfer, gboolean follow, GtkWindow *parent);
GtkWidget *sd_files_widget(SdFiles *f);
void sd_files_set_terminal_cwd(SdFiles *f, const char *path);   /* OSC 7 from the shell */
void sd_files_note_activity(SdFiles *f);
GtkCheckButton *sd_files_follow_check(SdFiles *f);
void sd_files_navigate(SdFiles *f, const char *path);           /* NULL = home */
const char *sd_files_path(SdFiles *f);
SdXfer *sd_files_xfer(SdFiles *f);

#endif
