/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

/* Mutation fuzzer for the parsers that take bytes from whatever runs in the terminal (built with ASan and UBSan): Sixel, kitty graphics
 * (chunked, base64, PNG, zlib), OSC 7/8/9/52/133/777, kitty keyboard and DECRQM replies, SGR underline styles, DCS/APC/OSC framing, resizes
 * and resets in between. Seeds are valid sequences; every round mutates one (bit flips, truncation, splices, repeats, random bytes) and
 * feeds the result in random chunk sizes. Checks: no crash or sanitizer report, the image store stays under its cap, replies stay small,
 * and the resident set does not grow without bound.
 * Usage: fuzz_vt [iterations] [seed] */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../src/vgfx.h"
#include "png_samples.h"
#include "../src/vt.h"

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 11); }
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

static std::string b64(const uint8_t *d, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += T[v >> 18 & 63]; o += T[v >> 12 & 63]; o += i + 1 < n ? T[v >> 6 & 63] : '='; o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

static std::vector<std::string> seeds() {
    std::vector<std::string> s;
    s.push_back("\x1bPq\"1;1;8;12#0;2;100;0;0#1;2;0;100;0#0!8~$#1!8B-#0!8~\x1b\\");
    s.push_back("\x1bP0;1;0q#0;2;0;0;0#0~~~~??@@-~~~~\x1b\\");
    s.push_back("\x1bPq#0;1;120;50;100;50#0!300~\x1b\\");
    s.push_back("\x1bPq!99999999~\x1b\\");
    s.push_back("\x1b_Ga=T,f=24,s=2,v=2,i=7;" + b64((const uint8_t *)"\xff\0\0\0\xff\0\0\0\xff\xff\xff\0", 12) + "\x1b\\");
    s.push_back("\x1b_Ga=T,f=32,s=1,v=1,c=3,r=2;/wAA/w==\x1b\\");
    s.push_back("\x1b_Ga=T,f=100,i=3;" + b64(PNG_RGBA, sizeof PNG_RGBA) + "\x1b\\");
    s.push_back("\x1b_Ga=T,f=100,i=4;" + b64(PNG_RGB_STORED, sizeof PNG_RGB_STORED) + "\x1b\\");
    s.push_back("\x1b_Ga=T,f=100,i=5;" + b64(PNG_PALETTE_TRNS, sizeof PNG_PALETTE_TRNS) + "\x1b\\");
    s.push_back("\x1b_Ga=t,f=100,i=6,m=1;" + b64(PNG_GRAYALPHA, 20) + "\x1b\\\x1b_Gm=0;" + b64(PNG_GRAYALPHA + 20, sizeof PNG_GRAYALPHA - 20) + "\x1b\\");
    s.push_back("\x1b_Ga=p,i=3,c=4,r=2,x=1,y=1,w=2,h=2,z=-1\x1b\\\x1b_Ga=d,d=a\x1b\\\x1b_Ga=q,i=1,s=1,v=1,f=24;AAAA\x1b\\");
    s.push_back("\x1b_Ga=T,f=24,s=99999,v=99999,o=z;AAAA\x1b\\");
    s.push_back("\x1b_Ga=T,f=24,t=f,s=1,v=1;L2V0Yy9wYXNzd2Q=\x1b\\");
    s.push_back("\x1b]8;id=a;https://example.com/x\x1b\\link\x1b]8;;\x1b\\ text http://a.b/c?d=e ");
    s.push_back("\x1b]8;;mailto:a@b.c\x07mail\x1b]8;;\x07");
    s.push_back("\x1b]9;hello\x07\x1b]777;notify;title;body text\x07\x1b]9;4;1;50\x07");
    s.push_back("\x1b]7;file://host/home/user/dir%20x\x07\x1b]0;title\x07\x1b]2;t2\x1b\\");
    s.push_back("\x1b]52;c;SGVsbG8=\x07\x1b]52;c;?\x07\x1b]52;p;!\x07");
    s.push_back("\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07out\r\n\x1b]133;D;0\x07\x1b]133;D;130\x07\x1b]133;A;k=s\x07");
    s.push_back("\x1b[>1u\x1b[?u\x1b[=11;2u\x1b[<2u\x1b[>31u\x1b[<9u");
    s.push_back("\x1b[?2026h\x1b[?2026$p\x1b[?2026l\x1b[?1$p\x1b[?9999$p\x1b[5$p");
    s.push_back("\x1b[4:3m\x1b[58:2::10:20:30m wavy \x1b[58;5;99m\x1b[4:5mdash\x1b[4:0m\x1b[59m\x1b[58;2;1;2;3;4m\x1b[4:9m");
    s.push_back("\x1b[14t\x1b[16t\x1b[18t\x1b[?1;1;0S\x1b[c\x1b[>c\x1b[=c\x1b[6n");
    s.push_back("\x1b[?1049h text \x1b[2J\x1b[H\x1b[?1049l\x1b[3J\x1bc\x1b[1;1H\x1b[999;999H\x1b[100000000;1H\x1b[%d@");
    s.push_back("\x1bP$qm\x1b\\\x1bP+q544e\x1b\\\x1bPtmux;\x1b\x1b[1m\x1b\\\x1b^pm\x1b\\\x1bXsos\x1b\\");
    return s;
}

static void mutate(std::string &s, const std::vector<std::string> &pool) {
    int n = 1 + (int)rn(4);
    while (n--) {
        switch (rn(9)) {
        case 0: if (!s.empty()) s[rn((uint32_t)s.size())] ^= (char)(1u << rn(8)); break;
        case 1: if (!s.empty()) s.resize(rn((uint32_t)s.size())); break;
        case 2: { std::string &o = const_cast<std::string &>(pool[rn((uint32_t)pool.size())]); size_t at = rn((uint32_t)s.size() + 1); s.insert(at, o); break; }
        case 3: if (!s.empty()) { size_t a = rn((uint32_t)s.size()), l = 1 + rn(64); std::string c = s.substr(a, l); int r = 1 + (int)rn(50); while (r--) s.insert(a, c); } break;
        case 4: { size_t at = rn((uint32_t)s.size() + 1), l = 1 + rn(32); std::string g; while (l--) g += (char)rnd(); s.insert(at, g); break; }
        case 5: if (!s.empty()) s[rn((uint32_t)s.size())] = (char)rnd(); break;
        case 6: { static const char *E[] = {"\x1b\\", "\x07", "\x1b", "\x18", "\x1a", ";", ":", "9999999999", "-1", "\x1b[", "\x1b]", "\x1bP", "\x1b_", "\x9c", "\x90", "\x9d", "\x9f"}; size_t at = rn((uint32_t)s.size() + 1); s.insert(at, E[rn(17)]); break; }
        case 7: if (s.size() > 4) { size_t a = rn((uint32_t)s.size() - 1), l = 1 + rn((uint32_t)(s.size() - a)); s.erase(a, l); } break;
        case 8: if (!s.empty()) { size_t a = rn((uint32_t)s.size()); for (size_t i = a; i < s.size() && i < a + 8; i++) s[i] = (char)(rn(2) ? '9' : '0'); } break;
        }
    }
}

static size_t reply_bytes;
static void on_write(const uint8_t *, size_t n, void *) { reply_bytes += n; }
static void on_event(VtEvent, const char *, void *) {}

static size_t rss_kb() {
    long pages = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0; fclose(f); }
    return (size_t)res * (size_t)(sysconf(_SC_PAGESIZE) / 1024);
}

int main(int argc, char **argv) {
    long iters = argc > 1 ? atol(argv[1]) : 200000;
    if (argc > 2) rs ^= (uint64_t)atoll(argv[2]) * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 10; i++) rnd();
    std::vector<std::string> pool = seeds();
    Vt *t = vt_new(80, 24, 2000);
    vt_set_callbacks(t, on_write, on_event, nullptr, nullptr);
    vt_set_cell_px(t, 8, 16);
    vt_set_gfx_files(t, false);
    size_t rss0 = 0, rss_max = 0, img_max = 0, bytes_fed = 0, max_reply_round = 0;
    int fails = 0;
    time_t t0 = time(nullptr);
    for (long it = 0; it < iters; it++) {
        std::string s = pool[rn((uint32_t)pool.size())];
        mutate(s, pool);
        if (rn(6) == 0) s += pool[rn((uint32_t)pool.size())];
        size_t before = reply_bytes;
        const uint8_t *p = (const uint8_t *)s.data();
        size_t n = s.size(), at = 0;
        while (at < n) {
            size_t c = rn(3) == 0 ? 1 + rn(7) : 1 + rn(4096);
            if (c > n - at) c = n - at;
            vt_feed(t, p + at, c);
            at += c;
        }
        bytes_fed += n;
        if (reply_bytes - before > max_reply_round) max_reply_round = reply_bytes - before;
        if (rn(8) == 0) {
            VtImageView v[64];
            int k = vt_images_in_view(t, -vt_history_count(t), vt_history_count(t) + vt_rows(t), v, 64);
            for (int i = 0; i < k; i++) vt_image_unref(v[i].img);
        }
        if (rn(50) == 0) vt_resize(t, 20 + (int)rn(200), 5 + (int)rn(60));
        if (rn(400) == 0) vt_reset(t);
        if (rn(300) == 0) { vt_prompt_find(t, 0, -1); int a, b; vt_last_output(t, &a, &b); vt_last_exit(t); }
        if (vt_image_bytes(t) > img_max) img_max = vt_image_bytes(t);
        if (vt_image_bytes(t) > vg::DEFAULT_CAP + ((size_t)16 << 20)) { printf("FAIL image bytes %zu over the cap (iteration %ld)\n", vt_image_bytes(t), it); fails++; break; }
        if (it == iters / 10) rss0 = rss_kb();
        size_t r = rss_kb();
        if (r > rss_max) rss_max = r;
        if (r > ((size_t)3 << 20)) { printf("FAIL resident set %zu kB (iteration %ld)\n", r, it); fails++; break; }
    }
    printf("fuzz_vt: %ld iterations, %zu MB fed, %lds, image store max %zu MB, replies max %zu B per round, RSS %zu -> max %zu MB, %d failures\n",
           iters, bytes_fed >> 20, (long)(time(nullptr) - t0), img_max >> 20, max_reply_round, rss0 >> 10, rss_max >> 10, fails);
    if (max_reply_round > 65536) { printf("FAIL: a single input made %zu bytes of replies\n", max_reply_round); fails++; }
    vt_free(t);

    /* PNG and zlib decoders hit directly: mutated valid files and pure noise */
    long png_ok = 0, png_bad = 0;
    const uint8_t *P[] = {PNG_RGBA, PNG_RGB_STORED, PNG_GRAY_FIXED, PNG_GRAYALPHA, PNG_PALETTE_TRNS};
    const size_t PL[] = {sizeof PNG_RGBA, sizeof PNG_RGB_STORED, sizeof PNG_GRAY_FIXED, sizeof PNG_GRAYALPHA, sizeof PNG_PALETTE_TRNS};
    for (long it = 0; it < iters; it++) {
        int k = (int)rn(5);
        std::string s((const char *)P[k], PL[k]);
        mutate(s, pool);
        int w = 0, h = 0;
        const char *err = nullptr;
        uint32_t *px = vg::png_decode((const uint8_t *)s.data(), s.size(), (size_t)8 << 20, 4096, &w, &h, &err);
        if (px) { png_ok++; if ((uint64_t)w * h * 4 > ((uint64_t)8 << 20)) { printf("FAIL png over cap %dx%d\n", w, h); fails++; } free(px); } else png_bad++;
        std::vector<uint8_t> out;
        vg::zl::inflate_zlib((const uint8_t *)s.data(), s.size(), out, (size_t)1 << 20);
        if (out.size() > ((size_t)1 << 20)) { printf("FAIL inflate over cap %zu\n", out.size()); fails++; }
    }
    printf("fuzz_png: %ld iterations, %ld decoded, %ld rejected, %d failures\n", iters, png_ok, png_bad, fails);
    return fails ? 1 : 0;
}
