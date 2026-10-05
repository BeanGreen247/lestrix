/* fleetwm.h - follow the fleetwm window manager's design language: its theme.toml (theme, corner style, accent) and the
 * wallpaper-derived accent it writes. Read-only; Lestrix never writes fleetwm's files. */
#ifndef SD_FLEETWM_H
#define SD_FLEETWM_H
#include <stdbool.h>

typedef struct {
    bool found;               /* a theme.toml exists (user's first, then the system default) */
    char theme[64];           /* "dark" | "catppuccin" | "dracula" | "oled_black" | "light" */
    bool rounded;             /* corner_style = "rounded" */
    bool accent_auto;         /* accent = "auto": taken from the wallpaper */
    char accent[16];          /* "#rrggbb" when explicit, or the extracted one when auto and available; "" otherwise */
    double stamp;             /* newest modification time of the files read, to notice changes */
} FleetCfg;

void fleetwm_read(FleetCfg *out);
double fleetwm_stamp(void);                       /* cheap: only stats the files */
const char *fleetwm_theme_name(const char *key);   /* "dark" -> "fleetwm Dark", NULL if unknown */
#endif
