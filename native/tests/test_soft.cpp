/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <vector>

#include "../src/ui/soft.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { W = 32, H = 24, ASZ = 16 };
static uint32_t fb[W * H];
static SoftTarget tgt() { return SoftTarget{fb, W, H, W, 16, 8, 0, 0xff000000u}; }   /* XRGB8888 */
static uint32_t rgb(int r, int g, int b) { return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static uint32_t px(int x, int y) { return fb[y * W + x] & 0xffffff; }
static uint16_t enc(int t, int size) { return (uint16_t)(t * 65535.0 / size + 0.5); }
static RInst rect(float x, float y, float w, float h, uint32_t rgba, uint32_t pad = 0) { return RInst{x, y, w, h, 100, 100, 100, 100, rgba, pad}; }
static RInst glyph(float x, float y, float w, float h, int tx0, int ty0, int tx1, int ty1, uint32_t rgba, int size, uint32_t pad = 0) {
    return RInst{x, y, w, h, enc(tx0, size), enc(ty0, size), enc(tx1, size), enc(ty1, size), rgba, pad};
}
static void clear(uint32_t rgba) { SoftTarget t = tgt(); soft_clear(&t, rgba); }
static void draw(const RInst *in, int n, const SoftClip *clip = nullptr, const uint8_t *atlas = nullptr, const uint32_t *catlas = nullptr) {
    SoftTarget t = tgt();
    soft_draw(&t, clip, in, (size_t)n, atlas, ASZ, catlas, ASZ);
}

int main() {
    clear(RGBA(10, 20, 30, 255));
    CHECK(px(0, 0) == rgb(10, 20, 30) && px(W - 1, H - 1) == rgb(10, 20, 30));
    CHECK((fb[5] & 0xff000000u) == 0xff000000u);   /* non-colour bits are opaque after a clear */

    /* solid rect, exact pixel coverage */
    RInst r = rect(2, 3, 4, 2, RGBA(200, 100, 50, 255));
    draw(&r, 1);
    CHECK(px(2, 3) == rgb(200, 100, 50) && px(5, 4) == rgb(200, 100, 50));
    CHECK(px(6, 3) == rgb(10, 20, 30) && px(1, 3) == rgb(10, 20, 30) && px(2, 5) == rgb(10, 20, 30) && px(2, 2) == rgb(10, 20, 30));

    /* half-pixel position: pixels whose centre is inside, 2 wide */
    clear(RGBA(0, 0, 0, 255));
    r = rect(0.5f, 0, 2, 1, RGBA(255, 255, 255, 255));
    draw(&r, 1);
    CHECK(px(0, 0) == rgb(255, 255, 255) && px(1, 0) == rgb(255, 255, 255) && px(2, 0) == 0);

    /* alpha blend */
    clear(RGBA(0, 0, 0, 255));
    r = rect(0, 0, 4, 4, RGBA(255, 255, 255, 128));
    draw(&r, 1);
    CHECK(px(1, 1) == rgb(128, 128, 128));
    clear(RGBA(100, 100, 100, 255));
    r = rect(0, 0, 4, 4, RGBA(200, 0, 0, 0));   /* alpha 0 draws nothing */
    draw(&r, 1);
    CHECK(px(1, 1) == rgb(100, 100, 100));

    /* clip rectangle */
    clear(RGBA(0, 0, 0, 255));
    SoftClip clip{1, 4, 4, 3, 3};
    r = rect(0, 0, W, H, RGBA(255, 0, 0, 255));
    draw(&r, 1, &clip);
    CHECK(px(4, 4) == rgb(255, 0, 0) && px(6, 6) == rgb(255, 0, 0) && px(7, 6) == 0 && px(3, 4) == 0 && px(4, 7) == 0);

    /* mono glyph, 1:1: the atlas texel value is the coverage */
    std::vector<uint8_t> atlas((size_t)ASZ * ASZ, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) atlas[(size_t)(4 + y) * ASZ + 6 + x] = (uint8_t)(x == 0 ? 0 : x == 3 ? 255 : 128);
    clear(RGBA(0, 0, 0, 255));
    RInst g = glyph(10, 5, 4, 4, 6, 4, 10, 8, RGBA(255, 255, 255, 255), ASZ);
    draw(&g, 1, nullptr, atlas.data());
    CHECK(px(10, 5) == 0);                       /* coverage 0 leaves the background */
    CHECK(px(11, 5) == rgb(128, 128, 128));
    CHECK(px(13, 8) == rgb(255, 255, 255));
    CHECK(px(14, 5) == 0 && px(9, 5) == 0 && px(10, 9) == 0);
    /* same glyph at half alpha, coloured */
    clear(RGBA(0, 0, 0, 255));
    g = glyph(10, 5, 4, 4, 6, 4, 10, 8, RGBA(255, 0, 0, 128), ASZ);
    draw(&g, 1, nullptr, atlas.data());
    CHECK(px(13, 5) == rgb(128, 0, 0));
    /* fractional position keeps the 1:1 texel mapping */
    clear(RGBA(0, 0, 0, 255));
    g = glyph(10.25f, 5, 4, 4, 6, 4, 10, 8, RGBA(255, 255, 255, 255), ASZ);
    draw(&g, 1, nullptr, atlas.data());
    CHECK(px(13, 5) == rgb(255, 255, 255) && px(12, 5) == rgb(128, 128, 128));
    /* scaled 2x: every texel covers two pixels */
    clear(RGBA(0, 0, 0, 255));
    g = glyph(8, 4, 8, 8, 6, 4, 10, 8, RGBA(255, 255, 255, 255), ASZ);
    draw(&g, 1, nullptr, atlas.data());
    CHECK(px(8, 4) == 0 && px(9, 4) == 0 && px(10, 4) == rgb(128, 128, 128) && px(11, 4) == rgb(128, 128, 128) && px(14, 4) == rgb(255, 255, 255) && px(15, 11) == rgb(255, 255, 255));

    /* colour glyph: straight RGBA, own colour, alpha from the texel times the text alpha */
    std::vector<uint32_t> cat((size_t)ASZ * ASZ, 0);
    cat[2 * ASZ + 2] = 255u << 24 | 30u << 16 | 20u << 8 | 10u;      /* r=10 g=20 b=30 opaque (RGBA bytes: r low) */
    cat[2 * ASZ + 3] = 128u << 24 | 200u;                             /* r=200 half alpha */
    clear(RGBA(0, 0, 0, 255));
    g = glyph(4, 4, 2, 1, 2, 2, 4, 3, RGBA(1, 2, 3, 255), ASZ, 255u << 16);
    draw(&g, 1, nullptr, nullptr, cat.data());
    CHECK(px(4, 4) == rgb(10, 20, 30));
    CHECK(px(5, 4) == rgb(100, 0, 0));

    /* rounded rectangle: corners cut, centre solid; bordered: ring only */
    clear(RGBA(0, 0, 0, 255));
    r = rect(4, 4, 16, 16, RGBA(255, 255, 255, 255), 6);
    draw(&r, 1);
    CHECK(px(4, 4) == 0 && px(19, 4) == 0 && px(4, 19) == 0 && px(19, 19) == 0);   /* corner pixels outside the arc */
    CHECK(px(12, 12) == rgb(255, 255, 255) && px(12, 4) == rgb(255, 255, 255) && px(4, 12) == rgb(255, 255, 255));
    clear(RGBA(0, 0, 0, 255));
    r = rect(4, 4, 16, 16, RGBA(255, 255, 255, 255), 6 | 1 << 8);
    draw(&r, 1);
    CHECK(px(12, 12) == 0);                        /* 1 px border: the middle stays empty */
    CHECK(px(12, 4) != 0 && px(4, 12) != 0 && px(12, 19) != 0 && px(19, 12) != 0);

    /* channel order follows the target's shifts */
    SoftTarget bgr{fb, W, H, W, 0, 8, 16, 0xff000000u};
    soft_clear(&bgr, RGBA(1, 2, 3, 255));
    CHECK((fb[0] & 0xffffff) == (1u | 2u << 8 | 3u << 16));

    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
