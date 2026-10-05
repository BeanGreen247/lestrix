#include "render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"

/* OpenGL ES 2 has no instancing, so each quad becomes four vertices of this shape */
typedef struct { float x, y, u, v; uint32_t rgba; float lx, ly, sw, sh, ar, ab; } V2;
#define MAXQ 16384   /* quads per draw call: 4 * MAXQ vertices must fit 16-bit indices */

struct Renderer {
    Atlas *atlas;
    GLuint prog, vao, vbo, ibo;
    V2 *v2;
    GLint u_res, u_atlas;
    RInst *buf;
    size_t n, cap;
    size_t vbo_cap;
    int w, h;
    int clip_on, cx, cy, cw, ch;
};

/* the shared fragment body: plain glyph/rectangle quads, plus rounded and outlined rectangles from a signed distance */
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

/* desktop OpenGL 3.3 core and OpenGL ES 3.0 share one source apart from the first lines */
static const char *VS3 =
    "layout(location=0) in vec4 a_rect;\n"
    "layout(location=1) in vec4 a_uv;\n"
    "layout(location=2) in vec4 a_col;\n"
    "layout(location=3) in vec4 a_aux;\n"
    "uniform vec2 u_res;\n"
    "out vec2 v_uv; out vec4 v_col; out vec2 v_local; out vec2 v_size; out vec2 v_aux;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  vec2 p = a_rect.xy + c * a_rect.zw;\n"
    "  gl_Position = vec4(p.x / u_res.x * 2.0 - 1.0, 1.0 - p.y / u_res.y * 2.0, 0.0, 1.0);\n"
    "  v_uv = mix(a_uv.xy, a_uv.zw, c);\n"
    "  v_col = a_col;\n"
    "  v_local = c * a_rect.zw; v_size = a_rect.zw; v_aux = a_aux.xy * 255.0;\n"
    "}\n";

static const char *FS3 =
    "in vec2 v_uv; in vec4 v_col; in vec2 v_local; in vec2 v_size; in vec2 v_aux;\n"
    "uniform sampler2D u_atlas;\n"
    "out vec4 o;\n"
    "#define TEX texture\n"
    "void main() {\n"
    SDF_BODY
    "  o = vec4(v_col.rgb, v_col.a * a);\n"
    "}\n";

/* OpenGL ES 2.0 (Raspberry Pi 2/3, Mali-T6xx): attributes and varyings, no gl_VertexID, per-vertex data */
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

/* the GLSL version line for the desktop 3.3 core and ES 3.0 variants */
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
    Renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->atlas = a;
    GLuint vs, fs;
    if (sd_gl_kind == GLK_ES2) { vs = compile_src(GL_VERTEX_SHADER, "", VS2); fs = compile_src(GL_FRAGMENT_SHADER, "", FS2); }
    else { vs = compile_src(GL_VERTEX_SHADER, glsl_head(), VS3); fs = compile_src(GL_FRAGMENT_SHADER, glsl_head(), FS3); }
    if (!vs || !fs) { free(r); return NULL; }
    r->prog = gl.CreateProgram();
    gl.AttachShader(r->prog, vs);
    gl.AttachShader(r->prog, fs);
    if (sd_gl_kind == GLK_ES2) {   /* no layout(location=...) in GLSL ES 1.00 */
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
    gl.GenBuffers(1, &r->vbo);
    if (sd_gl_kind == GLK_ES2) {
        /* a fixed index pattern for MAXQ quads: two triangles each over the four corners (0,0) (1,0) (0,1) (1,1) */
        uint16_t *idx = malloc((size_t)MAXQ * 6 * sizeof *idx);
        r->v2 = malloc((size_t)MAXQ * 4 * sizeof *r->v2);
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
    free(r->buf);
    free(r->v2);
    free(r);
}

Atlas *r_atlas(Renderer *r) { return r->atlas; }

void r_begin(Renderer *r, int w, int h, uint32_t clear_rgba) {
    r->w = w; r->h = h; r->n = 0; r->clip_on = 0;
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor((clear_rgba & 255) / 255.f, ((clear_rgba >> 8) & 255) / 255.f, ((clear_rgba >> 16) & 255) / 255.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

/* OpenGL ES 2: no instancing, so every instance is expanded into four vertices and drawn with an index pattern */
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

void r_flush(Renderer *r) {
    if (!r->n) return;
    gl.UseProgram(r->prog);
    gl.Uniform2f(r->u_res, (float)r->w, (float)r->h);
    gl.ActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_texture(r->atlas));
    gl.Uniform1i(r->u_atlas, 0);
    if (sd_gl_kind == GLK_ES2) { flush_es2(r); r->n = 0; return; }
    gl.BindVertexArray(r->vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, r->vbo);
    size_t bytes = r->n * sizeof(RInst);
    if (bytes > r->vbo_cap) { r->vbo_cap = bytes * 2; gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r->vbo_cap, NULL, GL_STREAM_DRAW); }
    else gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)r->vbo_cap, NULL, GL_STREAM_DRAW);   /* orphan the old storage */
    gl.BufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, r->buf);
    gl.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)r->n);
    r->n = 0;
}

void r_end(Renderer *r) { r_flush(r); }

void r_clip(Renderer *r, int x, int y, int w, int h) {
    r_flush(r);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, r->h - (y + h), w < 0 ? 0 : w, h < 0 ? 0 : h);
    r->clip_on = 1;
}

void r_clip_off(Renderer *r) { r_flush(r); glDisable(GL_SCISSOR_TEST); r->clip_on = 0; }

static void grow(Renderer *r, size_t extra) {
    if (r->n + extra <= r->cap) return;
    size_t nc = r->cap ? r->cap * 2 : 4096;
    while (nc < r->n + extra) nc *= 2;
    RInst *nb = realloc(r->buf, nc * sizeof *nb);
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
    uint16_t u = (uint16_t)((wx + 1) * inv), v = (uint16_t)((wy + 1) * inv);   /* the middle of the 2x2 white block */
    *o = (RInst){x, y, w, h, u, v, u, v, rgba, 0};
}

void r_make_glyph(Renderer *r, RInst *o, const AtlasGlyph *g, float pen_x, float baseline_y, uint32_t rgba) {
    float inv = 65535.f / (float)atlas_size(r->atlas);
    *o = (RInst){pen_x + g->left, baseline_y - g->top, (float)g->w, (float)g->h,
                 (uint16_t)(g->x * inv + 0.5f), (uint16_t)(g->y * inv + 0.5f),
                 (uint16_t)((g->x + g->w) * inv + 0.5f), (uint16_t)((g->y + g->h) * inv + 0.5f), rgba, 0};
}

/* radius and border width in pixels (0..255); border 0 = filled */
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
