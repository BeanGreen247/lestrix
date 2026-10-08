/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_APP_H
#define SD_APP_H
#include <glib.h>
#include <stdbool.h>
#include "../store.h"
#include "../xfer.h"
#include "atlas.h"
#include "fleetwm.h"
#include "font.h"
#include "render.h"
#include "tcore.h"
#include "termview.h"
#include "ui.h"
#include "uitheme.h"

#define APP_VERSION "0.3.0"
#define APP_CREDIT "Copyright (c) 2026 Thomas Mozdren (BeanGreen247)\nhttps://github.com/BeanGreen247/lestrix\n"

typedef struct App App;
typedef struct Dialog Dialog;
typedef struct FilesPane FilesPane;

typedef enum { TAB_LOCAL, TAB_SSH, TAB_FTP } TabKind;
typedef enum { ST_IDLE, ST_ACTIVITY, ST_BELL, ST_ENDED } TabState;

typedef struct {
    App *app;
    TabKind kind;
    TermCore *term;
    FilesPane *files;
    SdConn *conn;
    char *title, *color, *control_path;
    char **local_argv;
    TabState state;
    bool asked;
    float x, w;
} Tab;

struct Dialog {
    void (*draw)(App *a, Dialog *d);
    void (*free_fn)(Dialog *d);
    bool done;
    bool wants_text;
};

struct App {
    SDL_Window *win;
    SDL_GLContext gl;
    Renderer *r;
    Atlas *atlas;
    Ui *ui;
    Font *term_font, *ui_font, *ui_mono, *big_font;
    float scale;
    char gl_desc[200];
    int W, H;
    double now;
    const UiTheme *theme;
    UiColors colors;
    TermPalette pal;
    char *accent;
    GKeyFile *settings;
    char *settings_path;
    SdStore *store;
    int font_size, scrollback;
    char *font_family;
    bool show_ssh_config, startup_shell, follow_default, hud, sb_spill, pinned;
    bool follow_fleet, rounded;
    FleetCfg fleet;
    double next_fleet;
    bool fleet_watch;
    size_t sb_ram_mb, sb_disk_mb;
    int panel_w;
    GPtrArray *tabs;
    int cur;
    float strip_scroll;
    int drag_tab; float drag_dx; bool drag_moved;
    bool side_open, dock_shown;
    int side_page;
    double side_anim;
    UiText search; char search_buf[128];
    float list_scroll;
    GHashTable *up;
    char *ctx_id;
    GPtrArray *recent_ids;
    Dialog *dialogs[4];
    int ndialogs;
    int menu_hover;
    double next_probe, next_hud, next_trim;
    bool fast_cat;
    bool quit_on_exit;
    char hud_cache_tip[448], hud_parse_tip[256], title_last[400];
    bool lite;
    uint64_t hud_prev_reads;
    char hud_seg[11][256];
    uint64_t hud_prev_bytes, trim_prev_bytes;
    double hud_prev_time;
    long hud_prev_ticks;
    uint64_t frames, hud_prev_frames;
    uint64_t hud_prev_irq, hud_irq_now;
    bool hud_irq_total;
    bool trim_dirty;
    bool running, dirty, blocked_for_term;
    bool blink_on;
    double last_blink;
    char *flood_file;
    double flood_t0;
    bool flood_sent;
    char *run_cmd;
};

extern App *g_app;
void app_redraw(App *a);
void app_wake(void);
void app_refresh(App *a);
Tab *app_cur_tab(App *a);
Tab *app_open_connection(App *a, const SdConn *c);
Tab *app_open_local(App *a, const char *title, char **argv, const char *cwd);
void app_close_tab(App *a, Tab *t);
void app_settings_save(App *a);
void app_apply_theme(App *a, const UiTheme *t);
void app_follow_fleetwm(App *a);
void app_apply_font(App *a);
void app_set_accent(App *a, const char *hex);
void app_dup_tab(App *a, Tab *t);
void app_set_tab_color(App *a, Tab *t, const char *hex);
char **app_default_shell_argv(const char *shell);

void dlg_push(App *a, Dialog *d);
bool dlg_active(const App *a);
void dlg_draw_top(App *a);
Rect dlg_frame(App *a, const char *title, float w, float h);
typedef void (*TextCb)(App *a, const char *text, void *user);
void dlg_text(App *a, const char *title, const char *label, const char *initial, bool password, TextCb cb, void *user);
typedef void (*ConfirmCb)(App *a, void *user);
void dlg_confirm(App *a, const char *title, const char *msg, const char *ok, ConfirmCb cb, void *user);
void dlg_message(App *a, const char *title, const char *msg);
typedef void (*ChoiceCb)(App *a, int index, void *user);
void dlg_choice(App *a, const char *title, const char *msg, const char *const *labels, int n, ChoiceCb cb, void *user);
typedef void (*PickCb)(App *a, char **paths, int n, void *user);
void dlg_pick_file(App *a, const char *title, bool folder, bool multi, const char *start, PickCb cb, void *user);
void dlg_conn_editor(App *a, const SdConn *c, bool copy_of_live);
void dlg_about(App *a);
void app_fastcat_apply(App *a);
void dlg_color(App *a, const char *title, const char *initial, TextCb cb, void *user);

FilesPane *files_new(App *a, SdXfer *x, bool follow);
void files_free(FilesPane *f);
void files_draw(App *a, FilesPane *f, Rect r);
void files_set_cwd(FilesPane *f, const char *path);
void files_note_activity(FilesPane *f);
bool files_follow(const FilesPane *f);
void files_set_follow(FilesPane *f, bool v);
const char *files_label(FilesPane *f);
void files_tick(App *a, FilesPane *f);
void files_drop(FilesPane *f, const char *path);
#endif
