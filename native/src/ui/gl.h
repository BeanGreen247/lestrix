/* gl.h - the few OpenGL 3.3 entry points the renderer needs, loaded at run time through the platform layer.
 * (GL 1.x functions such as glClear and glTexImage2D come straight from the system library.) */
#ifndef SD_GL_H
#define SD_GL_H
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

/* Which flavour of OpenGL the window got: chosen at start-up, tried in this order unless LESTRIX_GL (core|es3|es2) forces one. */
typedef enum { GLK_CORE = 0, GLK_ES3 = 1, GLK_ES2 = 2 } GlKind;
extern GlKind sd_gl_kind;

typedef struct {
    /* the old 1.x entry points are loaded too: an ES context on a board need not have a desktop libGL to link against */
    void (APIENTRY *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (APIENTRY *Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (APIENTRY *Enable)(GLenum);
    void (APIENTRY *Disable)(GLenum);
    void (APIENTRY *BlendFunc)(GLenum, GLenum);
    void (APIENTRY *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (APIENTRY *Clear)(GLbitfield);
    void (APIENTRY *GenTextures)(GLsizei, GLuint *);
    void (APIENTRY *DeleteTextures)(GLsizei, const GLuint *);
    void (APIENTRY *BindTexture)(GLenum, GLuint);
    void (APIENTRY *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (APIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
    void (APIENTRY *TexParameteri)(GLenum, GLenum, GLint);
    void (APIENTRY *PixelStorei)(GLenum, GLint);
    void (APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (APIENTRY *GetIntegerv)(GLenum, GLint *);
    const GLubyte *(APIENTRY *GetString)(GLenum);
    void (APIENTRY *DrawElements)(GLenum, GLsizei, GLenum, const void *);
    void (APIENTRY *DeleteBuffers)(GLsizei, const GLuint *);
    void (APIENTRY *DisableVertexAttribArray)(GLuint);
    void (APIENTRY *BindAttribLocation)(GLuint, GLuint, const GLchar *);
    GLuint (APIENTRY *CreateShader)(GLenum);
    void (APIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void (APIENTRY *CompileShader)(GLuint);
    void (APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
    void (APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    GLuint (APIENTRY *CreateProgram)(void);
    void (APIENTRY *AttachShader)(GLuint, GLuint);
    void (APIENTRY *LinkProgram)(GLuint);
    void (APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
    void (APIENTRY *GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void (APIENTRY *UseProgram)(GLuint);
    void (APIENTRY *DeleteShader)(GLuint);
    void (APIENTRY *DeleteProgram)(GLuint);
    GLint (APIENTRY *GetUniformLocation)(GLuint, const GLchar *);
    void (APIENTRY *Uniform1i)(GLint, GLint);
    void (APIENTRY *Uniform2f)(GLint, GLfloat, GLfloat);
    void (APIENTRY *GenVertexArrays)(GLsizei, GLuint *);
    void (APIENTRY *BindVertexArray)(GLuint);
    void (APIENTRY *GenBuffers)(GLsizei, GLuint *);
    void (APIENTRY *BindBuffer)(GLenum, GLuint);
    void (APIENTRY *BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
    void (APIENTRY *BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
    void (APIENTRY *EnableVertexAttribArray)(GLuint);
    void (APIENTRY *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void (APIENTRY *VertexAttribDivisor)(GLuint, GLuint);
    void (APIENTRY *DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
    void (APIENTRY *ActiveTexture)(GLenum);
} SdGL;

extern SdGL gl;
int sd_gl_load(GlKind kind);   /* after the context is current; 0 on success (ES 2 does not need VAOs or instancing) */

/* the rest of the code keeps writing glClear(...) and gets the run-time-loaded function */
#define glViewport gl.Viewport
#define glScissor gl.Scissor
#define glEnable gl.Enable
#define glDisable gl.Disable
#define glBlendFunc gl.BlendFunc
#define glClearColor gl.ClearColor
#define glClear gl.Clear
#define glGenTextures gl.GenTextures
#define glDeleteTextures gl.DeleteTextures
#define glBindTexture gl.BindTexture
#define glTexImage2D gl.TexImage2D
#define glTexSubImage2D gl.TexSubImage2D
#define glTexParameteri gl.TexParameteri
#define glPixelStorei gl.PixelStorei
#define glReadPixels gl.ReadPixels
#define glGetIntegerv gl.GetIntegerv
#define glGetString gl.GetString
#endif
