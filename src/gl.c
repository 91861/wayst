#include "gl.h"
#include <GL/gl.h>
#include "util.h"
#include <unistd.h>

void* (*gl_load_ext)(const char* procname) = NULL;

PFNGLBUFFERSUBDATAARBPROC glBufferSubData;
PFNGLUNIFORM4FPROC glUniform4f;
PFNGLUNIFORM3FPROC glUniform3f;
PFNGLUNIFORM2FPROC glUniform2f;
PFNGLBUFFERDATAPROC glBufferData;
PFNGLDELETEPROGRAMPROC glDeleteProgram;
PFNGLUSEPROGRAMPROC glUseProgram;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation;
PFNGLDELETESHADERPROC glDeleteShader;
PFNGLDETACHSHADERPROC glDetachShader;
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
PFNGLGETPROGRAMIVPROC glGetProgramiv;
PFNGLLINKPROGRAMPROC glLinkProgram;
PFNGLATTACHSHADERPROC glAttachShader;
PFNGLCOMPILESHADERPROC glCompileShader;
PFNGLSHADERSOURCEPROC glShaderSource;
PFNGLCREATESHADERPROC glCreateShader;
PFNGLCREATEPROGRAMPROC glCreateProgram;
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
PFNGLGETSHADERIVPROC glGetShaderiv;
PFNGLDELETEBUFFERSPROC glDeleteBuffers;
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
PFNGLBINDBUFFERPROC glBindBuffer;
PFNGLGENBUFFERSPROC glGenBuffers;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
PFNGLBINDBUFFERPROC glBindBuffer;
PFNGLGENBUFFERSPROC glGenBuffers;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
PFNGLGENERATEMIPMAPPROC glGenerateMipmap;
#ifdef DEBUG
PFNGLDEBUGMESSAGECALLBACKPROC glDebugMessageCallback;
#endif

void
gl_load_exts()
{
    if (!gl_load_ext) {
        ERR("gl extension loader not set");
    }

    glBufferSubData           = gl_load_ext("glBufferSubData");
    glUniform4f               = gl_load_ext("glUniform4f");
    glUniform3f               = gl_load_ext("glUniform3f");
    glUniform2f               = gl_load_ext("glUniform2f");
    glBufferData              = gl_load_ext("glBufferData");
    glDeleteProgram           = gl_load_ext("glDeleteProgram");
    glUseProgram              = gl_load_ext("glUseProgram");
    glGetUniformLocation      = gl_load_ext("glGetUniformLocation");
    glGetAttribLocation       = gl_load_ext("glGetAttribLocation");
    glDeleteShader            = gl_load_ext("glDeleteShader");
    glDetachShader            = gl_load_ext("glDetachShader");
    glGetProgramInfoLog       = gl_load_ext("glGetProgramInfoLog");
    glGetProgramiv            = gl_load_ext("glGetProgramiv");
    glLinkProgram             = gl_load_ext("glLinkProgram");
    glAttachShader            = gl_load_ext("glAttachShader");
    glCompileShader           = gl_load_ext("glCompileShader");
    glShaderSource            = gl_load_ext("glShaderSource");
    glCreateShader            = gl_load_ext("glCreateShader");
    glCreateProgram           = gl_load_ext("glCreateProgram");
    glGetShaderInfoLog        = gl_load_ext("glGetShaderInfoLog");
    glGetShaderiv             = gl_load_ext("glGetShaderiv");
    glDeleteBuffers           = gl_load_ext("glDeleteBuffers");
    glVertexAttribPointer     = gl_load_ext("glVertexAttribPointer");
    glEnableVertexAttribArray = gl_load_ext("glEnableVertexAttribArray");
    glBindBuffer              = gl_load_ext("glBindBuffer");
    glGenBuffers              = gl_load_ext("glGenBuffers");
    glDeleteFramebuffers      = gl_load_ext("glDeleteFramebuffers");
    glFramebufferRenderbuffer = gl_load_ext("glFramebufferRenderbuffer");
    glRenderbufferStorage     = gl_load_ext("glRenderbufferStorage");
    glBindBuffer              = gl_load_ext("glBindBuffer");
    glGenBuffers              = gl_load_ext("glGenBuffers");
    glDeleteFramebuffers      = gl_load_ext("glDeleteFramebuffers");
    glCheckFramebufferStatus  = gl_load_ext("glCheckFramebufferStatus");
    glFramebufferTexture2D    = gl_load_ext("glFramebufferTexture2D");
    glBindFramebuffer         = gl_load_ext("glBindFramebuffer");
    glBindRenderbuffer        = gl_load_ext("glBindRenderbuffer");
    glGenRenderbuffers        = gl_load_ext("glGenRenderbuffers");
    glGenFramebuffers         = gl_load_ext("glGenFramebuffers");
    glGenerateMipmap          = gl_load_ext("glGenerateMipmap");
    #ifdef DEBUG
    glDebugMessageCallback    = gl_load_ext("glDebugMessageCallback");
    #endif
    LOG("all gl extensions loaded succesfully\n");
}
