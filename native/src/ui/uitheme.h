/* uitheme.h - colours, with no toolkit types. The same theme table as the GTK build. */
#ifndef SD_UITHEME_H
#define SD_UITHEME_H
#include <stdbool.h>
#include <stdint.h>
#include "termview.h"

typedef struct {
    const char *name;
    bool dark;
    const char *bg0, *bg1, *bg2, *ink, *accent, *term_bg, *term_fg;
    const char *ansi[16];
    const char *up, *down, *blue, *red;
} UiTheme;

extern const UiTheme UI_THEMES[];
extern const int UI_THEME_COUNT;
const UiTheme *ui_theme_find(const char *name);
uint32_t ui_rgba(const char *hex, int alpha);              /* "#rrggbb" */
uint32_t ui_mix(uint32_t a, uint32_t b, double f);          /* blend toward b */
void ui_palette(TermPalette *p, const UiTheme *t, const char *accent);
#endif
