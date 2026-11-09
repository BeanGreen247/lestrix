/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "scan.h"

#include <chrono>
#include <ctype.h>
#include <string.h>

uint32_t scan_fold(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) || (c >= 0x410 && c <= 0x42F)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x178 && c != 0x17F) {
        bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        if (odd_upper) return (c & 1) ? c + 1 : c;
        return (c & 1) ? c : c + 1;
    }
    return c;
}

size_t scan_utf8_decode(const char *s, std::vector<uint32_t> *out) {
    out->clear();
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p;
        int n = 0;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
        else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
        else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
        else { p++; continue; }
        p++;
        for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++) c = (c << 6) | (*p & 0x3F);
        out->push_back(c);
    }
    return out->size();
}

void scan_row_build(const VtCell *cells, int len, bool fold, ScanRow *o) {
    o->cp.clear(); o->col.clear(); o->w.clear();
    int last = len;
    while (last > 0 && VT_CELL_CH(cells[last - 1]) <= 0x20) last--;
    for (int x = 0; x < last; x++) {
        if (VT_CELL_FLAGS(cells[x]) & VT_F_TAIL) continue;
        uint32_t c = VT_CELL_CH(cells[x]);
        if (c < 0x20) c = 0x20;
        o->cp.push_back(fold ? scan_fold(c) : c);
        o->col.push_back(x);
        o->w.push_back((VT_CELL_FLAGS(cells[x]) & VT_F_WIDE) ? 2 : 1);
    }
}

static bool url_char(uint32_t c) {
    if (c >= 0x80 || c <= 0x20) return false;
    switch (c) { case '"': case '<': case '>': case '\\': case '^': case '`': case '{': case '}': case '|': return false; }
    return true;
}

static const char *const SCHEMES[] = {"https://", "http://", "ftp://", "file://", "mailto:"};

static bool scheme_at(const ScanRow &r, size_t p, size_t *slen) {
    for (const char *s : SCHEMES) {
        size_t n = strlen(s), k = 0;
        if (p + n > r.cp.size()) continue;
        while (k < n && scan_fold(r.cp[p + k]) == (uint32_t)(unsigned char)s[k]) k++;
        if (k == n) { *slen = n; return true; }
    }
    return false;
}

bool scan_row_url(const VtCell *cells, int len, int col, int *c0, int *c1, std::string *url) {
    ScanRow r;
    scan_row_build(cells, len, false, &r);
    size_t n = r.cp.size(), i = 0;
    while (i < n && r.col[i] + r.w[i] - 1 < col) i++;
    if (i >= n || r.col[i] > col || !url_char(r.cp[i])) return false;
    size_t lo = i, hi = i;
    while (lo > 0 && url_char(r.cp[lo - 1])) lo--;
    while (hi + 1 < n && url_char(r.cp[hi + 1])) hi++;
    size_t start = n, slen = 0;
    for (size_t p = hi + 1; p-- > lo;) {
        size_t sl;
        if (p <= i && scheme_at(r, p, &sl) && (p == lo || !(r.cp[p - 1] < 0x80 && (isalnum((int)r.cp[p - 1]) || r.cp[p - 1] == '+' || r.cp[p - 1] == '-' || r.cp[p - 1] == '.')))) { start = p; slen = sl; break; }
    }
    if (start == n) return false;
    size_t end = hi;
    for (;;) {   /* trailing punctuation and an unbalanced bracket belong to the sentence, not the address */
        if (end < start + slen) break;
        uint32_t c = r.cp[end];
        bool drop = c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'' || c == '*';
        if (!drop && (c == ')' || c == ']')) {
            uint32_t open = c == ')' ? '(' : '[';
            int bal = 0;
            for (size_t k = start; k <= end; k++) { if (r.cp[k] == open) bal++; else if (r.cp[k] == c) bal--; }
            drop = bal < 0;
        }
        if (!drop) break;
        if (end == 0) return false;
        end--;
    }
    if (end < start + slen || i > end) return false;   /* nothing after the scheme, or the pointer is on the trimmed tail */
    url->clear();
    for (size_t k = start; k <= end; k++) url->push_back((char)r.cp[k]);
    *c0 = r.col[start];
    *c1 = r.col[end] + r.w[end] - 1;
    return true;
}

bool scan_url_allowed(const char *u) {
    size_t n = strlen(u);
    if (n == 0 || n > 4000) return false;
    for (const unsigned char *q = (const unsigned char *)u; *q; q++) if (*q < 0x20 || *q == 0x7f) return false;
    static const char *const ok[] = {"http://", "https://", "ftp://", "file://", "mailto:"};
    for (const char *s : ok) {
        size_t k = strlen(s), j = 0;
        if (n <= k) continue;
        while (j < k && (unsigned char)(u[j] | 0x20) == (unsigned char)s[j]) j++;
        if (j == k) return true;
    }
    return false;
}

void Finder::start(const char *utf8, bool cs, bool word) {
    cs_ = cs; word_ = word;
    scan_utf8_decode(utf8, &needle_);
    if (!cs_) for (uint32_t &c : needle_) c = scan_fold(c);
    m_.clear();
    done_ = needle_.empty();
    truncated_ = false; began_ = false; scanned_ = 0;
}

static bool is_word(uint32_t c) { return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool Finder::step(Vt *vt, int budget_us) {
    if (done_) return true;
    if (!began_) { began_ = true; next_ = vt_rows(vt) - 1; last_ = -vt_history_count(vt); }
    auto t0 = std::chrono::steady_clock::now();
    const size_t nm = needle_.size();
    while (next_ >= last_) {
        int len = 0;
        VtCell *line = vt_line(vt, next_, &len);
        if (line && len > 0) {
            scan_row_build(line, len, !cs_, &row_);
            const std::vector<uint32_t> &h = row_.cp;
            if (h.size() >= nm) {
                for (size_t i = 0; i + nm <= h.size(); i++) {
                    if (h[i] != needle_[0]) continue;
                    size_t k = 1;
                    while (k < nm && h[i + k] == needle_[k]) k++;
                    if (k < nm) continue;
                    if (word_ && ((i > 0 && is_word(h[i - 1]) && is_word(h[i])) || (i + nm < h.size() && is_word(h[i + nm]) && is_word(h[i + nm - 1])))) continue;
                    if (m_.size() >= 200000) { truncated_ = true; done_ = true; return true; }
                    int c0 = row_.col[i], c1 = row_.col[i + nm - 1] + row_.w[i + nm - 1];
                    m_.push_back({next_, c0, c1 - c0});
                    i += nm - 1;   /* matches do not overlap */
                }
            }
        }
        next_--;
        scanned_++;
        if ((scanned_ & 63) == 0 && std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() >= budget_us) return false;
    }
    done_ = true;
    return true;
}

void pane_split_rect(PaneRect r, bool horiz, float ratio, PaneRect *a, PaneRect *b) {
    if (horiz) {
        float w1 = (r.w - PANE_GAP) * ratio;
        *a = {r.x, r.y, w1, r.h};
        *b = {r.x + w1 + PANE_GAP, r.y, r.w - PANE_GAP - w1, r.h};
    } else {
        float h1 = (r.h - PANE_GAP) * ratio;
        *a = {r.x, r.y, r.w, h1};
        *b = {r.x, r.y + h1 + PANE_GAP, r.w, r.h - PANE_GAP - h1};
    }
}

float pane_ratio_for(PaneRect r, bool horiz, float pos) {
    float total = (horiz ? r.w : r.h) - PANE_GAP, min = horiz ? PANE_MIN_W : PANE_MIN_H;
    if (total <= 2 * min) return 0.5f;
    float v = pos - (horiz ? r.x : r.y) - PANE_GAP / 2.f;
    if (v < min) v = min;
    if (v > total - min) v = total - min;
    return v / total;
}
