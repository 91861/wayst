#include "gl.h"
#include "util.h"
#include <GL/gl.h>
#include <unistd.h>

void* gl_ext_loader;
void* (*gl_load_ext)(void* loader, const char* procname) = NULL;

PFNGLBUFFERSUBDATAARBPROC        glBufferSubData;
PFNGLUNIFORM4FPROC               glUniform4f;
PFNGLUNIFORM3FPROC               glUniform3f;
PFNGLUNIFORM2FPROC               glUniform2f;
PFNGLBUFFERDATAPROC              glBufferData;
PFNGLDELETEPROGRAMPROC           glDeleteProgram;
PFNGLUSEPROGRAMPROC              glUseProgram;
PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation;
PFNGLGETATTRIBLOCATIONPROC       glGetAttribLocation;
PFNGLDELETESHADERPROC            glDeleteShader;
PFNGLDETACHSHADERPROC            glDetachShader;
PFNGLGETPROGRAMINFOLOGPROC       glGetProgramInfoLog;
PFNGLGETPROGRAMIVPROC            glGetProgramiv;
PFNGLLINKPROGRAMPROC             glLinkProgram;
PFNGLATTACHSHADERPROC            glAttachShader;
PFNGLCOMPILESHADERPROC           glCompileShader;
PFNGLSHADERSOURCEPROC            glShaderSource;
PFNGLCREATESHADERPROC            glCreateShader;
PFNGLCREATEPROGRAMPROC           glCreateProgram;
PFNGLGETSHADERINFOLOGPROC        glGetShaderInfoLog;
PFNGLGETSHADERIVPROC             glGetShaderiv;
PFNGLDELETEBUFFERSPROC           glDeleteBuffers;
PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
PFNGLBINDBUFFERPROC              glBindBuffer;
PFNGLGENBUFFERSPROC              glGenBuffers;
PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage;
PFNGLBINDBUFFERPROC              glBindBuffer;
PFNGLGENBUFFERSPROC              glGenBuffers;
PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D;
PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer;
PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer;
PFNGLGENRENDERBUFFERSPROC        glGenRenderbuffers;
PFNGLDELETERENDERBUFFERSPROC     glDeleteRenderbuffers;
PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers;
PFNGLGENERATEMIPMAPPROC          glGenerateMipmap;
#ifdef DEBUG
PFNGLDEBUGMESSAGECALLBACKPROC   glDebugMessageCallback;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
#endif

void gl_load_exts()
{
    if (!gl_load_ext) {
        ERR("gl extension loader not set");
    }

    glBufferSubData           = gl_load_ext(gl_ext_loader, "glBufferSubData");
    glUniform4f               = gl_load_ext(gl_ext_loader, "glUniform4f");
    glUniform3f               = gl_load_ext(gl_ext_loader, "glUniform3f");
    glUniform2f               = gl_load_ext(gl_ext_loader, "glUniform2f");
    glBufferData              = gl_load_ext(gl_ext_loader, "glBufferData");
    glDeleteProgram           = gl_load_ext(gl_ext_loader, "glDeleteProgram");
    glUseProgram              = gl_load_ext(gl_ext_loader, "glUseProgram");
    glGetUniformLocation      = gl_load_ext(gl_ext_loader, "glGetUniformLocation");
    glGetAttribLocation       = gl_load_ext(gl_ext_loader, "glGetAttribLocation");
    glDeleteShader            = gl_load_ext(gl_ext_loader, "glDeleteShader");
    glDetachShader            = gl_load_ext(gl_ext_loader, "glDetachShader");
    glGetProgramInfoLog       = gl_load_ext(gl_ext_loader, "glGetProgramInfoLog");
    glGetProgramiv            = gl_load_ext(gl_ext_loader, "glGetProgramiv");
    glLinkProgram             = gl_load_ext(gl_ext_loader, "glLinkProgram");
    glAttachShader            = gl_load_ext(gl_ext_loader, "glAttachShader");
    glCompileShader           = gl_load_ext(gl_ext_loader, "glCompileShader");
    glShaderSource            = gl_load_ext(gl_ext_loader, "glShaderSource");
    glCreateShader            = gl_load_ext(gl_ext_loader, "glCreateShader");
    glCreateProgram           = gl_load_ext(gl_ext_loader, "glCreateProgram");
    glGetShaderInfoLog        = gl_load_ext(gl_ext_loader, "glGetShaderInfoLog");
    glGetShaderiv             = gl_load_ext(gl_ext_loader, "glGetShaderiv");
    glDeleteBuffers           = gl_load_ext(gl_ext_loader, "glDeleteBuffers");
    glVertexAttribPointer     = gl_load_ext(gl_ext_loader, "glVertexAttribPointer");
    glEnableVertexAttribArray = gl_load_ext(gl_ext_loader, "glEnableVertexAttribArray");
    glBindBuffer              = gl_load_ext(gl_ext_loader, "glBindBuffer");
    glGenBuffers              = gl_load_ext(gl_ext_loader, "glGenBuffers");
    glDeleteFramebuffers      = gl_load_ext(gl_ext_loader, "glDeleteFramebuffers");
    glFramebufferRenderbuffer = gl_load_ext(gl_ext_loader, "glFramebufferRenderbuffer");
    glBindBuffer              = gl_load_ext(gl_ext_loader, "glBindBuffer");
    glGenBuffers              = gl_load_ext(gl_ext_loader, "glGenBuffers");
    glDeleteFramebuffers      = gl_load_ext(gl_ext_loader, "glDeleteFramebuffers");
    glFramebufferTexture2D    = gl_load_ext(gl_ext_loader, "glFramebufferTexture2D");
    glBindFramebuffer         = gl_load_ext(gl_ext_loader, "glBindFramebuffer");
    glRenderbufferStorage     = gl_load_ext(gl_ext_loader, "glRenderbufferStorage");
    glDeleteRenderbuffers     = gl_load_ext(gl_ext_loader, "glDeleteRenderbuffers");
    glBindRenderbuffer        = gl_load_ext(gl_ext_loader, "glBindRenderbuffer");
    glGenRenderbuffers        = gl_load_ext(gl_ext_loader, "glGenRenderbuffers");
    glGenFramebuffers         = gl_load_ext(gl_ext_loader, "glGenFramebuffers");
    glGenerateMipmap          = gl_load_ext(gl_ext_loader, "glGenerateMipmap");
#ifdef DEBUG
    glDebugMessageCallback   = gl_load_ext(gl_ext_loader, "glDebugMessageCallback");
    glCheckFramebufferStatus = gl_load_ext(gl_ext_loader, "glCheckFramebufferStatus");
#endif
    LOG("all gl extensions loaded succesfully\n");
}
