/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_VT_H
#define SD_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VT_BOLD = 1, VT_DIM = 2, VT_ITALIC = 4, VT_UNDERLINE = 8, VT_BLINK = 16, VT_REVERSE = 32,
    VT_INVIS = 64, VT_STRIKE = 128
};

#define VT_DEFAULT_COLOR 0u
#define VT_COLOR_IDX(i) (0x01000000u | (uint32_t)(i))
#define VT_COLOR_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define VT_COLOR_KIND(c) ((c) >> 24)

typedef struct { uint32_t fg, bg; uint16_t attrs; } VtStyle;

typedef struct { uint32_t cp, sf; } VtCell;
enum { VT_F_WIDE = 1, VT_F_TAIL = 2 };
#define VT_CELL_CH(c) ((c).cp & 0x1FFFFFu)
#define VT_CELL_COMB(c) ((c).cp >> 21)
#define VT_CELL_STYLE(c) ((c).sf >> 8)
#define VT_CELL_FLAGS(c) ((c).sf & 0xFFu)

typedef struct {
    uint8_t dirty;   /* 0 clean, 1 dirty, 2 dirty and the row holds only plain ASCII of one style (vt.c shadow bytes) */
    uint16_t hw;
    uint32_t sf;     /* style of that ASCII row, valid while dirty == 2 */
    void *cache;
} VtLineMeta;

enum {
    VT_M_AUTOWRAP = 1u << 0, VT_M_ORIGIN = 1u << 1, VT_M_INSERT = 1u << 2, VT_M_NEWLINE = 1u << 3,
    VT_M_APP_CURSOR = 1u << 4, VT_M_CURSOR_VISIBLE = 1u << 5, VT_M_BRACKETED_PASTE = 1u << 6,
    VT_M_MOUSE_BTN = 1u << 7, VT_M_MOUSE_DRAG = 1u << 8, VT_M_MOUSE_ANY = 1u << 9, VT_M_MOUSE_SGR = 1u << 10,
    VT_M_ALT_SCREEN = 1u << 11, VT_M_APP_KEYPAD = 1u << 12, VT_M_FOCUS_EVENTS = 1u << 13,
    VT_M_REVERSE_VIDEO = 1u << 14
};

typedef enum { VT_EV_TITLE, VT_EV_CWD, VT_EV_BELL } VtEvent;

typedef struct Vt Vt;
typedef void (*VtWriteFn)(const uint8_t *data, size_t len, void *user);
typedef void (*VtEventFn)(VtEvent ev, const char *text, void *user);
typedef void (*VtCacheFree)(void *cache, void *user);

Vt *vt_new(int cols, int rows, int scrollback_lines);
void vt_free(Vt *t);
void vt_set_callbacks(Vt *t, VtWriteFn write, VtEventFn event, VtCacheFree cache_free, void *user);
void vt_resize(Vt *t, int cols, int rows);
void vt_feed(Vt *t, const uint8_t *data, size_t len);
size_t vt_feed_stream(Vt *t, const uint8_t *data, size_t len);
void vt_set_stream_token(Vt *t, const char *token);
void vt_set_threads(int parse_workers, int compress_workers);
bool vt_take_stream(Vt *t, char *path, size_t cap, uint64_t *off, uint64_t *len, unsigned *flags);
void vt_reset(Vt *t);

int vt_cols(const Vt *t);
int vt_rows(const Vt *t);
uint32_t vt_modes(const Vt *t);
int vt_cursor_x(const Vt *t);
int vt_cursor_y(const Vt *t);
int vt_cursor_style(const Vt *t);
int vt_history_count(const Vt *t);

VtCell *vt_line(Vt *t, int idx, int *len);
VtLineMeta *vt_line_meta(Vt *t, int idx);
const VtStyle *vt_style(const Vt *t, uint32_t id);
uint32_t vt_comb_char(const Vt *t, unsigned idx);
void vt_mark_all_dirty(Vt *t);
void vt_clear_history(Vt *t);
size_t vt_memory_used(const Vt *t);

void vt_set_history(Vt *t, int max_lines, size_t ram_budget, size_t disk_budget, bool spill);
typedef struct {
    long lines;
    long hot_lines;
    long block_count;
    size_t hot_bytes;
    size_t packed_bytes;
    size_t disk_bytes;
    size_t cache_bytes;
    size_t raw_bytes;
} VtHistoryStats;
void vt_history_stats(const Vt *t, VtHistoryStats *out);
size_t vt_compact(Vt *t);

void vt_set_fast_paths(bool on);
void vt_set_force_nl(Vt *t, bool on);
void vt_set_stream_nl(Vt *t, bool on);

int vt_wcwidth(uint32_t cp);

#ifdef __cplusplus
}
#endif
#endif
