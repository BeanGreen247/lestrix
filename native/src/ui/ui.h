/* ui.h - a small immediate-mode toolkit on top of the renderer: text, buttons, text inputs, scroll areas, menus,
 * tooltips, icons. No toolkit library: everything is drawn by Lestrix with the same shader as the terminal. */
#ifndef SD_UI_H
#define SD_UI_H
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "render.h"

typedef struct { float x, y, w, h; } Rect;
static inline Rect R(float x, float y, float w, float h) { return (Rect){x, y, w, h}; }
static inline bool rect_has(Rect r, float px, float py) { return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h; }
static inline Rect rect_inset(Rect r, float d) { return (Rect){r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d}; }

typedef struct {
    uint32_t bg0, bg1, bg2, ink, ink2, muted, border, border2, accent, on_accent, hover, up, down, blue, red, term_bg;
} UiColors;

typedef enum {
    IC_NONE, IC_PIN, IC_CLOSE, IC_PLUS, IC_COPY, IC_COPYALL, IC_PASTE, IC_DUPLICATE, IC_TERMINAL, IC_SERVER, IC_FOLDER, IC_FILE,
    IC_CHEV_R, IC_CHEV_D, IC_CHEV_L, IC_UP, IC_REFRESH, IC_UPLOAD, IC_DOWNLOAD, IC_SEARCH, IC_TRASH, IC_EDIT, IC_CHECK,
    IC_LINK, IC_HOME, IC_NEWFOLDER, IC_COUNT
} UiIcon;

typedef struct Ui Ui;

typedef struct { char *s; int cap, len, caret, anchor; float scroll; } UiText;
void ui_text_init(UiText *t, char *buf, int cap);
void ui_text_set(UiText *t, const char *s);

enum { MI_SEP = 1, MI_CHECK = 2, MI_CHECKED = 4, MI_DISABLED = 8, MI_HEADER = 16 };
typedef struct {
    const char *label, *hint;     /* hint: shortcut text on the right */
    int id, flags;
    const uint32_t *swatch;       /* a row of colour swatches instead of a label; result id = id + index */
    int nswatch;
} UiMenuItem;

Ui *ui_new(Renderer *r, void *font /* Font* */, void *mono /* Font* for code-like text */, float scale);
void ui_free(Ui *u);
void ui_set_colors(Ui *u, const UiColors *c);
void ui_set_rounded(Ui *u, bool rounded);          /* false: every corner is square (fleetwm's "sharp" corner style) */
const UiColors *ui_colors(const Ui *u);
float ui_scale(const Ui *u);
float S(const Ui *u, float v);                     /* logical to device pixels */
void ui_event(Ui *u, const SDL_Event *e);
void ui_begin(Ui *u, int w, int h, double now);
void ui_end(Ui *u);                                /* draws tooltips and the open menu */
bool ui_animating(const Ui *u);                    /* something wants more frames */
void ui_want_frames(Ui *u, double seconds);
void ui_set_blocked(Ui *u, bool b);                /* ignore the mouse for widgets until set false again */
bool ui_blocked(const Ui *u);
SDL_Cursor *ui_wanted_cursor(Ui *u);
void ui_set_cursor(Ui *u, int kind);               /* 0 arrow, 1 text, 2 hand */

/* input */
float ui_mx(const Ui *u);
float ui_my(const Ui *u);
bool ui_hover(const Ui *u, Rect r);
bool ui_mouse_pressed(const Ui *u, int button, Rect r);     /* pressed this frame inside r */
bool ui_mouse_released(const Ui *u, int button);
bool ui_mouse_down(const Ui *u, int button);
int ui_clicks(const Ui *u);
float ui_wheel(const Ui *u);                                 /* consumed by whoever calls ui_take_wheel */
float ui_take_wheel(Ui *u, Rect r);
bool ui_keyboard_taken(const Ui *u);                         /* a text widget (or dialog) owns the keyboard */
bool ui_key(Ui *u, SDL_Keycode k, int mods);                 /* pressed this frame */
const char *ui_text_in(const Ui *u);
void ui_release_focus(Ui *u);

/* text */
float ui_text_w(Ui *u, const char *s);
float ui_line_h(const Ui *u);
void ui_text(Ui *u, float x, float y, const char *s, uint32_t color);          /* y = top of the line box */
void ui_text_fit(Ui *u, Rect r, const char *s, uint32_t color, int align);    /* clipped with an ellipsis; align 0 left 1 centre 2 right */
float ui_text_mono_w(Ui *u, const char *s);
void ui_text_mono(Ui *u, float x, float y, const char *s, uint32_t color);
void ui_text_font(Ui *u, void *font /* Font* */, int style, float x, float y, const char *s, uint32_t color);
float ui_text_font_w(Ui *u, void *font, int style, const char *s);
/* text turned 90 degrees (reads bottom to top), centred in r; cached as a bitmap */
void ui_text_vertical(Ui *u, Rect r, const char *s, uint32_t color);
float ui_text_vertical_len(Ui *u, const char *s);

/* drawing */
void ui_rect(Ui *u, Rect r, uint32_t c);
void ui_rrect(Ui *u, Rect r, uint32_t c, float radius);
void ui_outline(Ui *u, Rect r, uint32_t c, float radius, float width);
void ui_icon(Ui *u, UiIcon ic, Rect r, uint32_t color);
void ui_clip(Ui *u, Rect r);
void ui_unclip(Ui *u);

/* widgets */
enum { UB_PRIMARY = 1, UB_FLAT = 2, UB_ACTIVE = 4, UB_DISABLED = 8 };
bool ui_button(Ui *u, Rect r, const char *label, int flags);
bool ui_icon_button(Ui *u, Rect r, UiIcon ic, const char *tip, int flags);
bool ui_checkbox(Ui *u, Rect r, const char *label, bool *v);
bool ui_input(Ui *u, Rect r, UiText *t, const char *placeholder, bool password);   /* true when Enter was pressed */
bool ui_input_focused(const Ui *u, const UiText *t);
void ui_input_focus(Ui *u, const UiText *t);
void ui_tip(Ui *u, Rect r, const char *text);
void ui_scroll_begin(Ui *u, Rect view, float content_h, float *offset);
void ui_scroll_end(Ui *u);

/* menus: one at a time, drawn by ui_end. ui_menu_take returns the chosen id (once) or -1. */
void ui_menu_open(Ui *u, float x, float y, const UiMenuItem *items, int n, int tag);
bool ui_menu_is_open(const Ui *u);
int ui_menu_tag(const Ui *u);                     /* tag of the open menu, or -1 */
void ui_menu_close(Ui *u);
int ui_menu_take(Ui *u, int *tag);
int ui_menu_peek(const Ui *u, int *tag);          /* the pending result and its tag, not consumed */
#endif
