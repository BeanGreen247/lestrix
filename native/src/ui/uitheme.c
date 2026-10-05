#include "uitheme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const UiTheme UI_THEMES[] = {
    {"Xylonic Dark", true, "#121212", "#1e1e1e", "#2a2a2a", "#ffffff", "#00bcd4", "#121212", "#e6e6e6",
     {"#2a2a2a", "#ef5350", "#66bb6a", "#ffca28", "#42a5f5", "#ba68c8", "#26c6da", "#cfd8dc",
      "#616161", "#ff7b78", "#8bd18d", "#ffd95a", "#7cc0ff", "#d28be0", "#5ce1ef", "#ffffff"},
     "#3fb68b", "#e5534b", "#4aa3ff", "#ef4b5b"},
    {"Xylonic Light", false, "#f7f7f8", "#efeff1", "#ffffff", "#1b1b1f", "#0097a7", "#fbfbfc", "#24272b",
     {"#24272b", "#c62828", "#2e7d32", "#9a6a00", "#1565c0", "#8e24aa", "#00838f", "#b0b5ba",
      "#6b7076", "#e53935", "#43a047", "#b8860b", "#1e88e5", "#ab47bc", "#00acc1", "#d4d7da"},
     "#2e9e6b", "#d0453d", "#1e78d6", "#d93a4a"},
    {"Graphite", true, "#171615", "#201f1d", "#2c2a27", "#ece8e1", "#e0a43a", "#171615", "#dcd7cf",
     {"#2c2a27", "#e0605a", "#9bbd6a", "#e0a43a", "#6fa3d6", "#c28ab8", "#6cbfb0", "#c9c3b8",
      "#6b665e", "#f08a84", "#b5d68a", "#f0c060", "#92bde8", "#d8a6cf", "#8fd6c8", "#f2eee7"},
     "#3fb68b", "#e5534b", "#4aa3ff", "#ef4b5b"},
    {"Nord", true, "#2b303b", "#323845", "#3b4252", "#eceff4", "#88c0d0", "#2e3440", "#d8dee9",
     {"#3b4252", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
      "#4c566a", "#d08770", "#b5d19c", "#f0d9a0", "#94b3d3", "#c4a0be", "#9fd0de", "#eceff4"},
     "#3fb68b", "#e5534b", "#4aa3ff", "#ef4b5b"},
    {"Gruvbox", true, "#1d2021", "#282828", "#3c3836", "#ebdbb2", "#d79921", "#282828", "#ebdbb2",
     {"#3c3836", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#a89984",
      "#928374", "#fb4934", "#b8bb26", "#fabd2f", "#83a598", "#d3869b", "#8ec07c", "#ebdbb2"},
     "#98971a", "#cc241d", "#83a598", "#fb4934"},
    {"Solarized Dark", true, "#00212b", "#002b36", "#073642", "#eee8d5", "#2aa198", "#002b36", "#93a1a1",
     {"#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#586e75", "#cb4b16", "#93a1a1", "#a58a2a", "#839496", "#6c71c4", "#35b8ae", "#fdf6e3"},
     "#859900", "#dc322f", "#268bd2", "#dc322f"},
    /* fleetwm's five themes: its bg_primary / bg_secondary / fg_primary / accent, with terminal palettes to match */
    {"fleetwm Dark", true, "#181825", "#1e1e2e", "#181825", "#cdd6f4", "#89b4fa", "#1e1e2e", "#cdd6f4",
     {"#45475a", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
      "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#a6adc8"},
     "#a6e3a1", "#f38ba8", "#89b4fa", "#f38ba8"},
    {"fleetwm Catppuccin", true, "#181825", "#1e1e2e", "#181825", "#cdd6f4", "#cba6f7", "#1e1e2e", "#cdd6f4",
     {"#45475a", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
      "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#a6adc8"},
     "#a6e3a1", "#f38ba8", "#89b4fa", "#f38ba8"},
    {"fleetwm Dracula", true, "#21222c", "#282a36", "#21222c", "#f8f8f2", "#bd93f9", "#282a36", "#f8f8f2",
     {"#21222c", "#ff5555", "#50fa7b", "#f1fa8c", "#bd93f9", "#ff79c6", "#8be9fd", "#f8f8f2",
      "#6272a4", "#ff6e6e", "#69ff94", "#ffffa5", "#d6acff", "#ff92df", "#a4ffff", "#ffffff"},
     "#50fa7b", "#ff5555", "#8be9fd", "#ff5555"},
    {"fleetwm OLED Black", true, "#0a0a0a", "#000000", "#0a0a0a", "#e0e0e0", "#4d9dfa", "#000000", "#e0e0e0",
     {"#1c1c1c", "#ff5f5f", "#5fd75f", "#ffd75f", "#4d9dfa", "#d787ff", "#5fd7d7", "#c0c0c0",
      "#808080", "#ff8787", "#87ff87", "#ffe787", "#87bfff", "#e7a7ff", "#87e7e7", "#ffffff"},
     "#5fd75f", "#ff5f5f", "#4d9dfa", "#ff5f5f"},
    {"fleetwm Light", false, "#e8e8e8", "#f5f5f5", "#e8e8e8", "#1a1a1a", "#3366cc", "#f5f5f5", "#1a1a1a",
     {"#1a1a1a", "#c62828", "#2e7d32", "#9a6a00", "#1565c0", "#8e24aa", "#00838f", "#b0b5ba",
      "#555555", "#e53935", "#43a047", "#b8860b", "#1e88e5", "#ab47bc", "#00acc1", "#d4d7da"},
     "#2e7d32", "#c62828", "#1565c0", "#c62828"},
};
const int UI_THEME_COUNT = sizeof UI_THEMES / sizeof *UI_THEMES;

const UiTheme *ui_theme_find(const char *name) {
    for (int i = 0; name && i < UI_THEME_COUNT; i++)
        if (strcmp(UI_THEMES[i].name, name) == 0) return &UI_THEMES[i];
    return &UI_THEMES[0];
}

uint32_t ui_rgba(const char *hex, int alpha) {
    unsigned r = 255, g = 0, b = 255;   /* a bad colour shows up as magenta */
    if (hex && *hex == '#' && strlen(hex) >= 7) sscanf(hex + 1, "%2x%2x%2x", &r, &g, &b);
    return RGBA(r, g, b, alpha);
}

uint32_t ui_mix(uint32_t a, uint32_t b, double f) {
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) {
        double x = (a >> s) & 255, y = (b >> s) & 255;
        o |= (uint32_t)(x + (y - x) * f + 0.5) << s;
    }
    return o | 0xff000000u;
}

void ui_palette(TermPalette *p, const UiTheme *t, const char *accent) {
    for (int i = 0; i < 16; i++) p->pal[i] = ui_rgba(t->ansi[i], 255);
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; i++) p->pal[16 + i] = RGBA(lv[i / 36], lv[(i / 6) % 6], lv[i % 6], 255);
    for (int i = 0; i < 24; i++) { int v = 8 + 10 * i; p->pal[232 + i] = RGBA(v, v, v, 255); }
    p->fg = ui_rgba(t->term_fg, 255);
    p->bg = ui_rgba(t->term_bg, 255);
    uint32_t acc = ui_rgba(accent && *accent ? accent : t->accent, 255);
    p->sel = (acc & 0x00ffffffu) | (102u << 24);
    p->cursor = p->fg;
    p->epoch++;
}
