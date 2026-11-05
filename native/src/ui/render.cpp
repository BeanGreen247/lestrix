/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "render.h"

#include <math.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"
#include "soft.h"

#include <vector>

typedef struct { float x, y, u, v; uint32_t rgba; float lx, ly, sw, sh, ar, ab; } V2;
#define MAXQ 16384

struct Renderer {
    Atlas *atlas;
    GLuint prog, vao, vbo, ibo;
    V2 *v2;
    GLint u_res, u_atlas, u_color;
    RInst *buf;
    size_t n, cap;
    size_t vbo_cap;
    int w, h;
    int clip_on, cx, cy, cw, ch;
    SoftTarget st;   /* software rendering target, set by r_set_target before each frame */
    SoftClip sclip;
    struct SoftState *ss;
};

/* Software rendering keeps the pixels of the previous frame and repaints only what changed. The window is cut into
 * tiles; every instance mixes a hash of itself (and its clip) into the tiles it touches, in draw order, so a tile whose
 * hash is the same as last frame would be painted identically and is skipped. Painted tiles of a row are merged into
 * runs, cleared to the background and redrawn from the instances that touch that row. */
enum { TILE = 32 };
struct SoftState {
    std::vector<RInst> inst;
    std::vector<uint16_t> clipid;
    std::vector<SoftClip> clips;
    uint16_t cur_clip = 0;
    std::vector<uint64_t> prev, cur;
    std::vector<uint8_t> dirty;
    std::vector<std::vector<uint32_t>> bins;
    std::vector<RDamage> damage;
    uint32_t bg = 0, prev_bg = 0, prev_gen = 0;
    SoftTarget prev_t{};
    bool invalid = true;
};

#define SDF_BODY \
    "  float a = TEX(u_atlas, v_uv).r;\n" \
    "  if (v_aux.x + v_aux.y > 0.0) {\n" \
    "    vec2 half_size = 0.5 * v_size;\n" \
    "    float rad = min(v_aux.x, min(half_size.x, half_size.y));\n" \
    "    vec2 q = abs(v_local - half_size) - (half_size - vec2(rad));\n" \
    "    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;\n" \
    "    float cov = clamp(0.5 - d, 0.0, 1.0);\n" \
    "    if (v_aux.y > 0.0) cov = max(cov - clamp(0.5 - (d + v_aux.y), 0.0, 1.0), 0.0);\n" \
    "    a *= cov;\n" \
    "  }\n"

static const char *VS3 =
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec4 a_aux;\n"
    "uniform vec2 u_res;\n"
    "out vec2 v_uv; out vec4 v_col; out vec2 v_local; out vec2 v_size; out vec2 v_aux; flat out float v_clr;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = a_rect.xy + c * a_rect.zw;\n"
    "  gl_Position = vec4(p.x / u_res.x * 2.0 - 1.0, 1.0 - p.y / u_res.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_col = a_col;\n"
    "  v_local = c * a_rect.zw; v_size = a_rect.zw; v_aux = a_aux.xy * 255.0; v_clr = a_aux.z;\n"
    "}\n";

static const char *FS3 =
    "in vec2 v_uv; in vec4 v_col; in vec2 v_local; in vec2 v_size; in vec2 v_aux; flat in float v_clr;\n"
    "uniform sampler2D u_atlas;\n"
    "uniform sampler2D u_color;\n"
    "out vec4 o;\n"
    "#define TEX texture\n"
    "void main() {\n"
    "  if (v_clr > 0.5) { vec4 t = texture(u_color, v_uv); o = vec4(t.rgb, t.a * v_col.a); return; }\n"
    SDF_BODY
    "  o = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";

static const char *VS2 =
    "#version 100\n"
    "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col; attribute vec2 a_local; attribute vec2 a_size; attribute vec2 a_aux;\n"
    "uniform vec2 u_res;\n"
    "varying vec2 v_uv; varying vec4 v_col; varying vec2 v_local; varying vec2 v_size; varying vec2 v_aux;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x / u_res.x * 2.0 - 1.0, 1.0 - a_pos.y / u_res.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = a_uv; v_col = a_col; v_local = a_local; v_size = a_size; v_aux = a_aux;\n"
    "}\n";

static const char *FS2 =
    "#version 100\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
    "varying vec2 v_uv; varying vec4 v_col; varying vec2 v_local; varying vec2 v_size; varying vec2 v_aux;\n"
    "uniform sampler2D u_atlas;\n"
    "#define TEX texture2D\n"
    "void main() {\n"
    SDF_BODY
    "  gl_FragColor = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";

static GLuint compile(GLenum kind, const char *src) {
    GLuint s = gl.CreateShader(kind);
    gl.ShaderSource(s, 1, &src, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

static const char *glsl_head(void) {
    return sd_gl_kind == GLK_ES3 ? "#version 300 es\nprecision highp float;\nprecision highp int;\n" : "#version 330 core\n";
}

static GLuint compile_src(GLenum kind, const char *head, const char *body) {
    const char *parts[2] = {head, body};
    GLuint s = gl.CreateShader(kind);
    gl.ShaderSource(s, 2, parts, NULL);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        gl.GetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        return 0;
    }
    return s;
}

Renderer *r_new(Atlas *a) {
    Renderer *r = static_cast<Renderer*>(calloc(1, sizeof *r));
    if (!r) return NULL;
    r->atlas = a;
    if (sd_soft) { r->ss = new SoftState(); return r; }
    GLuint vs, fs;
    if (sd_gl_kind == GLK_ES2) { vs = compile_src(GL_VERTEX_SHADER, "", VS2); fs = compile_src(GL_FRAGMENT_SHADER, "", FS2); }
    else { vs = compile_src(GL_VERTEX_SHADER, glsl_head(), VS3); fs = compile_src(GL_FRAGMENT_SHADER, glsl_head(), FS3); }
    if (!vs || !fs) { free(r); return NULL; }
    r->prog = gl.CreateProgram();
    gl.AttachShader(r->prog, vs);
    gl.AttachShader(r->prog, fs);
    if (sd_gl_kind == GLK_ES2) {
        static const char *const names[] = {"a_pos", "a_uv", "a_col", "a_local", "a_size", "a_aux"};
        for (GLuint i = 0; i < 6; i++) gl.BindAttribLocation(r->prog, i, names[i]);
    }
    gl.LinkProgram(r->prog);
    GLint ok = 0;
    gl.GetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    if (!ok) { free(r); return NULL; }
    r->u_res = gl.GetUniformLocation(r->prog, "u_res");
    r->u_atlas = gl.GetUniformLocation(r->prog, "u_atlas");
    r->u_color = gl.GetUniformLocation(r->prog, "u_color");
    gl.GenBuffers(1, &r->vbo);
    if (sd_gl_kind == GLK_ES2) {
        uint16_t *idx = static_cast<uint16_t*>(malloc((size_t)MAXQ * 6 * sizeof *idx));
        r->v2 = static_cast<V2*>(malloc((size_t)MAXQ * 4 * sizeof *r->v2));
        if (!idx || !r->v2) { free(idx); free(r->v2); free(r); return NULL; }
        for (int q = 0; q < MAXQ; q++) {
            uint16_t base = (uint16_t)(q * 4);
            uint16_t *p = idx + (size_t)q * 6;
            p[0] = base; p[1] = (uint16_t)(base + 1); p[2] = (uint16_t)(base + 2);
            p[3] = (uint16_t)(base + 2); p[4] = (uint16_t)(base + 1); p[5] = (uint16_t)(base + 3);
        }
        gl.GenBuffers(1, &r->ibo);
        gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo);
        gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)((size_t)MAXQ * 6 * sizeof *idx), idx, GL_STATIC_DRAW);
        free(idx);
        return r;
    }
    gl.GenVertexArrays(1, &r->vao);
    gl.BindVertexArray(r->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(RInst), (void *)0);
    gl.VertexAttribDivisor(0, 1);
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_SHORT, GL_TRUE, sizeof(RInst), (void *)16);
    gl.VertexAttribDivisor(1, 1);
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RInst), (void *)24);
    gl.VertexAttribDivisor(2, 1);
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(RInst), (void *)28);
    gl.VertexAttribDivisor(3, 1);
    return r;
}

void r_free(Renderer *r) {
    if (!r) return;
    delete r->ss;
    free(r->buf);
    free(r->v2);
    free(r);
}

Atlas *r_atlas(Renderer *r) { return r->atlas; }

void r_begin(Renderer *r, int w, int h, uint32_t clear_rgba) {
    r->w = w; r->h = h; r->n = 0; r->clip_on = 0;
    if (sd_soft) {
        SoftState *s = r->ss;
        r->sclip.on = 0;
        s->inst.clear(); s->clipid.clear(); s->clips.assign(1, SoftClip{0, 0, 0, 0, 0}); s->cur_clip = 0;
        s->bg = clear_rgba;
        return;
    }
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor((clear_rgba & 255) / 255.f, ((clear_rgba >> 8) & 255) / 255.f, ((clear_rgba >> 16) & 255) / 255.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void flush_es2(Renderer *r) {
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ibo);
    const GLsizei stride = (GLsizei)sizeof(V2);
    gl.EnableVertexAttribArray(0); gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
    gl.EnableVertexAttribArray(1); gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void *)8);
    gl.EnableVertexAttribArray(2); gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, (void *)16);
    gl.EnableVertexAttribArray(3); gl.VertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void *)20);
    gl.EnableVertexAttribArray(4); gl.VertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, (void *)28);
    gl.EnableVertexAttribArray(5); gl.VertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, stride, (void *)36);
    const float inv = 1.f / 65535.f;
    for (size_t first = 0; first < r->n; first += MAXQ) {
        size_t cnt = r->n - first < MAXQ ? r->n - first : MAXQ;
        V2 *v = r->v2;
        for (size_t i = 0; i < cnt; i++) {
            const RInst *s = &r->buf[first + i];
            float ar = (float)(s->pad & 255), ab = (float)((s->pad >> 8) & 255);
            float u0 = s->u0 * inv, v0 = s->v0 * inv, u1 = s->u1 * inv, v1 = s->v1 * inv;
            for (int c = 0; c < 4; c++, v++) {
                float cx = (float)(c & 1), cy = (float)(c >> 1);
                v->x = s->x + cx * s->w; v->y = s->y + cy * s->h;
                v->u = u0 + (u1 - u0) * cx; v->v = v0 + (v1 - v0) * cy;
                v->rgba = s->rgba;
                v->lx = cx * s->w; v->ly = cy * s->h; v->sw = s->w; v->sh = s->h; v->ar = ar; v->ab = ab;
            }
        }
        gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(cnt * 4 * sizeof(V2)), r->v2, GL_STREAM_DRAW);
        gl.DrawElements(GL_TRIANGLES, (GLsizei)(cnt * 6), GL_UNSIGNED_SHORT, (void *)0);
    }
}

void r_set_target(Renderer *r, uint32_t *px, int w, int h, int pitch_px, int rs, int gs, int bs, uint32_t amask) {
    r->st = SoftTarget{px, w, h, pitch_px, rs, gs, bs, amask};
}

void r_flush(Renderer *r) {
    if (!r->n) return;
    if (sd_soft) {   /* drawing is deferred to r_end, where the whole frame is known */
        SoftState *s = r->ss;
        s->inst.insert(s->inst.end(), r->buf, r->buf + r->n);
        s->clipid.insert(s->clipid.end(), r->n, s->cur_clip);
        r->n = 0;
        return;
    }
    gl.UseProgram(r->prog);
    gl.Uniform2f(r->u_res, (float)r->w, (float)r->h);
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_texture(r->atlas));
    gl.Uniform1i(r->u_atlas, 0);
    if (sd_gl_kind != GLK_ES2 && atlas_color_texture(r->atlas)) {
        gl.ActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, atlas_color_texture(r->atlas));
        gl.Uniform1i(r->u_color, 1);
        gl.ActiveTexture(GL_TEXTURE0);
    }
    if (sd_gl_kind == GLK_ES2) { flush_es2(r); r->n = 0; return; }
    gl.BindVertexArray(r->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    size_t bytes = r->n * sizeof(RInst);
    if (bytes > r->vbo_cap) { r->vbo_cap = bytes * 2; gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r->vbo_cap, NULL, GL_STREAM_DRAW); }
    else gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r->vbo_cap, NULL, GL_STREAM_DRAW);
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->buf);
    gl.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)r->n);
    r->n = 0;
}

static inline uint64_t mix64(uint64_t h, uint64_t v) { h ^= v; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }

static uint64_t inst_hash(const RInst &q, const SoftClip &c) {
    uint64_t w[4];
    memcpy(w, &q, sizeof w);   /* RInst is 32 bytes */
    uint64_t h = 0x243F6A8885A308D3ull;
    for (int i = 0; i < 4; i++) h = mix64(h, w[i]);
    if (c.on) h = mix64(mix64(h, (uint64_t)(uint32_t)c.x << 32 | (uint32_t)c.y), (uint64_t)(uint32_t)c.w << 32 | (uint32_t)c.h);
    return h;
}

static void soft_finish(Renderer *r) {
    SoftState *s = r->ss;
    const SoftTarget &t = r->st;
    s->damage.clear();
    if (!t.px || t.w <= 0 || t.h <= 0) return;
    const int tw = (t.w + TILE - 1) / TILE, th = (t.h + TILE - 1) / TILE;
    const uint32_t gen = atlas_generation(r->atlas);
    bool full = s->invalid || s->prev.size() != (size_t)tw * th || s->prev_t.px != t.px || s->prev_t.w != t.w || s->prev_t.h != t.h ||
                s->prev_t.pitch != t.pitch || s->prev_t.rs != t.rs || s->prev_t.gs != t.gs || s->prev_t.bs != t.bs || s->bg != s->prev_bg || gen != s->prev_gen;
    const uint64_t seed = mix64(0x13198A2E03707344ull, s->bg);
    s->cur.assign((size_t)tw * th, seed);
    const size_t n = s->inst.size();
    for (size_t i = 0; i < n; i++) {   /* pass 1: hash every tile */
        const RInst &q = s->inst[i];
        const SoftClip &c = s->clips[s->clipid[i]];
        float fx0 = q.x, fy0 = q.y, fx1 = q.x + q.w, fy1 = q.y + q.h;
        if (c.on) { if (fx0 < c.x) fx0 = (float)c.x; if (fy0 < c.y) fy0 = (float)c.y; if (fx1 > c.x + c.w) fx1 = (float)(c.x + c.w); if (fy1 > c.y + c.h) fy1 = (float)(c.y + c.h); }
        if (fx0 < 0) fx0 = 0;
        if (fy0 < 0) fy0 = 0;
        if (fx1 > (float)t.w) fx1 = (float)t.w;
        if (fy1 > (float)t.h) fy1 = (float)t.h;
        if (!(fx1 > fx0) || !(fy1 > fy0)) continue;
        int tx0 = (int)fx0 / TILE, ty0 = (int)fy0 / TILE, tx1 = ((int)ceilf(fx1) - 1) / TILE, ty1 = ((int)ceilf(fy1) - 1) / TILE;
        if (tx1 >= tw) tx1 = tw - 1;
        if (ty1 >= th) ty1 = th - 1;
        uint64_t ih = inst_hash(q, c);
        for (int ty = ty0; ty <= ty1; ty++) {
            uint64_t *row = &s->cur[(size_t)ty * tw];
            for (int tx = tx0; tx <= tx1; tx++) row[tx] = mix64(row[tx], ih);
        }
    }
    s->dirty.assign((size_t)tw * th, 1);
    size_t ndirty = (size_t)tw * th;
    if (!full) {
        ndirty = 0;
        for (size_t k = 0; k < s->dirty.size(); k++) { s->dirty[k] = s->cur[k] != s->prev[k]; ndirty += s->dirty[k]; }
    }
    if (ndirty) {
        const uint8_t *amono = atlas_cpu(r->atlas);
        const int asz = atlas_size(r->atlas), csz = atlas_color_size(r->atlas);
        const uint32_t *acol = atlas_color_cpu(r->atlas);
        s->bins.resize((size_t)th);
        for (int ty = 0; ty < th; ty++) s->bins[ty].clear();
        std::vector<char> rowdirty((size_t)th, 0);
        for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) if (s->dirty[(size_t)ty * tw + tx]) { rowdirty[ty] = 1; break; }
        for (size_t i = 0; i < n; i++) {   /* pass 2: instances per tile row, in draw order, only rows that repaint */
            const RInst &q = s->inst[i];
            int ty0 = (int)floorf(q.y) / TILE, ty1 = (int)ceilf(q.y + q.h) / TILE;
            if (q.y < 0) ty0 = 0;
            if (ty0 < 0) ty0 = 0;
            if (ty1 >= th) ty1 = th - 1;
            for (int ty = ty0; ty <= ty1; ty++) if (rowdirty[ty]) s->bins[ty].push_back((uint32_t)i);
        }
        for (int ty = 0; ty < th; ty++) {
            if (!rowdirty[ty]) continue;
            for (int tx = 0; tx < tw;) {
                if (!s->dirty[(size_t)ty * tw + tx]) { tx++; continue; }
                int tx1 = tx;
                while (tx1 + 1 < tw && s->dirty[(size_t)ty * tw + tx1 + 1]) tx1++;
                int x0 = tx * TILE, x1 = (tx1 + 1) * TILE < t.w ? (tx1 + 1) * TILE : t.w;
                int y0 = ty * TILE, y1 = (ty + 1) * TILE < t.h ? (ty + 1) * TILE : t.h;
                soft_clear_rect(&t, x0, y0, x1, y1, s->bg);
                for (uint32_t i : s->bins[ty]) {
                    SoftClip c = s->clips[s->clipid[i]];
                    int cx0 = x0, cy0 = y0, cx1 = x1, cy1 = y1;
                    if (c.on) { if (c.x > cx0) cx0 = c.x; if (c.y > cy0) cy0 = c.y; if (c.x + c.w < cx1) cx1 = c.x + c.w; if (c.y + c.h < cy1) cy1 = c.y + c.h; }
                    if (cx1 <= cx0 || cy1 <= cy0) continue;
                    SoftClip run{1, cx0, cy0, cx1 - cx0, cy1 - cy0};
                    soft_draw(&t, &run, &s->inst[i], 1, amono, asz, acol, csz);
                }
                s->damage.push_back(RDamage{x0, y0, x1 - x0, y1 - y0});
                tx = tx1 + 1;
            }
        }
    }
    static const bool verify = getenv("LESTRIX_SOFT_VERIFY") != NULL;
    if (verify) {   /* test aid: paint the whole frame from scratch and compare with the incrementally painted one */
        std::vector<uint32_t> ref((size_t)t.w * t.h);
        SoftTarget rt = t;
        rt.px = ref.data(); rt.pitch = t.w;
        soft_clear(&rt, s->bg);
        for (size_t i = 0; i < n; i++) soft_draw(&rt, &s->clips[s->clipid[i]], &s->inst[i], 1, atlas_cpu(r->atlas), atlas_size(r->atlas), atlas_color_cpu(r->atlas), atlas_color_size(r->atlas));
        for (int y = 0; y < t.h; y++)
            if (memcmp(ref.data() + (size_t)y * t.w, t.px + (size_t)y * t.pitch, (size_t)t.w * 4)) {
                int x = 0;
                while (ref[(size_t)y * t.w + x] == t.px[(size_t)y * t.pitch + x]) x++;
                fprintf(stderr, "soft damage mismatch: frame differs from a full repaint at %d,%d (full=%d dirty tiles=%zu)\n", x, y, (int)full, ndirty);
                if (!strcmp(getenv("LESTRIX_SOFT_VERIFY"), "fatal")) _exit(3);   /* make test-gui fails on a mismatch */
                break;
            }
    }
    s->prev.swap(s->cur);
    s->prev_t = t; s->prev_bg = s->bg; s->prev_gen = gen; s->invalid = false;
}

const RDamage *r_damage(const Renderer *r, size_t *n) {
    *n = r->ss ? r->ss->damage.size() : 0;
    return r->ss && !r->ss->damage.empty() ? r->ss->damage.data() : NULL;
}

void r_soft_invalidate(Renderer *r) { if (r->ss) r->ss->invalid = true; }

void r_end(Renderer *r) {
    r_flush(r);
    if (sd_soft) soft_finish(r);
}

void r_clip(Renderer *r, int x, int y, int w, int h) {
    r_flush(r);
    r->clip_on = 1;
    if (sd_soft) { r->sclip = SoftClip{1, x, y, w < 0 ? 0 : w, h < 0 ? 0 : h}; r->ss->clips.push_back(r->sclip); r->ss->cur_clip = (uint16_t)(r->ss->clips.size() - 1); return; }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, r->h - (y + h), w < 0 ? 0 : w, h < 0 ? 0 : h);
}

void r_clip_off(Renderer *r) {
    r_flush(r);
    if (sd_soft) { r->sclip.on = 0; r->ss->cur_clip = 0; } else glDisable(GL_SCISSOR_TEST);
    r->clip_on = 0;
}

static void grow(Renderer *r, size_t extra) {
    if (r->n + extra <= r->cap) return;
    size_t nc = r->cap ? r->cap * 2 : 4096;
    while (nc < r->n + extra) nc *= 2;
    RInst *nb = static_cast<RInst*>(realloc(r->buf, nc * sizeof *nb));
    if (!nb) return;
    r->buf = nb; r->cap = nc;
}

void r_push(Renderer *r, const RInst *inst, int n) {
    grow(r, (size_t)n);
    if (r->n + (size_t)n > r->cap) return;
    memcpy(r->buf + r->n, inst, (size_t)n * sizeof *inst);
    r->n += (size_t)n;
}

void r_push_at(Renderer *r, const RInst *inst, int n, float dx, float dy) {
    grow(r, (size_t)n);
    if (r->n + (size_t)n > r->cap) return;
    RInst *o = r->buf + r->n;
    for (int i = 0; i < n; i++) { o[i] = inst[i]; o[i].x += dx; o[i].y += dy; }
    r->n += (size_t)n;
}

size_t r_mark(const Renderer *r) { return r->n; }
void r_rewind(Renderer *r, size_t mark) { if (mark <= r->n) r->n = mark; }

void r_make_rect(Renderer *r, RInst *o, float x, float y, float w, float h, uint32_t rgba) {
    uint16_t wx, wy;
    atlas_white(r->atlas, &wx, &wy);
    float inv = 65535.f / (float)atlas_size(r->atlas);
    uint16_t u = (uint16_t)((wx + 1) * inv), v = (uint16_t)((wy + 1) * inv);
    *o = (RInst){x, y, w, h, u, v, u, v, rgba, 0};
}

void r_make_glyph(Renderer *r, RInst *o, const AtlasGlyph *g, float pen_x, float baseline_y, uint32_t rgba) {
    float inv = 65535.f / (float)(g->color ? atlas_color_size(r->atlas) : atlas_size(r->atlas));
    *o = (RInst){pen_x + g->left, baseline_y - g->top, (float)g->w, (float)g->h,
                 (uint16_t)(g->x * inv + 0.5f), (uint16_t)(g->y * inv + 0.5f),
                 (uint16_t)((g->x + g->w) * inv + 0.5f), (uint16_t)((g->y + g->h) * inv + 0.5f), rgba, g->color ? 255u << 16 : 0u};
}

void r_rrect(Renderer *r, float x, float y, float w, float h, uint32_t rgba, int radius, int border) {
    RInst i;
    r_make_rect(r, &i, x, y, w, h, rgba);
    i.pad = (uint32_t)(radius > 255 ? 255 : radius) | (uint32_t)(border > 255 ? 255 : border) << 8;
    r_push(r, &i, 1);
}

void r_rect(Renderer *r, float x, float y, float w, float h, uint32_t rgba) {
    RInst i;
    r_make_rect(r, &i, x, y, w, h, rgba);
    r_push(r, &i, 1);
}
