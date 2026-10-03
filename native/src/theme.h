#ifndef SD_THEME_H
#define SD_THEME_H

#include <gdk/gdk.h>
#include <glib.h>

typedef struct {
    const char *name;
    gboolean dark;
    const char *bg0;   /* window chrome, terminal surround */
    const char *bg1;   /* sidebar / panels */
    const char *bg2;   /* raised: inputs, buttons, selected rows */
    const char *ink;   /* primary text */
    const char *accent;
    const char *term_bg, *term_fg;
    const char *ansi[16];
    const char *up, *down, *blue, *red;
} SdTheme;

extern const SdTheme SD_THEMES[];
extern const int SD_THEME_COUNT;

const SdTheme *sd_theme_find(const char *name);
/* GTK CSS for the whole application; accent may be NULL to use the theme's own */
char *sd_theme_css(const SdTheme *t, const char *accent);
/* blend colour a toward b by f (0..1), result "#rrggbb" (caller frees) */
char *sd_mix(const char *a, const char *b, double f);
void sd_color(GdkRGBA *out, const char *spec);

#endif
