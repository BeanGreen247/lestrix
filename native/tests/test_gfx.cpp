/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* Inline images: the Sixel decoder, the base64 and kitty command parsers and the image store hit directly with truncated, oversize and
 * garbage input (this binary is built with ASan and UBSan), then the same paths through a real terminal engine: placement, scrolling into
 * the history, erasing, the alternate screen, kitty chunking, deletion and the replies a program reads back. */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "../src/vgfx.h"
#include "png_samples.h"
#include "../src/vt.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint32_t RED = 0xff0000ffu, GREEN = 0xff00ff00u, BLUE = 0xffff0000u;   /* RGBA bytes read as a little-endian word */

static std::vector<uint8_t> S(const char *s) { return std::vector<uint8_t>(s, s + strlen(s)); }

static int sx(const char *s, int *w, int *h, uint32_t **px, size_t cap = vg::DEFAULT_CAP) {
    std::vector<uint8_t> v = S(s);
    return vg::sixel_decode(v.data(), v.size(), cap, w, h, px);
}

static void test_sixel() {
    int w = 0, h = 0;
    uint32_t *px = NULL;
    CHECK(sx("\"1;1;4;6#0;2;100;0;0#0!4~", &w, &h, &px) == vg::SX_OK && w == 4 && h == 6);
    if (px) { bool all = true; for (int i = 0; i < w * h; i++) all &= px[i] == RED; CHECK(all); free(px); px = NULL; }
    /* two bands, two colours, graphics CR ($) and NL (-) */
    CHECK(sx("#1;2;0;100;0#2;2;0;0;100#1~~$#2??-#1~", &w, &h, &px) == vg::SX_OK && w == 2 && h == 12);
    if (px) {
        CHECK(px[0] == GREEN);                 /* band 0: green written first ... */
        CHECK(px[5 * 2 + 0] == GREEN);         /* '~' sets all six rows */
        CHECK(px[6 * 2 + 0] == GREEN);         /* band 1 */
        CHECK(px[6 * 2 + 1] == 0);             /* never painted: transparent */
        free(px); px = NULL;
    }
    /* only the low bit: just the top pixel of the band */
    CHECK(sx("#0;2;100;100;100#0@", &w, &h, &px) == vg::SX_OK && w == 1 && h == 6);
    if (px) { CHECK(px[0] != 0 && px[1 * 1 + 0] == 0 && px[5] == 0); free(px); px = NULL; }
    /* HLS: hue 120 is red in the sixel wheel, 0 is blue, 240 green */
    CHECK(sx("#0;1;120;50;100#0~", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(px[0] == RED); free(px); px = NULL; }
    CHECK(sx("#0;1;0;50;100#0~", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(px[0] == BLUE); free(px); px = NULL; }
    CHECK(sx("#0;1;240;50;100#0~", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(px[0] == GREEN); free(px); px = NULL; }
    /* empty and data-free strings */
    CHECK(sx("", &w, &h, &px) == vg::SX_EMPTY);
    CHECK(sx("#0;2;1;2;3", &w, &h, &px) == vg::SX_EMPTY);
    CHECK(sx("$-$-", &w, &h, &px) == vg::SX_EMPTY);
    /* truncated in the middle of a repeat, a colour definition, a raster attribute */
    CHECK(sx("~!", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(w == 1); free(px); px = NULL; }
    CHECK(sx("~!5", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(w == 1); free(px); px = NULL; }
    CHECK(sx("~#", &w, &h, &px) == vg::SX_OK); if (px) free(px), px = NULL;
    CHECK(sx("~#0;2;10", &w, &h, &px) == vg::SX_OK); if (px) free(px), px = NULL;
    CHECK(sx("~\"1;1;", &w, &h, &px) == vg::SX_OK); if (px) free(px), px = NULL;
    /* a palette index past 255 defines and selects nothing; the colour in force stays */
    CHECK(sx("#3;2;100;0;0#3~#999;2;0;100;0#999~", &w, &h, &px) == vg::SX_OK && w == 2); if (px) { CHECK(px[0] == RED && px[1] == RED); free(px); px = NULL; }
    CHECK(sx("#4294967295;2;0;100;0~", &w, &h, &px) == vg::SX_OK); if (px) free(px), px = NULL;
    CHECK(sx("#-1~", &w, &h, &px) == vg::SX_OK); if (px) free(px), px = NULL;
    /* percentages over 100 clamp */
    CHECK(sx("#0;2;900;0;0#0~", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(px[0] == RED); free(px); px = NULL; }
    /* repeat over the dimension limit, a wall of newlines, a wall of columns */
    CHECK(sx("!99999~", &w, &h, &px) == vg::SX_TOO_BIG);
    CHECK(sx("!16385~", &w, &h, &px) == vg::SX_TOO_BIG);
    CHECK(sx("!16384~", &w, &h, &px) == vg::SX_OK); if (px) { CHECK(w == 16384 && h == 6); free(px); px = NULL; }
    { std::string nl = "~"; for (int i = 0; i < 20000; i++) nl += '-'; nl += "~"; std::vector<uint8_t> v(nl.begin(), nl.end()); CHECK(vg::sixel_decode(v.data(), v.size(), vg::DEFAULT_CAP, &w, &h, &px) == vg::SX_TOO_BIG); }
    /* the byte cap: 1000 x 1000 needs 4 MB */
    { std::string big = "!1000~"; for (int i = 0; i < 166; i++) big += "-!1000~"; std::vector<uint8_t> v(big.begin(), big.end());
      CHECK(vg::sixel_decode(v.data(), v.size(), (size_t)1 << 20, &w, &h, &px) == vg::SX_TOO_BIG);
      CHECK(vg::sixel_decode(v.data(), v.size(), (size_t)8 << 20, &w, &h, &px) == vg::SX_OK); if (px) { CHECK(w == 1000 && h == 167 * 6); free(px); px = NULL; } }
    /* a huge declared raster size changes nothing: only drawn pixels size the image */
    CHECK(sx("\"1;1;99999;99999#0;2;100;0;0#0~", &w, &h, &px) == vg::SX_OK && w == 1 && h == 6); if (px) free(px), px = NULL;
    /* garbage: never crashes, always a valid verdict */
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
    static const char alpha[] = "#;!\"$-?~@0123456789:\x1b\n\0 AZaz";
    for (int it = 0; it < 30000; it++) {
        size_t n = next() % 200;
        std::vector<uint8_t> v(n);
        for (auto &b : v) b = (next() & 3) ? (uint8_t)alpha[next() % (sizeof alpha - 1)] : (uint8_t)next();
        int rc = vg::sixel_decode(v.data(), v.size(), (size_t)4 << 20, &w, &h, &px);
        if (rc == vg::SX_OK) { CHECK(w > 0 && h > 0 && w <= vg::MAX_DIM && h <= vg::MAX_DIM && px); free(px); px = NULL; }
        else CHECK(!px);
    }
}

static void test_base64_and_kitty() {
    std::vector<uint8_t> out;
    vg::B64 b;
    vg::b64_feed(b, (const uint8_t *)"aGVsbG8gd29ybGQ=", 16, out, 100);
    CHECK(!b.bad && std::string(out.begin(), out.end()) == "hello world");
    /* split at every possible boundary gives the same bytes */
    for (size_t cut = 0; cut <= 16; cut++) {
        out.clear(); b = vg::B64();
        vg::b64_feed(b, (const uint8_t *)"aGVsbG8gd29ybGQ=", cut, out, 100);
        vg::b64_feed(b, (const uint8_t *)"aGVsbG8gd29ybGQ=" + cut, 16 - cut, out, 100);
        CHECK(!b.bad && std::string(out.begin(), out.end()) == "hello world");
    }
    out.clear(); b = vg::B64(); vg::b64_feed(b, (const uint8_t *)"aGV$bG8=", 8, out, 100); CHECK(b.bad);
    out.clear(); b = vg::B64(); vg::b64_feed(b, (const uint8_t *)"aGVs=bG8=", 9, out, 100); CHECK(b.bad);   /* data after padding */
    out.clear(); b = vg::B64(); vg::b64_feed(b, (const uint8_t *)"aGVsbG8gd29ybGQ=", 16, out, 4); CHECK(b.bad && out.size() <= 4);   /* output cap */
    out.clear(); b = vg::B64(); vg::b64_feed(b, (const uint8_t *)"", 0, out, 4); CHECK(!b.bad && out.empty());

    vg::KCmd k;
    const char *c = "a=T,f=24,s=10,v=20,i=7,I=9,p=3,c=4,r=2,x=1,y=2,w=3,h=4,z=-5,C=1,q=2,m=1";
    vg::kitty_parse((const uint8_t *)c, strlen(c), &k);
    CHECK(k.a == 'T' && k.f == 24 && k.s == 10 && k.v == 20 && k.i == 7 && k.I == 9 && k.p == 3 && k.c == 4 && k.r == 2 && k.x == 1 && k.y == 2 && k.w == 3 && k.h == 4);
    CHECK(k.z == -5 && k.C == 1 && k.q == 2 && k.m == 1 && k.has_i && k.has_I);
    vg::KCmd d;
    const char *g = "i=99999999999999999999,s=-3,zz=1,=,,x,a,t=f,z=99999999999,f=abc";
    vg::kitty_parse((const uint8_t *)g, strlen(g), &d);
    CHECK(d.i == 0xffffffffu && d.s == 0 && d.t == 'f' && d.z == 0x7fffffff && d.f == 32);
    vg::KCmd e;
    vg::kitty_parse((const uint8_t *)"", 0, &e);
    CHECK(e.a == 't' && !e.has_i);

    const char *err = NULL;
    uint8_t rgb[12] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 1, 2, 3};
    uint32_t *px = vg::kitty_pixels(rgb, sizeof rgb, 24, 2, 2, vg::DEFAULT_CAP, &err);
    CHECK(px && px[0] == RED && px[1] == GREEN && px[2] == BLUE); free(px);
    px = vg::kitty_pixels(rgb, 11, 24, 2, 2, vg::DEFAULT_CAP, &err); CHECK(!px && err && !strncmp(err, "EINVAL", 6));
    px = vg::kitty_pixels(rgb, sizeof rgb, 100, 2, 2, vg::DEFAULT_CAP, &err); CHECK(!px);   /* PNG goes through kitty_decode */
    px = vg::kitty_pixels(rgb, sizeof rgb, 7, 2, 2, vg::DEFAULT_CAP, &err); CHECK(!px);
    px = vg::kitty_pixels(rgb, sizeof rgb, 24, 100000, 100000, vg::DEFAULT_CAP, &err); CHECK(!px && !strncmp(err, "EFBIG", 5));
    px = vg::kitty_pixels(rgb, sizeof rgb, 24, 0, 5, vg::DEFAULT_CAP, &err); CHECK(!px);
    px = vg::kitty_pixels(rgb, sizeof rgb, 24, 2, 2, 8, &err); CHECK(!px);   /* over the byte cap */
}

static void test_store() {
    vg::Store s;
    s.cap = 4000;
    auto mk = [](int w, int h) { return static_cast<uint32_t *>(calloc((size_t)w * h, 4)); };
    vg::Image *a = s.add_image(10, 10, mk(10, 10), 1, 0);   /* 400 bytes */
    vg::Image *b = s.add_image(10, 10, mk(10, 10), 2, 0);
    vg::Place pl; pl.img = a; s.add_place(pl); pl.img = b; s.add_place(pl);
    CHECK(s.bytes == 800 && s.places.size() == 2 && s.by_kid(1) == a && s.by_kid(2) == b && !s.by_kid(3) && !s.by_kid(0));
    /* 3200 more does not fit next to 800: the least recently used image (a) is evicted with its placement */
    vg::Image *c = s.add_image(40, 21, mk(40, 21), 3, 0);
    CHECK(c && !s.by_kid(1) && s.by_kid(2) && s.places.size() == 1 && s.bytes == 3360 + 400);
    CHECK(!s.add_image(100, 100, mk(100, 100), 4, 0));       /* bigger than the cap: refused, nothing evicted */
    CHECK(s.by_kid(2) && s.by_kid(3));
    b->stamp = ++s.clock;                                    /* touching b makes c the old one */
    vg::Image *d = s.add_image(10, 10, mk(10, 10), 5, 0);
    CHECK(d && !s.by_kid(3) && s.by_kid(2));
    /* an anonymous image lives only while placed */
    vg::Image *an = s.add_image(5, 5, mk(5, 5), 0, 0);
    CHECK(an); s.sweep(); CHECK(s.imgs.size() == 2);   /* the unplaced anonymous image is gone */
    pl.img = d; s.add_place(pl); s.sweep(); CHECK(s.by_kid(5));
    /* the reference count keeps pixels alive for a renderer that still holds them */
    d->refs.fetch_add(1);
    s.drop_image(d);
    CHECK(d->px != NULL);
    vg::unref(d);
}

/* ---- through the engine ---- */

static std::string g_reply;
static void on_write(const uint8_t *p, size_t n, void *) { g_reply.append((const char *)p, n); }

static Vt *mk_vt(int cols = 40, int rows = 12, int hist = 100) {
    Vt *t = vt_new(cols, rows, hist);
    vt_set_callbacks(t, on_write, NULL, NULL, NULL);
    vt_set_cell_px(t, 10, 20);
    g_reply.clear();
    return t;
}

static void feed(Vt *t, const std::string &s) { vt_feed(t, (const uint8_t *)s.data(), s.size()); }
/* the same bytes in uneven pieces: a string must survive being split anywhere */
static void feed_split(Vt *t, const std::string &s, size_t step) { for (size_t i = 0; i < s.size(); i += step) vt_feed(t, (const uint8_t *)s.data() + i, std::min(step, s.size() - i)); }

static int view(Vt *t, int first, int n, VtImageView *v, int max = 16) {
    int k = vt_images_in_view(t, first, n, v, max);
    return k;
}
static void release(VtImageView *v, int n) { for (int i = 0; i < n; i++) vt_image_unref(v[i].img); }

static std::string sixel(int w, int bands) {   /* w columns, `bands` bands of 6 rows, solid red */
    std::string s = "\x1bPq\"1;1;" + std::to_string(w) + ";" + std::to_string(bands * 6) + "#1;2;100;0;0#1";
    for (int b = 0; b < bands; b++) { s += "!" + std::to_string(w) + "~"; if (b + 1 < bands) s += "-"; }
    return s + "\x1b\\";
}

static std::string b64(const uint8_t *d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += T[v >> 18 & 63]; o += T[v >> 12 & 63]; o += i + 1 < n ? T[v >> 6 & 63] : '='; o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

static std::string kitty(const std::string &ctrl, const std::string &payload = "") { return "\x1b_G" + ctrl + (payload.empty() ? "" : ";" + payload) + "\x1b\\"; }

static std::vector<uint8_t> rgb_blob(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> v((size_t)w * h * 3);
    for (size_t i = 0; i < v.size(); i += 3) { v[i] = r; v[i + 1] = g; v[i + 2] = b; }
    return v;
}

static void test_engine_sixel() {
    Vt *t = mk_vt();
    VtImageView v[16];
    feed(t, "ab");
    feed(t, sixel(25, 2));   /* 25 x 12 px: 3 cols, 1 row at 10x20 */
    int n = view(t, 0, 12, v);
    CHECK(n == 1 && v[0].idx == 0 && v[0].col == 2 && v[0].cols == 3 && v[0].rows == 1 && v[0].iw == 25 && v[0].ih == 12 && !v[0].stretch);
    CHECK(n == 1 && v[0].px[0] == RED && v[0].px[25 * 12 - 1] == RED);
    release(v, n);
    CHECK(vt_cursor_y(t) == 1 && vt_cursor_x(t) == 0);                        /* below the image, column 0 */
    /* the same bytes cut into 1-, 2- and 7-byte pieces */
    for (size_t step : {1, 2, 7}) {
        Vt *u = mk_vt();
        feed_split(u, sixel(25, 4), step);
        n = view(u, 0, 12, v);
        CHECK(n == 1 && v[0].rows == 2 && v[0].ih == 24);                    /* 24 px / 20 = 2 rows */
        release(v, n);
        vt_free(u);
    }
    /* an image 3 rows tall puts the cursor 3 rows down */
    Vt *u = mk_vt();
    feed(u, sixel(10, 10));   /* 60 px = 3 rows */
    CHECK(vt_cursor_y(u) == 3);
    n = view(u, 0, 12, v); CHECK(n == 1 && v[0].rows == 3); release(v, n);
    /* scroll it into the history: 12-row screen, 15 more lines */
    for (int i = 0; i < 15; i++) feed(u, "\r\nline");
    n = view(u, 0, 12, v); CHECK(n == 0);                                     /* gone from the screen ... */
    n = view(u, -vt_history_count(u), vt_history_count(u), v); CHECK(n == 1 && v[0].idx < 0); release(v, n);   /* ... still in the history */
    vt_free(u);
    /* with no scrollback the image is dropped once it has left the screen */
    u = mk_vt(40, 12, 0);
    feed(u, sixel(10, 10));
    for (int i = 0; i < 30; i++) feed(u, "\r\nline");
    n = view(u, -50, 100, v); CHECK(n == 0);
    CHECK(vt_image_count(u) == 0);                                            /* the anonymous image was freed with its last placement */
    vt_free(u);
    /* ED 2 clears; ED 0 only what is below the cursor */
    u = mk_vt();
    feed(u, sixel(10, 10)); feed(u, "\x1b[2J");
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, "\x1b[H"); feed(u, sixel(10, 10)); feed(u, "\x1b[1;1H\x1b[J");
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, "\x1b[H"); feed(u, sixel(10, 10)); feed(u, "\x1b[10;1H\x1b[J");   /* erase below row 10: the image is above */
    n = view(u, 0, 12, v); CHECK(n == 1); release(v, n);
    /* the alternate screen hides main images and keeps none of its own */
    feed(u, "\x1b[?1049h");
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, sixel(10, 6));
    n = view(u, 0, 12, v); CHECK(n == 1); release(v, n);
    feed(u, "\x1b[?1049l");
    n = view(u, 0, 12, v); CHECK(n == 1 && v[0].rows == 3); release(v, n);    /* the main one is back, the alt one is gone */
    /* RIS wipes everything */
    feed(u, "\x1b" "c");
    CHECK(view(u, 0, 12, v) == 0 && vt_image_count(u) == 0);
    vt_free(u);
    /* cancelled and malformed strings leave no image and no stuck state */
    u = mk_vt();
    feed(u, "\x1bPq#0;2;100;0;0~~~\x18");                                      /* CAN aborts */
    feed(u, "text");
    CHECK(view(u, 0, 12, v) == 0 && vt_cursor_x(u) == 4);
    feed(u, "\x1bPq~~~\x1b[31mred");                                           /* ESC not followed by \ aborts and is reprocessed */
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, "\x1bP$qm\x1b\\");                                                 /* DECRQSS: some other DCS: skipped */
    feed(u, "\x1bPtmux;\x1b\x1b[1m\x1b\\");
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, "\x1bPq\x1b\\");                                                   /* empty sixel */
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, "\x1bPq" + std::string(3, '~') + "\x07");                          /* BEL terminates too */
    n = view(u, 0, 12, v); CHECK(n == 1); release(v, n);
    vt_free(u);
    /* a payload past the 32 MiB cap is dropped, and the terminal still works after it */
    u = mk_vt();
    feed(u, "\x1bPq");
    std::string chunk(1 << 20, '~');
    for (int i = 0; i < 34; i++) feed(u, chunk);
    feed(u, "\x1b\\");
    CHECK(view(u, 0, 12, v) == 0);
    feed(u, "\r\nok");
    CHECK(vt_cursor_x(u) == 2);
    feed(u, sixel(8, 1));
    n = view(u, 0, 12, v); CHECK(n == 1); release(v, n);
    vt_free(u);
}

static std::string kitty_t(int w, int h, const char *extra, uint8_t r = 255, uint8_t g = 0, uint8_t b = 0, const char *act = "T") {
    std::vector<uint8_t> blob = rgb_blob(w, h, r, g, b);
    return kitty(std::string("a=") + act + ",f=24,s=" + std::to_string(w) + ",v=" + std::to_string(h) + (extra[0] ? "," : "") + extra, b64(blob.data(), blob.size()));
}

static void test_engine_kitty() {
    VtImageView v[16];
    Vt *t = mk_vt();
    feed(t, kitty_t(20, 40, "i=5"));
    int n = view(t, 0, 12, v);
    CHECK(n == 1 && v[0].cols == 2 && v[0].rows == 2 && v[0].px[0] == RED);
    release(v, n);
    CHECK(g_reply == "\x1b_Gi=5;OK\x1b\\");
    CHECK(vt_cursor_y(t) == 1 && vt_cursor_x(t) == 2);                          /* right of the last row, like kitty */
    /* quiet modes */
    g_reply.clear(); feed(t, kitty_t(10, 10, "i=6,q=1")); CHECK(g_reply.empty());
    g_reply.clear(); feed(t, kitty_t(10, 10, "i=7,q=2,f=99")); CHECK(g_reply.empty());
    g_reply.clear(); feed(t, kitty_t(10, 10, "q=0")); CHECK(g_reply.empty());   /* no id: no reply */
    g_reply.clear(); feed(t, kitty_t(10, 10, "i=8,f=99", 1, 2, 3)); CHECK(g_reply.find("i=8;EINVAL") != std::string::npos);
    /* a=q tests without storing */
    int before = vt_image_count(t);
    g_reply.clear(); feed(t, kitty_t(10, 10, "i=31", 0, 0, 0, "q")); CHECK(g_reply == "\x1b_Gi=31;OK\x1b\\" && vt_image_count(t) == before);
    /* explicit cells, source rectangle, cursor movement off */
    vt_free(t); t = mk_vt();
    feed(t, "\x1b[3;5H");
    feed(t, kitty_t(100, 100, "i=1,c=6,r=3,x=10,y=20,w=30,h=40,z=-2,C=1"));
    n = view(t, 0, 12, v);
    CHECK(n == 1 && v[0].idx == 2 && v[0].col == 4 && v[0].cols == 6 && v[0].rows == 3 && v[0].stretch && v[0].sx == 10 && v[0].sy == 20 && v[0].sw == 30 && v[0].sh == 40 && v[0].z == -2);
    release(v, n);
    CHECK(vt_cursor_y(t) == 2 && vt_cursor_x(t) == 4);                          /* C=1 */
    /* only c given: rows follow the aspect ratio (100x50 px at 10x20 cells: c=4 -> 40 px wide, 20 tall -> 1 row) */
    feed(t, "\x1b[H");
    feed(t, kitty_t(100, 50, "i=2,c=4,C=1"));
    n = view(t, 0, 12, v);
    { bool ok = false; for (int i = 0; i < n; i++) if (v[i].cols == 4 && v[i].rows == 1) ok = true; CHECK(ok); }
    release(v, n);
    /* place by id; z order sorts the view; same id+p replaces */
    feed(t, "\x1b[2J\x1b[H");
    feed(t, kitty_t(10, 10, "i=10,C=1,z=5", 0, 255, 0, "t"));
    CHECK(view(t, 0, 12, v) == 0);                                              /* a=t transmits only */
    feed(t, kitty("a=p,i=10,p=1,C=1,z=5"));
    feed(t, kitty("a=p,i=10,p=2,C=1,z=-1"));
    feed(t, kitty("a=p,i=10,p=2,C=1,z=-3"));                                    /* replaces p=2 */
    n = view(t, 0, 12, v);
    CHECK(n == 2 && v[0].z == -3 && v[1].z == 5 && v[0].serial == v[1].serial && v[0].px[0] == GREEN);
    release(v, n);
    g_reply.clear(); feed(t, kitty("a=p,i=999")); CHECK(g_reply == "\x1b_Gi=999;ENOENT:no such image\x1b\\");
    /* retransmitting an id replaces the image and its placements */
    feed(t, kitty_t(10, 10, "i=10,C=1", 0, 0, 255));
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].px[0] == BLUE); release(v, n);
    /* delete: by placement, by id, all */
    feed(t, kitty("a=d,d=i,i=10"));
    CHECK(view(t, 0, 12, v) == 0);
    int base = vt_image_count(t);
    feed(t, kitty_t(10, 10, "i=11,C=1")); feed(t, kitty_t(10, 10, "i=12,C=1"));
    feed(t, kitty("a=d,d=a"));
    CHECK(view(t, 0, 12, v) == 0 && vt_image_count(t) == base + 2);                    /* lower case keeps the data */
    feed(t, kitty("a=p,i=11,C=1"));
    n = view(t, 0, 12, v); CHECK(n == 1); release(v, n);
    feed(t, kitty("a=d,d=A"));
    CHECK(view(t, 0, 12, v) == 0 && vt_image_count(t) == 0);                    /* upper case frees it */
    /* delete at the cursor, by column, row, z */
    feed(t, "\x1b[2J\x1b[H");
    feed(t, kitty_t(10, 20, "i=20,C=1")); feed(t, "\x1b[5;5H"); feed(t, kitty_t(10, 20, "i=21,C=1,z=9"));
    feed(t, "\x1b[5;5H"); feed(t, kitty("a=d,d=c"));
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].col == 0); release(v, n);
    feed(t, kitty("a=d,d=x,x=1"));
    CHECK(view(t, 0, 12, v) == 0);
    feed(t, "\x1b[H"); feed(t, kitty_t(10, 20, "i=22,C=1,z=7")); feed(t, kitty("a=d,d=z,z=7"));
    CHECK(view(t, 0, 12, v) == 0);
    /* chunked transmission, m=1 ... m=0, split anywhere, and a second command between chunks cannot hijack it */
    vt_free(t); t = mk_vt();
    std::vector<uint8_t> blob = rgb_blob(16, 16, 255, 0, 0);
    std::string all = b64(blob.data(), blob.size());
    std::string chunks;
    for (size_t i = 0; i < all.size(); i += 100) {
        bool more = i + 100 < all.size();
        chunks += kitty(i == 0 ? "a=T,f=24,s=16,v=16,i=40,m=1" : (more ? "m=1" : "m=0"), all.substr(i, 100));
    }
    feed_split(t, chunks, 13);
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].iw == 16 && v[0].px[0] == RED && v[0].px[255] == RED); release(v, n);
    CHECK(g_reply == "\x1b_Gi=40;OK\x1b\\");
    /* chunked with broken base64 half way */
    g_reply.clear();
    feed(t, kitty("a=T,f=24,s=16,v=16,i=41,m=1", all.substr(0, 100)));
    feed(t, kitty("m=0", "!!!!"));
    CHECK(g_reply.find("i=41;EINVAL") != std::string::npos);
    /* too little data, too much dimension, unsupported medium, unsupported compression */
    g_reply.clear(); feed(t, kitty("a=T,f=24,s=16,v=16,i=42", b64(blob.data(), 30)));
    CHECK(g_reply.find("EINVAL") != std::string::npos);
    g_reply.clear(); feed(t, kitty("a=T,f=24,s=99999,v=99999,i=43", "AAAA"));
    CHECK(g_reply.find("EFBIG") != std::string::npos);
    g_reply.clear(); feed(t, kitty("a=T,f=24,s=2,v=2,i=44,t=s", "AAAA"));
    CHECK(g_reply.find("ENOTSUP") != std::string::npos);
    g_reply.clear(); feed(t, kitty("a=T,f=24,s=2,v=2,i=45,o=z", "AAAA"));
    CHECK(g_reply.find("EINVAL") != std::string::npos);
    g_reply.clear(); feed(t, kitty("a=T,f=100,i=46", "AAAA"));
    CHECK(g_reply.find("PNG") != std::string::npos);
    /* an APC that is not graphics, and an unterminated one followed by CAN */
    feed(t, "\x1b_Xsomething\x1b\\\x1b_Gi=1\x18" "ok");
    /* the memory cap evicts the oldest image: 128 MB / (4096*4096*4) = 8 images fit */
    vt_free(t); t = mk_vt();
    vt_set_image_cap(t, (size_t)8 << 20);
    std::vector<uint8_t> big((size_t)1024 * 1024 * 4, 7);
    std::string bigb = b64(big.data(), big.size());
    for (int i = 1; i <= 4; i++) feed(t, kitty("a=t,f=32,s=1024,v=1024,i=" + std::to_string(i) + ",q=1", bigb));
    CHECK(vt_image_count(t) == 2 && vt_image_bytes(t) == (size_t)8 << 20);      /* 4 MB each: the two newest remain */
    feed(t, kitty("a=p,i=1,q=0"));
    feed(t, kitty("a=p,i=4"));
    n = view(t, 0, 12, v); CHECK(n == 1); release(v, n);
    /* an image bigger than the whole cap is refused whatever its dimensions */
    g_reply.clear();
    std::vector<uint8_t> huge((size_t)2048 * 2048 * 4, 1);
    feed(t, kitty("a=t,f=32,s=2048,v=2048,i=9,m=1", b64(huge.data(), 3000)));   /* refused on the header: the chunks that follow are not kept */
    CHECK(g_reply.find("i=9;EFBIG") != std::string::npos);
    g_reply.clear();
    feed(t, kitty("m=1", b64(huge.data(), 3000))); feed(t, kitty("m=0", b64(huge.data(), 3000)));
    CHECK(g_reply.empty());
    feed(t, kitty_t(4, 4, "i=33,C=1"));
    CHECK(g_reply.find("i=33;OK") != std::string::npos);                         /* and the next command works */
    vt_free(t);
}

static void write_file(const char *path, const void *d, size_t n) { FILE *f = fopen(path, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); } }

static void test_engine_files() {
    char dir[] = "/tmp/lestrix-gfx-XXXXXX";
    if (!mkdtemp(dir)) { printf("skip file tests: no temp dir\n"); return; }
    std::string img = std::string(dir) + "/img.rgb", tmpimg = std::string(dir) + "/x-tty-graphics-protocol-1";
    std::vector<uint8_t> blob = rgb_blob(8, 8, 0, 255, 0);
    write_file(img.c_str(), blob.data(), blob.size());
    write_file(tmpimg.c_str(), blob.data(), blob.size());
    std::string link = std::string(dir) + "/link";
    CHECK(symlink(img.c_str(), link.c_str()) == 0);
    VtImageView v[4];
    Vt *t = mk_vt();
    auto go = [&](const std::string &ctrl, const std::string &path) { g_reply.clear(); feed(t, kitty(ctrl, b64((const uint8_t *)path.data(), path.size()))); };
    go("a=T,f=24,s=8,v=8,t=f,i=1", img);                                         /* files off by default */
    CHECK(g_reply.find("EPERM") != std::string::npos && vt_image_count(t) == 0);
    vt_set_gfx_files(t, true);
    go("a=T,f=24,s=8,v=8,t=f,i=1", img);
    CHECK(g_reply == "\x1b_Gi=1;OK\x1b\\" && vt_image_count(t) == 1);
    int n = view(t, 0, 12, v); CHECK(n == 1 && v[0].px[0] == GREEN); release(v, n);
    go("a=T,f=24,s=8,v=8,t=f,i=2", link);                                        /* a symlink is followed to a regular file like any path */
    CHECK(g_reply == "\x1b_Gi=2;OK\x1b\\");
    go("a=T,f=24,s=8,v=8,t=f,i=3", "relative/path");                             /* relative */
    CHECK(g_reply.find("EINVAL") != std::string::npos);
    go("a=T,f=24,s=8,v=8,t=f,i=4", "/proc/self/environ");                        /* special filesystems */
    CHECK(g_reply.find("EPERM") != std::string::npos);
    go("a=T,f=24,s=8,v=8,t=f,i=5", "/dev/zero");
    CHECK(g_reply.find("EPERM") != std::string::npos);
    go("a=T,f=24,s=8,v=8,t=f,i=6", std::string(dir));                            /* a directory */
    CHECK(g_reply.find("E") != std::string::npos && g_reply.find("OK") == std::string::npos);
    go("a=T,f=24,s=8,v=8,t=f,i=7", std::string(dir) + "/missing");
    CHECK(g_reply.find("ENOENT") != std::string::npos);
    go("a=T,f=24,s=8,v=8,t=f,i=8,O=64,S=100", img);                             /* offset past the data: not enough pixels */
    CHECK(g_reply.find("EINVAL") != std::string::npos);
    go("a=T,f=24,s=8,v=8,t=t,i=9", img);                                         /* t=t needs the tty-graphics-protocol name */
    CHECK(g_reply.find("EPERM") != std::string::npos && access(img.c_str(), F_OK) == 0);
    go("a=T,f=24,s=8,v=8,t=t,i=10", tmpimg);                                     /* inside /tmp with the right name: read, then deleted */
    CHECK(g_reply == "\x1b_Gi=10;OK\x1b\\" && access(tmpimg.c_str(), F_OK) != 0);
    vt_free(t);
    unlink(img.c_str()); unlink(link.c_str()); rmdir(dir);
}

static void test_reports() {
    Vt *t = mk_vt();
    feed(t, "\x1b[c");
    CHECK(g_reply == "\x1b[?62;4;22c");                                          /* DA1 now lists 4: sixel */
    g_reply.clear(); feed(t, "\x1b[16t"); CHECK(g_reply == "\x1b[6;20;10t");
    g_reply.clear(); feed(t, "\x1b[14t"); CHECK(g_reply == "\x1b[4;240;400t");
    g_reply.clear(); feed(t, "\x1b[18t"); CHECK(g_reply == "\x1b[8;12;40t");
    g_reply.clear(); feed(t, "\x1b[?1;1;0S"); CHECK(g_reply == "\x1b[?1;0;256S");
    g_reply.clear(); feed(t, "\x1b[?2;1;0S"); CHECK(g_reply == "\x1b[?2;0;400;240S");
    g_reply.clear(); feed(t, "\x1b[?1;3;100S"); CHECK(g_reply == "\x1b[?1;3;0S");   /* setting the register count: refused */
    vt_free(t);
}

/* Scrolling inside a region and with reverse index moves image anchors with the text, and an image whose anchor leaves the region goes. */
static void test_region_scroll() {
    VtImageView v[4];
    Vt *t = mk_vt(40, 12, 100);
    feed(t, "\x1b[4;8r\x1b[5;1H");                                                 /* scroll region rows 4..8 (1-based), cursor on row 5 */
    feed(t, sixel(10, 4));                                                         /* 24 px: 2 rows, anchored at y=4 (0-based) */
    int n = view(t, 0, 12, v); CHECK(n == 1 && v[0].idx == 4); release(v, n);
    feed(t, "\x1b[8;1H\n");                                                        /* line feed at the region bottom scrolls the region up by one */
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].idx == 3); release(v, n);
    feed(t, "\x1b[8;1H\n\n\n");
    CHECK(view(t, 0, 12, v) == 0);                                                 /* anchor scrolled above the region top */
    vt_free(t);
    t = mk_vt();
    feed(t, "\x1b[3;1H"); feed(t, sixel(10, 2));
    feed(t, "\x1b[1;1H\x1bM");                                                     /* reverse index at the top: everything moves down */
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].idx == 3); release(v, n);
    vt_free(t);
    /* the alternate screen scrolls images too (it has no history to carry them) */
    t = mk_vt();
    feed(t, "\x1b[?1049h\x1b[3;1H"); feed(t, sixel(10, 2));
    feed(t, "\x1b[12;1H\n");
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].idx == 1); release(v, n);
    vt_free(t);
    /* resize keeps main-screen images with their rows */
    t = mk_vt();
    feed(t, "\x1b[10;1H"); feed(t, sixel(10, 2));
    vt_resize(t, 40, 8);
    n = view(t, -10, 20, v); CHECK(n == 1); release(v, n);
    vt_free(t);
}

/* ---- PNG and zlib (f=100, o=z) ---- */
#define PNG_CASE(N) do { int w = 0, h = 0; const char *e = NULL; \
    uint32_t *px = vg::png_decode(PNG_##N, sizeof PNG_##N, vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e); \
    CHECK(px && w == PNG_##N##_W && h == PNG_##N##_H); \
    if (px) { bool same = true; for (int i = 0; i < w * h; i++) same &= px[i] == PNG_##N##_PX[i]; CHECK(same); free(px); } } while (0)

static void test_png() {
    PNG_CASE(RGBA); PNG_CASE(RGB_STORED); PNG_CASE(GRAY_FIXED); PNG_CASE(GRAYALPHA); PNG_CASE(PALETTE_TRNS); PNG_CASE(RGB_DYNAMIC);
    int w, h;
    const char *e = NULL;
    /* every truncation and every single-byte corruption of the samples: never a crash or a leak, only a clean refusal or some image */
    const uint8_t *all[] = {PNG_RGBA, PNG_RGB_STORED, PNG_GRAY_FIXED, PNG_PALETTE_TRNS};
    const size_t lens[] = {sizeof PNG_RGBA, sizeof PNG_RGB_STORED, sizeof PNG_GRAY_FIXED, sizeof PNG_PALETTE_TRNS};
    for (int k = 0; k < 4; k++) {
        for (size_t n = 0; n + 12 < lens[k]; n++) {   /* the last 12 bytes are IEND: without it the pixels are still all there */ uint32_t *px = vg::png_decode(all[k], n, vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e); CHECK(!px); free(px); }
        std::vector<uint8_t> v(all[k], all[k] + lens[k]);
        for (size_t i = 0; i < v.size(); i++) for (int bit = 0; bit < 8; bit += 3) {
            v[i] ^= (uint8_t)(1 << bit);
            uint32_t *px = vg::png_decode(v.data(), v.size(), vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e);
            CHECK(!px || (w > 0 && h > 0)); free(px);
            v[i] ^= (uint8_t)(1 << bit);
        }
    }
    {   /* the dynamic-block sample with each byte of the deflate data flipped (sampled) */
        std::vector<uint8_t> v(PNG_RGB_DYNAMIC, PNG_RGB_DYNAMIC + sizeof PNG_RGB_DYNAMIC);
        for (size_t i = 40; i < v.size(); i += 37) { v[i] ^= 0x5a; uint32_t *px = vg::png_decode(v.data(), v.size(), vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e); free(px); v[i] ^= 0x5a; }
        uint32_t *px = vg::png_decode(v.data(), v.size() - 40, vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e); CHECK(!px);
    }
    /* limits: a byte cap below the image, an IHDR claiming a huge size, random garbage, wrong signature */
    uint32_t *px = vg::png_decode(PNG_RGB_DYNAMIC, sizeof PNG_RGB_DYNAMIC, 100 * 60 * 4 - 1, vg::MAX_DIM, &w, &h, &e); CHECK(!px && e && !strncmp(e, "EFBIG", 5));
    std::vector<uint8_t> big(PNG_RGBA, PNG_RGBA + sizeof PNG_RGBA);
    big[16] = 0x00; big[17] = 0x01; big[18] = 0x11; big[19] = 0x70;   /* width 70000 */
    px = vg::png_decode(big.data(), big.size(), vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e); CHECK(!px && e && !strncmp(e, "EFBIG", 5));
    uint32_t seed = 12345;
    for (int round = 0; round < 300; round++) {
        std::vector<uint8_t> g(8 + (size_t)(round * 7 % 200));
        for (auto &b : g) { seed = seed * 1664525u + 1013904223u; b = (uint8_t)(seed >> 24); }
        if (round & 1) memcpy(g.data(), PNG_RGBA, std::min<size_t>(8, g.size()));
        px = vg::png_decode(g.data(), g.size(), vg::DEFAULT_CAP, vg::MAX_DIM, &w, &h, &e); free(px);
    }
    /* a deflate bomb: an IDAT that would inflate to far more than the header allows is refused at the header's size */
    std::vector<uint8_t> out;
    CHECK(!vg::zl::inflate_zlib(ZRAW_RGB_2X2, sizeof ZRAW_RGB_2X2, out, 5));   /* output cap smaller than the data */
    out.clear();
    CHECK(vg::zl::inflate_zlib(ZRAW_RGB_2X2, sizeof ZRAW_RGB_2X2, out, 12) && out.size() == 12 && out[0] == 255);
    /* kitty_decode: o=z with raw pixels, f=100 with and without o=z, an unknown o, bad zlib header */
    int iw = 0, ih = 0;
    px = vg::kitty_decode(ZRAW_RGB_2X2, sizeof ZRAW_RGB_2X2, 24, 'z', 2, 2, vg::DEFAULT_CAP, &iw, &ih, &e);
    CHECK(px && iw == 2 && ih == 2 && px[0] == RED && px[1] == GREEN && px[2] == BLUE); free(px);
    px = vg::kitty_decode(ZRAW_RGB_2X2, sizeof ZRAW_RGB_2X2, 24, 'q', 2, 2, vg::DEFAULT_CAP, &iw, &ih, &e); CHECK(!px);
    uint8_t junk[16] = {0x78, 0x9c, 1, 2, 3};
    px = vg::kitty_decode(junk, sizeof junk, 24, 'z', 2, 2, vg::DEFAULT_CAP, &iw, &ih, &e); CHECK(!px);
    px = vg::kitty_decode(PNG_GRAY_FIXED, sizeof PNG_GRAY_FIXED, 100, 0, 0, 0, vg::DEFAULT_CAP, &iw, &ih, &e); CHECK(px && iw == 4 && ih == 1); free(px);
}

static void test_engine_png() {
    Vt *t = mk_vt();
    VtImageView v[16];
    /* f=100 direct, with no s/v: the PNG's own size is used (100 x 60 px = 10 x 3 cells at 10x20) */
    feed(t, kitty("a=T,f=100,i=7", b64(PNG_RGB_DYNAMIC, sizeof PNG_RGB_DYNAMIC)));
    int n = view(t, 0, 12, v); CHECK(n == 1 && v[0].iw == 100 && v[0].ih == 60); release(v, n);
    CHECK(g_reply.find("OK") != std::string::npos || g_reply.empty());
    vt_free(t);
    t = mk_vt();
    /* chunked PNG, split anywhere */
    std::string all = b64(PNG_RGB_DYNAMIC, sizeof PNG_RGB_DYNAMIC);
    for (size_t i = 0; i < all.size(); i += 1000) feed(t, kitty(std::string(i == 0 ? "a=T,f=100,i=8," : "") + (i + 1000 < all.size() ? "m=1" : "m=0"), all.substr(i, 1000)));
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].iw == 100); release(v, n);
    vt_free(t);
    /* o=z raw RGB */
    t = mk_vt();
    feed(t, kitty("a=T,f=24,o=z,s=2,v=2,i=9", b64(ZRAW_RGB_2X2, sizeof ZRAW_RGB_2X2)));
    n = view(t, 0, 12, v); CHECK(n == 1 && v[0].iw == 2 && v[0].ih == 2); release(v, n);
    /* garbage PNG answers with an error and places nothing */
    g_reply.clear();
    feed(t, "\x1b[2J\x1b[H");
    feed(t, kitty("a=T,f=100,i=10", b64((const uint8_t *)"not a png at all, really not", 28)));
    n = view(t, 0, 12, v); CHECK(n == 0); release(v, n);
    CHECK(g_reply.find("EBADPNG") != std::string::npos);
    vt_free(t);
}

static void test_overprint() {
    Vt *t = mk_vt();
    VtImageView v[16];
    /* text over a sixel erases it */
    feed(t, "\x1b[3;1H"); feed(t, sixel(25, 2));
    int n = view(t, 0, 12, v); CHECK(n == 1); release(v, n);
    feed(t, "\x1b[3;2Hx");
    n = view(t, 0, 12, v); CHECK(n == 0); release(v, n);
    /* text elsewhere does not */
    feed(t, "\x1b[3;1H"); feed(t, sixel(25, 2));
    feed(t, "\x1b[10;1Hhello world");
    n = view(t, 0, 12, v); CHECK(n == 1); release(v, n);
    feed(t, "\x1b[3;30Hz");   /* same row, right of the image (3 cells wide) */
    n = view(t, 0, 12, v); CHECK(n == 1); release(v, n);
    feed(t, "\x1b[3;3H\xc3\xa9");   /* a UTF-8 letter on the image's last cell */
    n = view(t, 0, 12, v); CHECK(n == 0); release(v, n);
    /* wide characters and the generic per-codepoint path */
    feed(t, "\x1b[3;1H"); feed(t, sixel(25, 2));
    feed(t, "\x1b[3;3H\xe4\xb8\xad");
    n = view(t, 0, 12, v); CHECK(n == 0); release(v, n);
    /* a kitty image survives text over it */
    feed(t, kitty_t(10, 10, "i=3,c=3,r=1", 255, 0, 0, "T"));
    feed(t, "\x1b[1;1Habc");
    n = view(t, 0, 12, v); CHECK(n >= 1); release(v, n);
    vt_free(t);
}

int main() {
    test_sixel();
    test_base64_and_kitty();
    test_store();
    test_engine_sixel();
    test_engine_kitty();
    test_engine_files();
    test_reports();
    test_region_scroll();
    test_png();
    test_engine_png();
    test_overprint();
    printf("test_gfx: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
