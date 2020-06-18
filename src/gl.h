/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#include <GL/gl.h>
#include <assert.h>
#include <stdarg.h>

#include "gl_exts/glext.h"

#include "util.h"

extern PFNGLBUFFERSUBDATAARBPROC        glBufferSubData;
extern PFNGLUNIFORM4FPROC               glUniform4f;
extern PFNGLUNIFORM3FPROC               glUniform3f;
extern PFNGLUNIFORM2FPROC               glUniform2f;
extern PFNGLBUFFERDATAPROC              glBufferData;
extern PFNGLDELETEPROGRAMPROC           glDeleteProgram;
extern PFNGLUSEPROGRAMPROC              glUseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation;
extern PFNGLGETATTRIBLOCATIONPROC       glGetAttribLocation;
extern PFNGLDELETESHADERPROC            glDeleteShader;
extern PFNGLDETACHSHADERPROC            glDetachShader;
extern PFNGLGETPROGRAMINFOLOGPROC       glGetProgramInfoLog;
extern PFNGLGETPROGRAMIVPROC            glGetProgramiv;
extern PFNGLLINKPROGRAMPROC             glLinkProgram;
extern PFNGLATTACHSHADERPROC            glAttachShader;
extern PFNGLCOMPILESHADERPROC           glCompileShader;
extern PFNGLSHADERSOURCEPROC            glShaderSource;
extern PFNGLCREATESHADERPROC            glCreateShader;
extern PFNGLCREATEPROGRAMPROC           glCreateProgram;
extern PFNGLGETSHADERINFOLOGPROC        glGetShaderInfoLog;
extern PFNGLGETSHADERIVPROC             glGetShaderiv;
extern PFNGLDELETEBUFFERSPROC           glDeleteBuffers;
extern PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern PFNGLBINDBUFFERPROC              glBindBuffer;
extern PFNGLGENBUFFERSPROC              glGenBuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage;
extern PFNGLBINDBUFFERPROC              glBindBuffer;
extern PFNGLGENBUFFERSPROC              glGenBuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D;
extern PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer;
extern PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer;
extern PFNGLGENRENDERBUFFERSPROC        glGenRenderbuffers;
extern PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers;
extern PFNGLGENERATEMIPMAPPROC          glGenerateMipmap;
#ifdef DEBUG
extern PFNGLDEBUGMESSAGECALLBACKPROC   glDebugMessageCallback;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
#endif

extern void* (*gl_load_ext)(const char* procname);

void gl_load_exts();

__attribute__((always_inline)) static inline void gl_check_error();

typedef struct
{
    char* name;
    GLint location;
} Uniform;

typedef struct
{
    char* name;
    GLint location;
} Attribute;

#define SHADER_MAX_NUM_VERT_ATTRIBS 1
#define SHADER_MAX_NUM_UNIFORMS     3
typedef struct
{
    GLuint    id;
    Attribute attribs[SHADER_MAX_NUM_VERT_ATTRIBS];
    Uniform   uniforms[SHADER_MAX_NUM_UNIFORMS];
} Shader;

typedef struct
{
    GLuint vbo;
    size_t size;
} VBO;

__attribute__((packed)) enum TextureFormat {
    TEX_FMT_RGBA,
    TEX_FMT_RGB,
    TEX_FMT_MONO,
};

typedef struct
{
    GLuint             id;
    enum TextureFormat format;
    uint32_t           w, h;
} Texture;

/* static Texture Texture_new_with_alignment(uint8_t alignment) */
/* { */
/*     GLuint id = 0; */

/*     glActiveTexture(GL_TEXTURE0); */

/*     glGenTextures(1, &id); */

/*     glBindTexture(GL_TEXTURE_2D, id); */
/*     glPixelStorei(GL_UNPACK_ALIGNMENT, alignment); */

/*     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); */
/*     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); */

/*     return (Texture){ id, false, 0, 0 }; */
/* } */

/* static Texture Texture_new() */
/* { */
/*     return Texture_new_with_alignment(4); */
/* } */

/* static inline void Texture_image_with_format(Texture*    self, */
/*                                              GLuint      w, */
/*                                              GLuint      h, */
/*                                              const void* data, */
/*                                              int         format) */
/* { */
/*     glBindTexture(GL_TEXTURE_2D, self->id); */
/*     self->has_alpha = (format == GL_RGBA || format == GL_BGRA); */

/*     glTexImage2D(GL_TEXTURE_2D, 0, format, (self->w = w), (self->h = h), 0,
 */
/*                  format, GL_UNSIGNED_BYTE, data); */
/* } */

/* static inline void Texture_image(Texture*    self, */
/*                                  bool        alpha, */
/*                                  GLuint      w, */
/*                                  GLuint      h, */
/*                                  const void* data) */
/* { */
/*     Texture_image_with_format(self, w, h, data, alpha ? GL_RGBA : GL_RGB); */
/* } */

/* static void Texture_sub_image(Texture*    self, */
/*                               GLuint      x, */
/*                               GLuint      y, */
/*                               GLuint      w, */
/*                               GLuint      h, */
/*                               const void* data) */
/* { */

/*     glBindTexture(GL_TEXTURE_2D, self->id); */

/*     glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, */
/*                     self->has_alpha ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE,
 * data); */
/* } */

static inline void Texture_destroy(Texture* self)
{
    glDeleteTextures(1, &self->id);
    self->id = 0;
}

/* FRAMEBUFFER */

typedef struct
{
    GLuint  id;
    Texture color_tex;
} Framebuffer;

static Framebuffer Framebuffer_new()
{
    Framebuffer ret = {
        .id        = 0,
        .color_tex = { 0 },
    };

    glGenFramebuffers(1, &ret.id);

    return ret;
}

static inline void Framebuffer_attach_texture(Framebuffer* self, Texture* tex)
{
    glBindFramebuffer(GL_FRAMEBUFFER, self->id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex->id, 0);
}

#ifdef DEBUG
static inline void Framebuffer_assert_complete(Framebuffer* self)
{
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        const char* ss;
        switch (status) {
            case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
                ss = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
                break;
            case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
                ss = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
                break;
            case GL_FRAMEBUFFER_UNSUPPORTED:
                ss = "GL_FRAMEBUFFER_UNSUPPORTED";
                break;
        }
        ERR("Framebuffer error, status %s", ss);
    }
}
#else
#define Framebuffer_assert_complete(_fb) ;
#endif

static inline void Framebuffer_attach_as_color(Framebuffer* self,
                                               Texture*     tex,
                                               uint32_t     w,
                                               int32_t      h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, self->id);
    assert(self->color_tex.id == 0);

    self->color_tex = *tex;
    glBindTexture(GL_TEXTURE_2D, self->color_tex.id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           self->color_tex.id, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, 0);
    glViewport(0, 0, w, h);
    gl_check_error();
}

static inline void Framebuffer_generate_color_attachment(
  Framebuffer* self,
  uint32_t     w,
  int32_t      h)
{
    glBindFramebuffer(GL_FRAMEBUFFER, self->id);

    glGenTextures(1, &self->color_tex.id);
    glBindTexture(GL_TEXTURE_2D, self->color_tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           self->color_tex.id, 0);

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, 0);

    glViewport(0, 0, w, h);
    gl_check_error();
}

static inline Texture Framebuffer_extract_color_texture(Framebuffer* self)
{
    Texture tmp        = self->color_tex;
    self->color_tex.id = 0;
    return tmp;
}

/**
 * @param self - NULL - unbind framebuffer
 */
static inline void Framebuffer_use(Framebuffer* self)
{
    if (self) {
        ASSERT(self->color_tex.id, "no color attachment");

        glBindFramebuffer(GL_FRAMEBUFFER, self->id);
        glViewport(0, 0, self->color_tex.w, self->color_tex.h);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

static inline void Framebuffer_destroy(Framebuffer* self)
{
    ASSERT(self->id, "framebuffer double delete");
    glDeleteFramebuffers(1, &self->id);
    self->id = 0;
}

/* VBO */
static VBO VBO_new(const uint32_t   vertices,
                   uint32_t         nattribs,
                   const Attribute* attr)
{
    GLuint id = 0;

    glGenBuffers(1, &id);
    glBindBuffer(GL_ARRAY_BUFFER, id);

    for (uint32_t i = 0; i < nattribs; ++i) {
        glEnableVertexAttribArray(attr[i].location);

        if (attr)
            glVertexAttribPointer(attr[i].location, vertices, GL_FLOAT,
                                  GL_FALSE, 0, 0);
    }

    return (VBO){ .vbo = id, .size = 0 };
}

__attribute__((always_inline)) static inline void VBO_destroy(VBO* self)
{
    glDeleteBuffers(1, &self->vbo);
}

__attribute__((cold)) static void check_compile_error(GLuint id)
{
    int result = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &result);

    if (result == GL_FALSE) {
        glGetShaderiv(id, GL_INFO_LOG_LENGTH, &result);
        char msg[result + 1];

        glGetShaderInfoLog(id, result, &result, msg);

        ERR("Shader compilation error:\n%s\n", msg);
    }
}

/**
 * Create a shader program from sources
 * @param vars - vertex attribute and uniform names
 */
__attribute__((sentinel, cold)) static Shader Shader_new(const char* vs_src,
                                                         const char* fs_src,
                                                         const char* vars,
                                                         ...)
{
    GLuint id = glCreateProgram();

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);

    glShaderSource(vs, 1, &vs_src, NULL);
    glCompileShader(vs);
    check_compile_error(vs);

    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    check_compile_error(fs);

    glAttachShader(id, vs);
    glAttachShader(id, fs);

    glLinkProgram(id);

    int lnkres;
    glGetProgramiv(id, GL_LINK_STATUS, &lnkres);
    if (lnkres == GL_FALSE) {
        glGetProgramiv(id, GL_INFO_LOG_LENGTH, &lnkres);
        char msg[lnkres + 1];
        glGetProgramInfoLog(id, lnkres, &lnkres, msg);
        ERR("Shader linking error:\n%s\n", msg);
    }

    glDetachShader(id, vs);
    glDeleteShader(vs);
    glDetachShader(id, fs);
    glDeleteShader(fs);

    Shader ret = { .id = id, .attribs = { { 0 } }, .uniforms = { { 0 } } };

    uint32_t attr_idx = 0, uni_idx = 0;
    va_list  ap;
    va_start(ap, vars);
    for (char* name = NULL; (name = va_arg(ap, char*));) {
        GLint res = glGetAttribLocation(id, name);
        if (res != -1) {
            ret.attribs[attr_idx++] =
              (Attribute){ .location = res, .name = name };
        } else {
            res = glGetUniformLocation(id, name);
            if (res != -1) {
                ret.uniforms[uni_idx++] =
                  (Uniform){ .location = res, .name = name };
            } else {
                ERR("Failed to bind shader variable \'%s\' location", name);
            }
        }
    }
    va_end(ap);

    return ret;
}

__attribute__((always_inline)) static inline void Shader_use(Shader* s)
{
    if (s) {
        ASSERT(s->id, "use of uninitialized shader");
        glUseProgram(s->id);
    } else
        glUseProgram(0);
}

__attribute__((always_inline)) static inline void Shader_destroy(Shader* s)
{
    ASSERT(s->id, "deleted uninitialized/deleted shader program");
    glDeleteProgram(s->id);
    s->id = 0;
}

__attribute__((cold)) static const char* gl_severity_to_str(GLenum severity)
{
    switch (severity) {
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            return "NOTIFICATION";
        case GL_DEBUG_SEVERITY_LOW:
            return "LOW";
        case GL_DEBUG_SEVERITY_MEDIUM:
            return "MEDIUM";
        case GL_DEBUG_SEVERITY_HIGH:
            return "HIGH";
        default:
            ASSERT_UNREACHABLE;
    }
}

__attribute__((cold)) static const char* gl_source_to_str(GLenum source)
{
    switch (source) {
        case GL_DEBUG_SOURCE_API:
            return "API";
        case GL_DEBUG_SOURCE_WINDOW_SYSTEM:
            return "WINDOW_SYSTEM";
        case GL_DEBUG_SOURCE_SHADER_COMPILER:
            return "SHADER_COMPILER";
        case GL_DEBUG_SOURCE_THIRD_PARTY:
            return "THIRD_PARTY";
        case GL_DEBUG_SOURCE_APPLICATION:
            return "APPLICATION";
        case GL_DEBUG_SOURCE_OTHER:
            return "OTHER";
        default:
            ASSERT_UNREACHABLE;
    }
}

__attribute__((cold)) static const char* gl_type_to_str(GLenum type)
{
    switch (type) {
        case GL_DEBUG_TYPE_ERROR:
            return "TYPE_ERROR";
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            return "DEPRECATED_BEHAVIOR";
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
            return "UNDEFINED_BEHAVIOR";
        case GL_DEBUG_TYPE_PORTABILITY:
            return "PORTABILITY";
        case GL_DEBUG_TYPE_PERFORMANCE:
            return "PERFORMANCE";
        case GL_DEBUG_TYPE_MARKER:
            return "MARKER";
        case GL_DEBUG_TYPE_PUSH_GROUP:
            return "PUSH_GROUP";
        case GL_DEBUG_TYPE_POP_GROUP:
            return "POP_GROUP";
        case GL_DEBUG_TYPE_OTHER:
            return "OTHER";
        default:
            return "?";
    }
}

__attribute__((cold)) static void on_gl_error(GLenum        source,
                                              GLenum        type,
                                              GLuint        id,
                                              GLenum        severity,
                                              GLsizei       length,
                                              const GLchar* message,
                                              const void*   user_param)
{
    if (severity == GL_DEBUG_SEVERITY_HIGH) {
        ERR("OpenGL error\n"
            "  severity: %s\n"
            "  source:   %s\n"
            "  type:     %s\n"
            "  id:       %d\n"
            "  message:\n%s",
            gl_severity_to_str(severity), gl_source_to_str(source),
            gl_type_to_str(type), id, message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        WRN("OpenGL warning\n"
            "  severity: %s\n"
            "  source:   %s\n"
            "  type:     %s\n"
            "  id:       %d\n"
            "  message:\n%s\n",
            gl_severity_to_str(severity), gl_source_to_str(source),
            gl_type_to_str(type), id, message);
    } /*
     else {
         LOG("OpenGL info\n"
             "  severity: %s\n"
             "  source:   %s\n"
             "  type:     %s\n"
             "  id:       %d\n"
             "  message:\n%s\n",
             gl_severity_to_str(severity),
             gl_source_to_str(source),
             gl_type_to_str(type),
             id,
             message);
     }*/
}

__attribute__((always_inline)) static inline void gl_check_error()
{
#ifdef DEBUG
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        WRN("OpenGL error: %d\n", e);
    }
#endif
}
