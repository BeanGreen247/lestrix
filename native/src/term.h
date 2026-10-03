/* term.h - GTK4 terminal widget: pty + terminal core + GPU-composited rendering. */
#ifndef SD_TERM_H
#define SD_TERM_H

#include <gtk/gtk.h>

#include "theme.h"
#include "vt.h"

G_BEGIN_DECLS
#define SD_TYPE_TERM (sd_term_get_type())
G_DECLARE_FINAL_TYPE(SdTerm, sd_term, SD, TERM, GtkWidget)

/* argv: NULL-terminated command line; cwd may be NULL. Signals:
 *   title-changed(char*), cwd-changed(char*), bell(), activity(), exited(int status), restarted() */
SdTerm *sd_term_new(char *const argv[], const char *cwd);
void sd_term_set_scrollback(int lines);   /* for terminals created afterwards: lines, -1 = unlimited (default 10000) */
void sd_term_set_history_defaults(int lines, size_t ram_mb, size_t disk_mb, gboolean spill);
void sd_term_set_history(SdTerm *t, int lines, size_t ram_mb, size_t disk_mb, gboolean spill);   /* live change */
void sd_term_history_stats(SdTerm *t, VtHistoryStats *out);
size_t sd_term_compact(SdTerm *t);        /* pack old scrollback and drop decoded caches; returns bytes freed */
guint64 sd_term_bytes_fed(SdTerm *t);     /* bytes the parser has consumed so far */
void sd_term_set_theme(SdTerm *t, const SdTheme *theme);
void sd_term_set_font(SdTerm *t, const char *family, int size_pt);
gboolean sd_term_is_running(SdTerm *t);
void sd_term_close(SdTerm *t);
void sd_term_send(SdTerm *t, const char *data, gsize len);
void sd_term_copy(SdTerm *t);
void sd_term_paste(SdTerm *t);
const char *sd_term_title(SdTerm *t);
G_END_DECLS

#endif
