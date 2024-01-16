#include "gl2_util.h"

PFNGLBUFFERSUBDATAARBPROC        glBufferSubData_;
PFNGLUNIFORM4FPROC               glUniform4f_;
PFNGLUNIFORM3FPROC               glUniform3f_;
PFNGLUNIFORM2FPROC               glUniform2f_;
PFNGLBUFFERDATAPROC              glBufferData_;
PFNGLDELETEPROGRAMPROC           glDeleteProgram_;
PFNGLUSEPROGRAMPROC              glUseProgram_;
PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation_;
PFNGLGETATTRIBLOCATIONPROC       glGetAttribLocation_;
PFNGLDELETESHADERPROC            glDeleteShader_;
PFNGLDETACHSHADERPROC            glDetachShader_;
PFNGLGETPROGRAMINFOLOGPROC       glGetProgramInfoLog_;
PFNGLGETPROGRAMIVPROC            glGetProgramiv_;
PFNGLLINKPROGRAMPROC             glLinkProgram_;
PFNGLATTACHSHADERPROC            glAttachShader_;
PFNGLCOMPILESHADERPROC           glCompileShader_;
PFNGLSHADERSOURCEPROC            glShaderSource_;
PFNGLCREATESHADERPROC            glCreateShader_;
PFNGLCREATEPROGRAMPROC           glCreateProgram_;
PFNGLGETSHADERINFOLOGPROC        glGetShaderInfoLog_;
PFNGLGETSHADERIVPROC             glGetShaderiv_;
PFNGLDELETEBUFFERSPROC           glDeleteBuffers_;
PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer_;
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray_;
PFNGLBINDBUFFERPROC              glBindBuffer_;
PFNGLGENBUFFERSPROC              glGenBuffers_;
PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers_;
PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D_;
PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer_;
PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers_;
PFNGLGENERATEMIPMAPPROC          glGenerateMipmap_;
PFNGLBLENDFUNCSEPARATEPROC       glBlendFuncSeparate_;

#ifndef GFX_GLES
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer_;
PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage_;
PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer_;
PFNGLGENRENDERBUFFERSPROC        glGenRenderbuffers_;
PFNGLDELETERENDERBUFFERSPROC     glDeleteRenderbuffers_;
#endif

#ifdef DEBUG
PFNGLDEBUGMESSAGECALLBACKPROC   glDebugMessageCallback_;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus_;
#endif

void gl2_maybe_load_gl_exts(void* loader, void* (*loader_func)(void* loader, const char* proc_name))
{
    static bool loaded = false;
    if (loaded)
        return;

    glBufferSubData_           = loader_func(loader, "glBufferSubData");
    glUniform4f_               = loader_func(loader, "glUniform4f");
    glUniform3f_               = loader_func(loader, "glUniform3f");
    glUniform2f_               = loader_func(loader, "glUniform2f");
    glBufferData_              = loader_func(loader, "glBufferData");
    glDeleteProgram_           = loader_func(loader, "glDeleteProgram");
    glUseProgram_              = loader_func(loader, "glUseProgram");
    glGetUniformLocation_      = loader_func(loader, "glGetUniformLocation");
    glGetAttribLocation_       = loader_func(loader, "glGetAttribLocation");
    glDeleteShader_            = loader_func(loader, "glDeleteShader");
    glDetachShader_            = loader_func(loader, "glDetachShader");
    glGetProgramInfoLog_       = loader_func(loader, "glGetProgramInfoLog");
    glGetProgramiv_            = loader_func(loader, "glGetProgramiv");
    glLinkProgram_             = loader_func(loader, "glLinkProgram");
    glAttachShader_            = loader_func(loader, "glAttachShader");
    glCompileShader_           = loader_func(loader, "glCompileShader");
    glShaderSource_            = loader_func(loader, "glShaderSource");
    glCreateShader_            = loader_func(loader, "glCreateShader");
    glCreateProgram_           = loader_func(loader, "glCreateProgram");
    glGetShaderInfoLog_        = loader_func(loader, "glGetShaderInfoLog");
    glGetShaderiv_             = loader_func(loader, "glGetShaderiv");
    glDeleteBuffers_           = loader_func(loader, "glDeleteBuffers");
    glVertexAttribPointer_     = loader_func(loader, "glVertexAttribPointer");
    glEnableVertexAttribArray_ = loader_func(loader, "glEnableVertexAttribArray");
    glBindBuffer_              = loader_func(loader, "glBindBuffer");
    glGenBuffers_              = loader_func(loader, "glGenBuffers");
    glDeleteFramebuffers_      = loader_func(loader, "glDeleteFramebuffers");
    glFramebufferTexture2D_    = loader_func(loader, "glFramebufferTexture2D");
    glBindFramebuffer_         = loader_func(loader, "glBindFramebuffer");
    glGenFramebuffers_         = loader_func(loader, "glGenFramebuffers");
    glGenerateMipmap_          = loader_func(loader, "glGenerateMipmap");
    glBlendFuncSeparate_       = loader_func(loader, "glBlendFuncSeparate");

#ifndef GFX_GLES
    glFramebufferRenderbuffer_ = loader_func(loader, "glFramebufferRenderbuffer");
    glRenderbufferStorage_     = loader_func(loader, "glRenderbufferStorage");
    glDeleteRenderbuffers_     = loader_func(loader, "glDeleteRenderbuffers");
    glBindRenderbuffer_        = loader_func(loader, "glBindRenderbuffer");
    glGenRenderbuffers_        = loader_func(loader, "glGenRenderbuffers");
#endif

#ifdef DEBUG
    glDebugMessageCallback_   = loader_func(loader, "glDebugMessageCallback");
    glCheckFramebufferStatus_ = loader_func(loader, "glCheckFramebufferStatus");
#endif

    loaded = true;
}
