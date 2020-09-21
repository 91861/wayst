/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#include "gl_exts/glext.h"
#include <GL/gl.h>
#include <assert.h>
#include <stdarg.h>

#include "util.h"

static void gl_check_error();

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

enum TextureFormat
{
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

static void Texture_destroy(Texture* self)
{
    glDeleteTextures(1, &self->id);
    self->id = 0;
}

#ifdef DEBUG
static inline void assert_farmebuffer_complete()
{
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        const char* string;
        switch (status) {
            case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
                string = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
                break;
            case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
                string = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
                break;
            case GL_FRAMEBUFFER_UNSUPPORTED:
                string = "GL_FRAMEBUFFER_UNSUPPORTED";
                break;
            default:
                string = "UNKNOWN ERROR CODE";
        }
        ERR("Framebuffer error, status %s", string);
    }
}
#else
#define assert_farmebuffer_complete() ;
#endif

static VBO VBO_new(const uint32_t vertices, uint32_t nattribs, const Attribute* attr)
{
    GLuint id = 0;

    glGenBuffers(1, &id);
    glBindBuffer(GL_ARRAY_BUFFER, id);

    for (uint32_t i = 0; i < nattribs; ++i) {
        glEnableVertexAttribArray(attr[i].location);
        glVertexAttribPointer(attr[i].location, vertices, GL_FLOAT, GL_FALSE, 0, 0);
    }

    return (VBO){ .vbo = id, .size = 0 };
}

static void VBO_destroy(VBO* self)
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
            ret.attribs[attr_idx++] = (Attribute){ .location = res, .name = name };
        } else {
            res = glGetUniformLocation(id, name);
            if (res != -1) {
                ret.uniforms[uni_idx++] = (Uniform){ .location = res, .name = name };
            } else {
                ERR("Failed to bind shader variable \'%s\' location", name);
            }
        }
    }
    va_end(ap);

    return ret;
}

static void Shader_use(Shader* s)
{
    if (s) {
        ASSERT(s->id, "use of uninitialized shader");
        glUseProgram(s->id);
    } else
        glUseProgram(0);
}

static void Shader_destroy(Shader* s)
{
    ASSERT(s->id, "deleted uninitialized/deleted shader program");
    glDeleteProgram(s->id);
    s->id = 0;
}

#ifdef DEBUG
static const char* gl_severity_to_str(GLenum severity)
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

static const char* gl_source_to_str(GLenum source)
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

static const char* gl_type_to_str(GLenum type)
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

static void on_gl_error(GLenum        source,
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
            gl_severity_to_str(severity),
            gl_source_to_str(source),
            gl_type_to_str(type),
            id,
            message);
    } else if (severity == GL_DEBUG_SEVERITY_MEDIUM) {
        WRN("OpenGL warning\n"
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
    } else {
        LOG("GL::info{ severity: %s, source: %s, type: %s, id: %d, message: %s }\n",
            gl_severity_to_str(severity),
            gl_source_to_str(source),
            gl_type_to_str(type),
            id,
            message);
    }
}
#endif

static void gl_check_error()
{
#ifdef DEBUG
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        WRN("OpenGL error: %d\n", e);
    }
#endif
}
