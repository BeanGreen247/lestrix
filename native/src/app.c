/* app.c - main window: sessions and files in one sidebar, terminal tabs on the right. */
#include <gtk/gtk.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>

#include "bench.h"
#include "files.h"
#include "store.h"
#include "term.h"
#include "theme.h"
#include "xfer.h"

#define APP_VERSION "0.2.1"
#define APP_ID "term.beangreen247.lestrix"

typedef struct App App;

typedef enum { TAB_LOCAL, TAB_SSH, TAB_FTP } TabKind;
typedef enum { ST_IDLE, ST_ACTIVITY, ST_BELL, ST_ENDED } TabState;

typedef struct {
    App *app;
    TabKind kind;
    GtkWidget *page;         /* notebook page: the terminal, or the file browser for FTP */
    GtkWidget *label_box, *icon, *label, *dot, *close;
    SdTerm *term;
    SdFiles *files;
    GtkWidget *files_widget; /* in the shared sidebar stack (ssh) or the page itself (ftp) */
    SdConn *conn;
    char *title, *color, *control_path;
    TabState state;
    GtkCssProvider *label_css;
    char **local_argv;
} Tab;

struct App {
    GtkApplication *gapp;
    GtkWindow *win;
    SdStore *store;
    GKeyFile *settings;
    char *settings_path;
    const SdTheme *base_theme;
    SdTheme cur_theme;
    char *accent_buf;
    int font_size;
    char *font_family;
    gboolean show_ssh_config, startup_shell, follow_default;
    int scrollback;                    /* lines; -1 = unlimited */
    size_t sb_ram_mb, sb_disk_mb;
    gboolean sb_spill, hud, lowmem;
    GtkWidget *hud_label;
    guint hud_src, trim_src;
    guint64 hud_prev_bytes, trim_prev_bytes;
    gint64 hud_prev_time;
    long hud_prev_ticks;
    gboolean trim_dirty;

    GtkWidget *paned, *side_nb, *list, *search, *files_stack, *files_hint, *files_label;
    GtkWidget *content_stack, *notebook, *welcome, *welcome_rows, *welcome_meta;
    GtkCssProvider *css;
    GHashTable *up;           /* conn id -> GINT_TO_POINTER(1 up, 2 down) */
    GHashTable *dots;         /* conn id -> GtkLabel* */
    GPtrArray *tabs;
    guint probe_src;
    GPtrArray *recent_ids;
    char *ctx_id;
    GtkWidget *ctx_popover;
};

static void refresh(App *a);
static Tab *open_connection(App *a, SdConn *c);
static Tab *open_local(App *a, const char *title, char **argv, const char *cwd);
static void edit_connection(App *a, SdConn *c, gboolean copy_of_live);

static const char *TAB_COLORS[][2] = {
    {"Red", "#e06c75"}, {"Orange", "#e59a5a"}, {"Yellow", "#e5c07b"}, {"Green", "#98c379"},
    {"Teal", "#4fb39a"}, {"Blue", "#61afef"}, {"Purple", "#c678dd"}, {"Pink", "#e88ab8"},
};

/* ---- small utilities -------------------------------------------------------------------------------- */

static char *relative_age(double ts) {
    double secs = (double)g_get_real_time() / 1e6 - ts;
    if (secs < 90) return g_strdup("just now");
    if (secs < 5400) return g_strdup_printf("%dm ago", (int)(secs / 60));
    if (secs < 129600) return g_strdup_printf("%dh ago", (int)(secs / 3600));
    return g_strdup_printf("%dd ago", (int)(secs / 86400));
}

static void set_widget_color(GtkWidget *w, GtkCssProvider **prov, const char *color) {
    if (*prov) { gtk_style_context_remove_provider(gtk_widget_get_style_context(w), GTK_STYLE_PROVIDER(*prov)); g_clear_object(prov); }
    if (color && *color) {
        char *css = g_strdup_printf("label { color: %s; }", color);
        *prov = gtk_css_provider_new();
        gtk_css_provider_load_from_string(*prov, css);
        gtk_style_context_add_provider(gtk_widget_get_style_context(w), GTK_STYLE_PROVIDER(*prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_free(css);
    }
}

typedef void (*TextCb)(const char *text, gpointer data);
typedef struct { GtkWidget *win, *entry; TextCb cb; gpointer data; } TextPrompt;

static void text_prompt_ok(GtkWidget *w, gpointer d) {
    (void)w;
    TextPrompt *p = d;
    const char *t = gtk_editable_get_text(GTK_EDITABLE(p->entry));
    if (*t) p->cb(t, p->data);
    gtk_window_destroy(GTK_WINDOW(p->win));
}
static void text_prompt_gone(GtkWidget *w, gpointer d) { (void)w; g_free(d); }

static void ask_text(GtkWindow *parent, const char *title, const char *initial, gboolean password, TextCb cb, gpointer data) {
    TextPrompt *p = g_new0(TextPrompt, 1);
    p->cb = cb; p->data = data;
    p->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(p->win), title);
    gtk_window_set_modal(GTK_WINDOW(p->win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(p->win), parent);
    gtk_window_set_default_size(GTK_WINDOW(p->win), 380, -1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    p->entry = password ? gtk_password_entry_new() : gtk_entry_new();
    if (initial && !password) gtk_editable_set_text(GTK_EDITABLE(p->entry), initial);
    GtkWidget *ok = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(ok, "clicked", G_CALLBACK(text_prompt_ok), p);
    g_signal_connect(p->entry, "activate", G_CALLBACK(text_prompt_ok), p);
    g_signal_connect(p->win, "destroy", G_CALLBACK(text_prompt_gone), p);
    gtk_box_append(GTK_BOX(box), p->entry);
    gtk_box_append(GTK_BOX(box), ok);
    gtk_window_set_child(GTK_WINDOW(p->win), box);
    gtk_window_present(GTK_WINDOW(p->win));
    gtk_widget_grab_focus(p->entry);
}

/* ---- settings -------------------------------------------------------------------------------------------- */

static void settings_load(App *a) {
    a->settings = g_key_file_new();
    char *dir = sd_config_dir();
    a->settings_path = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    g_key_file_load_from_file(a->settings, a->settings_path, G_KEY_FILE_NONE, NULL);
    char *theme = g_key_file_get_string(a->settings, "ui", "theme", NULL);
    a->base_theme = sd_theme_find(theme);
    g_free(theme);
    a->accent_buf = g_key_file_get_string(a->settings, "ui", "accent", NULL);
    a->font_size = g_key_file_has_key(a->settings, "ui", "font_size", NULL) ? g_key_file_get_integer(a->settings, "ui", "font_size", NULL) : 11;
    if (a->font_size < 7 || a->font_size > 32) a->font_size = 11;
    a->font_family = g_key_file_get_string(a->settings, "ui", "font_family", NULL);
    a->show_ssh_config = !g_key_file_has_key(a->settings, "ui", "show_ssh_config", NULL) || g_key_file_get_boolean(a->settings, "ui", "show_ssh_config", NULL);
    a->startup_shell = g_key_file_get_boolean(a->settings, "ui", "startup_shell", NULL);
    a->follow_default = !g_key_file_has_key(a->settings, "ui", "follow_path", NULL) || g_key_file_get_boolean(a->settings, "ui", "follow_path", NULL);
    a->scrollback = g_key_file_has_key(a->settings, "ui", "scrollback", NULL) ? g_key_file_get_integer(a->settings, "ui", "scrollback", NULL) : 10000;
    a->sb_ram_mb = g_key_file_has_key(a->settings, "ui", "scrollback_ram_mb", NULL) ? (size_t)g_key_file_get_integer(a->settings, "ui", "scrollback_ram_mb", NULL) : 32;
    a->sb_disk_mb = g_key_file_has_key(a->settings, "ui", "scrollback_disk_mb", NULL) ? (size_t)g_key_file_get_integer(a->settings, "ui", "scrollback_disk_mb", NULL) : 4096;
    a->sb_spill = !g_key_file_has_key(a->settings, "ui", "scrollback_spill", NULL) || g_key_file_get_boolean(a->settings, "ui", "scrollback_spill", NULL);
    a->hud = !g_key_file_has_key(a->settings, "ui", "performance_overlay", NULL) || g_key_file_get_boolean(a->settings, "ui", "performance_overlay", NULL);   /* on by default: speed is visible */
    char *renderer = g_key_file_get_string(a->settings, "ui", "renderer", NULL);
    a->lowmem = renderer && g_str_equal(renderer, "cpu");
    g_free(renderer);
}

static void settings_save(App *a) {
    g_key_file_set_string(a->settings, "ui", "theme", a->base_theme->name);
    g_key_file_set_string(a->settings, "ui", "accent", a->accent_buf ? a->accent_buf : "");
    g_key_file_set_integer(a->settings, "ui", "font_size", a->font_size);
    g_key_file_set_boolean(a->settings, "ui", "show_ssh_config", a->show_ssh_config);
    g_key_file_set_boolean(a->settings, "ui", "startup_shell", a->startup_shell);
    g_key_file_set_boolean(a->settings, "ui", "follow_path", a->follow_default);
    g_key_file_set_integer(a->settings, "ui", "scrollback", a->scrollback);
    g_key_file_set_integer(a->settings, "ui", "scrollback_ram_mb", (int)a->sb_ram_mb);
    g_key_file_set_integer(a->settings, "ui", "scrollback_disk_mb", (int)a->sb_disk_mb);
    g_key_file_set_boolean(a->settings, "ui", "scrollback_spill", a->sb_spill);
    g_key_file_set_boolean(a->settings, "ui", "performance_overlay", a->hud);
    g_key_file_set_string(a->settings, "ui", "renderer", a->lowmem ? "cpu" : "gpu");
    char *dir = g_path_get_dirname(a->settings_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(a->settings, a->settings_path, NULL);
}

static char *pick_font_family(void) {
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    PangoFontFamily **fams;
    int n;
    pango_font_map_list_families(fm, &fams, &n);
    const char *want[] = {"JetBrains Mono", "DejaVu Sans Mono", "Cascadia Mono", "Menlo", "Consolas", NULL};
    char *result = g_strdup("monospace");
    for (int w = 0; want[w]; w++) {
        for (int i = 0; i < n; i++)
            if (g_ascii_strcasecmp(pango_font_family_get_name(fams[i]), want[w]) == 0) { g_free(result); g_free(fams); return g_strdup(want[w]); }
    }
    g_free(fams);
    return result;
}

/* ---- theme ----------------------------------------------------------------------------------------------------- */

static void apply_theme(App *a) {
    a->cur_theme = *a->base_theme;
    if (a->accent_buf && *a->accent_buf) a->cur_theme.accent = a->accent_buf;
    char *css = sd_theme_css(&a->cur_theme, NULL);
    gtk_css_provider_load_from_string(a->css, css);
    g_free(css);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", a->cur_theme.dark, NULL);
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (t->term) sd_term_set_theme(t->term, &a->cur_theme);
    }
}

/* ---- tabs ---------------------------------------------------------------------------------------------------------- */

static const char *kind_icon(TabKind k) {
    return k == TAB_LOCAL ? "utilities-terminal-symbolic" : k == TAB_SSH ? "network-server-symbolic" : "folder-remote-symbolic";
}

static void tab_refresh_label(Tab *t) {
    gtk_label_set_text(GTK_LABEL(t->label), t->title);
    set_widget_color(t->label, &t->label_css, t->color);
    gtk_widget_set_visible(t->dot, t->state != ST_IDLE);
    gtk_widget_remove_css_class(t->dot, "activity");
    gtk_widget_remove_css_class(t->dot, "alert");
    if (t->state == ST_ACTIVITY) gtk_widget_add_css_class(t->dot, "activity");
    else if (t->state == ST_BELL || t->state == ST_ENDED) gtk_widget_add_css_class(t->dot, "alert");
    const char *tips[] = {"", "New output", "Bell", "Session ended"};
    gtk_widget_set_tooltip_text(t->label_box, tips[t->state]);
}

static Tab *tab_for_page(App *a, GtkWidget *page) {
    for (guint i = 0; i < a->tabs->len; i++) if (((Tab *)a->tabs->pdata[i])->page == page) return a->tabs->pdata[i];
    return NULL;
}

static Tab *current_tab(App *a) {
    int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(a->notebook));
    if (i < 0) return NULL;
    return tab_for_page(a, gtk_notebook_get_nth_page(GTK_NOTEBOOK(a->notebook), i));
}

static void sync_sidebar_files(App *a) {
    Tab *t = current_tab(a);
    if (t && t->files_widget && t->kind == TAB_SSH) {
        gtk_stack_set_visible_child(GTK_STACK(a->files_stack), t->files_widget);
        char *u = g_ascii_strup(t->title, -1);
        gtk_label_set_text(GTK_LABEL(a->files_label), u);
        g_free(u);
    } else {
        gtk_stack_set_visible_child(GTK_STACK(a->files_stack), a->files_hint);
        gtk_label_set_text(GTK_LABEL(a->files_label), "NO SSH SESSION");
    }
}

static void update_content_stack(App *a) {
    gtk_stack_set_visible_child_name(GTK_STACK(a->content_stack), gtk_notebook_get_n_pages(GTK_NOTEBOOK(a->notebook)) ? "tabs" : "welcome");
    if (!gtk_notebook_get_n_pages(GTK_NOTEBOOK(a->notebook))) gtk_widget_grab_focus(a->welcome);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint idx, gpointer data) {
    (void)nb; (void)idx;
    App *a = data;
    Tab *t = tab_for_page(a, page);
    if (!t) return;
    if (t->state == ST_ACTIVITY || t->state == ST_BELL) { t->state = ST_IDLE; tab_refresh_label(t); }
    if (t->term) gtk_widget_grab_focus(GTK_WIDGET(t->term));
    /* the stack shows the new tab's files after the switch completes */
    g_idle_add_once((GSourceOnceFunc)sync_sidebar_files, a);
}

static void on_term_activity(SdTerm *term, gpointer data) {
    (void)term;
    Tab *t = data;
    if (t->files) sd_files_note_activity(t->files);
    if (current_tab(t->app) != t && t->state == ST_IDLE) { t->state = ST_ACTIVITY; tab_refresh_label(t); }
}

static void on_term_bell(SdTerm *term, gpointer data) {
    (void)term;
    Tab *t = data;
    if (current_tab(t->app) != t && (t->state == ST_IDLE || t->state == ST_ACTIVITY)) { t->state = ST_BELL; tab_refresh_label(t); }
}

static void on_term_exited(SdTerm *term, int status, gpointer data) {
    (void)term; (void)status;
    Tab *t = data;
    t->state = ST_ENDED;
    tab_refresh_label(t);
}

static void on_term_restarted(SdTerm *term, gpointer data) {
    (void)term;
    Tab *t = data;
    t->state = ST_IDLE;
    tab_refresh_label(t);
}

static void on_term_cwd(SdTerm *term, const char *path, gpointer data) {
    (void)term;
    Tab *t = data;
    if (t->files) sd_files_set_terminal_cwd(t->files, path);
}

static void tab_free(Tab *t) {
    g_free(t->title); g_free(t->color); g_free(t->control_path);
    if (t->conn) sd_conn_free(t->conn);
    if (t->label_css) g_object_unref(t->label_css);
    g_strfreev(t->local_argv);
    g_free(t);
}

static void tab_destroy(Tab *t) {
    App *a = t->app;
    if (t->term) sd_term_close(t->term);
    gboolean ssh_files = t->files_widget && t->kind == TAB_SSH;
    if (ssh_files) gtk_stack_remove(GTK_STACK(a->files_stack), t->files_widget);
    int idx = gtk_notebook_page_num(GTK_NOTEBOOK(a->notebook), t->page);
    if (idx >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(a->notebook), idx);
    if (t->control_path) sd_xfer_remove_control_path(t->control_path);
    g_ptr_array_remove(a->tabs, t);
    tab_free(t);
    update_content_stack(a);
    sync_sidebar_files(a);
}

static void close_confirm(GObject *src, GAsyncResult *res, gpointer data) {
    Tab *t = data;
    if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL) == 1) tab_destroy(t);
}

static void close_tab(Tab *t) {
    if (t->term && sd_term_is_running(t->term)) {
        GtkAlertDialog *d = gtk_alert_dialog_new("This session is still running. Close it?");
        const char *btn[] = {"Cancel", "Close", NULL};
        gtk_alert_dialog_set_buttons(d, btn);
        gtk_alert_dialog_set_cancel_button(d, 0);
        gtk_alert_dialog_set_default_button(d, 0);
        gtk_alert_dialog_choose(d, t->app->win, NULL, close_confirm, t);
        g_object_unref(d);
    } else {
        tab_destroy(t);
    }
}

static void on_close_clicked(GtkButton *b, gpointer data) { (void)b; close_tab(data); }

static void rename_done(const char *text, gpointer data) {
    Tab *t = data;
    g_free(t->title);
    t->title = g_strdup(text);
    tab_refresh_label(t);
    sync_sidebar_files(t->app);
}

static void set_tab_color(Tab *t, const char *color) {
    g_free(t->color);
    t->color = g_strdup(color ? color : "");
    if (t->conn) { sd_store_set_color(t->app->store, t->conn->id, t->color); refresh(t->app); }
    tab_refresh_label(t);
}

static void on_color_pick(GtkButton *b, gpointer data) {
    Tab *t = g_object_get_data(G_OBJECT(b), "tab");
    set_tab_color(t, data);
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}

static void custom_color_done(GObject *src, GAsyncResult *res, gpointer data) {
    Tab *t = data;
    GdkRGBA *c = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(src), res, NULL);
    if (c) {
        char *hex = g_strdup_printf("#%02x%02x%02x", (int)(c->red * 255 + 0.5), (int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
        set_tab_color(t, hex);
        g_free(hex);
        gdk_rgba_free(c);
    }
}

static void on_color_custom(GtkButton *b, gpointer data) {
    Tab *t = data;
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    GtkColorDialog *d = gtk_color_dialog_new();
    gtk_color_dialog_choose_rgba(d, t->app->win, NULL, NULL, custom_color_done, t);
    g_object_unref(d);
}

static void tab_context_action(GtkButton *b, gpointer data) {
    Tab *t = data;
    const char *act = g_object_get_data(G_OBJECT(b), "act");
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
    App *a = t->app;
    if (g_str_equal(act, "rename")) ask_text(a->win, "Rename tab", t->title, FALSE, rename_done, t);
    else if (g_str_equal(act, "default-color")) set_tab_color(t, "");
    else if (g_str_equal(act, "duplicate")) {
        if (t->conn) open_connection(a, t->conn);
        else if (t->local_argv) open_local(a, t->title, t->local_argv, NULL);
    } else if (g_str_equal(act, "close")) close_tab(t);
    else if (g_str_equal(act, "close-others")) {
        GPtrArray *others = g_ptr_array_new();
        for (guint i = 0; i < a->tabs->len; i++) if (a->tabs->pdata[i] != t) g_ptr_array_add(others, a->tabs->pdata[i]);
        for (guint i = 0; i < others->len; i++) close_tab(others->pdata[i]);
        g_ptr_array_free(others, TRUE);
    }
}

static GtkWidget *menu_button(const char *text, const char *act, Tab *t) {
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_add_css_class(b, "sd-menuitem");
    gtk_widget_set_halign(b, GTK_ALIGN_FILL);
    GtkWidget *l = gtk_button_get_child(GTK_BUTTON(b));
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    g_object_set_data_full(G_OBJECT(b), "act", g_strdup(act), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(tab_context_action), t);
    return b;
}

static void show_tab_menu(Tab *t, double x, double y) {
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_set_parent(pop, t->label_box);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(box), menu_button("Rename…", "rename", t));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_margin_start(grid, 8); gtk_widget_set_margin_top(grid, 4); gtk_widget_set_margin_bottom(grid, 4);
    for (guint i = 0; i < G_N_ELEMENTS(TAB_COLORS); i++) {
        GtkWidget *sw = gtk_button_new();
        gtk_widget_set_size_request(sw, 22, 22);
        gtk_widget_set_tooltip_text(sw, TAB_COLORS[i][0]);
        GtkCssProvider *p = gtk_css_provider_new();
        char *css = g_strdup_printf("button { background: %s; min-width: 22px; min-height: 22px; padding: 0; border-radius: 11px; }", TAB_COLORS[i][1]);
        gtk_css_provider_load_from_string(p, css);
        gtk_style_context_add_provider(gtk_widget_get_style_context(sw), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
        g_object_unref(p); g_free(css);
        g_object_set_data(G_OBJECT(sw), "tab", t);
        g_signal_connect(sw, "clicked", G_CALLBACK(on_color_pick), (gpointer)TAB_COLORS[i][1]);
        gtk_grid_attach(GTK_GRID(grid), sw, (int)(i % 4), (int)(i / 4), 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    GtkWidget *custom = gtk_button_new_with_label("Custom color…");
    gtk_widget_add_css_class(custom, "flat");
    gtk_widget_add_css_class(custom, "sd-menuitem");
    gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(custom))), 0);
    g_signal_connect(custom, "clicked", G_CALLBACK(on_color_custom), t);
    gtk_box_append(GTK_BOX(box), custom);
    gtk_box_append(GTK_BOX(box), menu_button("Default color", "default-color", t));
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), menu_button("Duplicate tab", "duplicate", t));
    gtk_box_append(GTK_BOX(box), menu_button("Close", "close", t));
    gtk_box_append(GTK_BOX(box), menu_button("Close other tabs", "close-others", t));
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", G_CALLBACK(gtk_widget_unparent), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

static void on_tab_pressed(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    Tab *t = data;
    guint btn = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (btn == 3) show_tab_menu(t, x, y);
    else if (btn == 1 && n == 2) ask_text(t->app->win, "Rename tab", t->title, FALSE, rename_done, t);
    else if (btn == 2) close_tab(t);
}

static Tab *tab_add(App *a, TabKind kind, const char *title, GtkWidget *page) {
    Tab *t = g_new0(Tab, 1);
    t->app = a; t->kind = kind; t->page = page; t->title = g_strdup(title); t->color = g_strdup("");
    t->label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    t->icon = gtk_image_new_from_icon_name(kind_icon(kind));
    t->label = gtk_label_new(title);
    t->dot = gtk_label_new("●");
    gtk_widget_add_css_class(t->dot, "sd-tabdot");
    gtk_widget_set_visible(t->dot, FALSE);
    t->close = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(t->close, "sd-tabclose");
    gtk_widget_set_focusable(t->close, FALSE);
    g_signal_connect(t->close, "clicked", G_CALLBACK(on_close_clicked), t);
    gtk_box_append(GTK_BOX(t->label_box), t->icon);
    gtk_box_append(GTK_BOX(t->label_box), t->label);
    gtk_box_append(GTK_BOX(t->label_box), t->dot);
    gtk_box_append(GTK_BOX(t->label_box), t->close);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_tab_pressed), t);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);   /* the notebook claims clicks on its tabs first */
    gtk_widget_add_controller(t->label_box, GTK_EVENT_CONTROLLER(click));
    g_ptr_array_add(a->tabs, t);
    int idx = gtk_notebook_append_page(GTK_NOTEBOOK(a->notebook), page, t->label_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(a->notebook), page, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(a->notebook), idx);
    update_content_stack(a);
    return t;
}

static SdTerm *make_term(App *a, Tab *t_unused, char *const argv[], const char *cwd) {
    (void)t_unused;
    SdTerm *term = sd_term_new(argv, cwd);
    sd_term_set_theme(term, &a->cur_theme);
    sd_term_set_font(term, a->font_family, a->font_size);
    return term;
}

static void wire_term(Tab *t) {
    g_signal_connect(t->term, "activity", G_CALLBACK(on_term_activity), t);
    g_signal_connect(t->term, "bell", G_CALLBACK(on_term_bell), t);
    g_signal_connect(t->term, "exited", G_CALLBACK(on_term_exited), t);
    g_signal_connect(t->term, "restarted", G_CALLBACK(on_term_restarted), t);
    g_signal_connect(t->term, "cwd-changed", G_CALLBACK(on_term_cwd), t);
}

static Tab *open_local(App *a, const char *title, char **argv, const char *cwd) {
    SdTerm *term = make_term(a, NULL, argv, cwd);
    Tab *t = tab_add(a, TAB_LOCAL, title, GTK_WIDGET(term));
    t->term = term;
    t->local_argv = g_strdupv(argv);
    wire_term(t);
    tab_refresh_label(t);
    gtk_widget_grab_focus(GTK_WIDGET(term));
    return t;
}

static char **default_shell_argv(const char *shell) {
    const char *s = shell ? shell : g_getenv("SHELL");
    if (!s || !*s) s = "/bin/sh";
    char **argv = g_new0(char *, 3);
    argv[0] = g_strdup(s);
    return argv;
}

static void on_ftp_password(const char *pw, gpointer data);

typedef struct { App *a; SdConn *conn; } FtpOpen;

static void ftp_open_with_password(App *a, SdConn *c, const char *password) {
    SdXfer *x = sd_xfer_new_ftp(c, password);
    SdFiles *f = sd_files_new(x, FALSE, a->win);
    Tab *t = tab_add(a, TAB_FTP, c->name, sd_files_widget(f));
    t->files = f;
    t->files_widget = sd_files_widget(f);
    t->conn = sd_conn_copy(c);
    set_widget_color(t->label, &t->label_css, c->color);
    g_free(t->color);
    t->color = g_strdup(c->color);
    tab_refresh_label(t);
}

static void on_ftp_password(const char *pw, gpointer data) {
    FtpOpen *o = data;
    ftp_open_with_password(o->a, o->conn, pw);
    sd_conn_free(o->conn);
    g_free(o);
}

static Tab *open_connection_copy(App *a, SdConn *c);

/* refresh() reloads ~/.ssh/config and frees the store's connection records, so work on a private copy. */
static Tab *open_connection(App *a, SdConn *conn) {
    SdConn *c = sd_conn_copy(conn);
    sd_store_touch(a->store, c->id);
    refresh(a);
    Tab *t = open_connection_copy(a, c);
    sd_conn_free(c);
    return t;
}

static Tab *open_connection_copy(App *a, SdConn *c) {
    if (g_str_equal(c->protocol, "ftp") || g_str_equal(c->protocol, "ftps")) {
        if (*c->user) {
            FtpOpen *o = g_new0(FtpOpen, 1);
            o->a = a; o->conn = sd_conn_copy(c);
            char *title = g_strdup_printf("Password for %s@%s", c->user, c->host);
            ask_text(a->win, title, NULL, TRUE, on_ftp_password, o);
            g_free(title);
        } else {
            ftp_open_with_password(a, c, NULL);
        }
        return NULL;
    }
    char *cp = sd_xfer_new_control_path();
    char **argv = sd_conn_argv(c, cp);
    SdTerm *term = make_term(a, NULL, argv, NULL);
    g_strfreev(argv);
    Tab *t = tab_add(a, TAB_SSH, c->name, GTK_WIDGET(term));
    t->term = term;
    t->conn = sd_conn_copy(c);
    t->control_path = cp;
    g_free(t->color);
    t->color = g_strdup(c->color);
    SdXfer *x = sd_xfer_new_ssh(c, cp);
    t->files = sd_files_new(x, a->follow_default, a->win);
    t->files_widget = sd_files_widget(t->files);
    gtk_stack_add_child(GTK_STACK(a->files_stack), t->files_widget);
    g_signal_connect_swapped(sd_files_follow_check(t->files), "toggled", G_CALLBACK(settings_save), a);
    wire_term(t);
    tab_refresh_label(t);
    sync_sidebar_files(a);
    gtk_widget_grab_focus(GTK_WIDGET(term));
    return t;
}

/* ---- sessions list ------------------------------------------------------------------------------------------------------- */

static void row_header(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data) {
    (void)data;
    const char *g = g_object_get_data(G_OBJECT(row), "group");
    const char *pg = before ? g_object_get_data(G_OBJECT(before), "group") : NULL;
    if (!pg || !g_str_equal(g, pg)) {
        char *u = g_utf8_strup(g, -1);
        GtkWidget *l = gtk_label_new(u);
        g_free(u);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_add_css_class(l, "sd-group-label");
        gtk_list_box_row_set_header(row, l);
    } else {
        gtk_list_box_row_set_header(row, NULL);
    }
}

static gboolean row_filter(GtkListBoxRow *row, gpointer data) {
    App *a = data;
    const char *needle = gtk_editable_get_text(GTK_EDITABLE(a->search));
    if (!*needle) return TRUE;
    const char *hay = g_object_get_data(G_OBJECT(row), "hay");
    char *n = g_utf8_strdown(needle, -1);
    gboolean hit = hay && strstr(hay, n);
    g_free(n);
    return hit;
}

static void on_search_changed(GtkSearchEntry *e, gpointer data) { (void)e; gtk_list_box_invalidate_filter(GTK_LIST_BOX(((App *)data)->list)); }

static int conn_cmp(gconstpointer x, gconstpointer y) {
    const SdConn *a = *(SdConn *const *)x, *b = *(SdConn *const *)y;
    int c = g_ascii_strcasecmp(a->group, b->group);
    return c ? c : g_ascii_strcasecmp(a->name, b->name);
}

static void set_dot(App *a, const char *id) {
    GtkWidget *dot = g_hash_table_lookup(a->dots, id);
    if (!dot) return;
    int st = GPOINTER_TO_INT(g_hash_table_lookup(a->up, id));
    gtk_widget_remove_css_class(dot, "up");
    gtk_widget_remove_css_class(dot, "down");
    if (st == 1) gtk_widget_add_css_class(dot, "up");
    else if (st == 2) gtk_widget_add_css_class(dot, "down");
}

static void refresh_welcome(App *a);

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer data) {
    (void)lb;
    App *a = data;
    SdConn *c = sd_store_get(a->store, g_object_get_data(G_OBJECT(row), "id"));
    if (c) open_connection(a, c);
}

static void refresh(App *a) {
    sd_store_reload_ssh_config(a->store, a->show_ssh_config, NULL);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(a->list))) gtk_list_box_remove(GTK_LIST_BOX(a->list), child);
    g_hash_table_remove_all(a->dots);
    GPtrArray *all = sd_store_all(a->store);
    g_ptr_array_sort(all, conn_cmp);
    for (guint i = 0; i < all->len; i++) {
        SdConn *c = all->pdata[i];
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *dot = gtk_label_new("●");
        gtk_widget_add_css_class(dot, "sd-dot");
        GtkWidget *name = gtk_label_new(c->name);
        gtk_widget_add_css_class(name, "sd-row-name");
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        if (*c->color) {
            GtkCssProvider *p = gtk_css_provider_new();
            char *css = g_strdup_printf("label { color: %s; }", c->color);
            gtk_css_provider_load_from_string(p, css);
            gtk_style_context_add_provider(gtk_widget_get_style_context(name), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
            g_object_unref(p); g_free(css);
        }
        gtk_box_append(GTK_BOX(box), dot);
        gtk_box_append(GTK_BOX(box), name);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data_full(G_OBJECT(row), "id", g_strdup(c->id), g_free);
        g_object_set_data_full(G_OBJECT(row), "group", g_strdup(c->group), g_free);
        char *hay = g_utf8_strdown(g_strdup_printf("%s %s %s", c->name, c->host, c->group), -1);
        g_object_set_data_full(G_OBJECT(row), "hay", hay, g_free);
        char *tip = sd_conn_is_live(c) ? g_strdup_printf("ssh %s  (from ~/.ssh/config)", c->alias)
                                       : g_strdup_printf("%s://%s:%d", c->protocol, c->host, c->port);
        gtk_widget_set_tooltip_text(row, tip);
        g_free(tip);
        gtk_list_box_append(GTK_LIST_BOX(a->list), row);
        g_hash_table_insert(a->dots, g_strdup(c->id), dot);
        set_dot(a, c->id);
    }
    g_ptr_array_free(all, TRUE);
    gtk_list_box_invalidate_headers(GTK_LIST_BOX(a->list));
    refresh_welcome(a);
}

/* background reachability probe */
static gpointer probe_thread(gpointer data) { (void)data; return NULL; }

typedef struct { App *a; char *id, *host; int port; } Probe;

static void probe_work(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    Probe *p = data;
    GSocketClient *cl = g_socket_client_new();
    g_socket_client_set_timeout(cl, 3);
    GSocketConnection *conn = g_socket_client_connect_to_host(cl, p->host, (guint16)p->port, NULL, NULL);
    gboolean ok = conn != NULL;
    if (conn) g_object_unref(conn);
    g_object_unref(cl);
    g_task_return_boolean(task, ok);
}

static void probe_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    Probe *p = data;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), NULL);
    g_hash_table_insert(p->a->up, g_strdup(p->id), GINT_TO_POINTER(ok ? 1 : 2));
    set_dot(p->a, p->id);
    g_free(p->id); g_free(p->host); g_free(p);
}

static gboolean probe_all(gpointer data) {
    App *a = data;
    (void)probe_thread;
    if (!gtk_window_is_active(a->win) && g_hash_table_size(a->up)) return G_SOURCE_CONTINUE;   /* nobody is looking at the dots */
    GPtrArray *all = sd_store_all(a->store);
    for (guint i = 0; i < all->len; i++) {
        SdConn *c = all->pdata[i];
        Probe *p = g_new0(Probe, 1);
        p->a = a; p->id = g_strdup(c->id); p->host = g_strdup(c->host); p->port = c->port;
        GTask *t = g_task_new(NULL, NULL, probe_done, p);
        g_task_set_task_data(t, p, NULL);
        g_task_run_in_thread(t, probe_work);
        g_object_unref(t);
    }
    g_ptr_array_free(all, TRUE);
    return G_SOURCE_CONTINUE;
}

/* ---- welcome page -------------------------------------------------------------------------------------------------------------- */

static void on_recent_clicked(GtkButton *b, gpointer data) {
    App *a = data;
    SdConn *c = sd_store_get(a->store, g_object_get_data(G_OBJECT(b), "id"));
    if (c) open_connection(a, c);
}

static void on_local_clicked(GtkButton *b, gpointer data) {
    (void)b;
    App *a = data;
    char **argv = default_shell_argv(NULL);
    open_local(a, "local", argv, NULL);
    g_strfreev(argv);
}

static GtkWidget *recent_row(App *a, const char *key, const char *name, const char *host, const char *when, const char *id) {
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "sd-recent");
    gtk_widget_set_focusable(btn, FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *k = gtk_label_new(key);
    gtk_widget_add_css_class(k, "sd-key");
    gtk_widget_set_size_request(k, 22, -1);
    gtk_widget_set_valign(k, GTK_ALIGN_CENTER);
    GtkWidget *n = gtk_label_new(name);
    gtk_widget_add_css_class(n, "sd-rname");
    gtk_widget_set_size_request(n, 140, -1);
    gtk_label_set_xalign(GTK_LABEL(n), 0);
    GtkWidget *h = gtk_label_new(host);
    gtk_widget_add_css_class(h, "sd-rhost");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_widget_set_hexpand(h, TRUE);
    GtkWidget *w = gtk_label_new(when);
    gtk_widget_add_css_class(w, "sd-muted");
    gtk_box_append(GTK_BOX(box), k);
    gtk_box_append(GTK_BOX(box), n);
    gtk_box_append(GTK_BOX(box), h);
    gtk_box_append(GTK_BOX(box), w);
    gtk_button_set_child(GTK_BUTTON(btn), box);
    if (id) {
        g_object_set_data_full(G_OBJECT(btn), "id", g_strdup(id), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_recent_clicked), a);
    } else {
        g_signal_connect(btn, "clicked", G_CALLBACK(on_local_clicked), a);
    }
    return btn;
}

static void refresh_welcome(App *a) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(a->welcome_rows))) gtk_box_remove(GTK_BOX(a->welcome_rows), child);
    GPtrArray *recent = sd_store_recent(a->store, 9);
    GPtrArray *all = sd_store_all(a->store);
    char *meta = all->len ? g_strdup_printf("%u connection%s saved", all->len, all->len == 1 ? "" : "s") : g_strdup("No connections yet");
    gtk_label_set_text(GTK_LABEL(a->welcome_meta), meta);
    g_free(meta);
    g_ptr_array_free(all, TRUE);
    g_ptr_array_set_size(a->recent_ids, 0);
    for (guint i = 0; i < recent->len; i++) {
        SdConn *c = recent->pdata[i];
        char *key = g_strdup_printf("%u", i + 1), *dest = sd_conn_dest(c), *age = relative_age(c->last_used);
        gtk_box_append(GTK_BOX(a->welcome_rows), recent_row(a, key, c->name, dest, age, c->id));
        g_ptr_array_add(a->recent_ids, g_strdup(c->id));
        g_free(key); g_free(dest); g_free(age);
    }
    gtk_box_append(GTK_BOX(a->welcome_rows), recent_row(a, "0", "Local shell", "this machine", "", NULL));
    if (!recent->len) {
        GtkWidget *hint = gtk_label_new("Double-click a host on the left, or press + to add one.");
        gtk_widget_add_css_class(hint, "sd-muted");
        gtk_label_set_xalign(GTK_LABEL(hint), 0);
        gtk_widget_set_margin_top(hint, 14);
        gtk_box_append(GTK_BOX(a->welcome_rows), hint);
    }
    g_ptr_array_free(recent, TRUE);
}

static gboolean on_welcome_key(GtkEventControllerKey *c, guint kv, guint code, GdkModifierType st, gpointer data) {
    (void)c; (void)code;
    App *a = data;
    if (st & (GDK_CONTROL_MASK | GDK_ALT_MASK)) return FALSE;
    if (kv == GDK_KEY_0) { on_local_clicked(NULL, a); return TRUE; }
    if (kv >= GDK_KEY_1 && kv <= GDK_KEY_9) {
        guint i = kv - GDK_KEY_1;
        if (i < a->recent_ids->len) {
            SdConn *conn = sd_store_get(a->store, a->recent_ids->pdata[i]);
            if (conn) open_connection(a, conn);
        }
        return TRUE;
    }
    return FALSE;
}

/* ---- connection dialog ------------------------------------------------------------------------------------------------------------ */

typedef struct {
    App *a;
    SdConn *conn;
    GtkWidget *win, *name, *group, *host, *user, *key, *options, *remote;
    GtkWidget *port, *proto, *x11;
} ConnDlg;

static const char *PROTOS[] = {"ssh", "ftp", "ftps", NULL};
static const char *X11S[] = {"off", "untrusted", "trusted", NULL};

static int index_of(const char **list, const char *v) { for (int i = 0; list[i]; i++) if (g_str_equal(list[i], v)) return i; return 0; }

static void conn_dlg_save(GtkButton *b, gpointer data) {
    (void)b;
    ConnDlg *d = data;
    const char *name = gtk_editable_get_text(GTK_EDITABLE(d->name)), *host = gtk_editable_get_text(GTK_EDITABLE(d->host));
    if (!*g_strstrip((char *)name) || !*g_strstrip((char *)host)) {
        gtk_widget_add_css_class(*name ? d->host : d->name, "error");
        return;
    }
    SdConn *c = d->conn;
    sd_conn_set(&c->name, name);
    sd_conn_set(&c->host, host);
    const char *g = gtk_editable_get_text(GTK_EDITABLE(d->group));
    sd_conn_set(&c->group, *g ? g : "Ungrouped");
    c->port = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(d->port));
    sd_conn_set(&c->user, gtk_editable_get_text(GTK_EDITABLE(d->user)));
    sd_conn_set(&c->key, gtk_editable_get_text(GTK_EDITABLE(d->key)));
    sd_conn_set(&c->options, gtk_editable_get_text(GTK_EDITABLE(d->options)));
    sd_conn_set(&c->remote_command, gtk_editable_get_text(GTK_EDITABLE(d->remote)));
    sd_conn_set(&c->protocol, PROTOS[gtk_drop_down_get_selected(GTK_DROP_DOWN(d->proto))]);
    sd_conn_set(&c->x11, X11S[gtk_drop_down_get_selected(GTK_DROP_DOWN(d->x11))]);
    sd_store_upsert(d->a->store, c);
    refresh(d->a);
    gtk_window_destroy(GTK_WINDOW(d->win));
}

static void conn_dlg_gone(GtkWidget *w, gpointer data) { (void)w; ConnDlg *d = data; sd_conn_free(d->conn); g_free(d); }

static void on_key_chosen(GObject *src, GAsyncResult *res, gpointer data) {
    ConnDlg *d = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (f) { char *p = g_file_get_path(f); gtk_editable_set_text(GTK_EDITABLE(d->key), p); g_free(p); g_object_unref(f); }
}

static void conn_dlg_browse(GtkButton *b, gpointer data) {
    (void)b;
    ConnDlg *d = data;
    GtkFileDialog *fd = gtk_file_dialog_new();
    char *ssh = g_build_filename(g_get_home_dir(), ".ssh", NULL);
    GFile *start = g_file_new_for_path(ssh);
    gtk_file_dialog_set_initial_folder(fd, start);
    gtk_file_dialog_open(fd, GTK_WINDOW(d->win), NULL, on_key_chosen, d);
    g_object_unref(start); g_free(ssh); g_object_unref(fd);
}

static GtkWidget *entry_with(const char *text, const char *placeholder) {
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), text ? text : "");
    if (placeholder) gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    gtk_widget_set_hexpand(e, TRUE);
    return e;
}

static void edit_connection(App *a, SdConn *conn, gboolean copy_of_live) {
    ConnDlg *d = g_new0(ConnDlg, 1);
    d->a = a;
    d->conn = conn ? sd_conn_copy(conn) : sd_conn_new();
    if (conn && copy_of_live) {   /* live ssh-config hosts are read-only: edit a saved copy */
        char *id = g_uuid_string_random();
        sd_conn_set(&d->conn->id, g_strdelimit(id, "-", 'x'));
        g_free(id);
        sd_conn_set(&d->conn->alias, "");
        sd_conn_set(&d->conn->group, "Saved");
    }
    SdConn *c = d->conn;
    d->win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(d->win), conn && !copy_of_live ? "Edit connection" : "New connection");
    gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(d->win), a->win);
    gtk_window_set_default_size(GTK_WINDOW(d->win), 480, -1);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 9);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_top(grid, 18); gtk_widget_set_margin_bottom(grid, 12);
    gtk_widget_set_margin_start(grid, 18); gtk_widget_set_margin_end(grid, 18);
    d->name = entry_with(c->name, NULL);
    d->group = entry_with(c->group, NULL);
    d->host = entry_with(c->host, NULL);
    d->port = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->port), c->port);
    d->user = entry_with(c->user, NULL);
    d->key = entry_with(c->key, "optional - defaults to ssh-agent and ~/.ssh keys");
    d->remote = entry_with(c->remote_command, "empty = the account's login shell (e.g. fish -l, tmux new -A -s main)");
    d->options = entry_with(c->options, "e.g. -J jumphost -L 8080:localhost:80");
    const char *proto_labels[] = {"SSH (terminal + SFTP)", "FTP (file browser)", "FTPS (file browser)", NULL};
    d->proto = gtk_drop_down_new_from_strings(proto_labels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->proto), (guint)index_of(PROTOS, c->protocol));
    const char *x11_labels[] = {"Off", "X11 forwarding (-X)", "Trusted X11 forwarding (-Y)", NULL};
    d->x11 = gtk_drop_down_new_from_strings(x11_labels);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->x11), (guint)index_of(X11S, c->x11));
    GtkWidget *browse = gtk_button_new_with_label("Browse…");
    g_signal_connect(browse, "clicked", G_CALLBACK(conn_dlg_browse), d);
    GtkWidget *keybox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(keybox), d->key);
    gtk_box_append(GTK_BOX(keybox), browse);
    struct { const char *l; GtkWidget *w; } rows[] = {
        {"Name", d->name}, {"Group", d->group}, {"Type", d->proto}, {"Host", d->host}, {"Port", d->port}, {"User", d->user},
        {"Private key", keybox}, {"X11", d->x11}, {"Remote shell", d->remote}, {"Extra ssh args", d->options},
    };
    for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
        GtkWidget *l = gtk_label_new(rows[i].l);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        gtk_widget_add_css_class(l, "sd-muted");
        gtk_grid_attach(GTK_GRID(grid), l, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), rows[i].w, 1, (int)i, 1, 1);
    }
    GtkWidget *save = gtk_button_new_with_label("Save"), *cancel = gtk_button_new_with_label("Cancel");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(save, "clicked", G_CALLBACK(conn_dlg_save), d);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), d->win);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(bar, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(bar), cancel);
    gtk_box_append(GTK_BOX(bar), save);
    gtk_grid_attach(GTK_GRID(grid), bar, 0, (int)G_N_ELEMENTS(rows), 2, 1);
    g_signal_connect(d->win, "destroy", G_CALLBACK(conn_dlg_gone), d);
    gtk_window_set_child(GTK_WINDOW(d->win), grid);
    gtk_window_present(GTK_WINDOW(d->win));
}

/* ---- list context menu ------------------------------------------------------------------------------------------------------------------ */

static void ctx_action(GSimpleAction *act, GVariant *p, gpointer data) {
    (void)p;
    App *a = data;
    const char *n = g_action_get_name(G_ACTION(act));
    SdConn *c = a->ctx_id ? sd_store_get(a->store, a->ctx_id) : NULL;
    if (g_str_equal(n, "new")) { edit_connection(a, NULL, FALSE); return; }
    if (!c) return;
    if (g_str_equal(n, "connect")) open_connection(a, c);
    else if (g_str_equal(n, "edit")) edit_connection(a, c, sd_conn_is_live(c));
    else if (g_str_equal(n, "duplicate") && !sd_conn_is_live(c)) {
        SdConn *cp = sd_conn_copy(c);
        char *id = g_uuid_string_random();
        sd_conn_set(&cp->id, g_strdelimit(id, "-", 'x'));
        g_free(id);
        char *nm = g_strconcat(c->name, " copy", NULL);
        sd_conn_set(&cp->name, nm);
        g_free(nm);
        cp->last_used = 0;
        sd_store_upsert(a->store, cp);
        sd_conn_free(cp);
        refresh(a);
    } else if (g_str_equal(n, "delete") && !sd_conn_is_live(c)) {
        sd_store_remove(a->store, c->id);
        refresh(a);
    }
}

static void on_list_pressed(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n;
    App *a = data;
    if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g)) != 3) return;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(a->list), (int)y);
    g_free(a->ctx_id);
    a->ctx_id = row ? g_strdup(g_object_get_data(G_OBJECT(row), "id")) : NULL;
    if (row) gtk_list_box_select_row(GTK_LIST_BOX(a->list), row);
    SdConn *c = a->ctx_id ? sd_store_get(a->store, a->ctx_id) : NULL;
    GMenu *m = g_menu_new();
    if (c) {
        g_menu_append(m, "Connect", "ctx.connect");
        g_menu_append(m, sd_conn_is_live(c) ? "Save a copy to edit…" : "Edit…", "ctx.edit");
        if (!sd_conn_is_live(c)) { g_menu_append(m, "Duplicate", "ctx.duplicate"); g_menu_append(m, "Delete", "ctx.delete"); }
    }
    g_menu_append(m, "New connection…", "ctx.new");
    if (a->ctx_popover) gtk_widget_unparent(a->ctx_popover);
    a->ctx_popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(m));
    gtk_widget_set_parent(a->ctx_popover, a->list);
    gtk_popover_set_has_arrow(GTK_POPOVER(a->ctx_popover), FALSE);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(a->ctx_popover), &r);
    gtk_popover_popup(GTK_POPOVER(a->ctx_popover));
    g_object_unref(m);
}

/* ---- actions / menus ----------------------------------------------------------------------------------------------------------------------- */

static void on_inventory_chosen(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!f) return;
    char *path = g_file_get_path(f);
    GError *err = NULL;
    GPtrArray *items = sd_parse_ansible_inventory(path, &err);
    if (!items) {
        GtkAlertDialog *d = gtk_alert_dialog_new("Import failed");
        gtk_alert_dialog_set_detail(d, err ? err->message : "");
        gtk_alert_dialog_show(d, a->win);
        g_object_unref(d);
        g_clear_error(&err);
    } else {
        sd_store_add_imported(a->store, items);
        refresh(a);
    }
    g_free(path);
    g_object_unref(f);
}

static void act_import_inventory(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Ansible inventory (INI or YAML)");
    gtk_file_dialog_open(d, ((App *)data)->win, NULL, on_inventory_chosen, data);
    g_object_unref(d);
}

static void act_import_ssh(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    App *a = data;
    char *path = g_build_filename(g_get_home_dir(), ".ssh", "config", NULL);
    GPtrArray *items = sd_parse_ssh_config(path, FALSE);
    g_free(path);
    if (items) { sd_store_add_imported(a->store, items); refresh(a); }
}

static void act_new_conn(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; edit_connection(data, NULL, FALSE); }
static void act_new_local(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; on_local_clicked(NULL, data); }

static void act_local_as(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x;
    App *a = data;
    const char *path = g_variant_get_string(p, NULL);
    char **argv = default_shell_argv(path);
    char *base = g_path_get_basename(path);
    open_local(a, base, argv, NULL);
    g_free(base);
    g_strfreev(argv);
}

static void act_quit(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; gtk_window_close(((App *)data)->win); }

static void act_sidebar(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    App *a = data;
    GtkWidget *side = gtk_paned_get_start_child(GTK_PANED(a->paned));
    gtk_widget_set_visible(side, !gtk_widget_get_visible(side));
}

static void act_files_toggle(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    App *a = data;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(a->side_nb), gtk_notebook_get_current_page(GTK_NOTEBOOK(a->side_nb)) == 0 ? 1 : 0);
}

static void apply_font(App *a) {
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (t->term) sd_term_set_font(t->term, a->font_family, a->font_size);
    }
}

static void act_font(GSimpleAction *act, GVariant *p, gpointer data) {
    (void)p;
    App *a = data;
    int d = g_str_equal(g_action_get_name(G_ACTION(act)), "bigger") ? 1 : -1;
    a->font_size = CLAMP(a->font_size + d, 7, 32);
    apply_font(a);
    settings_save(a);
}

static void act_recheck(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; probe_all(data); }

static void act_theme(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->base_theme = sd_theme_find(g_variant_get_string(p, NULL));
    apply_theme(a);
    settings_save(a);
}

static void accent_done(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    GdkRGBA *c = gtk_color_dialog_choose_rgba_finish(GTK_COLOR_DIALOG(src), res, NULL);
    if (!c) return;
    g_free(a->accent_buf);
    a->accent_buf = g_strdup_printf("#%02x%02x%02x", (int)(c->red * 255 + 0.5), (int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
    gdk_rgba_free(c);
    apply_theme(a);
    settings_save(a);
}

static void act_accent(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    GtkColorDialog *d = gtk_color_dialog_new();
    gtk_color_dialog_choose_rgba(d, ((App *)data)->win, NULL, NULL, accent_done, data);
    g_object_unref(d);
}

static void act_accent_reset(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    App *a = data;
    g_free(a->accent_buf);
    a->accent_buf = NULL;
    apply_theme(a);
    settings_save(a);
}

static void act_toggle_ssh_config(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->show_ssh_config = g_variant_get_boolean(p);
    settings_save(a);
    refresh(a);
}

static void act_toggle_startup(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->startup_shell = g_variant_get_boolean(p);
    settings_save(a);
}

static void apply_history(App *a) {
    sd_term_set_history_defaults(a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (t->term) sd_term_set_history(t->term, a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
    }
}

static void act_scrollback(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->scrollback = g_variant_get_int32(p);
    apply_history(a);
    settings_save(a);
}

static void act_spill(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->sb_spill = g_variant_get_boolean(p);
    apply_history(a);
    settings_save(a);
}

static gboolean hud_tick(gpointer data);

static void act_hud(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->hud = g_variant_get_boolean(p);
    gtk_widget_set_visible(a->hud_label, a->hud);
    if (a->hud && !a->hud_src) { a->hud_prev_time = 0; a->hud_src = g_timeout_add_seconds(1, hud_tick, a); hud_tick(a); }
    settings_save(a);
}

static void act_lowmem(GSimpleAction *act, GVariant *p, gpointer data) {
    App *a = data;
    g_simple_action_set_state(act, p);
    a->lowmem = g_variant_get_boolean(p);
    settings_save(a);
    GtkAlertDialog *d = gtk_alert_dialog_new("Restart Lestrix to switch renderer");
    gtk_alert_dialog_set_detail(d, a->lowmem
        ? "The low-memory renderer draws on the CPU and does not load the GPU stack (roughly 25 MB less memory). It takes effect the next time Lestrix starts."
        : "The GPU renderer takes effect the next time Lestrix starts.");
    gtk_alert_dialog_show(d, a->win);
    g_object_unref(d);
}

/* ---- live performance numbers ---------------------------------------------------------------------- */

static long proc_rss_kb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

static long proc_cpu_ticks(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    long ut = 0, st = 0;
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        char *rp = strrchr(buf, ')');
        if (rp) { sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st); }
        fclose(f);
    }
    return ut + st;
}

static int proc_threads(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    int n = 0;
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "Threads: %d", &n) == 1) break;
    if (f) fclose(f);
    return n;
}

static char *fmt_mb(double bytes) { return bytes >= 1048576 ? g_strdup_printf("%.1f MB", bytes / 1048576) : g_strdup_printf("%.0f KB", bytes / 1024); }

static gboolean hud_tick(gpointer data) {
    App *a = data;
    if (!a->hud) { a->hud_src = 0; return G_SOURCE_REMOVE; }
    gint64 now = g_get_monotonic_time();
    long ticks = proc_cpu_ticks();
    Tab *t = current_tab(a);
    guint64 fed = t && t->term ? sd_term_bytes_fed(t->term) : 0;
    double dt = a->hud_prev_time ? (now - a->hud_prev_time) / 1e6 : 0;
    double cpu = dt > 0 ? (double)(ticks - a->hud_prev_ticks) / (double)sysconf(_SC_CLK_TCK) / dt * 100 : 0;
    double rate = dt > 0 && fed >= a->hud_prev_bytes ? (double)(fed - a->hud_prev_bytes) / dt / 1048576 : 0;
    a->hud_prev_time = now; a->hud_prev_ticks = ticks; a->hud_prev_bytes = fed;
    char *hist = g_strdup("");
    if (t && t->term) {
        VtHistoryStats st;
        sd_term_history_stats(t->term, &st);
        char *packed = fmt_mb((double)(st.hot_bytes + st.packed_bytes)), *disk = fmt_mb((double)st.disk_bytes);
        g_free(hist);
        hist = g_strdup_printf("  ·  scrollback %ld lines (%s in memory, %s on disk)", st.lines, packed, disk);
        g_free(packed); g_free(disk);
    }
    char *rss = fmt_mb((double)proc_rss_kb() * 1024);
    char *txt = g_strdup_printf("parse %.1f MB/s  ·  cpu %.0f%%  ·  memory %s  ·  %d threads  ·  %u cores%s",
                                rate, cpu, rss, proc_threads(), (unsigned)sysconf(_SC_NPROCESSORS_ONLN), hist);
    gtk_label_set_text(GTK_LABEL(a->hud_label), txt);
    g_free(txt); g_free(rss); g_free(hist);
    return G_SOURCE_CONTINUE;
}

/* ---- giving memory back ---------------------------------------------------------------------------------- */

/* Every few seconds of quiet: pack old scrollback, drop decoded caches, and return freed heap to the OS. */
static gboolean trim_tick(gpointer data) {
    App *a = data;
    guint64 total = 0;
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (t->term) total += sd_term_bytes_fed(t->term);
    }
    if (total != a->trim_prev_bytes) { a->trim_prev_bytes = total; a->trim_dirty = TRUE; return G_SOURCE_CONTINUE; }
    if (!a->trim_dirty) return G_SOURCE_CONTINUE;
    a->trim_dirty = FALSE;
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (t->term) sd_term_compact(t->term);
    }
    malloc_trim(0);
    return G_SOURCE_CONTINUE;
}

static void act_about(GSimpleAction *x, GVariant *p, gpointer data) {
    (void)x; (void)p;
    GtkWidget *d = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(d), "Lestrix");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(d), APP_VERSION);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(d), "SSH sessions, SFTP/FTP and local shells in one window.");
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(d), GTK_LICENSE_MIT_X11);
    gtk_window_set_transient_for(GTK_WINDOW(d), ((App *)data)->win);
    gtk_window_present(GTK_WINDOW(d));
}

static void act_next_tab(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; gtk_notebook_next_page(GTK_NOTEBOOK(((App *)data)->notebook)); }
static void act_prev_tab(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; gtk_notebook_prev_page(GTK_NOTEBOOK(((App *)data)->notebook)); }
static void act_close_tab(GSimpleAction *x, GVariant *p, gpointer data) { (void)x; (void)p; Tab *t = current_tab(data); if (t) close_tab(t); }

static GMenuModel *build_shell_menu(void) {
    GMenu *m = g_menu_new();
    const char *names[] = {"bash", "zsh", "fish", "sh", "dash", "ksh", "tcsh", "csh", "nu", "xonsh", "elvish", "pwsh", NULL};
    for (int i = 0; names[i]; i++) {
        char *p = g_find_program_in_path(names[i]);
        if (!p) continue;
        GMenuItem *it = g_menu_item_new(names[i], NULL);
        g_menu_item_set_action_and_target_value(it, "app.local-as", g_variant_new_string(p));
        g_menu_append_item(m, it);
        g_object_unref(it);
        g_free(p);
    }
    return G_MENU_MODEL(m);
}

static void build_actions_and_menu(App *a) {
    GActionEntry plain[] = {
        {"new-connection", act_new_conn}, {"new-local", act_new_local}, {"import-inventory", act_import_inventory},
        {"import-ssh-config", act_import_ssh}, {"quit", act_quit}, {"sidebar", act_sidebar}, {"files-toggle", act_files_toggle},
        {"bigger", act_font}, {"smaller", act_font}, {"recheck", act_recheck}, {"accent", act_accent},
        {"accent-reset", act_accent_reset}, {"about", act_about}, {"next-tab", act_next_tab}, {"prev-tab", act_prev_tab},
        {"close-tab", act_close_tab},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(a->gapp), plain, G_N_ELEMENTS(plain), a);
    GSimpleAction *la = g_simple_action_new("local-as", G_VARIANT_TYPE_STRING);
    g_signal_connect(la, "activate", G_CALLBACK(act_local_as), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(la));
    g_object_unref(la);
    GSimpleAction *th = g_simple_action_new_stateful("theme", G_VARIANT_TYPE_STRING, g_variant_new_string(a->base_theme->name));
    g_signal_connect(th, "change-state", G_CALLBACK(act_theme), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(th));
    g_object_unref(th);
    GSimpleAction *sc = g_simple_action_new_stateful("show-ssh-config", NULL, g_variant_new_boolean(a->show_ssh_config));
    g_signal_connect(sc, "change-state", G_CALLBACK(act_toggle_ssh_config), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(sc));
    g_object_unref(sc);
    GSimpleAction *sb = g_simple_action_new_stateful("scrollback", G_VARIANT_TYPE_INT32, g_variant_new_int32(a->scrollback));
    g_signal_connect(sb, "change-state", G_CALLBACK(act_scrollback), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(sb));
    g_object_unref(sb);
    GSimpleAction *sp = g_simple_action_new_stateful("spill", NULL, g_variant_new_boolean(a->sb_spill));
    g_signal_connect(sp, "change-state", G_CALLBACK(act_spill), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(sp));
    g_object_unref(sp);
    GSimpleAction *hu = g_simple_action_new_stateful("hud", NULL, g_variant_new_boolean(a->hud));
    g_signal_connect(hu, "change-state", G_CALLBACK(act_hud), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(hu));
    g_object_unref(hu);
    GSimpleAction *lm = g_simple_action_new_stateful("lowmem", NULL, g_variant_new_boolean(a->lowmem));
    g_signal_connect(lm, "change-state", G_CALLBACK(act_lowmem), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(lm));
    g_object_unref(lm);
    GSimpleAction *su = g_simple_action_new_stateful("startup-shell", NULL, g_variant_new_boolean(a->startup_shell));
    g_signal_connect(su, "change-state", G_CALLBACK(act_toggle_startup), a);
    g_action_map_add_action(G_ACTION_MAP(a->gapp), G_ACTION(su));
    g_object_unref(su);

    GSimpleActionGroup *ctx = g_simple_action_group_new();
    const char *cn[] = {"connect", "edit", "duplicate", "delete", "new", NULL};
    for (int i = 0; cn[i]; i++) {
        GSimpleAction *x = g_simple_action_new(cn[i], NULL);
        g_signal_connect(x, "activate", G_CALLBACK(ctx_action), a);
        g_action_map_add_action(G_ACTION_MAP(ctx), G_ACTION(x));
        g_object_unref(x);
    }
    gtk_widget_insert_action_group(GTK_WIDGET(a->win), "ctx", G_ACTION_GROUP(ctx));
    g_object_unref(ctx);

    GMenu *menubar = g_menu_new();
    GMenu *file = g_menu_new();
    g_menu_append(file, "New connection…", "app.new-connection");
    g_menu_append(file, "New local shell", "app.new-local");
    GMenuModel *shells = build_shell_menu();
    g_menu_append_submenu(file, "New local shell as…", shells);
    GMenu *imp = g_menu_new();
    g_menu_append(imp, "Import Ansible inventory…", "app.import-inventory");
    g_menu_append(imp, "Copy ~/.ssh/config hosts into saved connections", "app.import-ssh-config");
    g_menu_append_section(file, NULL, G_MENU_MODEL(imp));
    GMenu *q = g_menu_new();
    g_menu_append(q, "Quit", "app.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(q));
    g_menu_append_submenu(menubar, "_File", G_MENU_MODEL(file));

    GMenu *view = g_menu_new();
    GMenu *v1 = g_menu_new();
    g_menu_append(v1, "Toggle sidebar", "app.sidebar");
    g_menu_append(v1, "Sessions / Files sidebar", "app.files-toggle");
    g_menu_append(v1, "Bigger font", "app.bigger");
    g_menu_append(v1, "Smaller font", "app.smaller");
    g_menu_append(v1, "Re-check hosts", "app.recheck");
    g_menu_append_section(view, NULL, G_MENU_MODEL(v1));
    GMenu *themes = g_menu_new();
    for (int i = 0; i < SD_THEME_COUNT; i++) {
        GMenuItem *it = g_menu_item_new(SD_THEMES[i].name, NULL);
        g_menu_item_set_action_and_target_value(it, "app.theme", g_variant_new_string(SD_THEMES[i].name));
        g_menu_append_item(themes, it);
        g_object_unref(it);
    }
    GMenu *ts = g_menu_new();
    g_menu_append(ts, "Accent color…", "app.accent");
    g_menu_append(ts, "Reset accent", "app.accent-reset");
    g_menu_append_section(themes, NULL, G_MENU_MODEL(ts));
    g_menu_append_submenu(view, "Theme", G_MENU_MODEL(themes));
    GMenu *sbm = g_menu_new();
    const struct { const char *label; int lines; } presets[] = {
        {"Off", 0}, {"1,000 lines", 1000}, {"10,000 lines (default)", 10000}, {"100,000 lines", 100000},
        {"1,000,000 lines", 1000000}, {"Unlimited (old lines move to disk)", -1},
    };
    for (guint i = 0; i < G_N_ELEMENTS(presets); i++) {
        GMenuItem *it = g_menu_item_new(presets[i].label, NULL);
        g_menu_item_set_action_and_target_value(it, "app.scrollback", g_variant_new_int32(presets[i].lines));
        g_menu_append_item(sbm, it);
        g_object_unref(it);
    }
    GMenu *sbs = g_menu_new();
    g_menu_append(sbs, "Spill old scrollback to disk", "app.spill");
    g_menu_append_section(sbm, NULL, G_MENU_MODEL(sbs));
    g_menu_append_submenu(view, "Scrollback", G_MENU_MODEL(sbm));
    GMenu *v2 = g_menu_new();
    g_menu_append(v2, "Show performance overlay", "app.hud");
    g_menu_append(v2, "Low-memory renderer (restart)", "app.lowmem");
    g_menu_append(v2, "Show hosts from ~/.ssh/config", "app.show-ssh-config");
    g_menu_append(v2, "Open a local shell on startup", "app.startup-shell");
    g_menu_append_section(view, NULL, G_MENU_MODEL(v2));
    g_menu_append_submenu(menubar, "_View", G_MENU_MODEL(view));

    GMenu *help = g_menu_new();
    g_menu_append(help, "About", "app.about");
    g_menu_append_submenu(menubar, "_Help", G_MENU_MODEL(help));
    gtk_application_set_menubar(a->gapp, G_MENU_MODEL(menubar));

    const char *acc_new[] = {"<Primary>n", NULL}, *acc_local[] = {"<Primary><Shift>t", NULL}, *acc_quit[] = {"<Primary>q", NULL};
    const char *acc_sb[] = {"<Primary>b", NULL}, *acc_files[] = {"<Primary><Shift>b", NULL}, *acc_big[] = {"<Primary>equal", "<Primary>plus", NULL};
    const char *acc_small[] = {"<Primary>minus", NULL}, *acc_re[] = {"F5", NULL}, *acc_close[] = {"<Primary><Shift>w", NULL};
    const char *acc_next[] = {"<Primary>Page_Down", NULL}, *acc_prev[] = {"<Primary>Page_Up", NULL};
    gtk_application_set_accels_for_action(a->gapp, "app.new-connection", acc_new);
    gtk_application_set_accels_for_action(a->gapp, "app.new-local", acc_local);
    gtk_application_set_accels_for_action(a->gapp, "app.quit", acc_quit);
    gtk_application_set_accels_for_action(a->gapp, "app.sidebar", acc_sb);
    gtk_application_set_accels_for_action(a->gapp, "app.files-toggle", acc_files);
    gtk_application_set_accels_for_action(a->gapp, "app.bigger", acc_big);
    gtk_application_set_accels_for_action(a->gapp, "app.smaller", acc_small);
    gtk_application_set_accels_for_action(a->gapp, "app.recheck", acc_re);
    gtk_application_set_accels_for_action(a->gapp, "app.close-tab", acc_close);
    gtk_application_set_accels_for_action(a->gapp, "app.next-tab", acc_next);
    gtk_application_set_accels_for_action(a->gapp, "app.prev-tab", acc_prev);
}

/* ---- window ------------------------------------------------------------------------------------------------------------------------------------ */

static gboolean on_close_request(GtkWindow *w, gpointer data) {
    (void)w;
    App *a = data;
    for (guint i = a->tabs->len; i > 0; i--) {
        Tab *t = a->tabs->pdata[i - 1];
        if (t->term) sd_term_close(t->term);
        if (t->control_path) sd_xfer_remove_control_path(t->control_path);
    }
    if (a->probe_src) { g_source_remove(a->probe_src); a->probe_src = 0; }
    if (a->trim_src) { g_source_remove(a->trim_src); a->trim_src = 0; }
    if (a->hud_src) { g_source_remove(a->hud_src); a->hud_src = 0; }
    settings_save(a);
    return FALSE;
}

static GtkWidget *build_sidebar(App *a) {
    GtkWidget *sessions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(sessions, 10); gtk_widget_set_margin_end(sessions, 6);
    gtk_widget_set_margin_top(sessions, 10); gtk_widget_set_margin_bottom(sessions, 8);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(a->search), "Search connections");
    gtk_widget_set_hexpand(a->search, TRUE);
    g_signal_connect(a->search, "search-changed", G_CALLBACK(on_search_changed), a);
    GtkWidget *add = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(add, "New connection (Ctrl+N)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(add), "app.new-connection");
    gtk_box_append(GTK_BOX(top), a->search);
    gtk_box_append(GTK_BOX(top), add);
    a->list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(a->list), FALSE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(a->list), row_header, a, NULL);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(a->list), row_filter, a, NULL);
    g_signal_connect(a->list, "row-activated", G_CALLBACK(on_row_activated), a);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 3);
    g_signal_connect(click, "pressed", G_CALLBACK(on_list_pressed), a);
    gtk_widget_add_controller(a->list, GTK_EVENT_CONTROLLER(click));
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), a->list);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(sessions), top);
    gtk_box_append(GTK_BOX(sessions), scroll);

    GtkWidget *files = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(files, 10); gtk_widget_set_margin_top(files, 10);
    a->files_label = gtk_label_new("NO SSH SESSION");
    gtk_widget_add_css_class(a->files_label, "sd-section");
    gtk_label_set_xalign(GTK_LABEL(a->files_label), 0);
    a->files_stack = gtk_stack_new();
    a->files_hint = gtk_label_new("The file browser appears\nwhen an SSH tab is open.");
    gtk_widget_add_css_class(a->files_hint, "sd-muted");
    gtk_stack_add_child(GTK_STACK(a->files_stack), a->files_hint);
    gtk_widget_set_vexpand(a->files_stack, TRUE);
    gtk_box_append(GTK_BOX(files), a->files_label);
    gtk_box_append(GTK_BOX(files), a->files_stack);

    a->side_nb = gtk_notebook_new();
    gtk_widget_add_css_class(a->side_nb, "sd-side");
    gtk_notebook_append_page(GTK_NOTEBOOK(a->side_nb), sessions, gtk_label_new("SESSIONS"));
    gtk_notebook_append_page(GTK_NOTEBOOK(a->side_nb), files, gtk_label_new("FILES"));
    gtk_notebook_set_show_border(GTK_NOTEBOOK(a->side_nb), FALSE);
    GtkWidget *wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(wrap, "sd-sidebar");
    gtk_widget_set_size_request(wrap, 230, -1);
    gtk_box_append(GTK_BOX(wrap), a->side_nb);
    gtk_widget_set_vexpand(a->side_nb, TRUE);
    return wrap;
}

static GtkWidget *build_welcome(App *a) {
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(outer, "sd-welcome");
    gtk_widget_set_focusable(outer, TRUE);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(col, 64); gtk_widget_set_margin_top(col, 56); gtk_widget_set_margin_end(col, 32);
    gtk_widget_set_size_request(col, 520, -1);
    GtkWidget *title = gtk_label_new("Lestrix");
    gtk_widget_add_css_class(title, "sd-wordmark");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    a->welcome_meta = gtk_label_new("");
    gtk_widget_add_css_class(a->welcome_meta, "sd-muted");
    gtk_label_set_xalign(GTK_LABEL(a->welcome_meta), 0);
    GtkWidget *sec = gtk_label_new("RECENT");
    gtk_widget_add_css_class(sec, "sd-section");
    gtk_label_set_xalign(GTK_LABEL(sec), 0);
    gtk_widget_set_margin_top(sec, 34);
    gtk_widget_set_margin_bottom(sec, 6);
    a->welcome_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(col), title);
    gtk_box_append(GTK_BOX(col), a->welcome_meta);
    gtk_box_append(GTK_BOX(col), sec);
    gtk_box_append(GTK_BOX(col), a->welcome_rows);
    gtk_box_append(GTK_BOX(outer), col);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_welcome_key), a);
    gtk_widget_add_controller(outer, key);
    return outer;
}

typedef struct { App *a; gboolean local; const char *cwd; char **exec; const char *theme; gboolean lite; } Startup;

static void build_window(App *a, Startup *s) {
    a->win = GTK_WINDOW(gtk_application_window_new(a->gapp));
    gtk_window_set_title(a->win, "Lestrix");
    gtk_window_set_default_size(a->win, 1240, 760);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(a->win), TRUE);
    a->tabs = g_ptr_array_new();
    a->up = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    a->dots = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    a->recent_ids = g_ptr_array_new_with_free_func(g_free);

    a->css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(a->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    apply_theme(a);

    GtkWidget *sidebar = build_sidebar(a);
    a->notebook = gtk_notebook_new();
    gtk_widget_add_css_class(a->notebook, "sd-tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(a->notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(a->notebook), FALSE);
    g_signal_connect(a->notebook, "switch-page", G_CALLBACK(on_switch_page), a);
    a->welcome = build_welcome(a);
    a->content_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(a->content_stack), a->welcome, "welcome");
    gtk_stack_add_named(GTK_STACK(a->content_stack), a->notebook, "tabs");

    a->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(a->paned), sidebar);
    gtk_paned_set_resize_start_child(GTK_PANED(a->paned), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(a->paned), FALSE);
    gtk_paned_set_end_child(GTK_PANED(a->paned), a->content_stack);
    gtk_paned_set_position(GTK_PANED(a->paned), 300);
    a->hud_label = gtk_label_new("");
    gtk_widget_add_css_class(a->hud_label, "sd-hud");
    gtk_label_set_xalign(GTK_LABEL(a->hud_label), 0);
    gtk_widget_set_visible(a->hud_label, a->hud);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(a->paned, TRUE);
    gtk_box_append(GTK_BOX(vbox), a->paned);
    gtk_box_append(GTK_BOX(vbox), a->hud_label);
    gtk_window_set_child(a->win, vbox);
    g_signal_connect(a->win, "close-request", G_CALLBACK(on_close_request), a);

    build_actions_and_menu(a);
    refresh(a);
    probe_all(a);
    a->probe_src = g_timeout_add_seconds(30, probe_all, a);
    a->trim_src = g_timeout_add_seconds(5, trim_tick, a);
    a->trim_dirty = TRUE;   /* startup allocations are garbage once the window is up */
    if (a->hud) a->hud_src = g_timeout_add_seconds(1, hud_tick, a);
    gtk_window_present(a->win);

    if (s->exec && s->exec[0]) {
        char *base = g_path_get_basename(s->exec[0]);
        open_local(a, base, s->exec, s->cwd);
        g_free(base);
    } else if (s->local || a->startup_shell) {
        char **argv = default_shell_argv(NULL);
        open_local(a, "local", argv, s->cwd);
        g_strfreev(argv);
    } else {
        gtk_widget_grab_focus(a->welcome);
    }
}

static void on_activate(GtkApplication *gapp, gpointer data) {
    Startup *s = data;
    gtk_window_set_default_icon_name("lestrix");  /* window and taskbar icon (the theme lookup finds the installed PNG) */
    App *a = g_new0(App, 1);
    a->gapp = gapp;
    s->a = a;
    settings_load(a);
    if (s->lite && !g_key_file_has_key(a->settings, "ui", "scrollback", NULL)) a->scrollback = 2000;
    sd_term_set_history_defaults(a->scrollback, a->sb_ram_mb, a->sb_disk_mb, a->sb_spill);
    if (!a->font_family || !*a->font_family) { g_free(a->font_family); a->font_family = pick_font_family(); }
    if (s->theme) a->base_theme = sd_theme_find(s->theme);
    a->store = sd_store_new(NULL);
    build_window(a, s);
}

int main(int argc, char **argv) {
    /* keep the heap small: two malloc arenas, big blocks straight from the kernel so freeing returns them */
    mallopt(M_ARENA_MAX, 2);
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);   /* block buffers (~60 KB) are mapped and unmapped, so they cannot fragment the heap */
    mallopt(M_TRIM_THRESHOLD, 256 * 1024);
    Startup s = {0};
    GPtrArray *rest = g_ptr_array_new();
    g_ptr_array_add(rest, argv[0]);
    for (int i = 1; i < argc; i++) {
        if (g_str_equal(argv[i], "--version")) { g_print("Lestrix %s\n", APP_VERSION); return 0; }
        if (g_str_equal(argv[i], "--benchmark")) return sd_benchmark(stdout, 8.0);
        if (g_str_equal(argv[i], "--help") || g_str_equal(argv[i], "-h")) {
            g_print("Usage: lestrix [--local] [--working-directory DIR] [-e COMMAND [ARGS...]]\n"
                    "  --local               open a local shell on startup\n"
                    "  --working-directory   start the local shell in DIR\n"
                    "  --lite                CPU renderer (no GPU stack) and a 2000-line scrollback: the smallest memory footprint\n"
                    "  --benchmark           measure this machine's terminal throughput and exit\n"
                    "  --theme NAME          Xylonic Dark, Xylonic Light, Graphite, Nord, Gruvbox or Solarized Dark\n"
                    "  -e COMMAND ...        run COMMAND in a new tab (works as x-terminal-emulator)\n");
            return 0;
        }
        if (g_str_equal(argv[i], "--local")) s.local = TRUE;
        else if (g_str_equal(argv[i], "--working-directory") && i + 1 < argc) s.cwd = argv[++i];
        else if (g_str_equal(argv[i], "--theme") && i + 1 < argc) s.theme = argv[++i];
        else if (g_str_equal(argv[i], "--lite")) s.lite = TRUE;
        else if (g_str_equal(argv[i], "-e") || g_str_equal(argv[i], "--execute")) {
            s.exec = g_new0(char *, (gsize)(argc - i));
            for (int k = i + 1; k < argc; k++) s.exec[k - i - 1] = argv[k];
            break;
        } else g_ptr_array_add(rest, argv[i]);
    }
    gboolean cpu_renderer = s.lite || g_strcmp0(g_getenv("LESTRIX_RENDERER"), "software") == 0;
    if (!cpu_renderer) {   /* the saved choice from the View menu */
        GKeyFile *kf = g_key_file_new();
        char *dir = sd_config_dir(), *path = g_build_filename(dir, "settings.ini", NULL);
        if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
            char *r = g_key_file_get_string(kf, "ui", "renderer", NULL);
            cpu_renderer = r && g_str_equal(r, "cpu");
            g_free(r);
        }
        g_free(path); g_free(dir); g_key_file_free(kf);
    }
    if (cpu_renderer) {
        /* No GL, no Vulkan: GTK never loads the GPU driver stack (about 25 MB of resident memory),
         * and the terminal paints only changed rows into one image. */
        g_setenv("GSK_RENDERER", "cairo", FALSE);
        g_setenv("GDK_DISABLE", "gl,vulkan", FALSE);
        g_setenv("GTK_A11Y", "none", FALSE);
    }
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &s);
    int rc = g_application_run(G_APPLICATION(app), (int)rest->len, (char **)rest->pdata);
    g_object_unref(app);
    g_ptr_array_free(rest, TRUE);
    g_free(s.exec);
    return rc;
}
