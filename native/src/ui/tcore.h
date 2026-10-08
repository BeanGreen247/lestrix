/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_TCORE_H
#define SD_TCORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../vt.h"

enum { TM_SHIFT = 1, TM_ALT = 2, TM_CTRL = 4, TM_META = 8 };

typedef enum {
    TK_NONE = 0, TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT, TK_HOME, TK_END, TK_INSERT, TK_DELETE, TK_PGUP, TK_PGDN,
    TK_F1, TK_F2, TK_F3, TK_F4, TK_F5, TK_F6, TK_F7, TK_F8, TK_F9, TK_F10, TK_F11, TK_F12,
    TK_ENTER, TK_BACKSPACE, TK_TAB, TK_ESCAPE
} TKey;

typedef struct {
    void (*title)(void *user, const char *title);
    void (*cwd)(void *user, const char *path);
    void (*bell)(void *user);
    void (*activity)(void *user);
    void (*exited)(void *user, int status);
    void (*restarted)(void *user);
    void (*clip_set)(void *user, const char *text, bool primary);
    void (*clip_request)(void *user, bool primary);
    void (*wake)(void *user);
} TermHooks;

typedef struct TermCore TermCore;

void tcore_set_history_defaults(int lines, size_t ram_mb, size_t disk_mb, bool spill);
TermCore *tcore_new(char *const argv[], const char *cwd, int cols, int rows, const TermHooks *hooks, void *user, bool fastcat);
void tcore_free(TermCore *t);
void tcore_close(TermCore *t);
bool tcore_pump(TermCore *t);

Vt *tcore_vt(TermCore *t);
void tcore_lock(TermCore *t);
void tcore_unlock(TermCore *t);
int tcore_cols(const TermCore *t);
int tcore_rows(const TermCore *t);
void tcore_resize(TermCore *t, int cols, int rows);
bool tcore_running(const TermCore *t);
void tcore_restart(TermCore *t);
uint64_t tcore_bytes_fed(const TermCore *t);
void tcore_set_fast_output(bool on);
void tcore_cmd_begin(TermCore *t, double now);
bool tcore_cmd_active(const TermCore *t);
bool tcore_cmd_poll(TermCore *t, double now, double *secs, uint64_t *bytes);
extern const char *tcore_cat_dir;
extern int tcore_io_threads;
extern int tcore_read_delay_us;
uint64_t tcore_reads(const TermCore *t);
uint32_t tcore_modes(TermCore *t);
const char *tcore_title(const TermCore *t);
void tcore_set_focus(TermCore *t, bool focused);
bool tcore_focused(const TermCore *t);

void tcore_send(TermCore *t, const char *data, size_t len);
void tcore_send_str(TermCore *t, const char *s);

int tcore_offset(const TermCore *t);
void tcore_scroll(TermCore *t, int delta);
void tcore_scroll_to_live(TermCore *t);

bool tcore_key(TermCore *t, TKey key, uint32_t cp, int mods);
void tcore_text(TermCore *t, const char *utf8);
void tcore_mouse_button(TermCore *t, int button, bool press, int col, int row, int clicks, int mods);
void tcore_mouse_move(TermCore *t, int col, int row, int mods);
void tcore_wheel(TermCore *t, double dy, int col, int row, int mods);
void tcore_paste(TermCore *t, const char *text);
void tcore_copy(TermCore *t);
void tcore_paste_request(TermCore *t);
void tcore_copy_all(TermCore *t);

bool tcore_has_selection(TermCore *t);
bool tcore_selection(TermCore *t, int *a_idx, int *a_col, int *b_idx, int *b_col);
void tcore_clear_selection(TermCore *t);
char *tcore_selection_text(TermCore *t);

void tcore_set_history(TermCore *t, int lines, size_t ram_mb, size_t disk_mb, bool spill);
void tcore_history_stats(TermCore *t, VtHistoryStats *out);
size_t tcore_compact(TermCore *t);
bool tcore_screen_contains(TermCore *t, const char *needle);
#endif
