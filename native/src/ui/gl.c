/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "gl.h"

GlKind sd_gl_kind = GLK_CORE;
SdGL gl;

int sd_gl_load(GlKind kind) {
    int bad = 0;
    sd_gl_kind = kind;
#define LOAD(n) do { gl.n = (void *)SDL_GL_GetProcAddress("gl" #n); if (!gl.n) bad++; } while (0)
    LOAD(Viewport); LOAD(Scissor); LOAD(Enable); LOAD(Disable); LOAD(BlendFunc); LOAD(ClearColor); LOAD(Clear);
    LOAD(GenTextures); LOAD(DeleteTextures); LOAD(BindTexture); LOAD(TexImage2D); LOAD(TexSubImage2D); LOAD(TexParameteri);
    LOAD(PixelStorei); LOAD(ReadPixels); LOAD(GetIntegerv); LOAD(GetString); LOAD(DrawElements); LOAD(DeleteBuffers);
    LOAD(DisableVertexAttribArray);
    LOAD(BindAttribLocation);
    LOAD(CreateShader); LOAD(ShaderSource); LOAD(CompileShader); LOAD(GetShaderiv); LOAD(GetShaderInfoLog);
    LOAD(CreateProgram); LOAD(AttachShader); LOAD(LinkProgram); LOAD(GetProgramiv); LOAD(GetProgramInfoLog);
    LOAD(UseProgram); LOAD(DeleteShader); LOAD(DeleteProgram); LOAD(GetUniformLocation); LOAD(Uniform1i); LOAD(Uniform2f);
    int need_vao = kind != GLK_ES2;
    { int before = bad; LOAD(GenVertexArrays); LOAD(BindVertexArray); LOAD(VertexAttribDivisor); LOAD(DrawArraysInstanced); if (!need_vao) bad = before; }
    LOAD(GenBuffers); LOAD(BindBuffer); LOAD(BufferData); LOAD(BufferSubData);
    LOAD(EnableVertexAttribArray); LOAD(VertexAttribPointer); 
    LOAD(ActiveTexture);
#undef LOAD
    return bad ? -1 : 0;
}
