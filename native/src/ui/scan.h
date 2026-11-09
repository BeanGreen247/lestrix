/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#ifndef SD_SCAN_H
#define SD_SCAN_H
/* Text scanning on terminal rows, free of any UI or window code so it can be tested alone: URL detection under the pointer, the
 * scrollback finder (incremental, sliced so a huge history never stalls the UI) and the split-pane layout arithmetic. */
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "../vt.h"

/* URL under column `col` of a row of cells (http, https, ftp, file, mailto). On success fills the column range [*c0, *c1] and the
 * URL text (trailing punctuation and an unbalanced closing bracket are left out). A URL cut by the end of the row is not joined. */
bool scan_row_url(const VtCell *cells, int len, int col, int *c0, int *c1, std::string *url);
/* Only these schemes are ever handed to xdg-open, and no control characters. */
bool scan_url_allowed(const char *url);

/* The cells of a row as lower-cased code points, one per cell that starts a character (wide-character tails dropped, empty cells are
 * spaces), with the column each one starts in and how many columns it covers. */
struct ScanRow { std::vector<uint32_t> cp; std::vector<int> col; std::vector<uint8_t> w; };
void scan_row_build(const VtCell *cells, int len, bool fold, ScanRow *out);
uint32_t scan_fold(uint32_t cp);
size_t scan_utf8_decode(const char *s, std::vector<uint32_t> *out);

struct FindMatch { int idx, col, len; };   /* idx as vt_line (0.. screen rows, negative = history), col and len in columns */

class Finder {
public:
    /* Starts a new search; any earlier results are dropped. An empty query finds nothing. */
    void start(const char *utf8, bool case_sensitive, bool whole_word);
    /* Scans lines from the newest back to the oldest until the time budget (microseconds) is used up. The caller holds the terminal
     * lock. True when the whole history has been scanned. */
    bool step(Vt *vt, int budget_us);
    bool done() const { return done_; }
    bool active() const { return !needle_.empty(); }
    /* Matches newest first, so m[0] is the match nearest the bottom. */
    const std::vector<FindMatch> &matches() const { return m_; }
    long scanned() const { return scanned_; }
    bool truncated() const { return truncated_; }

private:
    std::vector<uint32_t> needle_;
    std::vector<FindMatch> m_;
    ScanRow row_;
    bool cs_ = false, word_ = false, done_ = true, truncated_ = false, began_ = false;
    int next_ = 0, last_ = 0;
    long scanned_ = 0;
};

/* Split-pane layout: a binary tree of leaves, every inner node splits its rectangle in two by `ratio`. */
struct PaneRect { float x, y, w, h; };
enum { PANE_MIN_W = 120, PANE_MIN_H = 80, PANE_GAP = 4 };
/* Rectangle of the first and second child of a split of r. `horiz` puts the children side by side. A gap of PANE_GAP pixels between
 * them is the divider. */
void pane_split_rect(PaneRect r, bool horiz, float ratio, PaneRect *first, PaneRect *second);
/* A ratio that keeps both children at least the minimum size, for the divider dragged to pixel position `pos`. */
float pane_ratio_for(PaneRect r, bool horiz, float pos);
#endif
