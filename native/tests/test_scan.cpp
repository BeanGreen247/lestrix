/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "../src/ui/scan.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void feed(Vt *t, const char *s) { vt_feed(t, (const uint8_t *)s, strlen(s)); }

static std::vector<VtCell> cells_of(const char *s) {
    std::vector<VtCell> v;
    for (; *s; s++) v.push_back({(uint32_t)(unsigned char)*s, 0});
    return v;
}

/* the URL found with the pointer on the first occurrence of `at` in text, or "" */
static std::string url_in(const char *text, const char *at, int *c0 = nullptr, int *c1 = nullptr) {
    std::vector<VtCell> c = cells_of(text);
    const char *p = strstr(text, at);
    if (!p) return "?";
    int a, b;
    std::string u;
    if (!scan_row_url(c.data(), (int)c.size(), (int)(p - text), &a, &b, &u)) return "";
    if (c0) *c0 = a;
    if (c1) *c1 = b;
    return u;
}

static void test_urls() {
    int a, b;
    CHECK(url_in("see https://example.com/a/b?x=1#top for more", "example", &a, &b) == "https://example.com/a/b?x=1#top");
    CHECK(a == 4 && b == 34);
    CHECK(url_in("see https://example.com. Next", "com") == "https://example.com");
    CHECK(url_in("(see http://example.com/x)", "example") == "http://example.com/x");
    CHECK(url_in("(http://example.com/a_(b))", "example") == "http://example.com/a_(b)");
    CHECK(url_in("<http://example.com/>", "example") == "http://example.com/");
    CHECK(url_in("\"http://example.com/q\", then", "example") == "http://example.com/q");
    CHECK(url_in("ftp://host/file.tar.gz!", "host") == "ftp://host/file.tar.gz");
    CHECK(url_in("file:///home/me/a.txt", "home") == "file:///home/me/a.txt");
    CHECK(url_in("mail mailto:bean@example.com now", "bean") == "mailto:bean@example.com");
    CHECK(url_in("HTTP://UPPER.example", "UPPER") == "HTTP://UPPER.example");
    CHECK(url_in("see https://example.com/a for more", "see") == "");        /* pointer on plain text */
    CHECK(url_in("see https://example.com/a for more", "for") == "");
    CHECK(url_in("see https://example.com/a for more", "https") == "https://example.com/a");   /* pointer on the scheme */
    CHECK(url_in("xhttp://nope.example", "nope") == "");                     /* scheme glued to a word */
    CHECK(url_in("http:// alone", "http") == "");                            /* nothing after the scheme */
    CHECK(url_in("javascript:alert(1)", "alert") == "");
    CHECK(url_in("a http://one.example/x http://two.example/y", "two") == "http://two.example/y");
    CHECK(url_in("a http://one.example/x http://two.example/y", "one") == "http://one.example/x");
    CHECK(url_in("http://a.example/x.", "x.") == "http://a.example/x");
    /* wide characters shift columns: the column range is in cells */
    std::vector<VtCell> w = {{0x4e2d, VT_F_WIDE}, {0, VT_F_TAIL}, {' ', 0}};
    for (const char *s = "http://w.example"; *s; s++) w.push_back({(uint32_t)*s, 0});
    std::string u;
    CHECK(scan_row_url(w.data(), (int)w.size(), 6, &a, &b, &u) && u == "http://w.example" && a == 3 && b == 18);

    CHECK(scan_url_allowed("https://x.test/a"));
    CHECK(scan_url_allowed("HTTP://x.test"));
    CHECK(scan_url_allowed("mailto:me@x.test") && scan_url_allowed("file:///etc/hostname") && scan_url_allowed("ftp://h/f"));
    CHECK(!scan_url_allowed("javascript:alert(1)") && !scan_url_allowed("vscode://open?x") && !scan_url_allowed("-oProxyCommand=x"));
    CHECK(!scan_url_allowed("https://x.test/\x01") && !scan_url_allowed("http://") && !scan_url_allowed("") && !scan_url_allowed("https://x\ny"));
}

static void test_fold_and_decode() {
    CHECK(scan_fold('A') == 'a' && scan_fold('z') == 'z' && scan_fold('1') == '1');
    CHECK(scan_fold(0xC9) == 0xE9 && scan_fold(0xD7) == 0xD7 && scan_fold(0x416) == 0x436 && scan_fold(0x391) == 0x3B1);
    std::vector<uint32_t> v;
    scan_utf8_decode("a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80", &v);
    CHECK(v.size() == 4 && v[0] == 'a' && v[1] == 0xE9 && v[2] == 0x4E2D && v[3] == 0x1F600);
}

/* matches as "idx:col:len" strings, newest first */
static std::string all(Finder &f, Vt *t) {
    while (!f.step(t, 1000000)) {}
    std::string s;
    for (const FindMatch &m : f.matches()) { char b[48]; snprintf(b, sizeof b, "%d:%d:%d ", m.idx, m.col, m.len); s += b; }
    return s;
}

static void test_finder() {
    Vt *t = vt_new(30, 4, 1000);
    feed(t, "alpha beta\r\nBeta gamma beta\r\n\xe4\xb8\xadx beta\r\nlast Alpha");
    Finder f;
    f.start("beta", false, false);
    CHECK(all(f, t) == "2:4:4 1:0:4 1:11:4 0:6:4 ");   /* case-insensitive, newest row first, columns after a wide character are cell columns */
    f.start("beta", true, false);
    CHECK(all(f, t) == "2:4:4 1:11:4 0:6:4 ");
    f.start("Beta", true, false);
    CHECK(all(f, t) == "1:0:4 ");
    f.start("alpha", false, true);
    CHECK(all(f, t) == "3:5:5 0:0:5 ");
    feed(t, "\r\nbetamax beta_x xbeta beta.");
    f.start("beta", false, true);
    {
        std::string s = all(f, t);
        CHECK(s.compare(0, 7, "3:21:4 ") == 0);   /* the whole word before the full stop */
        CHECK(s.find("3:0:4") == std::string::npos && s.find("3:8:4") == std::string::npos && s.find("3:16:4") == std::string::npos);   /* betamax, beta_x, xbeta are not whole words */
    }
    f.start("", false, false);
    CHECK(f.done() && !f.active() && f.matches().empty());
    f.start("zzz", false, false);
    CHECK(all(f, t).empty() && f.done());

    /* history: write a numbered line per row, then find one that scrolled far back, through packed blocks and a tiny time budget */
    Vt *h = vt_new(40, 5, 5000);
    char b[64];
    for (int i = 0; i < 3000; i++) { snprintf(b, sizeof b, "line %d needle%d\r\n", i, i % 500); feed(h, b); }
    vt_compact(h);
    f.start("needle42", false, true);
    int steps = 0;
    while (!f.step(h, 50)) steps++;
    CHECK(steps >= 0 && f.done() && f.scanned() > 3000);
    size_t n1 = f.matches().size();
    CHECK(n1 == 6);   /* i = 42, 542, ... 2542 */
    for (size_t i = 1; i < f.matches().size(); i++) CHECK(f.matches()[i - 1].idx > f.matches()[i].idx);
    int len = 0;
    VtCell *l = f.matches().empty() ? nullptr : vt_line(h, f.matches().back().idx, &len);
    CHECK(l && VT_CELL_CH(l[5]) == '4' && VT_CELL_CH(l[6]) == '2' && VT_CELL_CH(l[7]) == ' ');   /* "line 42 needle42": the oldest match really is line 42 */
    /* restarting mid-scan drops the old results */
    f.start("line 7", false, false);
    f.step(h, 10);
    f.start("needle499", false, true);
    CHECK(all(f, h).find(":") != std::string::npos);
    vt_free(h);
    vt_free(t);
}

static void test_pane_math() {
    PaneRect r{0, 0, 1004, 600}, a, b;
    pane_split_rect(r, true, 0.5f, &a, &b);
    CHECK(a.x == 0 && a.w == 500 && b.x == 504 && b.w == 500 && a.h == 600 && b.h == 600);
    CHECK(a.w + PANE_GAP + b.w == r.w);
    pane_split_rect(r, false, 0.25f, &a, &b);
    CHECK(a.y == 0 && a.h == 149 && b.y == 153 && b.h == 447 && a.w == 1004);
    CHECK(pane_ratio_for(r, true, 502) > 0.49f && pane_ratio_for(r, true, 502) < 0.51f);
    float lo = pane_ratio_for(r, true, -50), hi = pane_ratio_for(r, true, 5000);
    CHECK(lo * (r.w - PANE_GAP) >= PANE_MIN_W - 0.01f && (1 - hi) * (r.w - PANE_GAP) >= PANE_MIN_W - 0.01f);
    PaneRect tiny{0, 0, 200, 100};
    CHECK(pane_ratio_for(tiny, true, 30) == 0.5f);   /* too small to move the divider */
    /* a dragged ratio and the split give back the pointer position */
    float ratio = pane_ratio_for(r, true, 300);
    pane_split_rect(r, true, ratio, &a, &b);
    CHECK(a.w + PANE_GAP / 2.f > 299.5f && a.w + PANE_GAP / 2.f < 300.5f);
}

int main() {
    test_urls(); test_fold_and_decode(); test_finder(); test_pane_math();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
