/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_APP_H
#define SD_APP_H
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../store.h"
#include "../util.h"
#include "../xfer.h"
#include "atlas.h"
#include "fleetwm.h"
#include "font.h"
#include "render.h"
#include "tcore.h"
#include "termview.h"
#include "ui.h"
#include "uitheme.h"

#define APP_VERSION "0.4.0"
#define APP_CREDIT "Copyright (c) 2026 Thomas Mozdren (BeanGreen247)\nhttps://github.com/BeanGreen247/lestrix\n"

struct App;
class FilesPane;

enum TabKind { TAB_LOCAL, TAB_SSH, TAB_FTP };
enum TabState { ST_IDLE, ST_ACTIVITY, ST_BELL, ST_ENDED };

struct Tab {
    App *app = nullptr;
    TabKind kind = TAB_LOCAL;
    TermCore *term = nullptr;
    std::shared_ptr<FilesPane> files;
    std::unique_ptr<SdConn> conn;
    std::string title, color, control_path;
    std::vector<std::string> local_argv;
    TabState state = ST_IDLE;
    bool asked = false;
    float x = 0, w = 0;

    Tab() = default;
    Tab(const Tab &) = delete;
    Tab &operator=(const Tab &) = delete;
    ~Tab();
};

/* One modal dialog. draw() is called every frame while it is on top; set done to close it. */
class Dialog {
public:
    virtual ~Dialog() = default;
    virtual void draw(App *a) = 0;
    bool done = false;
};

struct App {
    SDL_Window *win = nullptr;
    SDL_GLContext gl = nullptr;
    Renderer *r = nullptr;
    Atlas *atlas = nullptr;
    Ui *ui = nullptr;
    Font *term_font = nullptr, *ui_font = nullptr, *ui_mono = nullptr, *big_font = nullptr;
    float scale = 1;
    char gl_desc[200] = "";
    int W = 0, H = 0;
    double now = 0;
    const UiTheme *theme = nullptr;
    UiColors colors{};
    TermPalette pal{};
    std::string accent;
    lx::IniFile settings;
    std::string settings_path;
    std::unique_ptr<SdStore> store;
    int font_size = 11, scrollback = 10000;
    std::string font_family;
    bool show_ssh_config = true, startup_shell = false, follow_default = true, hud = true, sb_spill = true, pinned = true;
    bool follow_fleet = true, rounded = true;
    FleetCfg fleet;
    double next_fleet = 0;
    bool fleet_watch = false;
    size_t sb_ram_mb = 32, sb_disk_mb = 4096;
    int panel_w = 300;
    std::vector<std::unique_ptr<Tab>> tabs;
    int cur = -1;
    float strip_scroll = 0;
    int drag_tab = -1; float drag_dx = 0; bool drag_moved = false;
    bool side_open = false, dock_shown = true;
    int side_page = 0;
    double side_anim = 0;
    UiText search{}; char search_buf[128] = "";
    float list_scroll = 0;
    std::unordered_map<std::string, int> up;
    std::string ctx_id;
    std::vector<std::unique_ptr<Dialog>> dialogs;
    int menu_hover = 0;
    double next_probe = 0, next_hud = 0, next_trim = 0;
    bool fast_cat = true;
    bool quit_on_exit = false;
    char hud_cache_tip[448] = "", hud_parse_tip[256] = "", title_last[400] = "";
    bool lite = false;
    uint64_t hud_prev_reads = 0;
    char hud_seg[11][256] = {};
    uint64_t hud_prev_bytes = 0, trim_prev_bytes = 0;
    double hud_prev_time = 0;
    long hud_prev_ticks = 0;
    uint64_t frames = 0, hud_prev_frames = 0;
    uint64_t hud_prev_irq = 0, hud_irq_now = 0;
    bool hud_irq_total = false;
    bool trim_dirty = false;
    bool running = false, dirty = false, blocked_for_term = false;
    bool blink_on = true;
    double last_blink = 0;
};

extern App *g_app;
void app_redraw(App *a);
void app_wake(void);
void app_refresh(App *a);
Tab *app_cur_tab(App *a);
Tab *app_open_connection(App *a, const SdConn &c);
Tab *app_open_local(App *a, const std::string &title, const std::vector<std::string> &argv, const char *cwd);
void app_close_tab(App *a, Tab *t);
void app_settings_save(App *a);
void app_apply_theme(App *a, const UiTheme *t);
void app_follow_fleetwm(App *a);
void app_apply_font(App *a);
void app_set_accent(App *a, const std::string &hex);
void app_dup_tab(App *a, Tab *t);
void app_set_tab_color(App *a, Tab *t, const std::string &hex);
std::vector<std::string> app_default_shell_argv(const std::string &shell);

void dlg_push(App *a, std::unique_ptr<Dialog> d);
bool dlg_active(const App *a);
void dlg_draw_top(App *a);
Rect dlg_frame(App *a, const char *title, float w, float h);
using TextCb = std::function<void(const std::string &text)>;
void dlg_text(App *a, const std::string &title, const std::string &label, const std::string &initial, bool password, TextCb cb);
using ConfirmCb = std::function<void()>;
void dlg_confirm(App *a, const std::string &title, const std::string &msg, const std::string &ok, ConfirmCb cb);
void dlg_message(App *a, const std::string &title, const std::string &msg);
using ChoiceCb = std::function<void(int index)>;
void dlg_choice(App *a, const std::string &title, const std::string &msg, const std::vector<std::string> &labels, ChoiceCb cb);
/* Called with the chosen paths, or with an empty list when the dialog is cancelled. */
using PickCb = std::function<void(const std::vector<std::string> &paths)>;
void dlg_pick_file(App *a, const std::string &title, bool folder, bool multi, const std::string &start, PickCb cb);
void dlg_conn_editor(App *a, const SdConn *c, bool copy_of_live);
void dlg_about(App *a);
void app_fastcat_apply(App *a);
void dlg_color(App *a, const std::string &title, const std::string &initial, TextCb cb);

/* The SFTP/FTP file browser of one tab. Background work holds a shared_ptr, so a pane closed mid-transfer stays valid until the
 * task has finished and then drops its result. */
class FilesPane : public std::enable_shared_from_this<FilesPane> {
public:
    static std::shared_ptr<FilesPane> create(App *a, std::unique_ptr<SdXfer> x, bool follow);
    ~FilesPane();

    void close();
    void draw(App *a, Rect r);
    void set_cwd(const std::string &path);
    void note_activity() { active_ = true; }
    bool follow() const { return follow_; }
    void set_follow(bool v) { follow_ = v && !ftp_; }
    std::string label() const { return xfer_->label(); }
    void tick(App *a);
    void drop(const std::string &path);

private:
    struct Task;
    FilesPane(App *a, std::unique_ptr<SdXfer> x, bool follow);

    void set_status(const std::string &s);
    void navigate_to(const std::string *path);
    void run_task(std::shared_ptr<Task> t);
    void finish_task(Task &t);
    void sort_entries();
    std::vector<int> selected() const;
    void start_download(const std::vector<int> &entries, const std::string &dest);
    void start_upload(const std::vector<std::string> &paths);
    void act_download(App *a);
    void act_delete(App *a);
    void act_rename(App *a);
    void activate(int i);

    App *app_;
    std::unique_ptr<SdXfer> xfer_;
    std::string path_, pending_cwd_, status_;
    bool ready_ = false, osc7_seen_ = false, active_ = false, polling_ = false, follow_ = false, ftp_ = false, alive_ = true;
    double last_poll_ = 0, next_ready_ = 0, next_poll_ = 0, drawn_at_ = 0;
    EntryList ent_;
    std::vector<char> sel_;
    int last_click_ = -1;
    float scroll_ = 0;
    int sort_col_ = 0;
    bool sort_desc_ = false;
    char pathbuf_[1024] = "";
    UiText pathin_{};
};
#endif
