/* vt.h - terminal emulation core: parser, screen, scrollback.
 *
 * Pure C, no dependencies. Cells are 8 bytes: a code point and a style id, with colours and
 * attributes interned in a small table. Screen rows are slots addressed through an order
 * array, so scrolling and line insertion rotate a few integers instead of copying cells, and
 * each slot keeps its dirty flag and renderer cache while it moves. Lines that scroll off the
 * top are stored compactly (trailing blanks trimmed) in a ring of up to `scrollback` lines. */
#ifndef SD_VT_H
#define SD_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Style attributes (VtStyle.attrs) */
enum {
    VT_BOLD = 1, VT_DIM = 2, VT_ITALIC = 4, VT_UNDERLINE = 8, VT_BLINK = 16, VT_REVERSE = 32,
    VT_INVIS = 64, VT_STRIKE = 128
};

/* Colours: 0 = theme default, 0x01000000|i = palette entry i (0-255), 0x02000000|rgb = truecolor */
#define VT_DEFAULT_COLOR 0u
#define VT_COLOR_IDX(i) (0x01000000u | (uint32_t)(i))
#define VT_COLOR_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define VT_COLOR_KIND(c) ((c) >> 24)

typedef struct { uint32_t fg, bg; uint16_t attrs; } VtStyle;

/* A cell: cp = code point (low 21 bits, 0 = blank) | combining-mark index << 21;
 * sf = style id << 8 | flags. A zero cell is a default-styled blank. */
typedef struct { uint32_t cp, sf; } VtCell;
enum { VT_F_WIDE = 1, VT_F_TAIL = 2 };   /* WIDE: first half of a double-width char; TAIL: its second half */
#define VT_CELL_CH(c) ((c).cp & 0x1FFFFFu)
#define VT_CELL_COMB(c) ((c).cp >> 21)
#define VT_CELL_STYLE(c) ((c).sf >> 8)
#define VT_CELL_FLAGS(c) ((c).sf & 0xFFu)

typedef struct {
    uint8_t dirty;  /* set whenever the line's content changes; the renderer clears it */
    uint16_t hw;    /* high-water mark: cells at or beyond it are untouched blanks (internal) */
    void *cache;    /* owned by the renderer; released through VtCacheFree */
} VtLineMeta;

/* Terminal modes, as a bitmask from vt_modes() */
enum {
    VT_M_AUTOWRAP = 1u << 0, VT_M_ORIGIN = 1u << 1, VT_M_INSERT = 1u << 2, VT_M_NEWLINE = 1u << 3,
    VT_M_APP_CURSOR = 1u << 4, VT_M_CURSOR_VISIBLE = 1u << 5, VT_M_BRACKETED_PASTE = 1u << 6,
    VT_M_MOUSE_BTN = 1u << 7, VT_M_MOUSE_DRAG = 1u << 8, VT_M_MOUSE_ANY = 1u << 9, VT_M_MOUSE_SGR = 1u << 10,
    VT_M_ALT_SCREEN = 1u << 11, VT_M_APP_KEYPAD = 1u << 12, VT_M_FOCUS_EVENTS = 1u << 13,
    VT_M_REVERSE_VIDEO = 1u << 14
};

typedef enum { VT_EV_TITLE, VT_EV_CWD, VT_EV_BELL } VtEvent;

typedef struct Vt Vt;
typedef void (*VtWriteFn)(const uint8_t *data, size_t len, void *user);          /* replies to the program */
typedef void (*VtEventFn)(VtEvent ev, const char *text, void *user);
typedef void (*VtCacheFree)(void *cache, void *user);                            /* line cache released */

/* scrollback_lines: 0 = no history, -1 = unlimited (bounded by the memory/disk budgets below) */
Vt *vt_new(int cols, int rows, int scrollback_lines);
void vt_free(Vt *t);
void vt_set_callbacks(Vt *t, VtWriteFn write, VtEventFn event, VtCacheFree cache_free, void *user);
void vt_resize(Vt *t, int cols, int rows);
void vt_feed(Vt *t, const uint8_t *data, size_t len);
void vt_reset(Vt *t);

int vt_cols(const Vt *t);
int vt_rows(const Vt *t);
uint32_t vt_modes(const Vt *t);
int vt_cursor_x(const Vt *t);   /* may equal cols when a wrap is pending */
int vt_cursor_y(const Vt *t);
int vt_cursor_style(const Vt *t);
int vt_history_count(const Vt *t);

/* Line access. idx 0..rows-1 are screen rows; -1 is the newest history line, -2 the one above, ...
 * *len receives the number of cells stored (history lines are trimmed); cells past it are blank. */
VtCell *vt_line(Vt *t, int idx, int *len);
VtLineMeta *vt_line_meta(Vt *t, int idx);
const VtStyle *vt_style(const Vt *t, uint32_t id);
uint32_t vt_comb_char(const Vt *t, unsigned idx);
void vt_mark_all_dirty(Vt *t);
void vt_clear_history(Vt *t);
size_t vt_memory_used(const Vt *t);   /* bytes held by screens, history and tables (for diagnostics) */

/* History limits. max_lines: 0 = none, -1 = unlimited. History older than ~1000 lines is packed into
 * compressed blocks (compressed on worker threads). Compressed blocks beyond ram_budget bytes are
 * moved to an unlinked temporary file (spill) until disk_budget bytes; past that the oldest block is
 * dropped, so memory and disk use are always bounded. spill=false drops instead of spilling.
 * A budget of 0 keeps the default. Not thread-safe: call with the terminal's lock held. */
void vt_set_history(Vt *t, int max_lines, size_t ram_budget, size_t disk_budget, bool spill);
typedef struct {
    long lines;            /* total lines of history */
    long hot_lines;        /* lines held uncompressed */
    long block_count;
    size_t hot_bytes;      /* uncompressed lines */
    size_t packed_bytes;   /* compressed blocks held in memory */
    size_t disk_bytes;     /* compressed blocks spilled to the temporary file */
    size_t cache_bytes;    /* decoded blocks kept for viewing */
    size_t raw_bytes;      /* what the compressed history would take uncompressed */
} VtHistoryStats;
void vt_history_stats(const Vt *t, VtHistoryStats *out);
/* Give memory back: pack all but the newest lines, drop decoded blocks, shrink tables. Returns bytes freed (approx). */
size_t vt_compact(Vt *t);

/* Width of a code point in cells: 0 (combining), 1 or 2; -1 for non-printing */
int vt_wcwidth(uint32_t cp);

#ifdef __cplusplus
}
#endif
#endif
