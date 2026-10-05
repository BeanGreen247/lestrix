/* termview.h - draws a TermCore into a Renderer. */
#ifndef SD_TERMVIEW_H
#define SD_TERMVIEW_H
#include <stdbool.h>
#include "render.h"
#include "tcore.h"

typedef struct {
    uint32_t pal[256];                 /* RGBA, see RGBA() */
    uint32_t fg, bg, sel, cursor;
    uint32_t epoch;                    /* bump whenever any colour changes: cached rows are rebuilt */
} TermPalette;

/* Draw the terminal into the rectangle (x, y, w, h) in pixels. Returns true when something was drawn that still
 * changes (nothing today), so callers can keep the frame loop simple. cell_w/cell_h come from the font. */
void tv_draw(TermCore *t, Renderer *r, Font *f, const TermPalette *pal, float x, float y, float w, float h, bool cursor_blink_on);
/* grid size that fits a pixel size */
void tv_fit(const Font *f, float w, float h, int *cols, int *rows);
#define TV_PAD 4
#endif
