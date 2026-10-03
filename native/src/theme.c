#include "theme.h"

#include <stdio.h>

const SdTheme SD_THEMES[] = {
    {"Xylonic Dark", TRUE, "#121212", "#1e1e1e", "#2a2a2a", "#ffffff", "#00bcd4", "#121212", "#e6e6e6",
     {"#2a2a2a", "#ef5350", "#66bb6a", "#ffca28", "#42a5f5", "#ba68c8", "#26c6da", "#cfd8dc",
      "#616161", "#ff7b78", "#8bd18d", "#ffd95a", "#7cc0ff", "#d28be0", "#5ce1ef", "#ffffff"},
     "#3fb68b", "#e5534b", "#4aa3ff", "#ef4b5b"},
    {"Xylonic Light", FALSE, "#f7f7f8", "#efeff1", "#ffffff", "#1b1b1f", "#0097a7", "#fbfbfc", "#24272b",
     {"#24272b", "#c62828", "#2e7d32", "#9a6a00", "#1565c0", "#8e24aa", "#00838f", "#b0b5ba",
      "#6b7076", "#e53935", "#43a047", "#b8860b", "#1e88e5", "#ab47bc", "#00acc1", "#d4d7da"},
     "#2e9e6b", "#d0453d", "#1e78d6", "#d93a4a"},
    {"Graphite", TRUE, "#171615", "#201f1d", "#2c2a27", "#ece8e1", "#e0a43a", "#171615", "#dcd7cf",
     {"#2c2a27", "#e0605a", "#9bbd6a", "#e0a43a", "#6fa3d6", "#c28ab8", "#6cbfb0", "#c9c3b8",
      "#6b665e", "#f08a84", "#b5d68a", "#f0c060", "#92bde8", "#d8a6cf", "#8fd6c8", "#f2eee7"},
     "#3fb68b", "#e5534b", "#4aa3ff", "#ef4b5b"},
    {"Nord", TRUE, "#2b303b", "#323845", "#3b4252", "#eceff4", "#88c0d0", "#2e3440", "#d8dee9",
     {"#3b4252", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
      "#4c566a", "#d08770", "#b5d19c", "#f0d9a0", "#94b3d3", "#c4a0be", "#9fd0de", "#eceff4"},
     "#3fb68b", "#e5534b", "#4aa3ff", "#ef4b5b"},
    {"Gruvbox", TRUE, "#1d2021", "#282828", "#3c3836", "#ebdbb2", "#d79921", "#282828", "#ebdbb2",
     {"#3c3836", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#a89984",
      "#928374", "#fb4934", "#b8bb26", "#fabd2f", "#83a598", "#d3869b", "#8ec07c", "#ebdbb2"},
     "#98971a", "#cc241d", "#83a598", "#fb4934"},
    {"Solarized Dark", TRUE, "#00212b", "#002b36", "#073642", "#eee8d5", "#2aa198", "#002b36", "#93a1a1",
     {"#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#586e75", "#cb4b16", "#93a1a1", "#a58a2a", "#839496", "#6c71c4", "#35b8ae", "#fdf6e3"},
     "#859900", "#dc322f", "#268bd2", "#dc322f"},
};
const int SD_THEME_COUNT = sizeof SD_THEMES / sizeof *SD_THEMES;

const SdTheme *sd_theme_find(const char *name) {
    for (int i = 0; name && i < SD_THEME_COUNT; i++)
        if (g_str_equal(SD_THEMES[i].name, name)) return &SD_THEMES[i];
    return &SD_THEMES[0];
}

void sd_color(GdkRGBA *out, const char *spec) {
    if (!gdk_rgba_parse(out, spec)) gdk_rgba_parse(out, "#ff00ff");
}

char *sd_mix(const char *a, const char *b, double f) {
    GdkRGBA ca, cb;
    sd_color(&ca, a);
    sd_color(&cb, b);
    return g_strdup_printf("#%02x%02x%02x", (int)((ca.red + (cb.red - ca.red) * f) * 255 + 0.5),
                           (int)((ca.green + (cb.green - ca.green) * f) * 255 + 0.5),
                           (int)((ca.blue + (cb.blue - ca.blue) * f) * 255 + 0.5));
}

static gboolean light_colour(const char *spec) {
    GdkRGBA c;
    sd_color(&c, spec);
    return (0.299 * c.red + 0.587 * c.green + 0.114 * c.blue) > 0.47;
}

char *sd_theme_css(const SdTheme *t, const char *accent_override) {
    const char *accent = accent_override && *accent_override ? accent_override : t->accent;
    char *text2 = sd_mix(t->bg1, t->ink, 0.70), *muted = sd_mix(t->bg1, t->ink, 0.48);
    char *border = sd_mix(t->bg1, t->ink, 0.14), *border2 = sd_mix(t->bg1, t->ink, 0.28);
    char *hover = sd_mix(t->bg1, t->ink, 0.06);
    const char *on_accent = light_colour(accent) ? "#101214" : "#ffffff";
    char *css = g_strdup_printf(
        "window, .background { background: %1$s; color: %4$s; }\n"
        "* { font-size: 13px; }\n"
        ".sd-sidebar { background: %2$s; }\n"
        ".sd-sidebar notebook > header { background: %2$s; border: none; }\n"
        ".sd-sidebar notebook > header > tabs > tab { padding: 8px 14px; min-width: 0; color: %9$s; border: none;\n"
        "    border-bottom: 2px solid transparent; font-weight: 700; font-size: 11px; letter-spacing: 1px; }\n"
        ".sd-sidebar notebook > header > tabs > tab { background: transparent; }\n"
        ".sd-sidebar notebook > header > tabs > tab:checked { color: %4$s; border-bottom-color: %5$s; background: transparent; }\n"
        ".sd-sidebar notebook > stack { background: %2$s; }\n"
        "entry, spinbutton, combobox button { background: %3$s; color: %4$s; border: 1px solid %7$s; border-radius: 6px;\n"
        "    padding: 4px 8px; box-shadow: none; min-height: 22px; }\n"
        "entry:focus-within { border-color: %5$s; }\n"
        "button { background: %3$s; color: %4$s; border: 1px solid %7$s; border-radius: 6px; padding: 4px 12px;\n"
        "    box-shadow: none; background-image: none; }\n"
        "button:hover { background: %10$s; border-color: %8$s; }\n"
        "button.suggested-action { background: %5$s; color: %6$s; border-color: %5$s; }\n"
        "button.sd-menuitem { color: %4$s; padding: 5px 10px; }\n"
        "button.flat, button.sd-tabclose { background: transparent; border-color: transparent; padding: 0 5px; color: %9$s; }\n"
        "button.sd-tabclose:hover { color: %11$s; background: %10$s; }\n"
        "list, listview, .sd-list { background: %2$s; color: %4$s; }\n"
        "row, listview > row { padding: 0; border-radius: 6px; margin: 0 4px; }\n"
        "row:hover { background: %10$s; }\n"
        "row:selected { background: %3$s; color: %4$s; }\n"
        "row.sd-group { background: transparent; margin: 8px 0 0 0; }\n"
        ".sd-group-label { color: %9$s; font-size: 11px; font-weight: 700; letter-spacing: 1px; padding: 6px 12px 2px 12px; }\n"
        ".sd-dot { font-size: 9px; margin: 0 6px 0 8px; color: %9$s; }\n"
        ".sd-dot.up { color: %12$s; } .sd-dot.down { color: %11$s; }\n"
        ".sd-row-name { padding: 6px 4px; }\n"
        ".sd-muted { color: %9$s; font-size: 11px; }\n"
        ".sd-hud { background: %2$s; color: %9$s; font-family: monospace; font-size: 11px; padding: 4px 12px; border-top: 1px solid %7$s; }\n"
        ".sd-status { color: %9$s; font-size: 11px; padding: 2px 8px; }\n"
        "notebook.sd-tabs > header { background: %1$s; border: none; padding: 0; }\n"
        "notebook.sd-tabs > header > tabs > tab { background: transparent; color: %13$s; padding: 5px 4px 5px 8px;\n"
        "    border: none; border-bottom: 2px solid transparent; margin: 0 1px 0 0; }\n"
        "notebook.sd-tabs > header > tabs > tab:checked { background: %14$s; color: %4$s; border-bottom-color: %5$s; }\n"
        "notebook.sd-tabs > header > tabs > tab:hover:not(:checked) { background: %10$s; }\n"
        "notebook.sd-tabs > stack { background: %14$s; }\n"
        ".sd-tabdot { font-size: 10px; }\n"
        ".sd-tabdot.activity { color: %15$s; } .sd-tabdot.alert { color: %16$s; }\n"
        "paned > separator { background: %7$s; min-width: 1px; min-height: 1px; }\n"
        "menubar, popover.menu contents, popover contents { background: %3$s; color: %4$s; border-radius: 6px; }\n"
        "menubar { background: %1$s; } menubar > item { padding: 4px 10px; }\n"
        "modelbutton:hover, menubar > item:hover { background: %5$s; color: %6$s; }\n"
        "scrollbar slider { background: %8$s; border-radius: 4px; min-width: 6px; min-height: 28px; }\n"
        ".sd-welcome { background: %14$s; }\n"
        ".sd-wordmark { font-size: 30px; font-weight: 800; letter-spacing: -1px; }\n"
        ".sd-section { color: %9$s; font-size: 11px; font-weight: 700; letter-spacing: 1.4px; }\n"
        "button.sd-recent { background: transparent; border: none; border-bottom: 1px solid %7$s; border-radius: 0;\n"
        "    padding: 9px 4px; }\n"
        "button.sd-recent:hover { background: %10$s; }\n"
        ".sd-key { color: %9$s; font-size: 11px; border: 1px solid %8$s; border-radius: 4px; padding: 1px 6px; }\n"
        ".sd-rname { font-size: 14px; font-weight: 700; } .sd-rhost { color: %13$s; font-size: 12px; }\n"
        "dialog, .sd-dialog { background: %1$s; }\n"
        "tooltip { background: %3$s; color: %4$s; border: 1px solid %8$s; }\n",
        t->bg0, t->bg1, t->bg2, t->ink, accent, on_accent, border, border2, muted, hover, t->down, t->up, text2,
        t->term_bg, t->blue, t->red);
    g_free(text2); g_free(muted); g_free(border); g_free(border2); g_free(hover);
    return css;
}
