/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "colors.h"
#include "ui.h"

#include "gfx_gl2.h"
#include "vt.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "freetype.h"
#include "fterrors.h"
#include <GL/gl.h>

#include "map.h"
#include "util.h"
#include "wcwidth/wcwidth.h"

#ifdef GFX_GLES
#include "shaders_gles20.h"
#else
#include "shaders_gl21.h"
#endif

DEF_PAIR(GLuint);

/* Number of buckets in the glyph atlas reference data hash map */
#define NUM_BUCKETS 513

/* Maximum number of frames we record damage for */
#define MAX_TRACKED_FRAME_DAMAGE 6

/* Maximum number of damaged cells that don't cause full surface damage */
#define CELL_DAMAGE_TO_SURF_LIMIT 10

#define ATLAS_SIZE_LIMIT INT32_MAX

#define DIM_COLOR_BLEND_FACTOR 0.4f

/* Maximum number of textures stored for reuse */
#define N_RECYCLED_TEXTURES 5

#define PROXY_INDEX_TEXTURE       0
#define PROXY_INDEX_TEXTURE_BLINK 1

#ifndef GFX_GLES
#define PROXY_INDEX_DEPTHBUFFER       2
#define PROXY_INDEX_DEPTHBUFFER_BLINK 3
#endif

#define IMG_PROXY_INDEX_TEXTURE_ID 0

#define IMG_VIEW_PROXY_INDEX_VBO_ID 0

#define SIXEL_PROXY_INDEX_TEXTURE_ID 0
#define SIXEL_PROXY_INDEX_VBO_ID     1

#define BOUND_RESOURCES_NONE      0
#define BOUND_RESOURCES_BG        1
#define BOUND_RESOURCES_FONT      2
#define BOUND_RESOURCES_LINES     3
#define BOUND_RESOURCES_IMAGE     4
#define BOUND_RESOURCES_FONT_MONO 5

/* GLES does not support GL_QUADS */
#ifdef GFX_GLES
#define QUAD_DRAW_MODE GL_TRIANGLES
#define QUAD_V_SZ      6 /* number of verts per quad (GL_TRIANGLES) */
#else
#define QUAD_DRAW_MODE GL_QUADS
#define QUAD_V_SZ      4 /* number of verts per quad (GL_QUADS) */
#endif

#include "gl_exts/glext.h"

static PFNGLBUFFERSUBDATAARBPROC        glBufferSubData;
static PFNGLUNIFORM4FPROC               glUniform4f;
static PFNGLUNIFORM3FPROC               glUniform3f;
static PFNGLUNIFORM2FPROC               glUniform2f;
static PFNGLBUFFERDATAPROC              glBufferData;
static PFNGLDELETEPROGRAMPROC           glDeleteProgram;
static PFNGLUSEPROGRAMPROC              glUseProgram;
static PFNGLGETUNIFORMLOCATIONPROC      glGetUniformLocation;
static PFNGLGETATTRIBLOCATIONPROC       glGetAttribLocation;
static PFNGLDELETESHADERPROC            glDeleteShader;
static PFNGLDETACHSHADERPROC            glDetachShader;
static PFNGLGETPROGRAMINFOLOGPROC       glGetProgramInfoLog;
static PFNGLGETPROGRAMIVPROC            glGetProgramiv;
static PFNGLLINKPROGRAMPROC             glLinkProgram;
static PFNGLATTACHSHADERPROC            glAttachShader;
static PFNGLCOMPILESHADERPROC           glCompileShader;
static PFNGLSHADERSOURCEPROC            glShaderSource;
static PFNGLCREATESHADERPROC            glCreateShader;
static PFNGLCREATEPROGRAMPROC           glCreateProgram;
static PFNGLGETSHADERINFOLOGPROC        glGetShaderInfoLog;
static PFNGLGETSHADERIVPROC             glGetShaderiv;
static PFNGLDELETEBUFFERSPROC           glDeleteBuffers;
static PFNGLVERTEXATTRIBPOINTERPROC     glVertexAttribPointer;
static PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
static PFNGLBINDBUFFERPROC              glBindBuffer;
static PFNGLGENBUFFERSPROC              glGenBuffers;
static PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
static PFNGLBINDBUFFERPROC              glBindBuffer;
static PFNGLGENBUFFERSPROC              glGenBuffers;
static PFNGLDELETEFRAMEBUFFERSPROC      glDeleteFramebuffers;
static PFNGLFRAMEBUFFERTEXTURE2DPROC    glFramebufferTexture2D;
static PFNGLBINDFRAMEBUFFERPROC         glBindFramebuffer;
static PFNGLGENFRAMEBUFFERSPROC         glGenFramebuffers;
static PFNGLGENERATEMIPMAPPROC          glGenerateMipmap;
static PFNGLBLENDFUNCSEPARATEPROC       glBlendFuncSeparate;

#ifndef GFX_GLES
static PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
static PFNGLRENDERBUFFERSTORAGEPROC     glRenderbufferStorage;
static PFNGLBINDRENDERBUFFERPROC        glBindRenderbuffer;
static PFNGLGENRENDERBUFFERSPROC        glGenRenderbuffers;
static PFNGLDELETERENDERBUFFERSPROC     glDeleteRenderbuffers;
#endif

#ifdef DEBUG
static PFNGLDEBUGMESSAGECALLBACKPROC   glDebugMessageCallback;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
#endif


static void maybe_load_gl_exts(void* loader,
                               void* (*loader_func)(void* loader, const char* proc_name))
{
    static bool loaded = false;
    if (loaded)
        return;

    glBufferSubData           = loader_func(loader, "glBufferSubData");
    glUniform4f               = loader_func(loader, "glUniform4f");
    glUniform3f               = loader_func(loader, "glUniform3f");
    glUniform2f               = loader_func(loader, "glUniform2f");
    glBufferData              = loader_func(loader, "glBufferData");
    glDeleteProgram           = loader_func(loader, "glDeleteProgram");
    glUseProgram              = loader_func(loader, "glUseProgram");
    glGetUniformLocation      = loader_func(loader, "glGetUniformLocation");
    glGetAttribLocation       = loader_func(loader, "glGetAttribLocation");
    glDeleteShader            = loader_func(loader, "glDeleteShader");
    glDetachShader            = loader_func(loader, "glDetachShader");
    glGetProgramInfoLog       = loader_func(loader, "glGetProgramInfoLog");
    glGetProgramiv            = loader_func(loader, "glGetProgramiv");
    glLinkProgram             = loader_func(loader, "glLinkProgram");
    glAttachShader            = loader_func(loader, "glAttachShader");
    glCompileShader           = loader_func(loader, "glCompileShader");
    glShaderSource            = loader_func(loader, "glShaderSource");
    glCreateShader            = loader_func(loader, "glCreateShader");
    glCreateProgram           = loader_func(loader, "glCreateProgram");
    glGetShaderInfoLog        = loader_func(loader, "glGetShaderInfoLog");
    glGetShaderiv             = loader_func(loader, "glGetShaderiv");
    glDeleteBuffers           = loader_func(loader, "glDeleteBuffers");
    glVertexAttribPointer     = loader_func(loader, "glVertexAttribPointer");
    glEnableVertexAttribArray = loader_func(loader, "glEnableVertexAttribArray");
    glBindBuffer              = loader_func(loader, "glBindBuffer");
    glGenBuffers              = loader_func(loader, "glGenBuffers");
    glDeleteFramebuffers      = loader_func(loader, "glDeleteFramebuffers");

#ifndef GFX_GLES
    glFramebufferRenderbuffer = loader_func(loader, "glFramebufferRenderbuffer");
    glRenderbufferStorage     = loader_func(loader, "glRenderbufferStorage");
    glDeleteRenderbuffers     = loader_func(loader, "glDeleteRenderbuffers");
    glBindRenderbuffer        = loader_func(loader, "glBindRenderbuffer");
    glGenRenderbuffers        = loader_func(loader, "glGenRenderbuffers");
#endif

    glBindBuffer           = loader_func(loader, "glBindBuffer");
    glGenBuffers           = loader_func(loader, "glGenBuffers");
    glDeleteFramebuffers   = loader_func(loader, "glDeleteFramebuffers");
    glFramebufferTexture2D = loader_func(loader, "glFramebufferTexture2D");
    glBindFramebuffer      = loader_func(loader, "glBindFramebuffer");
    glGenFramebuffers      = loader_func(loader, "glGenFramebuffers");
    glGenerateMipmap       = loader_func(loader, "glGenerateMipmap");
    glBlendFuncSeparate    = loader_func(loader, "glBlendFuncSeparate");
#ifdef DEBUG
    glDebugMessageCallback   = loader_func(loader, "glDebugMessageCallback");
    glCheckFramebufferStatus = loader_func(loader, "glCheckFramebufferStatus");
#endif

    loaded = true;
}

#include "gl.h"

#ifdef DEBUG
static size_t dbg_line_proxy_textures_created = 0;
static size_t dbg_line_proxy_textures_destroyed = 0;
#define DBG_DELTEX                                                                                 \
    ++dbg_line_proxy_textures_destroyed;                                                           \
    INFO("proxy-- created: %zu, destroyed: %zu (total: %zu)\n",                                    \
         dbg_line_proxy_textures_created,                                                          \
         dbg_line_proxy_textures_destroyed,                                                        \
         (dbg_line_proxy_textures_created - dbg_line_proxy_textures_destroyed))

#define DBG_MAKETEX                                                                                \
    ++dbg_line_proxy_textures_created;                                                             \
    INFO("proxy++ created: %zu, destroyed: %zu (total: %zu)\n",                                    \
         dbg_line_proxy_textures_created,                                                          \
         dbg_line_proxy_textures_destroyed,                                                        \
         (dbg_line_proxy_textures_created - dbg_line_proxy_textures_destroyed))
#else
#define DBG_DELTEX ;
#define DBG_MAKETEX ;
#endif


enum GlyphColor
{
    GLYPH_COLOR_MONO,
    GLYPH_COLOR_LCD,
    GLYPH_COLOR_COLOR,
};

DEF_VECTOR(Texture, Texture_destroy);

static inline size_t Rune_hash(const Rune* self)
{
    return self->code;
}

static inline size_t Rune_eq(const Rune* self, const Rune* other)
{
    bool combinable_eq = true;
    for (int i = 0; i < VT_RUNE_MAX_COMBINE; ++i) {
        if (self->combine[i] == other->combine[i]) {
            if (!self->combine[i])
                break;
            continue;
        } else {
            combinable_eq = false;
            break;
        }
    }
    return self->code == other->code && self->style == other->style && combinable_eq;
}

#define ATLAS_RENDERABLE_START ' '
#define ATLAS_RENDERABLE_END   CHAR_MAX

typedef struct
{
    float x, y;
} vertex_t;

DEF_VECTOR(float, NULL);
DEF_VECTOR(Vector_float, Vector_destroy_float);

DEF_VECTOR(vertex_t, NULL);

typedef struct
{
    GLuint color_tex;
    GLuint depth_rb;
} LineTexture;

static void LineTexture_destroy(LineTexture* self)
{
    if (self->color_tex) {
        DBG_DELTEX
        glDeleteTextures(1, &self->color_tex);
        self->color_tex = 0;

#ifndef GFX_GLES
        ASSERT(self->depth_rb, "deleted texture has depth renderbuffer");
        glDeleteRenderbuffers(1, &self->depth_rb);
        self->depth_rb = 0;
#endif
    }
}

typedef struct
{
    GLint framebuffer;
    GLint shader;

#ifndef GFX_GLES
    GLboolean depth_test;
#endif

    GLboolean scissor_test;
    GLboolean blend;
    GLint     viewport[4];
    GLint     blend_dst, blend_src;
} stored_common_gl_state_t;

static stored_common_gl_state_t store_common_state()
{
    stored_common_gl_state_t state;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state.framebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state.shader);

#ifndef GFX_GLES
    glGetBooleanv(GL_DEPTH_TEST, &state.depth_test);
#endif

    glGetBooleanv(GL_SCISSOR_TEST, &state.scissor_test);
    glGetBooleanv(GL_BLEND, &state.blend);
    glGetIntegerv(GL_VIEWPORT, state.viewport);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &state.blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &state.blend_dst);
    return state;
}

static void restore_gl_state(stored_common_gl_state_t* state)
{
    glUseProgram(state->shader);
    glBindFramebuffer(GL_FRAMEBUFFER, state->framebuffer);
    glViewport(state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
    glBlendFunc(state->blend_src, state->blend_dst);

#ifndef GFX_GLES
    if (state->depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
#endif

    if (state->scissor_test) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }

    if (state->blend) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static uint32_t titlebar_height_px(Ui* ui)
{
    return Ui_csd_titlebar_visible(ui) ? UI_CSD_TITLEBAR_HEIGHT_PX : 0;
}

struct _GfxOpenGL2;

typedef struct
{
    uint32_t           page_id;
    GLuint             texture_id;
    GLenum             internal_format;
    enum TextureFormat texture_format;
    uint32_t           width_px, height_px;
    uint32_t           current_line_height_px, current_offset_y, current_offset_x;
    float              sx, sy;
} GlyphAtlasPage;

typedef struct
{
    uint8_t page_id;
    GLuint  texture_id;

    float   left, top;
    int32_t height, width;
    float   tex_coords[4];
} GlyphAtlasEntry;

static void GlyphAtlasPage_destroy(GlyphAtlasPage* self)
{
    if (self->texture_id) {
        glDeleteTextures(1, &self->texture_id);
        self->texture_id = 0;
    }
}

DEF_VECTOR(GlyphAtlasPage, GlyphAtlasPage_destroy);
DEF_MAP(Rune, GlyphAtlasEntry, Rune_hash, Rune_eq, NULL);

typedef struct
{
    Vector_GlyphAtlasPage    pages;
    GlyphAtlasPage*          current_rgb_page;
    GlyphAtlasPage*          current_rgba_page;
    GlyphAtlasPage*          current_grayscale_page;
    Map_Rune_GlyphAtlasEntry entry_map;
    uint32_t                 page_size_px;
    uint32_t                 color_page_size_px;
} GlyphAtlas;

typedef struct
{
    uint32_t width, height, top, left;
} freetype_output_scaling_t;

typedef struct
{
    uint32_t curosr_position_x;
    uint32_t curosr_position_y;
    uint16_t line_index;
    bool     cursor_drawn;
    bool     overlay_state;
} overlay_damage_record_t;

typedef struct
{
    bool*     damage_history;
    uint32_t* proxy_color_component;
    uint16_t* line_length;

    uint16_t n_lines;
} lines_damage_record_t;

typedef struct _GfxOpenGL2
{
    GLint max_tex_res;

    Vector_vertex_t vec_vertex_buffer;
    Vector_vertex_t vec_vertex_buffer2;

    VBO flex_vbo;

    GLuint full_framebuffer_quad_vbo;
    GLuint line_quads_vbo;

    /* pen position to begin drawing font */
    float pen_begin_y;
    int   pen_begin_pixels_y;
    int   pen_begin_pixels_x;

    uint32_t win_w, win_h;
    float    line_height, glyph_width;
    uint16_t line_height_pixels, glyph_width_pixels;
    size_t   max_cells_in_line;
    float    sx, sy;
    uint32_t gw;

    /* padding offset from the top right corner */
    uint8_t pixel_offset_x;
    uint8_t pixel_offset_y;

    GLuint line_framebuffer;

    Shader solid_fill_shader;
    Shader font_shader;
    Shader font_shader_blend;
    Shader font_shader_gray;
    Shader line_shader;
    Shader image_shader;
    Shader image_tint_shader;
    Shader circle_shader;

    GLuint csd_close_button_vbo;

    ColorRGB  color;
    ColorRGBA bg_color;

    GlyphAtlas          glyph_atlas;
    Vector_Vector_float float_vec;

    // keep textures for reuse in order of length
    LineTexture recycled_textures[5];

    Texture squiggle_texture;
    Texture csd_close_button_texture;

    TimePoint blink_switch;
    TimePoint blink_switch_text;
    TimePoint action;
    TimePoint inactive;

    bool is_main_font_rgb;

    Freetype* freetype;

    int_fast8_t bound_resources;

    Pair_uint32_t cells;

    window_partial_swap_request_t modified_region;

    lines_damage_record_t   line_damage;
    overlay_damage_record_t frame_overlay_damage[MAX_TRACKED_FRAME_DAMAGE];
} GfxOpenGL2;

#define gfxBase(gfxGl2) ((Gfx*)(((uint8_t*)(gfxGl2)) - offsetof(Gfx, extend_data)))
Pair_uint32_t GfxOpenGL2_get_char_size(Gfx* self, Pair_uint32_t pixels);
#define gfxOpenGL2(gfx) ((GfxOpenGL2*)&gfx->extend_data)

void GfxOpenGL2_external_framebuffer_damage(Gfx* self)
{
    GfxOpenGL2* gl2     = gfxOpenGL2(self);
    uint32_t    n_lines = gl2->cells.second;

    for (int i = 0; i < MAX_TRACKED_FRAME_DAMAGE; ++i) {
        gl2->frame_overlay_damage[i].overlay_state = 1;
    }

    memset(gl2->line_damage.damage_history, 1, MAX_TRACKED_FRAME_DAMAGE * MAX(1, n_lines));
    memset(gl2->line_damage.proxy_color_component,
           0,
           MAX_TRACKED_FRAME_DAMAGE * n_lines * sizeof(uint32_t));
}

static void GfxOpenGL2_realloc_damage_record(GfxOpenGL2* self, uint32_t n_lines)
{
    free(self->line_damage.damage_history);
    free(self->line_damage.proxy_color_component);
    free(self->line_damage.line_length);

    self->line_damage.damage_history = _calloc(MAX_TRACKED_FRAME_DAMAGE, MAX(1, n_lines));
    self->line_damage.line_length =
      _calloc(MAX_TRACKED_FRAME_DAMAGE * sizeof(uint16_t), MAX(1, n_lines));

    self->line_damage.proxy_color_component =
      _calloc(MAX_TRACKED_FRAME_DAMAGE * sizeof(uint32_t), MAX(1, n_lines));

    memset(self->line_damage.damage_history, 1, MAX_TRACKED_FRAME_DAMAGE * MAX(1, n_lines));

    for (int i = 0; i < MAX_TRACKED_FRAME_DAMAGE; ++i) {
        self->frame_overlay_damage[i].overlay_state = 1;
    }
}

static void GfxOpenGL2_rotate_damage_record(GfxOpenGL2* self)
{
    memmove(self->line_damage.damage_history + self->line_damage.n_lines,
            self->line_damage.damage_history,
            (MAX_TRACKED_FRAME_DAMAGE - 1) * sizeof(bool));
    memset(self->line_damage.damage_history, 0, self->line_damage.n_lines * sizeof(bool));

    memmove(self->line_damage.proxy_color_component + self->line_damage.n_lines,
            self->line_damage.proxy_color_component,
            (MAX_TRACKED_FRAME_DAMAGE - 1) * sizeof(uint32_t));
    memset(self->line_damage.proxy_color_component,
           0,
           self->line_damage.n_lines * sizeof(uint32_t));

    memmove(self->line_damage.line_length + self->line_damage.n_lines,
            self->line_damage.line_length,
            (MAX_TRACKED_FRAME_DAMAGE - 1) * sizeof(uint16_t));

    memmove(self->frame_overlay_damage + 1,
            self->frame_overlay_damage,
            (MAX_TRACKED_FRAME_DAMAGE - 1) * sizeof(*self->frame_overlay_damage));

    self->frame_overlay_damage->overlay_state = false;
}

static freetype_output_scaling_t scale_ft_glyph(GfxOpenGL2* gfx, FreetypeOutput* glyph);
Pair_GLuint                      GfxOpenGL2_pop_recycled(GfxOpenGL2* self);
void                             GfxOpenGL2_load_font(Gfx* self);
void                             GfxOpenGL2_destroy_recycled(GfxOpenGL2* self);

void                           GfxOpenGL2_destroy(Gfx* self);
window_partial_swap_request_t* GfxOpenGL2_draw(Gfx* self, const Vt* vt, Ui* ui, uint8_t age);
Pair_uint32_t                  GfxOpenGL2_get_char_size(Gfx* self, Pair_uint32_t pixels);
void          GfxOpenGL2_resize(Gfx* self, uint32_t w, uint32_t h, Pair_uint32_t cells);
void          GfxOpenGL2_init_with_context_activated(Gfx* self);
bool          GfxOpenGL2_set_focus(Gfx* self, bool focus);
Pair_uint32_t GfxOpenGL2_pixels(Gfx* self, uint32_t c, uint32_t r);
void          GfxOpenGL2_reload_font(Gfx* self);
void          GfxOpenGL2_external_framebuffer_damage(Gfx* self);
void          GfxOpenGL2_destroy_proxy(Gfx* self, uint32_t* proxy);
void          GfxOpenGL2_destroy_image_proxy(Gfx* self, uint32_t* proxy);
void          GfxOpenGL2_destroy_image_view_proxy(Gfx* self, uint32_t* proxy);
void          GfxOpenGL2_destroy_sixel_proxy(Gfx* self, uint32_t* proxy);
static void   GfxOpenGL2_regenerate_line_quad_vbo(GfxOpenGL2* gfx, uint32_t n_lines);

static struct IGfx gfx_interface_opengl2 = {
    .draw                        = GfxOpenGL2_draw,
    .resize                      = GfxOpenGL2_resize,
    .get_char_size               = GfxOpenGL2_get_char_size,
    .init_with_context_activated = GfxOpenGL2_init_with_context_activated,
    .reload_font                 = GfxOpenGL2_reload_font,
    .pixels                      = GfxOpenGL2_pixels,
    .destroy                     = GfxOpenGL2_destroy,
    .destroy_proxy               = GfxOpenGL2_destroy_proxy,
    .destroy_image_proxy         = GfxOpenGL2_destroy_image_proxy,
    .destroy_image_view_proxy    = GfxOpenGL2_destroy_image_view_proxy,
    .destroy_sixel_proxy         = GfxOpenGL2_destroy_sixel_proxy,
    .external_framebuffer_damage = GfxOpenGL2_external_framebuffer_damage,
};

Gfx* Gfx_new_OpenGL2(Freetype* freetype)
{
    Gfx* self                  = _calloc(1, sizeof(Gfx) + sizeof(GfxOpenGL2) - sizeof(uint8_t));
    self->interface            = &gfx_interface_opengl2;
    gfxOpenGL2(self)->freetype = freetype;
    gfxOpenGL2(self)->is_main_font_rgb = !(freetype->primary_output_type == FT_OUTPUT_GRAYSCALE);
    GfxOpenGL2_load_font(self);
    return self;
}

#define ARRAY_BUFFER_SUB_OR_SWAP(_buf, _size, _newsize)                                            \
    if ((_newsize) > _size) {                                                                      \
        _size = (_newsize);                                                                        \
        glBufferData(GL_ARRAY_BUFFER, (_newsize), (_buf), GL_STREAM_DRAW);                         \
    } else {                                                                                       \
        glBufferSubData(GL_ARRAY_BUFFER, 0, (_newsize), (_buf));                                   \
    }

/*
 * GL2.X does not provide any "proper" way to signal that synchronization is not required.
 * Reallocating the buffer drops all references to the previous data (other than the possibly still
 * running/queued draw command) and hopefully the driver realizes this and can paralelize the next
 * data transfer.
 *
 * https://www.khronos.org/opengl/wiki/Buffer_Object_Streaming#Buffer_re-specification
 * https://www.khronos.org/opengl/wiki/Buffer_Object#Streaming
 * */
#define ARRAY_BUFFER_ORPHAN(_size) glBufferData(GL_ARRAY_BUFFER, (_size), NULL, GL_STREAM_DRAW);

static GlyphAtlasPage GlyphAtlasPage_new(GfxOpenGL2*        gfx,
                                         uint32_t           page_id,
                                         bool               filter,
                                         GLenum             internal_texture_format,
                                         enum TextureFormat texture_format,
                                         GLint              width_px,
                                         GLint              height_px)
{
    GlyphAtlasPage self;
    self.page_id                = page_id;
    self.current_offset_x       = 0;
    self.current_offset_y       = 0;
    self.current_line_height_px = 0;
    self.width_px               = MIN(gfx->max_tex_res, width_px);
    self.height_px              = MIN(gfx->max_tex_res, height_px);
    self.sx                     = 2.0 / self.width_px;
    self.sy                     = 2.0 / self.height_px;
    self.texture_format         = texture_format;
    self.internal_format        = internal_texture_format;
    self.texture_id             = 0;

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &self.texture_id);
    glBindTexture(GL_TEXTURE_2D, self.texture_id);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter ? GL_LINEAR : GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 self.internal_format,
                 self.width_px,
                 self.height_px,
                 0,
                 self.internal_format,
                 GL_UNSIGNED_BYTE,
                 0);
    return self;
}

static inline bool GlyphAtlasPage_can_push(GfxOpenGL2*     gfx,
                                           GlyphAtlasPage* self,
                                           FreetypeOutput* glyph)
{
    if (unlikely(glyph->type == FT_OUTPUT_COLOR_BGRA)) {
        freetype_output_scaling_t scaling = scale_ft_glyph(gfx, glyph);

        return self->current_offset_y +
                   MAX((uint32_t)scaling.height, self->current_line_height_px) + 1 <
                 self->height_px &&
               self->current_offset_x + scaling.width + 1 < self->width_px;
    } else {
        return self->current_offset_y + MAX((uint32_t)glyph->height, self->current_line_height_px) +
                   1 <
                 self->height_px &&
               self->current_offset_x + glyph->width + 1 < self->width_px;
    }
}

static inline bool GlyphAtlasPage_can_push_tex(GlyphAtlasPage* self, Texture tex)
{
    return self->current_offset_y + MAX(tex.h, self->current_line_height_px) + 1 <
             self->height_px &&
           self->current_offset_x + tex.w + 1 < self->width_px;
}

static GlyphAtlasEntry GlyphAtlasPage_push_tex(GfxOpenGL2*                gfx,
                                               GlyphAtlasPage*            self,
                                               FreetypeOutput*            glyph,
                                               Texture                    tex,
                                               freetype_output_scaling_t* opt_scaling)
{
    ASSERT(GlyphAtlasPage_can_push_tex(self, tex), "does not overflow");

    uint32_t final_width, final_height, final_top, final_left;

    if (opt_scaling) {
        final_width  = opt_scaling->width;
        final_height = opt_scaling->height;
        final_top    = opt_scaling->top;
        final_left   = opt_scaling->left;
    } else {
        final_width  = tex.w;
        final_height = tex.h;
        final_top    = glyph->top;
        final_left   = glyph->left;
    }

    if (self->current_offset_x + final_width >= self->width_px) {
        self->current_offset_y += (self->current_line_height_px + 1);
        self->current_offset_x       = 0;
        self->current_line_height_px = 0;
    }

    self->current_line_height_px = MAX(self->current_line_height_px, final_height);

    stored_common_gl_state_t old_state = store_common_state();

    GLuint tmp_fb;
    glGenFramebuffers(1, &tmp_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, tmp_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           self->texture_id,
                           0);

#ifndef GFX_GLES
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
#endif
    glViewport(0, 0, self->width_px, self->height_px);

    glDisable(GL_SCISSOR_TEST);

    if (opt_scaling) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }

#ifndef GFX_GLES
    glDisable(GL_DEPTH_TEST);
#endif

    GLuint tmp_vbo;
    glGenBuffers(1, &tmp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, tmp_vbo);
    glUseProgram(gfx->image_shader.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);

    float sx = 2.0f / self->width_px;
    float sy = 2.0f / self->height_px;
    float w  = final_width * sx;
    float h  = final_height * sy;
    float x  = -1.0f + self->current_offset_x * sx;
    float y  = -1.0f + self->current_offset_y * sy + h;

#ifndef GFX_GLES
    float vbo_data[4][4] = {
        { x, y, 0.0f, 1.0f },
        { x + w, y, 1.0f, 1.0f },
        { x + w, y - h, 1.0f, 0.0f },
        { x, y - h, 0.0f, 0.0f },
    };
#else
    float vbo_data[][4] = {
        { x, y, 0.0f, 1.0f },         { x + w, y, 1.0f, 1.0f }, { x + w, y - h, 1.0f, 0.0f },
        { x + w, y - h, 1.0f, 0.0f }, { x, y, 0.0f, 1.0f },     { x, y - h, 0.0f, 0.0f },
    };
#endif

    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * QUAD_V_SZ, vbo_data, GL_STREAM_DRAW);
    glVertexAttribPointer(gfx->image_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);

    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDeleteFramebuffers(1, &tmp_fb);
    glDeleteBuffers(1, &tmp_vbo);

    restore_gl_state(&old_state);

    float tc[] = {
        (float)self->current_offset_x / self->width_px,
        1.0f - (((float)self->height_px - (float)self->current_offset_y) / self->height_px),
        (float)self->current_offset_x / self->width_px + (float)final_width / self->width_px,
        1.0f - (((float)self->height_px - (float)self->current_offset_y) / self->height_px -
                (float)final_height / self->height_px)
    };

    GlyphAtlasEntry retval = {
        .page_id    = self->page_id,
        .texture_id = self->texture_id,
        .left       = MIN(0, final_left),
        .top        = final_top,
        .height     = final_height,
        .width      = final_width,
        .tex_coords = { tc[0], tc[1], tc[2], tc[3] },
    };

    self->current_offset_x += final_width;
    return retval;
}

static freetype_output_scaling_t scale_ft_glyph(GfxOpenGL2* gfx, FreetypeOutput* glyph)
{
    double scale_factor = 1.0;
    if (glyph->height > gfx->line_height_pixels) {
        scale_factor = (double)gfx->line_height_pixels / (double)glyph->height;

        return (freetype_output_scaling_t){
            .width  = (double)glyph->width * scale_factor,
            .height = (double)glyph->height * scale_factor,
            .top    = (double)glyph->top * scale_factor,
            .left   = (double)glyph->left * scale_factor,
        };
    } else {
        return (freetype_output_scaling_t){
            .width  = glyph->width,
            .height = glyph->height,
            .top    = glyph->top,
            .left   = glyph->left,
        };
    }
}

static GlyphAtlasEntry GlyphAtlasPage_push(GfxOpenGL2*     gfx,
                                           GlyphAtlasPage* self,
                                           FreetypeOutput* glyph)
{
    ASSERT(GlyphAtlasPage_can_push(gfx, self, glyph), "does not overflow");

    if (self->current_offset_x + glyph->width >= self->width_px) {
        self->current_offset_y += (self->current_line_height_px + 1);
        self->current_offset_x       = 0;
        self->current_line_height_px = 0;
    }

    GLenum format;
    switch (glyph->type) {
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_BGR_V:
            format = GL_BGR;
            break;
        case FT_OUTPUT_RGB_H:
        case FT_OUTPUT_RGB_V:
            format = GL_RGB;
            break;
        case FT_OUTPUT_GRAYSCALE:
            format = GL_RED;
            break;
        case FT_OUTPUT_COLOR_BGRA:
            format = GL_BGRA;
            break;
        default:
            ASSERT_UNREACHABLE
    }

    GLsizei final_width = glyph->width, final_height = glyph->height, final_top = glyph->top,
            final_left = glyph->left;

    glPixelStorei(GL_UNPACK_ALIGNMENT, glyph->alignment);

    if (unlikely(glyph->type == FT_OUTPUT_COLOR_BGRA)) {
        freetype_output_scaling_t scale = scale_ft_glyph(gfx, glyph);
        final_height                    = scale.height;
        final_width                     = scale.width;
        final_top                       = scale.top;
        final_left                      = scale.left;

        GLuint tmp_tex;
        glGenTextures(1, &tmp_tex);
        glBindTexture(GL_TEXTURE_2D, tmp_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     glyph->width,
                     glyph->height,
                     0,
                     GL_BGRA,
                     GL_UNSIGNED_BYTE,
                     glyph->pixels);

        glGenerateMipmap(GL_TEXTURE_2D);

        Texture tex = {
            .format = TEX_FMT_RGBA,
            .w      = glyph->width,
            .h      = glyph->height,
            .id     = tmp_tex,
        };

        return GlyphAtlasPage_push_tex(gfx, self, glyph, tex, &scale);
    } else {
        glBindTexture(GL_TEXTURE_2D, self->texture_id);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        self->current_offset_x,
                        self->current_offset_y,
                        glyph->width,
                        glyph->height,
                        format,
                        GL_UNSIGNED_BYTE,
                        glyph->pixels);
    }

    self->current_line_height_px = MAX(self->current_line_height_px, (uint32_t)glyph->height);

    if (unlikely(glyph->type == FT_OUTPUT_COLOR_BGRA)) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    float tc[] = {
        (float)self->current_offset_x / self->width_px,
        1.0f - (((float)self->height_px - (float)self->current_offset_y) / self->height_px),
        (float)self->current_offset_x / self->width_px + (float)final_width / self->width_px,
        1.0f - (((float)self->height_px - (float)self->current_offset_y) / self->height_px -
                (float)final_height / self->height_px)
    };

    GlyphAtlasEntry retval = {
        .page_id    = self->page_id,
        .texture_id = self->texture_id,
        .left       = final_left,
        .top        = final_top,
        .height     = final_height,
        .width      = final_width,
        .tex_coords = { tc[0], tc[1], tc[2], tc[3] },
    };

    self->current_offset_x += final_width;

    return retval;
}

/* Generate a private atlas page with consistant looking block elements from unicode block
 * "Block Elements" and mirrored equivalents from "Symbols for Legacy Computing". */
__attribute__((cold)) static void GfxOpenGL2_maybe_generate_boxdraw_atlas_page(GfxOpenGL2* gfx)
{
    if (settings.font_box_drawing_chars) {
        return;
    }

    uint32_t page_width  = gfx->glyph_width_pixels * 2;
    uint32_t page_height = gfx->line_height_pixels * 3 + 1;

    GlyphAtlasPage self = (GlyphAtlasPage){
        .height_px       = page_height,
        .width_px        = page_width,
        .texture_id      = 0,
        .texture_format  = TEX_FMT_MONO,
        .internal_format = GL_RED,
        .page_id         = gfx->glyph_atlas.pages.size,
    };

    self.sx = 2.0 / self.width_px;
    self.sy = 2.0 / self.height_px;

    float scale_tex_u = 1.0 / self.width_px;
    float scale_tex_v = 1.0 / self.height_px;

    size_t bs = self.height_px * self.width_px * sizeof(uint8_t) * 4;

    uint8_t* fragments = _calloc(1, bs);

#define L_OFFSET(_x, _y) (self.width_px * (_y) + (_x))

    /* |LMD     |
       |####    |
       |####    |
       |####    |
       |    ####|
       |    ####|
       |    ####|
       |    ####|
       |##  #   |
       |########|
       |########|
       |##  #   | */

    fragments[L_OFFSET(0, 0)] = 50;  // LIGHT SHADE
    fragments[L_OFFSET(1, 0)] = 100; // MEDIUM SHADE
    fragments[L_OFFSET(2, 0)] = 200; // DARK SHADE

    for (int32_t x = 0; x < gfx->glyph_width_pixels; ++x) {
        for (int32_t y = 1; y < (gfx->line_height_pixels + 1); ++y) {
            fragments[L_OFFSET(x, y)] = UINT8_MAX;
        }
    }

    for (uint32_t x = gfx->glyph_width_pixels; x < (gfx->glyph_width_pixels * 2); ++x) {
        for (int32_t y = (gfx->line_height_pixels + 1); y < (gfx->line_height_pixels * 2 + 1);
             ++y) {
            fragments[L_OFFSET(x, y)] = UINT8_MAX;
        }
    }

    //  and  from private-use area (filled triangles)
    const float    sx      = 1.0 / gfx->glyph_width_pixels;
    const uint32_t yoffset = 1 + gfx->line_height_pixels * 2;
    const uint32_t xoffset = gfx->glyph_width_pixels;
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        for (int32_t dy = 0; dy <= gfx->line_height_pixels; ++dy) {
            double x     = ((double)dx + 0.5) / (double)gfx->glyph_width_pixels;
            double y     = ((double)dy + 0.5) / ((double)gfx->line_height_pixels / 2.0) - 1.0;
            double sd    = CLAMP((x - fabs(y)), -sx, sx);
            double value = (sd / (2.0 * sx)) + 0.5;
            fragments[L_OFFSET(xoffset + dx, yoffset + dy)] = value * UINT8_MAX;
        }
    }

    //  and  from private-use area (filled semielipses)
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
            int32_t  y_out = 1 + gfx->line_height_pixels * 2 + dy;
            uint32_t x_out = dx;
            double   x     = (double)dx / gfx->glyph_width_pixels;
            double   y     = (double)((double)dy + 0.5 - (double)gfx->line_height_pixels / 2.0) /
                       gfx->line_height_pixels * 2.0;
            float   x2 = x * x;
            float   y2 = y * y;
            float   w2 = gfx->glyph_width_pixels * gfx->glyph_width_pixels;
            float   h2 = (gfx->line_height_pixels) * (gfx->line_height_pixels);
            float   f  = sqrt(x * x + y * y);
            float   sd = (f - 1.0) * f / (2.0 * sqrt(x2 / w2 + y2 / h2));
            uint8_t value;
            if (sd > 0.5) {
                value = 0;
            } else if (sd > -0.5) {
                value = (0.5 - sd) * UINT8_MAX;
            } else if (sd > +0.5) {
                value = UINT8_MAX;
            } else if (sd > -0.5) {
                value = (sd + 0.5) * UINT8_MAX;
            } else {
                value = UINT8_MAX;
            }

            fragments[L_OFFSET(x_out, y_out)] = value;
        }
    }

#undef L_OFFSET

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &self.texture_id);
    glBindTexture(GL_TEXTURE_2D, self.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGB,
                 self.width_px,
                 self.height_px,
                 0,
                 GL_RED,
                 GL_UNSIGNED_BYTE,
                 fragments);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(fragments);

    Vector_push_GlyphAtlasPage(&gfx->glyph_atlas.pages, self);
    GlyphAtlasPage* page = Vector_last_GlyphAtlasPage(&gfx->glyph_atlas.pages);
    float           t    = gfx->pen_begin_pixels_y;

#define L_TC_U(_u) (((float)(_u)) * scale_tex_u)
#define L_TC_V(_v) (((float)(_v)) * scale_tex_v)
#define L_ENT_PROPS                                                                                \
    .page_id = page->page_id, .texture_id = self.texture_id, .height = gfx->line_height_pixels,    \
    .width = gfx->glyph_width_pixels, .top = t, .left = 0

    { /* LIGHT SHADE */
        Rune rune = (Rune){
            .code    = 0x2591,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.5f),
                                                     L_TC_V(0.5f),
                                                     L_TC_U(0.5f),
                                                     L_TC_V(0.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* MEDIUM SHADE */
        Rune rune = (Rune){
            .code    = 0x2592,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(1.5f),
                                                     L_TC_V(0.5f),
                                                     L_TC_U(1.5f),
                                                     L_TC_V(0.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* DARK SHADE */
        Rune rune = (Rune){
            .code    = 0x2593,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(2.5f),
                                                     L_TC_V(0.5f),
                                                     L_TC_U(2.5f),
                                                     L_TC_V(0.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* FULL BLOCK */
        Rune rune = (Rune){
            .code    = 0x2588,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.5f),
                                                     L_TC_V(1.5f),
                                                     L_TC_U(0.5f),
                                                     L_TC_V(1.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x2580,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + (gfx->line_height_pixels / 2) * 3),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + (gfx->line_height_pixels / 2)),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x2584,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + (gfx->line_height_pixels / 2)),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + (gfx->line_height_pixels * 3) / 2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x2582,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4)),
                                                     L_TC_U(gfx->glyph_width_pixels * 2),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4) +
                                                            gfx->line_height_pixels),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB82,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4) +
                                                            gfx->line_height_pixels),
                                                     L_TC_U(gfx->glyph_width_pixels * 2),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4)),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2586,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 4.0f * 3.0f))),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + (int)((float)gfx->line_height_pixels / 4.0f * 3.0f) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB85,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + (int)((float)gfx->line_height_pixels / 4.0f * 3.0f) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 4.0f * 3.0f))),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER ONE EIGTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x2581,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8)),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER ONE EIGTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x2594,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8)),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER THREE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2583,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f))),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f)) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER THREE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1fb83,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f)) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f))),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER FIVE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2585,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f))),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f)) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER FIVE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB84,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f)) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f))),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB86,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8)),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2587,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8)),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2589,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB8B,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x258A,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB8A,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT FIVE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x258B,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT FIVE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB89,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x258C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels / 2.0),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels / 2.0),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x2590,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels / 2.0f),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels / 2.0f),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT THREE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x258D,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT THREE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB88,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x258E,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB87,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT ONE EIGHTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x258E,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)(gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT ONE EIGHTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x2595,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* left semielipse */
        Rune rune = (Rune){
            .code    = 0xE0B6,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2),
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* right semielipse */
        Rune rune = (Rune){
            .code    = 0xE0B4,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* left filled triangle */
        Rune rune = (Rune){
            .code    = 0xE0B0,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* right filled triangle */
        Rune rune = (Rune){
            .code    = 0xE0B2,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

#undef L_TC_U
#undef L_TC_V
#undef L_ENT_PROPS
}

static GlyphAtlas GlyphAtlas_new(uint32_t page_size_px, uint32_t color_page_size_px)
{
    return (GlyphAtlas){
        .pages                  = Vector_new_with_capacity_GlyphAtlasPage(4),
        .entry_map              = Map_new_Rune_GlyphAtlasEntry(1024),
        .current_rgba_page      = NULL,
        .current_rgb_page       = NULL,
        .current_grayscale_page = NULL,
        .page_size_px           = page_size_px,
        .color_page_size_px     = color_page_size_px,
    };
}

static void GlyphAtlas_destroy(GlyphAtlas* self)
{
    Vector_destroy_GlyphAtlasPage(&self->pages);
    Map_destroy_Rune_GlyphAtlasEntry(&self->entry_map);
}

__attribute__((cold)) GlyphAtlasEntry* GlyphAtlas_get_combined(GfxOpenGL2* gfx,
                                                               GlyphAtlas* self,
                                                               const Rune* rune)
{
    enum FreetypeFontStyle style = FT_STYLE_REGULAR;
    switch (rune->style) {
        case VT_RUNE_BOLD:
            style = FT_STYLE_BOLD;
            break;
        case VT_RUNE_ITALIC:
            style = FT_STYLE_ITALIC;
            break;
        case VT_RUNE_BOLD_ITALIC:
            style = FT_STYLE_BOLD_ITALIC;
            break;
        default:;
    }
    FreetypeOutput *output, *base_output;
    output = base_output = Freetype_load_and_render_glyph(gfx->freetype, rune->code, style);

    if (!output)
        return NULL;

    GLenum internal_format;
    GLenum load_format;
    bool   scale = false;
    switch (output->type) {
        case FT_OUTPUT_RGB_H:
            internal_format = GL_RGB;
            load_format     = GL_RGB;
            break;
        case FT_OUTPUT_BGR_H:
            internal_format = GL_RGB;
            load_format     = GL_BGR;
            break;
        case FT_OUTPUT_RGB_V:
            internal_format = GL_RGB;
            load_format     = GL_RGB;
            break;
        case FT_OUTPUT_BGR_V:
            internal_format = GL_RGB;
            load_format     = GL_BGR;
            break;
        case FT_OUTPUT_GRAYSCALE:
            internal_format = GL_RED;
            load_format     = GL_RED;
            break;
        case FT_OUTPUT_COLOR_BGRA:
            scale           = true;
            internal_format = GL_RGBA;
            load_format     = GL_BGRA;
            break;
        default:
            ASSERT_UNREACHABLE
    }

    stored_common_gl_state_t old_state = store_common_state();

    Texture tex = {
        .id = 0,
        .w  = MAX(gfx->glyph_width_pixels, output->width),
        .h  = MAX(gfx->line_height_pixels, output->height),
    };

    float scalex = 2.0 / tex.w;
    float scaley = 2.0 / tex.h;

    glDisable(GL_SCISSOR_TEST);
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D,
                    GL_TEXTURE_MIN_FILTER,
                    scale ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, scale ? GL_LINEAR : GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 internal_format,
                 tex.w,
                 tex.h,
                 0,
                 load_format,
                 GL_UNSIGNED_BYTE,
                 NULL);

#ifndef GFX_GLES
    GLuint tmp_rb;
    glGenRenderbuffers(1, &tmp_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, tmp_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, tex.w, tex.h);
#endif

    GLuint tmp_fb;
    glGenFramebuffers(1, &tmp_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, tmp_fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex.id, 0);

#ifndef GFX_GLES
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tmp_rb);
#endif

    glViewport(0, 0, tex.w, tex.h);

#ifndef GFX_GLES
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDepthRange(0.0f, 1.0f);
#endif

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT

#ifndef GFX_GLES
            | GL_DEPTH_BUFFER_BIT
#endif
    );

    GLuint tmp_vbo;
    glGenBuffers(1, &tmp_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, tmp_vbo);
    glUseProgram(gfx->font_shader_blend.id);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float[QUAD_V_SZ][4]), NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(gfx->font_shader_blend.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);

#ifdef GFX_GLES
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#endif

    /* Combine additional glyphs into the base texture */
    for (uint32_t i = 0; i < VT_RUNE_MAX_COMBINE + 1; ++i) {
        char32_t c = i ? rune->combine[i - 1] : rune->code;
        if (!c) {
            break;
        }

        if (i) {
            output = Freetype_load_and_render_glyph(gfx->freetype, c, style);
        }

        if (!output) {
            WRN("Missing combining glyph u+%X\n", c);
            continue;
        }

        GLuint tmp_tex;
        glGenTextures(1, &tmp_tex);
        glBindTexture(GL_TEXTURE_2D, tmp_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     internal_format,
                     output->width,
                     output->height,
                     0,
                     load_format,
                     GL_UNSIGNED_BYTE,
                     output->pixels);
        gl_check_error();

        float l = scalex * output->left;
        float t = scaley * output->top;
        float w = scalex * output->width;
        float h = scaley * output->height;

        float x = -1.0 + (i ? ((tex.w - output->width) / 2 * scalex) : l);
        float y = 1.0 - t + h;
        y       = CLAMP(y, -1.0 + h, 1.0);

#ifndef GFX_GLES
        float vbo_data[][4] = {
            { x, y, 0.0f, 1.0f },
            { x + w, y, 1.0f, 1.0f },
            { x + w, y - h, 1.0f, 0.0f },
            { x, y - h, 0.0f, 0.0f },
        };
#else
        float vbo_data[][4] = {
            { x, y, 0.0f, 1.0f }, { x + w, y, 1.0f, 1.0f }, { x + w, y - h, 1.0f, 0.0f },
            { x, y, 0.0f, 1.0f }, { x, y - h, 0.0f, 0.0f }, { x + w, y - h, 1.0f, 0.0f },
        };
#endif

        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * QUAD_V_SZ * 4, vbo_data);
        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);

        glDeleteTextures(1, &tmp_tex);
        gl_check_error();
    }

    glDeleteFramebuffers(1, &tmp_fb);

#ifndef GFX_GLES
    glDeleteRenderbuffers(1, &tmp_rb);
#endif

    glDeleteBuffers(1, &tmp_vbo);

    restore_gl_state(&old_state);

    GlyphAtlasPage* tgt_page;

    if (!output) {
        return NULL;
    }

    switch (output->type) {
        case FT_OUTPUT_RGB_H:
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_RGB_V:
        case FT_OUTPUT_BGR_V:
            tgt_page = self->current_rgb_page;
            if (unlikely(!tgt_page || !GlyphAtlasPage_can_push_tex(tgt_page, tex))) {
                Vector_push_GlyphAtlasPage(&self->pages,
                                           GlyphAtlasPage_new(gfx,
                                                              self->pages.size,
                                                              false,
                                                              GL_RGB,
                                                              TEX_FMT_RGB,
                                                              self->page_size_px,
                                                              self->page_size_px));
                tgt_page = self->current_rgb_page = Vector_last_GlyphAtlasPage(&self->pages);
            }
            break;

        case FT_OUTPUT_GRAYSCALE:
            tgt_page = self->current_grayscale_page;
            if (unlikely(!tgt_page || !GlyphAtlasPage_can_push_tex(tgt_page, tex))) {
                Vector_push_GlyphAtlasPage(&self->pages,
                                           GlyphAtlasPage_new(gfx,
                                                              self->pages.size,
                                                              false,
                                                              GL_RED,
                                                              TEX_FMT_MONO,
                                                              self->page_size_px,
                                                              self->page_size_px));
                tgt_page = self->current_grayscale_page = Vector_last_GlyphAtlasPage(&self->pages);
            }
            break;

        case FT_OUTPUT_COLOR_BGRA:
            tgt_page = self->current_rgba_page;
            if (unlikely(!tgt_page || !GlyphAtlasPage_can_push_tex(tgt_page, tex))) {
                Vector_push_GlyphAtlasPage(&self->pages,
                                           GlyphAtlasPage_new(gfx,
                                                              self->pages.size,
                                                              true,
                                                              GL_RGBA,
                                                              TEX_FMT_RGBA,
                                                              self->color_page_size_px,
                                                              self->color_page_size_px));
                tgt_page = self->current_rgba_page = Vector_last_GlyphAtlasPage(&self->pages);
            }
            break;
        default:
            ASSERT_UNREACHABLE;
    }

    Rune key = *rune;
    if (output->style == FT_STYLE_NONE)
        key.style = VT_RUNE_UNSTYLED;

    return Map_insert_Rune_GlyphAtlasEntry(
      &self->entry_map,
      key,
      GlyphAtlasPage_push_tex(gfx, tgt_page, base_output, tex, NULL));
}

__attribute__((hot, always_inline)) static inline GlyphAtlasEntry*
GlyphAtlas_get_regular(GfxOpenGL2* gfx, GlyphAtlas* self, const Rune* rune)
{
    enum FreetypeFontStyle style = FT_STYLE_REGULAR;
    switch (rune->style) {
        case VT_RUNE_BOLD:
            style = FT_STYLE_BOLD;
            break;
        case VT_RUNE_ITALIC:
            style = FT_STYLE_ITALIC;
            break;
        case VT_RUNE_BOLD_ITALIC:
            style = FT_STYLE_BOLD_ITALIC;
            break;
        default:;
    }

    FreetypeOutput* output = Freetype_load_and_render_glyph(gfx->freetype, rune->code, style);
    if (!output) {
        WRN("Missing glyph u+%X\n", rune->code);
        return NULL;
    }
    GlyphAtlasPage* tgt_page;
    switch (output->type) {
        case FT_OUTPUT_RGB_H:
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_RGB_V:
        case FT_OUTPUT_BGR_V:
            tgt_page = self->current_rgb_page;
            if (unlikely(!tgt_page || !GlyphAtlasPage_can_push(gfx, tgt_page, output))) {
                Vector_push_GlyphAtlasPage(&self->pages,
                                           GlyphAtlasPage_new(gfx,
                                                              self->pages.size,
                                                              false,
                                                              GL_RGB,
                                                              TEX_FMT_RGB,
                                                              self->page_size_px,
                                                              self->page_size_px));
                tgt_page = self->current_rgb_page = Vector_last_GlyphAtlasPage(&self->pages);
            }
            break;
        case FT_OUTPUT_GRAYSCALE:
            tgt_page = self->current_grayscale_page;
            if (unlikely(!tgt_page || !GlyphAtlasPage_can_push(gfx, tgt_page, output))) {
                Vector_push_GlyphAtlasPage(&self->pages,
                                           GlyphAtlasPage_new(gfx,
                                                              self->pages.size,
                                                              false,
                                                              GL_RED,
                                                              TEX_FMT_MONO,
                                                              self->page_size_px,
                                                              self->page_size_px));
                tgt_page = self->current_grayscale_page = Vector_last_GlyphAtlasPage(&self->pages);
            }
            break;
        case FT_OUTPUT_COLOR_BGRA:
            tgt_page = self->current_rgba_page;
            if (unlikely(!tgt_page || !GlyphAtlasPage_can_push(gfx, tgt_page, output))) {
                Vector_push_GlyphAtlasPage(&self->pages,
                                           GlyphAtlasPage_new(gfx,
                                                              self->pages.size,
                                                              true,
                                                              GL_RGBA,
                                                              TEX_FMT_RGBA,
                                                              self->page_size_px,
                                                              self->page_size_px));
                tgt_page = self->current_rgba_page = Vector_last_GlyphAtlasPage(&self->pages);
            }
            break;
        default:
            ASSERT_UNREACHABLE;
    }
    Rune key = *rune;
    if (output->style == FT_STYLE_NONE)
        key.style = VT_RUNE_UNSTYLED;
    return Map_insert_Rune_GlyphAtlasEntry(&self->entry_map,
                                           key,
                                           GlyphAtlasPage_push(gfx, tgt_page, output));
}

__attribute__((hot)) static GlyphAtlasEntry* GlyphAtlas_get(GfxOpenGL2* gfx,
                                                            GlyphAtlas* self,
                                                            const Rune* rune)
{
    GlyphAtlasEntry* entry = Map_get_Rune_GlyphAtlasEntry(&self->entry_map, rune);

    if (likely(entry)) {
        return entry;
    }

    Rune alt = *rune;

    if (!settings.has_bold_fonts && rune->style == VT_RUNE_BOLD) {
        alt.style = VT_RUNE_NORMAL;
        entry     = Map_get_Rune_GlyphAtlasEntry(&self->entry_map, &alt);
        if (likely(entry)) {
            return entry;
        }
    }
    if (!settings.has_italic_fonts && rune->style == VT_RUNE_ITALIC) {
        alt.style = VT_RUNE_NORMAL;
        entry     = Map_get_Rune_GlyphAtlasEntry(&self->entry_map, &alt);
        if (likely(entry)) {
            return entry;
        }
    }
    if (!settings.has_bold_italic_fonts && rune->style == VT_RUNE_BOLD_ITALIC) {
        alt.style = settings.has_bold_fonts     ? VT_RUNE_BOLD
                    : settings.has_italic_fonts ? VT_RUNE_ITALIC
                                                : VT_RUNE_NORMAL;
        entry     = Map_get_Rune_GlyphAtlasEntry(&self->entry_map, &alt);
        if (likely(entry)) {
            return entry;
        }
    }

    alt.style = VT_RUNE_UNSTYLED;
    entry     = Map_get_Rune_GlyphAtlasEntry(&self->entry_map, &alt);
    if (likely(entry)) {
        return entry;
    }

    if (unlikely(rune->combine[0])) {
        return GlyphAtlas_get_combined(gfx, self, rune);
    } else {
        return GlyphAtlas_get_regular(gfx, self, rune);
    }
}

// Generate a sinewave image and store it as an OpenGL texture
__attribute__((cold)) static Texture create_squiggle_texture(uint32_t w,
                                                             uint32_t h,
                                                             uint32_t thickness)
{
    const double MSAA = 4;
    w *= MSAA;
    h *= MSAA;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    uint8_t* fragments                 = _calloc(w * h * 4, 1);
    double   pixel_size                = 2.0 / h;
    double   stroke_width              = thickness * pixel_size * (MSAA / 1.3);
    double   stroke_fade               = pixel_size * MSAA * 2;
    double   distance_limit_full_alpha = POW2(stroke_width / 1.0);
    double   distance_limit_zero_alpha = POW2(stroke_width / 1.0 + stroke_fade);

    for (uint_fast32_t x = 0; x < w; ++x)
        for (uint_fast32_t y = 0; y < h; ++y) {
#define DISTANCE_SQR(_x, _y, _x2, _y2) (pow((_x2) - (_x), 2) + pow((_y2) - (_y), 2))
            uint8_t* fragment = &fragments[(y * w + x) * 4];
            double   x_frag   = (double)x / (double)w * 2.0 * M_PI;
            double y_frag = (double)y / (double)h * (2.0 + stroke_width * 2.0 + stroke_fade * 2.0) -
                            1.0 - stroke_width - stroke_fade;
            double y_curve          = sin(x_frag);
            double dx_frag          = cos(x_frag);
            double y_dist           = y_frag - y_curve;
            double closest_distance = DISTANCE_SQR(x_frag, y_frag, x_frag, y_curve);
            double step             = dx_frag * y_dist < 0.0 ? 0.001 : -0.001;

            for (double i = x_frag + step;; i += step / 2.0) {
                double i_distance = DISTANCE_SQR(x_frag, y_frag, i, sin(i));
                if (likely(i_distance <= closest_distance)) {
                    closest_distance = i_distance;
                } else {
                    break;
                }
            }

            fragments[3] = 0;

            if (closest_distance <= distance_limit_full_alpha) {
                fragment[0] = fragment[1] = fragment[2] = fragment[3] = UINT8_MAX;
            } else if (closest_distance < distance_limit_zero_alpha) {
                double alpha = (1.0 - (closest_distance - distance_limit_full_alpha) /
                                        (distance_limit_zero_alpha - distance_limit_full_alpha));

                fragment[0] = fragment[1] = fragment[2] = UINT8_MAX;
                fragment[3]                             = CLAMP(alpha * UINT8_MAX, 0, UINT8_MAX);
            }
        }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fragments);

    free(fragments);

    return (Texture){ .id = tex, .format = TEX_FMT_RGBA, .w = w / MSAA, .h = h / MSAA };
}

static void GfxOpenGL2_update_metrics(Gfx* self)
{
    GfxOpenGL2* gl2 = gfxOpenGL2(self);

    FreetypeOutput* output =
      Freetype_load_ascii_glyph(gl2->freetype, settings.center_char, FT_STYLE_REGULAR);

    if (!output) {
        ERR("Failed to load character metrics, is font set up correctly?");
    }

    uint32_t hber = output->ft_slot->metrics.horiBearingY / 64 / 2 / 2 + 1;

    gl2->pen_begin_y = gl2->sy * (gl2->line_height_pixels / 2.0) + gl2->sy * hber;
    gl2->pen_begin_pixels_y =
      (float)(gl2->line_height_pixels / 1.75) + (float)hber + settings.offset_glyph_y;
    gl2->pen_begin_pixels_x = settings.offset_glyph_x;

    uint32_t height        = (gl2->line_height_pixels + settings.padd_glyph_y) * 64;
    gl2->line_height       = (float)height * gl2->sy / 64.0;
    gl2->glyph_width       = gl2->glyph_width_pixels * gl2->sx;
    gl2->max_cells_in_line = gl2->win_w / gl2->glyph_width_pixels;
}

void GfxOpenGL2_resize(Gfx* self, uint32_t w, uint32_t h, Pair_uint32_t cells)
{
    GfxOpenGL2* gl2 = gfxOpenGL2(self);
    gl2->cells      = cells;
    GfxOpenGL2_destroy_recycled(gl2);

    gl2->win_w = w;
    gl2->win_h = h;

    gl2->sx = 2.0f / gl2->win_w;
    gl2->sy = 2.0f / gl2->win_h;

    gl2->line_height_pixels = gl2->freetype->line_height_pixels + settings.padd_glyph_y;
    gl2->glyph_width_pixels = gl2->freetype->glyph_width_pixels + settings.padd_glyph_x;
    gl2->gw                 = gl2->freetype->gw;

    GfxOpenGL2_update_metrics(self);

    glViewport(0, 0, gl2->win_w, gl2->win_h);

    GfxOpenGL2_realloc_damage_record(gl2, cells.second);
    GfxOpenGL2_regenerate_line_quad_vbo(gl2, cells.second);
    gl2->line_damage.n_lines = cells.second;
}

Pair_uint32_t GfxOpenGL2_get_char_size(Gfx* self, Pair_uint32_t pixels)
{
    ASSERT(gfxOpenGL2(self)->freetype, "font renderer active");

    uint32_t minsize = settings.padding * 2;
    int32_t  cellx   = gfxOpenGL2(self)->freetype->glyph_width_pixels + settings.padd_glyph_x;
    int32_t  celly   = gfxOpenGL2(self)->freetype->line_height_pixels + settings.padd_glyph_y;

    if (pixels.first < minsize + cellx || pixels.second < minsize + celly) {
        return (Pair_uint32_t){ .first = 0, .second = 0 };
    }

    int32_t cols = MAX((pixels.first - 2 * settings.padding) / cellx, 0);
    int32_t rows = MAX((pixels.second - 2 * settings.padding) / celly, 0);

    if (pixels.first < minsize + cellx * 2) {
        cols = 1;
    }

    if (pixels.second < minsize + celly * 2) {
        rows = 1;
    }

    return (Pair_uint32_t){ .first = cols, .second = rows };
}

Pair_uint32_t GfxOpenGL2_pixels(Gfx* self, uint32_t c, uint32_t r)
{
    float x, y;
    x = c * (gfxOpenGL2(self)->freetype->glyph_width_pixels + settings.padd_glyph_x);
    y = r * (gfxOpenGL2(self)->freetype->line_height_pixels + settings.padd_glyph_y);
    return (Pair_uint32_t){ .first = x + 2 * settings.padding, .second = y + 2 * settings.padding };
}

void GfxOpenGL2_load_font(Gfx* self) {}

void GfxOpenGL2_init_with_context_activated(Gfx* self)
{
    GfxOpenGL2* gl2 = gfxOpenGL2(self);

    ASSERT(self->callbacks.user_data, "callback user data defined");
    ASSERT(self->callbacks.load_extension_proc_address, "callback func defined");

    maybe_load_gl_exts(self->callbacks.user_data, self->callbacks.load_extension_proc_address);

#ifdef DEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(on_gl_error, NULL);
#endif

    if (unlikely(settings.debug_gfx)) {
        fprintf(stderr, "GL_VENDOR = %s\n", glGetString(GL_VENDOR));
        fprintf(stderr, "GL_RENDERER = %s\n", glGetString(GL_RENDERER));
        fprintf(stderr, "GL_VERSION = %s\n", glGetString(GL_VERSION));
        fprintf(stderr,
                "GL_SHADING_LANGUAGE_VERSION = %s\n",
                glGetString(GL_SHADING_LANGUAGE_VERSION));
    }

    gl2->float_vec = Vector_new_with_capacity_Vector_float(3);
    Vector_push_Vector_float(&gl2->float_vec, Vector_new_float());

#ifndef GFX_GLES
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
#endif

    glDisable(GL_FRAMEBUFFER_SRGB);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));

    gl2->solid_fill_shader = Shader_new(solid_fill_vs_src, solid_fill_fs_src, "pos", "clr", NULL);
    gl2->font_shader = Shader_new(font_vs_src, font_fs_src, "coord", "tex", "clr", "bclr", NULL);
    gl2->font_shader_gray = Shader_new(font_vs_src,
                                       font_gray_fs_src,
                                       "coord",
                                       "tex",
                                       "clr",
#ifndef GFX_GLES
                                       "bclr",
#endif
                                       NULL);
    gl2->font_shader_blend = Shader_new(font_vs_src, font_depth_blend_fs_src, "coord", "tex", NULL);
    gl2->line_shader       = Shader_new(line_vs_src, line_fs_src, "pos", "clr", NULL);
    gfxOpenGL2(self)->image_shader =
      Shader_new(image_rgb_vs_src, image_rgb_fs_src, "coord", "tex", "offset", NULL);
    gl2->image_tint_shader =
      Shader_new(image_rgb_vs_src, image_tint_rgb_fs_src, "coord", "tex", "tint", "offset", NULL);

    gl2->flex_vbo = VBO_new(4, 1, gl2->font_shader.attribs);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    GLuint new_vbos[2];
    glGenBuffers(2, new_vbos);
    gl2->full_framebuffer_quad_vbo = new_vbos[0];
    gl2->line_quads_vbo            = new_vbos[1];

    glBindBuffer(GL_ARRAY_BUFFER, gl2->full_framebuffer_quad_vbo);

#ifdef GFX_GLES
    const float vertex_data[] = {
        1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f,
    };
#else
    const float vertex_data[] = {
        1.0f, 1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f,
    };
#endif

    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
    GfxOpenGL2_regenerate_line_quad_vbo(gl2, gl2->cells.second);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl2->max_tex_res);

    gl2->color    = settings.fg;
    gl2->bg_color = settings.bg;

    gl2->glyph_atlas = GlyphAtlas_new(1024, 512);

    Shader_use(&gl2->font_shader);
    glUniform3f(gl2->font_shader.uniforms[1].location,
                ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1),
                ColorRGB_get_float(settings.fg, 2));

    glGenFramebuffers(1, &gl2->line_framebuffer);

    gl2->blink_switch       = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
    gl2->blink_switch_text  = TimePoint_now();
    gl2->vec_vertex_buffer  = Vector_new_vertex_t();
    gl2->vec_vertex_buffer2 = Vector_new_vertex_t();

    Freetype* ft = gl2->freetype;

    gl2->line_height_pixels = ft->line_height_pixels + settings.padd_glyph_y;
    gl2->glyph_width_pixels = ft->glyph_width_pixels + settings.padd_glyph_x;
    uint32_t t_height       = CLAMP(gl2->line_height_pixels / 8.0 + 2, 4, UINT8_MAX);
    gl2->squiggle_texture =
      create_squiggle_texture(t_height * M_PI / 2.0, t_height, CLAMP((t_height / 4), 1, 20));

    GfxOpenGL2_update_metrics(self);
    GfxOpenGL2_maybe_generate_boxdraw_atlas_page(gl2);
}

void GfxOpenGL2_reload_font(Gfx* self)
{
    GfxOpenGL2* gl2 = gfxOpenGL2(self);

    GfxOpenGL2_load_font(self);
    GfxOpenGL2_resize(self, gl2->win_w, gl2->win_h, gl2->cells);

    GlyphAtlas_destroy(&gl2->glyph_atlas);
    gl2->glyph_atlas = GlyphAtlas_new(1024, 512);

    // regenerate the squiggle texture
    glDeleteTextures(1, &gl2->squiggle_texture.id);
    uint32_t t_height = CLAMP(gl2->line_height_pixels / 8.0 + 2, 4, UINT8_MAX);
    gl2->squiggle_texture =
      create_squiggle_texture(t_height * M_PI / 2.0, t_height, CLAMP(t_height / 4, 1, 20));

    GfxOpenGL2_maybe_generate_boxdraw_atlas_page(gl2);
}

static void GfxOpenGL2_regenerate_line_quad_vbo(GfxOpenGL2* gfx, uint32_t n_lines)
{
    Vector_float transfer = Vector_new_with_capacity_float(n_lines * 4 * QUAD_V_SZ);

    for (uint32_t i = 0; i < n_lines; ++i) {
        float tex_end_x   = -1.0f + gfx->max_cells_in_line * gfx->glyph_width_pixels * gfx->sx;
        float tex_begin_y = 1.0f - gfx->line_height_pixels * (i + 1) * gfx->sy;

#ifdef GFX_GLES
        float buf[] = {
            -1.0f,     tex_begin_y + gfx->line_height,
            0.0f,      0.0f,
            tex_end_x, tex_begin_y,
            1.0f,      1.0f,
            -1.0,      tex_begin_y,
            0.0f,      1.0f,
            tex_end_x, tex_begin_y + gfx->line_height,
            1.0f,      0.0f,
            tex_end_x, tex_begin_y,
            1.0f,      1.0f,
            -1.0f,     tex_begin_y + gfx->line_height,
            0.0f,      0.0f,
        };
#else
        float buf[] = {
            -1.0f,     tex_begin_y + gfx->line_height,
            0.0f,      0.0f,
            -1.0,      tex_begin_y,
            0.0f,      1.0f,
            tex_end_x, tex_begin_y,
            1.0f,      1.0f,
            tex_end_x, tex_begin_y + gfx->line_height,
            1.0f,      0.0f,
        };
#endif

        Vector_pushv_float(&transfer, buf, ARRAY_SIZE(buf));
    }

    glBindBuffer(GL_ARRAY_BUFFER, gfx->line_quads_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * transfer.size, transfer.buf, GL_STATIC_DRAW);
    Vector_destroy_float(&transfer);
}

static void GfxOpenGL2_draw_line_quads(GfxOpenGL2*   gfx,
                                       Ui*           ui,
                                       VtLine* const vt_line,
                                       uint32_t      quad_index)
{
    if (likely(vt_line->proxy.data[PROXY_INDEX_TEXTURE] ||
               (vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]))) {
        uint8_t tidx = PROXY_INDEX_TEXTURE;
        if (unlikely(vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] && !ui->draw_text_blinking)) {
            tidx = PROXY_INDEX_TEXTURE_BLINK;
        }

        GLuint tex_id = vt_line->proxy.data[tidx];
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glDrawArrays(QUAD_DRAW_MODE, quad_index * QUAD_V_SZ, QUAD_V_SZ);
    }
}

typedef struct
{
    GfxOpenGL2*       gl2;
    const Vt* const   vt;
    VtLine*           vt_line;
    VtLineProxy*      proxy;
    vt_line_damage_t* damage;
    size_t            visual_index;
    uint16_t          cnd_cursor_column;
    bool              is_for_cursor;
    bool              is_for_blinking;
} line_render_pass_args_t;

typedef struct
{
    uint16_t render_range_begin;
    uint16_t render_range_end;
} line_render_subpass_args_t;

typedef struct
{
    line_render_subpass_args_t* prepared_subpass;
    line_render_pass_args_t     args;
    line_render_subpass_args_t  subpass_args[2];
    GLuint                      final_texture;
    GLuint                      final_depthbuffer;
    uint32_t                    texture_width;
    uint32_t                    texture_height;
    uint16_t                    length;
    uint8_t                     n_queued_subpasses;
    bool                        has_blinking_chars;
    bool                        has_underlined_chars;
    bool                        is_reusing;
} line_render_pass_t;

static void line_reder_pass_run(line_render_pass_t* self);

static bool should_create_line_render_pass(const line_render_pass_args_t* args)
{
    vt_line_damage_t* dmg = OR(args->damage, &args->vt_line->damage);
    return !likely(!args->vt_line->data.size || (dmg->type == VT_LINE_DAMAGE_NONE));
}

static line_render_pass_t create_line_render_pass(const line_render_pass_args_t* args)
{
    line_render_pass_t rp = { .args                 = *args,
                              .final_texture        = 0,
                              .final_depthbuffer    = 0,
                              .prepared_subpass     = NULL,
                              .length               = args->vt_line->data.size,
                              .has_underlined_chars = false,
                              .has_blinking_chars   = false,
                              .is_reusing           = false,
                              .texture_width =
                                args->gl2->max_cells_in_line * args->gl2->glyph_width_pixels,
                              .texture_height     = args->gl2->line_height_pixels,
                              .n_queued_subpasses = 0,
                              .subpass_args       = {
                                {
                                        .render_range_begin = 0,
                                        .render_range_end   = 0,
                                },
                                {
                                        .render_range_begin = 0,
                                        .render_range_end   = 0,
                                },
                              } };

    if (!rp.args.proxy) {
        rp.args.proxy = &rp.args.vt_line->proxy;
    }

    if (!rp.args.damage) {
        rp.args.damage = &rp.args.vt_line->damage;
    }

    return rp;
}

static void line_render_pass_try_to_recover_proxies(line_render_pass_t* self)
{
    size_t proxy_tex_idx = PROXY_INDEX_TEXTURE;

#ifndef GFX_GLES
    size_t proxy_depth_idx = PROXY_INDEX_DEPTHBUFFER;
#endif

    if (unlikely(self->args.is_for_blinking)) {
        proxy_tex_idx = PROXY_INDEX_TEXTURE_BLINK;

#ifndef GFX_GLES
        proxy_depth_idx = PROXY_INDEX_DEPTHBUFFER_BLINK;
#endif
    }

    GLuint recovered_texture = self->args.proxy->data[proxy_tex_idx];

#ifndef GFX_GLES
    GLuint recovered_depthbuffer = self->args.proxy->data[proxy_depth_idx];
#endif

    self->is_reusing    = recovered_texture;
    self->final_texture = recovered_texture;

#ifndef GFX_GLES
    self->final_depthbuffer = recovered_depthbuffer;
#endif
}

static void line_render_pass_set_up_subpasses(line_render_pass_t* self)
{
#define CURSOR_OVERPAINT_FWD  3
#define CURSOR_OVERPAINT_BACK 4

    if (self->args.is_for_cursor) {
        uint16_t range_begin_idx = self->args.cnd_cursor_column;
        uint16_t range_end_idx   = range_begin_idx;

        for (uint_fast8_t i = 0; i < CURSOR_OVERPAINT_BACK && range_begin_idx; ++i) {
            --range_begin_idx;
        }

        for (uint_fast8_t i = 0; i < CURSOR_OVERPAINT_FWD && range_end_idx < self->length; ++i) {
            ++range_end_idx;
        }

        self->subpass_args[0] = (line_render_subpass_args_t){
            .render_range_begin = range_begin_idx,
            .render_range_end   = range_end_idx,
        };

        self->n_queued_subpasses = 1;

    } else /* not for cursor */ {
        const VtLine* const ln = self->args.vt_line;

        switch (self->args.damage->type) {
            case VT_LINE_DAMAGE_RANGE: {
                uint16_t range_begin_idx = self->args.damage->front;
                uint16_t range_end_idx   = self->args.damage->end + 1;

                while (range_begin_idx > 1) {
                    Rune* this_rune = &ln->data.buf[range_begin_idx - 1].rune;
                    Rune* prev_rune = &ln->data.buf[range_begin_idx - 2].rune;

                    if (Rune_is_blank(*this_rune) && Rune_width(*prev_rune) < 2) {
                        break;
                    }
                    --range_begin_idx;
                }

                if (range_begin_idx == 1 &&
                    !Rune_is_blank(ln->data.buf[range_begin_idx - 1].rune) &&
                    Rune_width(ln->data.buf[0].rune) > 1) {
                    range_begin_idx = 0;
                }

                while (range_end_idx < self->args.vt_line->data.size && range_end_idx) {
                    Rune this_rune = ln->data.buf[range_end_idx].rune;
                    Rune prev_rune = ln->data.buf[range_end_idx - 1].rune;
                    ++range_end_idx;

                    if (Rune_is_blank(this_rune) && Rune_width_spill(prev_rune) < 2) {
                        break;
                    }
                }

                self->subpass_args[0] = (line_render_subpass_args_t){
                    .render_range_begin = range_begin_idx,
                    .render_range_end   = range_end_idx,
                };

                self->n_queued_subpasses = 1;
            } break;

            case VT_LINE_DAMAGE_SHIFT:
                // TODO:
            case VT_LINE_DAMAGE_FULL: {
                self->subpass_args[0] = (line_render_subpass_args_t){
                    .render_range_begin = 0,
                    .render_range_end   = self->length,
                };

                self->n_queued_subpasses = 1;
            } break;

            default:
                ASSERT_UNREACHABLE
        }
    }
}

static void line_render_pass_finalize(line_render_pass_t* self)
{
    // set proxy data to generated texture
    if (unlikely(self->args.is_for_blinking)) {
        self->args.proxy->data[PROXY_INDEX_TEXTURE_BLINK] = self->final_texture;

#ifndef GFX_GLES
        self->args.proxy->data[PROXY_INDEX_DEPTHBUFFER_BLINK] = self->final_depthbuffer;
#endif

        self->args.damage->type  = VT_LINE_DAMAGE_NONE;
        self->args.damage->shift = 0;
        self->args.damage->front = 0;
        self->args.damage->end   = 0;
    } else {
        self->args.proxy->data[PROXY_INDEX_TEXTURE] = self->final_texture;

#ifndef GFX_GLES
        self->args.proxy->data[PROXY_INDEX_DEPTHBUFFER] = self->final_depthbuffer;
#endif

        if (!self->has_blinking_chars) {
            self->args.damage->type  = VT_LINE_DAMAGE_NONE;
            self->args.damage->shift = 0;
            self->args.damage->front = 0;
            self->args.damage->end   = 0;
        }
    }

    if (unlikely(settings.debug_gfx)) {
        static float debug_tint = 0.0f;
        glDisable(GL_SCISSOR_TEST);
        glBindTexture(GL_TEXTURE_2D, 0);
        Shader_use(&self->args.gl2->solid_fill_shader);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform4f(self->args.gl2->solid_fill_shader.uniforms[0].location,
                    fabs(sin(debug_tint)),
                    fabs(cos(debug_tint)),
                    sin(debug_tint),
                    0.1);
        glBindBuffer(GL_ARRAY_BUFFER, self->args.gl2->full_framebuffer_quad_vbo);
        glVertexAttribPointer(self->args.gl2->solid_fill_shader.attribs->location,
                              2,
                              GL_FLOAT,
                              GL_FALSE,
                              0,
                              0);

        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);

        glDisable(GL_BLEND);
        debug_tint += 0.5f;
        if (debug_tint > M_PI) {
            debug_tint -= M_PI;
        }
    }

#ifndef GFX_GLES
    glDisable(GL_DEPTH_TEST);
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, self->args.gl2->line_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

#ifndef GFX_GLES
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_check_error();

    glViewport(0, 0, self->args.gl2->win_w, self->args.gl2->win_h);

    // There are no blinking characters, but their resources still exist
    if (!self->has_blinking_chars && self->args.proxy->data[PROXY_INDEX_TEXTURE_BLINK]) {
        // TODO: recycle?
        DBG_DELTEX
        glDeleteTextures(1, (GLuint*)&self->args.proxy->data[PROXY_INDEX_TEXTURE_BLINK]);
        self->args.proxy->data[PROXY_INDEX_TEXTURE_BLINK] = 0;

#ifndef GFX_GLES
        ASSERT(self->args.proxy->data[PROXY_INDEX_DEPTHBUFFER_BLINK],
               "deleted proxy texture has depth rb");
        glDeleteRenderbuffers(1, (GLuint*)&self->args.proxy->data[PROXY_INDEX_DEPTHBUFFER_BLINK]);
        self->args.proxy->data[PROXY_INDEX_DEPTHBUFFER_BLINK] = 0;
#endif
    }
}

static void line_render_pass_set_up_framebuffer(line_render_pass_t* self)
{
    if (self->is_reusing) {
        glBindFramebuffer(GL_FRAMEBUFFER, self->args.gl2->line_framebuffer);

        glBindTexture(GL_TEXTURE_2D, self->final_texture);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               self->final_texture,
                               0);

#ifndef GFX_GLES
        glBindRenderbuffer(GL_RENDERBUFFER, self->final_depthbuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER,
                                  self->final_depthbuffer);
#endif

        glViewport(0, 0, self->texture_width, self->texture_height);

        gl_check_error();
    } else {
        if (!self->args.is_for_blinking) {
            GfxOpenGL2_destroy_proxy((void*)((uint8_t*)self->args.gl2 - offsetof(Gfx, extend_data)),
                                     self->args.proxy->data);
        }
        if (!self->args.vt_line->data.size) {
            return;
        }

        GLuint recycle_tex_id = self->args.gl2->recycled_textures[0].color_tex;

        if (recycle_tex_id) {
            Pair_GLuint recycled    = GfxOpenGL2_pop_recycled(self->args.gl2);
            self->final_texture     = recycled.first;
            self->final_depthbuffer = recycled.second;
            glBindFramebuffer(GL_FRAMEBUFFER, self->args.gl2->line_framebuffer);
            glBindTexture(GL_TEXTURE_2D, self->final_texture);

            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D,
                                   self->final_texture,
                                   0);

#ifndef GFX_GLES
            ASSERT(recycled.second, "recovered texture has a depth rb");
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                      GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER,
                                      self->final_depthbuffer);
#endif

            gl_check_error();

        } else {
            /* Generate new framebuffer attachments */
            DBG_MAKETEX
            glGenTextures(1, &self->final_texture);
            glBindTexture(GL_TEXTURE_2D, self->final_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA,
                         self->texture_width,
                         self->texture_height,
                         0,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         0);

#ifndef GFX_GLES
            glGenRenderbuffers(1, &self->final_depthbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, self->final_depthbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER,
                                  GL_DEPTH_COMPONENT,
                                  self->texture_width,
                                  self->texture_height);
#endif

            glBindFramebuffer(GL_FRAMEBUFFER, self->args.gl2->line_framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D,
                                   self->final_texture,
                                   0);

#ifndef GFX_GLES
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                      GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER,
                                      self->final_depthbuffer);
#endif
            gl_check_error();
        }
    }

    assert_farmebuffer_complete();
}

typedef struct
{
    line_render_subpass_args_t args;
} line_render_subpass_t;

static line_render_subpass_t line_render_pass_create_subpass(line_render_pass_t*         self,
                                                             line_render_subpass_args_t* args)
{
    line_render_subpass_t sub = {
        .args =
          (line_render_subpass_args_t){
            .render_range_begin = MIN(args->render_range_begin, self->args.vt_line->data.size),
            .render_range_end   = MIN(args->render_range_end, self->args.vt_line->data.size) }
    };

    return sub;
}

static void line_reder_pass_run_cell_subpass(line_render_pass_t*    pass,
                                             line_render_subpass_t* subpass)
{
    ASSERT(subpass->args.render_range_begin <= pass->args.vt_line->data.size &&
             subpass->args.render_range_end <= pass->args.vt_line->data.size,
           "subpass range within line size");

    const double scalex = 2.0 / pass->texture_width;
    const double scaley = 2.0 / pass->texture_height;

    GLint bg_pixels_begin = subpass->args.render_range_begin * pass->args.gl2->glyph_width_pixels,
          bg_pixels_end;

    VtRune* each_rune = pass->args.vt_line->data.buf + subpass->args.render_range_begin;
    VtRune* same_bg_block_begin_rune = each_rune;
    VtRune* cursor_rune              = Vt_cursor_cell(pass->args.vt);

    ColorRGBA active_bg_color;
    if (pass->args.is_for_cursor) {
        if (unlikely(Vt_is_cell_selected(pass->args.vt,
                                         pass->args.vt->cursor.col,
                                         pass->args.visual_index))) {
            active_bg_color = pass->args.vt->colors.highlight.bg;
        } else {
            active_bg_color = Vt_rune_cursor_bg(pass->args.vt, cursor_rune);
        }
    } else {
        active_bg_color = pass->args.vt->colors.bg;
    }

    for (uint16_t idx_each_rune = subpass->args.render_range_begin;
         idx_each_rune <= subpass->args.render_range_end;) {
        each_rune = pass->args.vt_line->data.buf + idx_each_rune;
        if (likely(idx_each_rune != subpass->args.render_range_end)) {
            if (unlikely(each_rune->blinkng)) {
                pass->has_blinking_chars = true;
            }
            if (!pass->has_underlined_chars &&
                unlikely(each_rune->underlined || each_rune->strikethrough ||
                         each_rune->doubleunderline || each_rune->curlyunderline ||
                         each_rune->overline || each_rune->hyperlink_idx)) {
                pass->has_underlined_chars = true;
            }
        }

#define L_COLOR_BG                                                                                 \
    Vt_rune_final_bg(pass->args.vt,                                                                \
                     pass->args.is_for_cursor ? cursor_rune : each_rune,                           \
                     idx_each_rune,                                                                \
                     pass->args.visual_index,                                                      \
                     pass->args.is_for_cursor)

        if (idx_each_rune == subpass->args.render_range_end ||
            !ColorRGBA_eq(L_COLOR_BG, active_bg_color)) {
            int32_t extra_width = 0;

            if (idx_each_rune > 1) {
                extra_width =
                  MAX(Rune_width(pass->args.vt_line->data.buf[idx_each_rune - 1].rune) - 2, 0);
            }

            bg_pixels_end = (idx_each_rune + extra_width) * pass->args.gl2->glyph_width_pixels;

            glEnable(GL_SCISSOR_TEST);
            glScissor(bg_pixels_begin, 0, bg_pixels_end - bg_pixels_begin, pass->texture_height);

            glClearColor(ColorRGBA_get_float(active_bg_color, 0),
                         ColorRGBA_get_float(active_bg_color, 1),
                         ColorRGBA_get_float(active_bg_color, 2),
                         ColorRGBA_get_float(active_bg_color, 3));

            glClear(GL_COLOR_BUFFER_BIT
#ifndef GFX_GLES
                    | GL_DEPTH_BUFFER_BIT
#endif
            );

            { // for each block of characters with the same background color
                ColorRGB active_fg_color = (pass->args.is_for_cursor && cursor_rune)
                                             ? Vt_rune_final_fg_apply_dim(pass->args.vt,
                                                                          cursor_rune,
                                                                          active_bg_color,
                                                                          pass->args.is_for_cursor)
                                             : settings.fg;

                const VtRune* same_colors_block_begin_rune = same_bg_block_begin_rune;

                for (const VtRune* each_rune_same_bg = same_bg_block_begin_rune;
                     each_rune_same_bg != each_rune + 1;
                     ++each_rune_same_bg) {

                    if (each_rune_same_bg == each_rune ||
                        !ColorRGB_eq(
                          Vt_rune_final_fg(pass->args.vt,
                                           each_rune_same_bg,
                                           each_rune_same_bg - pass->args.vt_line->data.buf,
                                           pass->args.visual_index,
                                           active_bg_color,
                                           pass->args.is_for_cursor),
                          active_fg_color)) {

                        /* Dummy value with we can point to to filter out a character */
                        VtRune        same_color_blank_space;
                        const VtRune* each_rune_filtered_visible;

                        for (Vector_float* i = NULL;
                             (i = Vector_iter_Vector_float(&pass->args.gl2->float_vec, i));) {
                            Vector_clear_float(i);
                        }

                        for (const VtRune* each_rune_same_colors = same_colors_block_begin_rune;
                             each_rune_same_colors != each_rune_same_bg;
                             ++each_rune_same_colors) {
                            size_t column = each_rune_same_colors - pass->args.vt_line->data.buf;

                            /* Filter out stuff that should be hidden on this pass */
                            if (unlikely(
                                  (pass->args.is_for_blinking && each_rune_same_colors->blinkng) ||
                                  each_rune_same_colors->hidden)) {
                                same_color_blank_space           = *each_rune_same_colors;
                                same_color_blank_space.rune.code = ' ';
                                each_rune_filtered_visible       = &same_color_blank_space;
                            } else {
                                each_rune_filtered_visible = each_rune_same_colors;
                            }

                            if (each_rune_filtered_visible->rune.code > ' ') {
                                GlyphAtlasEntry* entry =
                                  GlyphAtlas_get(pass->args.gl2,
                                                 &pass->args.gl2->glyph_atlas,
                                                 &each_rune_filtered_visible->rune);
                                if (!entry) {
                                    continue;
                                }

                                float h = (float)entry->height * scaley;
                                float w = (float)entry->width * scalex;
                                float t = entry->top * scaley;
                                float l = entry->left * scalex;

                                float x3 =
                                  -1.0 +
                                  (double)column * pass->args.gl2->glyph_width_pixels * scalex + l +
                                  pass->args.gl2->pen_begin_pixels_x * scalex;
                                float y3 = -1.0 + pass->args.gl2->pen_begin_pixels_y * scaley - t;

#ifdef GFX_GLES
                                float buf[] = {
                                    x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
                                    x3 + w, y3,     entry->tex_coords[2], entry->tex_coords[1],
                                    x3 + w, y3 + h, entry->tex_coords[2], entry->tex_coords[3],

                                    x3,     y3 + h, entry->tex_coords[0], entry->tex_coords[3],
                                    x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
                                    x3 + w, y3 + h, entry->tex_coords[2], entry->tex_coords[3],
                                };
#else
                                float buf[] = {
                                    x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
                                    x3 + w, y3,     entry->tex_coords[2], entry->tex_coords[1],
                                    x3 + w, y3 + h, entry->tex_coords[2], entry->tex_coords[3],
                                    x3,     y3 + h, entry->tex_coords[0], entry->tex_coords[3],
                                };
#endif

                                while (pass->args.gl2->float_vec.size <= entry->page_id) {
                                    Vector_push_Vector_float(&pass->args.gl2->float_vec,
                                                             Vector_new_float());
                                }

                                Vector_pushv_float(&pass->args.gl2->float_vec.buf[entry->page_id],
                                                   buf,
                                                   ARRAY_SIZE(buf));
                            }
                        }

                        {
                            GLint clip_begin =
                              (same_colors_block_begin_rune - pass->args.vt_line->data.buf) *
                              pass->args.gl2->glyph_width_pixels;

                            ptrdiff_t clip_end_idx =
                              each_rune_same_bg - pass->args.vt_line->data.buf;

                            uint8_t width =
                              clip_end_idx < (long)pass->args.vt_line->data.size
                                ? Rune_width_spill(pass->args.vt_line->data.buf[clip_end_idx].rune)
                                : 0;

                            GLsizei clip_end =
                              (clip_end_idx + width) * pass->args.gl2->glyph_width_pixels;

                            glEnable(GL_SCISSOR_TEST);
                            glScissor(clip_begin, 0, clip_end - clip_begin, pass->texture_height);
                        }

                        /* Actual drawing */
                        for (size_t i = 0; i < pass->args.gl2->glyph_atlas.pages.size; ++i) {
                            Vector_float*   v    = &pass->args.gl2->float_vec.buf[i];
                            GlyphAtlasPage* page = &pass->args.gl2->glyph_atlas.pages.buf[i];

                            glBindTexture(GL_TEXTURE_2D, page->texture_id);
                            glBindBuffer(GL_ARRAY_BUFFER, pass->args.gl2->flex_vbo.vbo);

                            size_t newsize = v->size * sizeof(float);
                            ARRAY_BUFFER_SUB_OR_SWAP(v->buf,
                                                     pass->args.gl2->flex_vbo.size,
                                                     newsize);

#ifdef GFX_GLES
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#endif

                            switch (page->texture_format) {
                                case TEX_FMT_RGB:
                                    if (pass->args.gl2->bound_resources != BOUND_RESOURCES_FONT) {
                                        pass->args.gl2->bound_resources = BOUND_RESOURCES_FONT;
                                        glUseProgram(pass->args.gl2->font_shader.id);
                                    }
                                    glVertexAttribPointer(
                                      pass->args.gl2->font_shader.attribs->location,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);
                                    glUniform3f(pass->args.gl2->font_shader.uniforms[1].location,
                                                ColorRGB_get_float(active_fg_color, 0),
                                                ColorRGB_get_float(active_fg_color, 1),
                                                ColorRGB_get_float(active_fg_color, 2));

                                    glUniform4f(pass->args.gl2->font_shader.uniforms[2].location,
                                                ColorRGBA_get_float(active_bg_color, 0),
                                                ColorRGBA_get_float(active_bg_color, 1),
                                                ColorRGBA_get_float(active_bg_color, 2),
                                                ColorRGBA_get_float(active_bg_color, 3));
                                    break;

                                case TEX_FMT_MONO:
                                    if (pass->args.gl2->bound_resources !=
                                        BOUND_RESOURCES_FONT_MONO) {
                                        pass->args.gl2->bound_resources = BOUND_RESOURCES_FONT_MONO;
                                        glUseProgram(pass->args.gl2->font_shader_gray.id);
                                    }
                                    glVertexAttribPointer(
                                      pass->args.gl2->font_shader_gray.attribs->location,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);

                                    glUniform3f(
                                      pass->args.gl2->font_shader_gray.uniforms[1].location,
                                      ColorRGB_get_float(active_fg_color, 0),
                                      ColorRGB_get_float(active_fg_color, 1),
                                      ColorRGB_get_float(active_fg_color, 2));
                                    glUniform4f(
                                      pass->args.gl2->font_shader_gray.uniforms[2].location,
                                      ColorRGBA_get_float(active_bg_color, 0),
                                      ColorRGBA_get_float(active_bg_color, 1),
                                      ColorRGBA_get_float(active_bg_color, 2),
                                      ColorRGBA_get_float(active_bg_color, 3));

                                    break;
                                case TEX_FMT_RGBA:
                                    if (pass->args.gl2->bound_resources != BOUND_RESOURCES_IMAGE) {
                                        pass->args.gl2->bound_resources = BOUND_RESOURCES_IMAGE;
                                        glUseProgram(pass->args.gl2->image_shader.id);
                                    }

#ifndef GFX_GLES
                                    glDisable(GL_DEPTH_TEST);
#endif
                                    glEnable(GL_BLEND);
                                    glBlendFuncSeparate(GL_SRC_ALPHA,
                                                        GL_ONE_MINUS_SRC_ALPHA,
                                                        GL_ONE,
                                                        GL_ONE_MINUS_SRC_ALPHA);
                                    glVertexAttribPointer(
                                      pass->args.gl2->image_shader.attribs->location,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);
                                default:;
                            }

                            glDrawArrays(QUAD_DRAW_MODE, 0, v->size / 4);
                            ARRAY_BUFFER_ORPHAN(pass->args.gl2->flex_vbo.size);
                            glDisable(GL_BLEND);

#ifndef GFX_GLES
                            glEnable(GL_DEPTH_TEST);
#endif
                        }
                        // end drawing

                        glDisable(GL_SCISSOR_TEST);

                        if (each_rune_same_bg != each_rune) {
                            same_colors_block_begin_rune = each_rune_same_bg;

                            if (!pass->args.is_for_cursor) {
                                // update active fg color;
                                if (settings.highlight_change_fg &&
                                    unlikely(Vt_is_cell_selected(pass->args.vt,
                                                                 each_rune_same_bg -
                                                                   pass->args.vt_line->data.buf,
                                                                 pass->args.visual_index))) {
                                    active_fg_color = pass->args.vt->colors.highlight.fg;
                                } else {
                                    active_fg_color =
                                      Vt_rune_final_fg_apply_dim(pass->args.vt,
                                                                 each_rune_same_bg,
                                                                 active_bg_color,
                                                                 pass->args.is_for_cursor);
                                }
                            }
                        }

                    } // end for each block with the same color
                }     // end for each char
            }         // end for each block with the same bg

            bg_pixels_begin = (idx_each_rune + extra_width) * pass->args.gl2->glyph_width_pixels;

            int clip_begin = idx_each_rune * pass->args.gl2->glyph_width_pixels;
            glEnable(GL_SCISSOR_TEST);
            glScissor(clip_begin, 0, pass->texture_width, pass->texture_height);

            if (idx_each_rune != subpass->args.render_range_end) {
                same_bg_block_begin_rune = each_rune;
                if (!pass->args.is_for_cursor) {
                    active_bg_color = Vt_rune_final_bg(pass->args.vt,
                                                       each_rune,
                                                       idx_each_rune,
                                                       pass->args.visual_index,
                                                       pass->args.is_for_cursor);
                }
            }
        } // end if bg color changed

        int w = likely(idx_each_rune != subpass->args.render_range_end)
                  ? Rune_width(pass->args.vt_line->data.buf[idx_each_rune].rune)
                  : 1;

        idx_each_rune = CLAMP(idx_each_rune + (unlikely(w > 1) ? w : 1),
                              subpass->args.render_range_begin,
                              (uint16_t)(pass->args.vt_line->data.size + 1));
    }
}

static void line_reder_pass_run_line_subpass(const line_render_pass_t* pass,
                                             line_render_subpass_t*    subpass)
{
    /* Scale from pixels to GL coordinates */
    const double scalex = 2.0f / pass->texture_width;
    const double scaley = 2.0f / pass->texture_height;

    /* draw lines */
    static float begin[6]   = { -1, -1, -1, -1, -1 };
    static float end[6]     = { 1, 1, 1, 1, 1, 1 };
    static bool  drawing[6] = { 0 };

    if (unlikely(subpass->args.render_range_begin)) {
        const float init_coord =
          unlikely(subpass->args.render_range_end)
            ? -1.0f + pass->args.gl2->glyph_width_pixels * scalex * subpass->args.render_range_begin
            : 0.0f;

        for (uint_fast8_t i = 0; i < ARRAY_SIZE(begin); ++i) {
            begin[i] = init_coord;
        }
    }

    /* lines are in the same color as the character, unless the line color was explicitly set */
    ColorRGB line_color =
      Vt_rune_ln_clr(pass->args.vt,
                     &pass->args.vt_line->data.buf[subpass->args.render_range_begin]);

    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const VtRune* each_rune = pass->args.vt_line->data.buf + subpass->args.render_range_begin;
         each_rune <= pass->args.vt_line->data.buf + subpass->args.render_range_end;
         ++each_rune) {

        /* text column where this should be drawn */
        size_t   column = each_rune - pass->args.vt_line->data.buf;
        ColorRGB nc     = { 0 };
        if (each_rune != pass->args.vt_line->data.buf + subpass->args.render_range_end) {
            nc = Vt_rune_ln_clr(pass->args.vt, each_rune);
        }
        /* State has changed */
        if (!ColorRGB_eq(line_color, nc) ||
            each_rune == pass->args.vt_line->data.buf + subpass->args.render_range_end ||
            each_rune->underlined != drawing[0] || each_rune->doubleunderline != drawing[1] ||
            each_rune->strikethrough != drawing[2] || each_rune->overline != drawing[3] ||
            each_rune->curlyunderline != drawing[4] ||
            (each_rune->hyperlink_idx != 0) != drawing[5]) {

            if (each_rune == pass->args.vt_line->data.buf + subpass->args.render_range_end) {
                for (uint_fast8_t tmp = 0; tmp < ARRAY_SIZE(end); tmp++) {
                    end[tmp] =
                      -1.0f + (float)column * scalex * (float)pass->args.gl2->glyph_width_pixels;
                }
            } else {
#define L_SET_BOUNDS_END(what, index)                                                              \
    if (drawing[index]) {                                                                          \
        end[index] = -1.0f + (float)column * scalex * (float)pass->args.gl2->glyph_width_pixels;   \
    }
                L_SET_BOUNDS_END(z->underlined, 0);
                L_SET_BOUNDS_END(z->doubleunderline, 1);
                L_SET_BOUNDS_END(z->strikethrough, 2);
                L_SET_BOUNDS_END(z->overline, 3);
                L_SET_BOUNDS_END(z->curlyunderline, 4);
                L_SET_BOUNDS_END(z->hyperlink_idx, 5);
            }

            Vector_vertex_t* vb  = &pass->args.gl2->vec_vertex_buffer;
            Vector_vertex_t* vb2 = &pass->args.gl2->vec_vertex_buffer2;

            Vector_clear_vertex_t(vb);
            Vector_clear_vertex_t(vb2);

            if (drawing[0]) {
                Vector_push_vertex_t(vb, (vertex_t){ begin[0], 1.0f - scaley });
                Vector_push_vertex_t(vb, (vertex_t){ end[0], 1.0f - scaley });
            }
            if (drawing[1]) {
                Vector_push_vertex_t(vb, (vertex_t){ begin[1], 1.0f });
                Vector_push_vertex_t(vb, (vertex_t){ end[1], 1.0f });
                Vector_push_vertex_t(vb, (vertex_t){ begin[1], 1.0f - 2 * scaley });
                Vector_push_vertex_t(vb, (vertex_t){ end[1], 1.0f - 2 * scaley });
            }
            if (drawing[2]) {
                Vector_push_vertex_t(vb, (vertex_t){ begin[2], .2f });
                Vector_push_vertex_t(vb, (vertex_t){ end[2], .2f });
            }
            if (drawing[3]) {
                Vector_push_vertex_t(vb, (vertex_t){ begin[3], -1.0f + scaley });
                Vector_push_vertex_t(vb, (vertex_t){ end[3], -1.0f + scaley });
            }
            if (drawing[4]) {
                float   cw      = pass->args.gl2->glyph_width_pixels * scalex;
                int32_t n_cells = round((end[4] - begin[4]) / cw);
                float   t_y     = 1.0f - pass->args.gl2->squiggle_texture.h * scaley;

#ifdef GFX_GLES
                Vector_push_vertex_t(vb2, (vertex_t){ begin[4], t_y });
                Vector_push_vertex_t(vb2, (vertex_t){ 0.0f, 0.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ begin[4], 1.0f });
                Vector_push_vertex_t(vb2, (vertex_t){ 0.0f, 1.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ end[4], 1.0f });
                Vector_push_vertex_t(vb2, (vertex_t){ 1.0f * n_cells, 1.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ end[4], t_y });
                Vector_push_vertex_t(vb2, (vertex_t){ 1.0f * n_cells, 0.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ end[4], 1.0f });
                Vector_push_vertex_t(vb2, (vertex_t){ 1.0f * n_cells, 1.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ begin[4], t_y });
                Vector_push_vertex_t(vb2, (vertex_t){ 0.0f, 0.0f });
#else
                Vector_push_vertex_t(vb2, (vertex_t){ begin[4], t_y });
                Vector_push_vertex_t(vb2, (vertex_t){ 0.0f, 0.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ begin[4], 1.0f });
                Vector_push_vertex_t(vb2, (vertex_t){ 0.0f, 1.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ end[4], 1.0f });
                Vector_push_vertex_t(vb2, (vertex_t){ 1.0f * n_cells, 1.0f });

                Vector_push_vertex_t(vb2, (vertex_t){ end[4], t_y });
                Vector_push_vertex_t(vb2, (vertex_t){ 1.0f * n_cells, 0.0f });
#endif
            }
            if (drawing[5] && !drawing[0]) {
                for (float i = begin[5]; i < (end[5] - scalex * 0.5f);
                     i += (scalex * pass->args.gl2->glyph_width_pixels)) {
                    float j = i + scalex * pass->args.gl2->glyph_width_pixels / 2.0;
                    Vector_push_vertex_t(vb, (vertex_t){ i, 1.0f - scaley });
                    Vector_push_vertex_t(vb, (vertex_t){ j, 1.0f - scaley });
                }
            }

            if (pass->args.gl2->vec_vertex_buffer.size) {
                if (pass->args.gl2->bound_resources != BOUND_RESOURCES_LINES) {
                    pass->args.gl2->bound_resources = BOUND_RESOURCES_LINES;
                    Shader_use(&pass->args.gl2->line_shader);
                    glBindBuffer(GL_ARRAY_BUFFER, pass->args.gl2->flex_vbo.vbo);
                    glVertexAttribPointer(pass->args.gl2->line_shader.attribs->location,
                                          2,
                                          GL_FLOAT,
                                          GL_FALSE,
                                          0,
                                          0);
                }
                glUniform3f(pass->args.gl2->line_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0),
                            ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));
                size_t new_size = sizeof(vertex_t) * pass->args.gl2->vec_vertex_buffer.size;
                ARRAY_BUFFER_SUB_OR_SWAP(pass->args.gl2->vec_vertex_buffer.buf,
                                         pass->args.gl2->flex_vbo.size,
                                         new_size);

                glDrawArrays(GL_LINES, 0, pass->args.gl2->vec_vertex_buffer.size);
                ARRAY_BUFFER_ORPHAN(pass->args.gl2->flex_vbo.size);
            }
            if (pass->args.gl2->vec_vertex_buffer2.size) {
                pass->args.gl2->bound_resources = BOUND_RESOURCES_NONE;
                Shader_use(&pass->args.gl2->image_tint_shader);
                glBindTexture(GL_TEXTURE_2D, pass->args.gl2->squiggle_texture.id);
                glUniform3f(pass->args.gl2->image_tint_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0),
                            ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));
                glBindBuffer(GL_ARRAY_BUFFER, pass->args.gl2->flex_vbo.vbo);
                glVertexAttribPointer(pass->args.gl2->font_shader.attribs->location,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);
                size_t new_size = sizeof(vertex_t) * pass->args.gl2->vec_vertex_buffer2.size;
                ARRAY_BUFFER_SUB_OR_SWAP(pass->args.gl2->vec_vertex_buffer2.buf,
                                         pass->args.gl2->flex_vbo.size,
                                         new_size);

                glDrawArrays(QUAD_DRAW_MODE, 0, pass->args.gl2->vec_vertex_buffer2.size / 2);
                ARRAY_BUFFER_ORPHAN(pass->args.gl2->flex_vbo.size);
            }

#define L_SET_BOUNDS_BEGIN(what, index)                                                            \
    if (what) {                                                                                    \
        begin[index] = -1.0f + (float)column * scalex * (float)pass->args.gl2->glyph_width_pixels; \
    }

            if (each_rune != pass->args.vt_line->data.buf + subpass->args.render_range_end) {
                L_SET_BOUNDS_BEGIN(each_rune->underlined, 0);
                L_SET_BOUNDS_BEGIN(each_rune->doubleunderline, 1);
                L_SET_BOUNDS_BEGIN(each_rune->strikethrough, 2);
                L_SET_BOUNDS_BEGIN(each_rune->overline, 3);
                L_SET_BOUNDS_BEGIN(each_rune->curlyunderline, 4);
                L_SET_BOUNDS_BEGIN(each_rune->hyperlink_idx, 5);
                drawing[0] = each_rune->underlined;
                drawing[1] = each_rune->doubleunderline;
                drawing[2] = each_rune->strikethrough;
                drawing[3] = each_rune->overline;
                drawing[4] = each_rune->curlyunderline;
                drawing[5] = each_rune->hyperlink_idx != 0;
            } else {
                memset(drawing, false, sizeof(drawing));
            }

            line_color = nc;
        }
    }
}

static void line_reder_subpass_run_clear_stage(const line_render_pass_t* self,
                                               line_render_subpass_t*    subpass)
{
    glViewport(0, 0, self->texture_width, self->texture_height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);

    if (self->args.is_for_cursor) {
        ColorRGBA clr = Vt_rune_cursor_bg(self->args.vt, NULL);
        glClearColor(ColorRGBA_get_float(clr, 0),
                     ColorRGBA_get_float(clr, 1),
                     ColorRGBA_get_float(clr, 2),
                     ColorRGBA_get_float(clr, 3));
    } else {
        glClearColor(ColorRGBA_get_float(self->args.vt->colors.bg, 0),
                     ColorRGBA_get_float(self->args.vt->colors.bg, 1),
                     ColorRGBA_get_float(self->args.vt->colors.bg, 2),
                     ColorRGBA_get_float(self->args.vt->colors.bg, 3));
    }

    if (self->args.damage->type == VT_LINE_DAMAGE_RANGE) {
        glEnable(GL_SCISSOR_TEST);
        size_t begin_px = self->args.gl2->glyph_width_pixels * self->args.damage->front;
        size_t width_px = ((self->args.damage->end + 1) - self->args.damage->front) *
                          self->args.gl2->glyph_width_pixels;
        glScissor(begin_px, 0, width_px, self->texture_height);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    // TODO: VT_LINE_DAMAGE_SHIFT

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifndef GFX_GLES
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LEQUAL);
    glDepthRange(0.0f, 1.0f);
#endif
}

static void line_reder_pass_run_subpass(line_render_pass_t* pass, line_render_subpass_t* subpass)
{
    line_reder_subpass_run_clear_stage(pass, subpass);
    line_reder_pass_run_cell_subpass(pass, subpass);

    if (pass->has_underlined_chars) {
        line_reder_pass_run_line_subpass(pass, subpass);
    }
}

static void line_reder_pass_run_initial_setup(line_render_pass_t* self)
{
    line_render_pass_try_to_recover_proxies(self);
    line_render_pass_set_up_framebuffer(self);
    line_render_pass_set_up_subpasses(self);
}

static Pair_uint16_t line_reder_pass_run_subpasses(line_render_pass_t* self)
{
    Pair_uint16_t retval = { .first = self->args.vt_line->data.size - 1, .second = 0 };

    for (int i = 0; i < self->n_queued_subpasses; ++i) {
        line_render_subpass_t sub = line_render_pass_create_subpass(self, &self->subpass_args[i]);

        retval.first  = MIN(retval.first, sub.args.render_range_begin);
        retval.second = MAX(retval.second, sub.args.render_range_end);

        line_reder_pass_run_subpass(self, &sub);
    }

    return retval;
}

static void line_reder_pass_run(line_render_pass_t* self)
{
    line_reder_pass_run_initial_setup(self);
    line_reder_pass_run_subpasses(self);
}

static void _GfxOpenGL2_draw_block_cursor(GfxOpenGL2* gfx,
                                          const Vt*   vt,
                                          const Ui*   ui,
                                          ColorRGBA   clr,
                                          size_t      row)
{
    ((vt_line_damage_t*)&ui->cursor_damage)->type = VT_LINE_DAMAGE_FULL;
    VtLine* vt_line                               = Vt_cursor_line(vt);

    line_render_pass_args_t rp_args = {
        .gl2               = gfx,
        .vt                = vt,
        .vt_line           = vt_line,
        .proxy             = (VtLineProxy*)&ui->cursor_proxy,
        .damage            = (vt_line_damage_t*)&ui->cursor_damage,
        .visual_index      = Vt_visual_cursor_row(vt),
        .cnd_cursor_column = ui->cursor->col,
        .is_for_cursor     = true,
        .is_for_blinking   = false,
    };

    if (should_create_line_render_pass(&rp_args)) {
        gfx->bound_resources  = BOUND_RESOURCES_NONE;
        line_render_pass_t rp = create_line_render_pass(&rp_args);

        line_reder_pass_run(&rp);

        if (rp.has_blinking_chars) {
            gfxBase(gfx)->has_blinking_text = true;
            rp_args.is_for_blinking         = true;
            gfx->bound_resources            = BOUND_RESOURCES_NONE;
            line_render_pass_t rp_b         = create_line_render_pass(&rp_args);
            line_reder_pass_run(&rp_b);
            rp_b.has_blinking_chars = true;
            line_render_pass_finalize(&rp_b);
            rp_args.is_for_blinking = false;
        }

        line_render_pass_finalize(&rp);
    }

    glViewport(0, 0, gfx->win_w, gfx->win_h);
    double dbl_col = ui->cursor_cell_fraction;
    {
        glEnable(GL_SCISSOR_TEST);
        GLint x   = dbl_col * gfx->glyph_width_pixels + gfx->pixel_offset_x,
              y   = gfx->win_h - (row + 1) * gfx->line_height_pixels - gfx->pixel_offset_y;
        GLsizei w = gfx->glyph_width_pixels, h = gfx->line_height_pixels;
        glScissor(x, y, w, h);
    }

    glClearColor(ColorRGBA_get_float(clr, 0),
                 ColorRGBA_get_float(clr, 1),
                 ColorRGBA_get_float(clr, 2),
                 ColorRGBA_get_float(clr, 3));

    glClear(GL_COLOR_BUFFER_BIT);

    float tex_begin_x = -1.0f + (gfx->pixel_offset_x) * gfx->sx;
    float tex_end_x =
      -1.0f + (gfx->max_cells_in_line * gfx->glyph_width_pixels + gfx->pixel_offset_x) * gfx->sx;
    float tex_begin_y = 1.0f - gfx->line_height_pixels * (Vt_visual_cursor_row(vt) + 1) * gfx->sy -
                        gfx->pixel_offset_y * gfx->sy;

#ifdef GFX_GLES
    float buf[] = {
        tex_begin_x, tex_begin_y + gfx->line_height,
        0.0f,        0.0f,
        tex_begin_x, tex_begin_y,
        0.0f,        1.0f,
        tex_end_x,   tex_begin_y,
        1.0f,        1.0f,
        tex_end_x,   tex_begin_y + gfx->line_height,
        1.0f,        0.0f,
        tex_end_x,   tex_begin_y,
        1.0f,        1.0f,
        tex_begin_x, tex_begin_y + gfx->line_height,
        0.0f,        0.0f,
    };
#else
    float buf[] = {
        tex_begin_x, tex_begin_y + gfx->line_height,
        0.0f,        0.0f,
        tex_begin_x, tex_begin_y,
        0.0f,        1.0f,
        tex_end_x,   tex_begin_y,
        1.0f,        1.0f,
        tex_end_x,   tex_begin_y + gfx->line_height,
        1.0f,        0.0f,
    };
#endif

#ifdef GFX_GLES
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#endif

    Shader_use(&gfx->image_shader);
    glBindTexture(GL_TEXTURE_2D,
                  ui->cursor_proxy.data[PROXY_INDEX_TEXTURE_BLINK] && !ui->draw_text_blinking
                    ? ui->cursor_proxy.data[PROXY_INDEX_TEXTURE_BLINK]
                    : ui->cursor_proxy.data[PROXY_INDEX_TEXTURE]);
    glUniform2f(gfx->image_shader.uniforms[1].location, 0, 0);

    glVertexAttribPointer(gfx->image_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);

    size_t newsize = sizeof(buf);
    ARRAY_BUFFER_SUB_OR_SWAP(buf, gfx->flex_vbo.size, newsize);
    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
    ARRAY_BUFFER_ORPHAN(gfx->flex_vbo.size);

#ifdef GFX_GLES
    glDisable(GL_BLEND);
#endif
}

static void GfxOpenGL2_draw_cursor(GfxOpenGL2* gfx, const Vt* vt, const Ui* ui)
{
    if (!ui || !ui->cursor) {
        return;
    }

    bool show_blink = !settings.enable_cursor_blink || !ui->window_in_focus ||
                      !ui->cursor->blinking || (ui->cursor->blinking && ui->draw_cursor_blinking);

    gfx->frame_overlay_damage->curosr_position_x =
      ui->cursor_cell_fraction * gfx->glyph_width_pixels + gfx->pixel_offset_x;

    size_t row = ui->cursor->row - Vt_visual_top_line(vt), st_col = ui->cursor->col;
    double col = ui->cursor_cell_fraction;

    gfx->frame_overlay_damage->curosr_position_y =
      row * gfx->line_height_pixels + gfx->pixel_offset_y;
    gfx->frame_overlay_damage->line_index = row;

    bool hidden = (!show_blink || ui->cursor->hidden);

    gfx->frame_overlay_damage->cursor_drawn = !hidden;

    if (hidden) {
        return;
    }

    bool filled_block = false;

    if (row >= Vt_row(vt)) {
        return;
    }

    gfx->frame_overlay_damage->cursor_drawn = true;

    Vector_clear_vertex_t(&gfx->vec_vertex_buffer);

    switch (ui->cursor->type) {
        case CURSOR_BEAM:
            Vector_pushv_vertex_t(
              &gfx->vec_vertex_buffer,
              (vertex_t[2]){ { .x = -1.0f + (1 + col * gfx->glyph_width_pixels) * gfx->sx,
                               .y = 1.0f - row * gfx->line_height_pixels * gfx->sy },
                             { .x = -1.0f + (1 + col * gfx->glyph_width_pixels) * gfx->sx,
                               .y = 1.0f - (row + 1) * gfx->line_height_pixels * gfx->sy } },
              2);
            break;

        case CURSOR_UNDERLINE:
            Vector_pushv_vertex_t(
              &gfx->vec_vertex_buffer,
              (vertex_t[2]){ { .x = -1.0f + col * gfx->glyph_width_pixels * gfx->sx,
                               .y = 1.0f - ((row + 1) * gfx->line_height_pixels) * gfx->sy },
                             { .x = -1.0f + (col + 1) * gfx->glyph_width_pixels * gfx->sx,
                               .y = 1.0f - ((row + 1) * gfx->line_height_pixels) * gfx->sy } },
              2);
            break;

        case CURSOR_BLOCK:
            if (!ui->window_in_focus)
                Vector_pushv_vertex_t(
                  &gfx->vec_vertex_buffer,
                  (vertex_t[4]){
                    { .x = -1.0f + (col * gfx->glyph_width_pixels) * gfx->sx + 0.9f * gfx->sx,
                      .y =
                        1.0f - ((row + 1) * gfx->line_height_pixels) * gfx->sy + 0.5f * gfx->sy },
                    { .x = -1.0f + ((col + 1) * gfx->glyph_width_pixels) * gfx->sx,
                      .y =
                        1.0f - ((row + 1) * gfx->line_height_pixels) * gfx->sy + 0.5f * gfx->sy },
                    { .x = -1.0f + ((col + 1) * gfx->glyph_width_pixels) * gfx->sx,
                      .y = 1.0f - (row * gfx->line_height_pixels) * gfx->sy - 0.5f * gfx->sy },
                    { .x = -1.0f + (col * gfx->glyph_width_pixels) * gfx->sx + 0.9f * gfx->sx,
                      .y = 1.0f - (row * gfx->line_height_pixels) * gfx->sy } },
                  4);
            else
                filled_block = true;
            break;
    }

    VtRune* cursor_rune = NULL;

    if (vt->lines.size > ui->cursor->row &&
        Vt_get_visible_line(vt, ui->cursor->row)->data.size > st_col) {
        cursor_rune = &Vt_get_visible_line(vt, ui->cursor->row)->data.buf[st_col];
    }

    ColorRGBA clr = Vt_rune_cursor_bg(vt, cursor_rune);

    if (!filled_block) {
        Shader_use(&gfx->line_shader);
        glBindTexture(GL_TEXTURE_2D, 0);

        glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
        glVertexAttribPointer(gfx->line_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);

        glUniform3f(gfx->line_shader.uniforms[1].location,
                    ColorRGBA_get_float(clr, 0),
                    ColorRGBA_get_float(clr, 1),
                    ColorRGBA_get_float(clr, 2));

        size_t newsize = gfx->vec_vertex_buffer.size * sizeof(vertex_t);
        ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer.buf, gfx->flex_vbo.size, newsize);
        glDrawArrays(gfx->vec_vertex_buffer.size == 2 ? GL_LINES : GL_LINE_LOOP,
                     0,
                     gfx->vec_vertex_buffer.size);
        ARRAY_BUFFER_ORPHAN(gfx->flex_vbo.size);
    } else {
        _GfxOpenGL2_draw_block_cursor(gfx, vt, ui, clr, row);
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
}

__attribute__((cold)) static void GfxOpenGL2_draw_unicode_input(GfxOpenGL2* gfx, const Vt* vt)
{
    size_t begin = MIN(vt->cursor.col, vt->ws.ws_col - vt->unicode_input.buffer.size - 1);
    size_t row   = vt->cursor.row - Vt_visual_top_line(vt);
    size_t col   = begin;
    glEnable(GL_SCISSOR_TEST);
    {
        glScissor(col * gfx->glyph_width_pixels + gfx->pixel_offset_x,
                  gfx->win_h - (row + 1) * gfx->line_height_pixels - gfx->pixel_offset_y,
                  gfx->glyph_width_pixels * (vt->unicode_input.buffer.size + 1),
                  gfx->line_height_pixels);
        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);

        Rune rune = {
            .code    = 'u',
            .combine = { 0 },
            .style   = VT_RUNE_NORMAL,
        };

        GlyphAtlasEntry* entry = GlyphAtlas_get(gfx, &gfx->glyph_atlas, &rune);
        float            h     = (float)entry->height * gfx->sy;
        float            w     = (float)entry->width * gfx->sx;
        float            t     = entry->top * gfx->sy;
        float            l     = entry->left * gfx->sx;
        float            x3    = -1.0f + (float)col * gfx->glyph_width_pixels * gfx->sx + l +
                   gfx->pen_begin_pixels_x * gfx->sx;
        float y3 = 1.0f - (float)(row)*gfx->line_height_pixels * gfx->sy -
                   gfx->pen_begin_pixels_y * gfx->sy + t;

#ifdef GFX_GLES
        float buf[] = {
            x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
            x3 + w, y3,     entry->tex_coords[2], entry->tex_coords[1],
            x3 + w, y3 - h, entry->tex_coords[2], entry->tex_coords[3],

            x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
            x3,     y3 - h, entry->tex_coords[0], entry->tex_coords[3],
            x3 + w, y3 - h, entry->tex_coords[2], entry->tex_coords[3],
        };
#else
        float buf[] = {
            x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
            x3 + w, y3,     entry->tex_coords[2], entry->tex_coords[1],
            x3 + w, y3 - h, entry->tex_coords[2], entry->tex_coords[3],
            x3,     y3 - h, entry->tex_coords[0], entry->tex_coords[3],
        };
#endif

        GlyphAtlasPage* page = &gfx->glyph_atlas.pages.buf[entry->page_id];
        glBindTexture(GL_TEXTURE_2D, page->texture_id);
        glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
        size_t newsize = sizeof(buf);
        ARRAY_BUFFER_SUB_OR_SWAP(buf, gfx->flex_vbo.size, newsize);
        switch (page->texture_format) {
            case TEX_FMT_RGB: {
                glUseProgram(gfx->font_shader.id);
                GLuint loc = gfx->font_shader.attribs->location;
                glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0);
                glUniform3f(gfx->font_shader.uniforms[1].location, 0.0f, 0.0f, 0.0f);
                glUniform4f(gfx->font_shader.uniforms[2].location, 1.0f, 1.0f, 1.0f, 1.0f);
            } break;
            case TEX_FMT_MONO: {
                glUseProgram(gfx->font_shader_gray.id);
                GLuint loc = gfx->font_shader_gray.attribs->location;
                glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0);
                glUniform3f(gfx->font_shader_gray.uniforms[1].location, 0.0f, 0.0f, 0.0f);

#ifndef GFX_GLES
                glUniform4f(gfx->font_shader_gray.uniforms[2].location, 1.0f, 1.0f, 1.0f, 1.0f);
#endif
            } break;
            default:
                ASSERT_UNREACHABLE;
        }

        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
        ARRAY_BUFFER_ORPHAN(gfx->flex_vbo.size);
    }

    for (size_t i = 0; i < vt->unicode_input.buffer.size; ++i) {
        ++col;
        glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
        Rune rune = (Rune){
            .code    = vt->unicode_input.buffer.buf[i],
            .combine = { 0 },
            .style   = VT_RUNE_NORMAL,
        };

        GlyphAtlasEntry* entry = GlyphAtlas_get(gfx, &gfx->glyph_atlas, &rune);
        float            h     = (float)entry->height * gfx->sy;
        float            w     = (float)entry->width * gfx->sx;
        float            t     = entry->top * gfx->sy;
        float            l     = entry->left * gfx->sx;
        float            x3    = -1.0f + (float)col * gfx->glyph_width_pixels * gfx->sx + l +
                   gfx->pen_begin_pixels_x * gfx->sx;
        float y3 = 1.0f - (float)(row)*gfx->line_height_pixels * gfx->sy -
                   gfx->pen_begin_pixels_y * gfx->sy + t;

#ifdef GFX_GLES
        float buf[] = {
            x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
            x3 + w, y3,     entry->tex_coords[2], entry->tex_coords[1],
            x3 + w, y3 - h, entry->tex_coords[2], entry->tex_coords[3],

            x3,     y3 - h, entry->tex_coords[0], entry->tex_coords[3],
            x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
            x3 + w, y3 - h, entry->tex_coords[2], entry->tex_coords[3],
        };
#else
        float buf[] = {
            x3,     y3,     entry->tex_coords[0], entry->tex_coords[1],
            x3 + w, y3,     entry->tex_coords[2], entry->tex_coords[1],
            x3 + w, y3 - h, entry->tex_coords[2], entry->tex_coords[3],
            x3,     y3 - h, entry->tex_coords[0], entry->tex_coords[3],
        };
#endif

        GlyphAtlasPage* page = &gfx->glyph_atlas.pages.buf[entry->page_id];
        glBindTexture(GL_TEXTURE_2D, page->texture_id);
        glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
        size_t newsize = sizeof(buf);
        ARRAY_BUFFER_SUB_OR_SWAP(buf, gfx->flex_vbo.size, newsize);

        switch (page->texture_format) {
            case TEX_FMT_RGB: {
                glUseProgram(gfx->font_shader.id);
                GLuint loc = gfx->font_shader.attribs->location;
                glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0);
                glUniform3f(gfx->font_shader.uniforms[1].location, 0.0f, 0.0f, 0.0f);

#ifndef GFX_GLES
                glUniform4f(gfx->font_shader.uniforms[2].location, 1.0f, 1.0f, 1.0f, 1.0f);
#endif
            } break;
            case TEX_FMT_MONO: {
                glUseProgram(gfx->font_shader_gray.id);
                GLuint loc = gfx->font_shader_gray.attribs->location;
                glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE, 0, 0);
                glUniform3f(gfx->font_shader_gray.uniforms[1].location, 0.0f, 0.0f, 0.0f);

#ifndef GFX_GLES
                glUniform4f(gfx->font_shader_gray.uniforms[2].location, 1.0f, 1.0f, 1.0f, 1.0f);
#endif
            } break;
            default:
                ASSERT_UNREACHABLE;
        }

        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
        ARRAY_BUFFER_ORPHAN(gfx->flex_vbo.size);
    }
    glDisable(GL_SCISSOR_TEST);
}

static void GfxOpenGL2_draw_scrollbar(GfxOpenGL2* self, const Scrollbar* scrollbar)
{
    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Shader_use(&self->solid_fill_shader);
    float alpha = scrollbar->dragging ? 0.8f : scrollbar->opacity * 0.5f;
    glUniform4f(self->solid_fill_shader.uniforms[0].location, 1.0f, 1.0f, 1.0f, alpha);
    glViewport(0, 0, self->win_w, self->win_h);
    glBindTexture(GL_TEXTURE_2D, 0);

    float length = scrollbar->length;
    float begin  = scrollbar->top;
    float width  = self->sx * scrollbar->width;
    float slide  = (1.0f - scrollbar->opacity) * scrollbar->width * self->sx;

#ifdef GFX_GLES
    float vertex_data[] = {
        1.0f - width + slide,
        1.0f - begin,
        1.0f,
        1.0f - begin,
        1.0f,
        1.0f - length - begin,
        1.0f - width + slide,
        1.0f - length - begin,
        1.0f - width + slide,
        1.0f - begin,
        1.0f,
        1.0f - length - begin,
    };
#else
    float vertex_data[] = {
        1.0f - width + slide,
        1.0f - begin,
        1.0f,
        1.0f - begin,
        1.0f,
        1.0f - length - begin,
        1.0f - width + slide,
        1.0f - length - begin,
    };
#endif

    glBindBuffer(GL_ARRAY_BUFFER, self->flex_vbo.vbo);
    ARRAY_BUFFER_SUB_OR_SWAP(vertex_data, self->flex_vbo.size, (sizeof vertex_data));
    glVertexAttribPointer(self->solid_fill_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
    ARRAY_BUFFER_ORPHAN(self->flex_vbo.size);
}

static void GfxOpenGL2_draw_hovered_link(GfxOpenGL2* self, const Vt* vt, const Ui* ui)
{
    Vector_clear_vertex_t(&self->vec_vertex_buffer);

    if (ui->hovered_link.start_line_idx == ui->hovered_link.end_line_idx) {
        size_t yidx = (ui->hovered_link.start_line_idx + 1) - Vt_visual_top_line(vt);

        float x = -1.0f + (ui->pixel_offset_x +
                           self->glyph_width_pixels * ui->hovered_link.start_cell_idx) *
                            self->sx;
        float y = 1.0f - (ui->pixel_offset_y + self->line_height_pixels * yidx - 1) * self->sy;

        Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
        x = -1.0f +
            (ui->pixel_offset_x + self->glyph_width_pixels * (ui->hovered_link.end_cell_idx + 1)) *
              self->sx;
        Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
    } else {
        size_t yidx = (ui->hovered_link.start_line_idx + 1) - Vt_visual_top_line(vt);
        float  x    = -1.0f + (ui->pixel_offset_x +
                           self->glyph_width_pixels * ui->hovered_link.start_cell_idx) *
                            self->sx;
        float y = 1.0f - (ui->pixel_offset_y + self->line_height_pixels * yidx - 1) * self->sy;
        Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
        x = -1.0f + (ui->pixel_offset_x + self->glyph_width_pixels * Vt_col(vt)) * self->sx;
        Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });

        for (size_t row = ui->hovered_link.start_line_idx + 1; row < ui->hovered_link.end_line_idx;
             ++row) {
            yidx = (row + 1) - Vt_visual_top_line(vt);
            y    = 1.0f - (ui->pixel_offset_y + self->line_height_pixels * yidx - 1) * self->sy;
            x    = -1.0f + ui->pixel_offset_x * self->sx;
            Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
            x =
              -1.0f + (ui->pixel_offset_x + self->glyph_width_pixels * (Vt_col(vt) - 1)) * self->sx;
            Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
        }
        yidx = (ui->hovered_link.end_line_idx + 1) - Vt_visual_top_line(vt);
        y    = 1.0f - (ui->pixel_offset_y + self->line_height_pixels * yidx - 1) * self->sy;
        x    = -1.0f + ui->pixel_offset_x * self->sx;
        Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
        x = -1.0f +
            (ui->pixel_offset_x + self->glyph_width_pixels * (ui->hovered_link.end_cell_idx + 1)) *
              self->sx;
        Vector_push_vertex_t(&self->vec_vertex_buffer, (vertex_t){ x, y });
    }

    glViewport(0, 0, self->win_w, self->win_h);
    glBindTexture(GL_TEXTURE_2D, 0);
    Shader_use(&self->line_shader);
    glBindBuffer(GL_ARRAY_BUFFER, self->flex_vbo.vbo);
    glVertexAttribPointer(self->line_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform3f(self->line_shader.uniforms[1].location,
                ColorRGB_get_float(vt->colors.fg, 0),
                ColorRGB_get_float(vt->colors.fg, 1),
                ColorRGB_get_float(vt->colors.fg, 2));

    size_t new_size = sizeof(vertex_t) * self->vec_vertex_buffer.size;
    ARRAY_BUFFER_SUB_OR_SWAP(self->vec_vertex_buffer.buf, self->flex_vbo.size, new_size);

    glDrawArrays(GL_LINES, 0, self->vec_vertex_buffer.size);
    ARRAY_BUFFER_ORPHAN(self->flex_vbo.size);
}

static void GfxOpenGL2_draw_overlays(GfxOpenGL2* self, const Vt* vt, const Ui* ui)
{
    if (vt->unicode_input.active) {
        GfxOpenGL2_draw_unicode_input(self, vt);
    } else {
        GfxOpenGL2_draw_cursor(self, vt, ui);
    }

    if (ui->scrollbar.visible) {
        GfxOpenGL2_draw_scrollbar(self, &ui->scrollbar);
    }

    if (ui->hovered_link.active) {
        GfxOpenGL2_draw_hovered_link(self, vt, ui);
    }
}

static void GfxOpenGL2_draw_flash(GfxOpenGL2* self, double fraction)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, 0);
    Shader_use(&self->solid_fill_shader);
    glUniform4f(self->solid_fill_shader.uniforms[0].location,
                ColorRGBA_get_float(settings.bell_flash, 0),
                ColorRGBA_get_float(settings.bell_flash, 1),
                ColorRGBA_get_float(settings.bell_flash, 2),
                (double)ColorRGBA_get_float(settings.bell_flash, 3) * fraction);
    glBindBuffer(GL_ARRAY_BUFFER, self->full_framebuffer_quad_vbo);
    glVertexAttribPointer(self->solid_fill_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
}

static void GfxOpenGL2_draw_tint(GfxOpenGL2* self)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
    glBindTexture(GL_TEXTURE_2D, 0);
    Shader_use(&self->solid_fill_shader);
    glUniform4f(self->solid_fill_shader.uniforms[0].location,
                ColorRGBA_get_float(settings.dim_tint, 0),
                ColorRGBA_get_float(settings.dim_tint, 1),
                ColorRGBA_get_float(settings.dim_tint, 2),
                ColorRGBA_get_float(settings.dim_tint, 3));
    glBindBuffer(GL_ARRAY_BUFFER, self->full_framebuffer_quad_vbo);
    glVertexAttribPointer(self->solid_fill_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
}

static void GfxOpenGL2_load_image(GfxOpenGL2* self, VtImageSurface* surface)
{
    if (surface->state != VT_IMAGE_SURFACE_READY ||
        surface->proxy.data[IMG_PROXY_INDEX_TEXTURE_ID]) {
        return;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 surface->bytes_per_pixel == 3 ? GL_RGB : GL_RGBA,
                 surface->width,
                 surface->height,
                 0,
                 surface->bytes_per_pixel == 3 ? GL_RGB : GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 surface->fragments.buf);
    glGenerateMipmap(GL_TEXTURE_2D);
    surface->proxy.data[IMG_PROXY_INDEX_TEXTURE_ID] = tex;
}

static void GfxOpenGL2_load_image_view(GfxOpenGL2* self, VtImageSurfaceView* view)
{
    if (view->proxy.data[IMG_VIEW_PROXY_INDEX_VBO_ID]) {
        return;
    }

    VtImageSurface* surf = RcPtr_get_VtImageSurface(&view->source_image_surface);

    float w = self->sx * (view->cell_scale_rect.first
                            ? (view->cell_scale_rect.first * self->glyph_width_pixels)
                            : OR(view->sample_dims_px.first, surf->width));

    float h = self->sy * (view->cell_scale_rect.second
                            ? (view->cell_scale_rect.second * self->line_height_pixels)
                            : OR(view->sample_dims_px.second, surf->height));

    float sample_x = (float)view->anchor_offset_px.first / surf->width;
    float sample_y = (float)view->anchor_offset_px.second / surf->height;
    float sample_w =
      view->sample_dims_px.first ? ((float)view->sample_dims_px.first / surf->width) : 1.0f;
    float sample_h =
      view->sample_dims_px.second ? ((float)view->sample_dims_px.second / surf->height) : 1.0f;

    /* set the origin points to the top left corner of framebuffer and image */
#ifdef GFX_GLES
    float vertex_data[][4] = {
        { -1.0f, 1.0f - h, sample_x, sample_y + sample_h },
        { -1.0f + w, 1.0f - h, sample_x + sample_w, sample_y + sample_h },
        { -1.0f + w, 1, sample_x + sample_w, sample_y },

        { -1.0f, 1, sample_x, sample_y },
        { -1.0f + w, 1, sample_x + sample_w, sample_y },
        { -1.0f, 1.0f - h, sample_x, sample_y + sample_h },
    };
#else
    float vertex_data[][4] = {
        { -1.0f, 1.0f - h, sample_x, sample_y + sample_h },
        { -1.0f + w, 1.0f - h, sample_x + sample_w, sample_y + sample_h },
        { -1.0f + w, 1, sample_x + sample_w, sample_y },
        { -1.0f, 1, sample_x, sample_y },
    };
#endif

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
    view->proxy.data[IMG_VIEW_PROXY_INDEX_VBO_ID] = vbo;
}

static void GfxOpenGL2_draw_image_view(GfxOpenGL2* self, const Vt* vt, VtImageSurfaceView* view)
{
    if (!Vt_ImageSurfaceView_is_visual_visible(vt, view)) {
        return;
    }

    VtImageSurface* surf = RcPtr_get_VtImageSurface(&view->source_image_surface);
    GfxOpenGL2_load_image(self, surf);
    GfxOpenGL2_load_image_view(self, view);

    GLuint vbo = view->proxy.data[IMG_VIEW_PROXY_INDEX_VBO_ID];
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    glEnable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);

    Shader_use(&self->image_shader);
    glBindTexture(GL_TEXTURE_2D, surf->proxy.data[IMG_PROXY_INDEX_TEXTURE_ID]);

    int64_t y_index = view->anchor_global_index - Vt_visual_top_line(vt);

    float offset_x =
      self->sx * (view->anchor_cell_idx * self->glyph_width_pixels + view->anchor_offset_px.first);
    float offset_y =
      -self->sy * (y_index * self->line_height_pixels + view->anchor_offset_px.second);

    glVertexAttribPointer(self->image_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform2f(self->image_shader.uniforms[1].location, offset_x, offset_y);

    glDrawArrays(QUAD_DRAW_MODE, 0, 4);
}

void GfxOpenGL2_load_sixel(GfxOpenGL2* self, Vt* vt, VtSixelSurface* srf)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGB,
                 srf->width,
                 srf->height,
                 0,
                 GL_RGB,
                 GL_UNSIGNED_BYTE,
                 srf->fragments.buf);
    srf->proxy.data[SIXEL_PROXY_INDEX_TEXTURE_ID] = tex;

    float w = self->sx * srf->width;
    float h = self->sy * srf->height;

#ifndef GFX_GLES
    float vertex_data[][4] = {
        { -1.0f, 1.0f - h, 0.0f, 1.0f },
        { -1.0f + w, 1.0f - h, 1.0f, 1.0f },
        { -1.0f + w, 1, 1.0f, 0.0f },
        { -1.0f, 1, 0.0f, 0.0f },
    };
#else
    float vertex_data[][4] = {
        { -1.0f, 1.0f - h, 0.0f, 1.0f }, { -1.0f + w, 1.0f - h, 1.0f, 1.0f },
        { -1.0f + w, 1, 1.0f, 0.0f },    { -1.0f, 1, 0.0f, 0.0f },
        { -1.0f, 1.0f - h, 0.0f, 1.0f }, { -1.0f + w, 1, 1.0f, 0.0f },
    };
#endif

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertex_data), vertex_data, GL_STATIC_DRAW);
    srf->proxy.data[SIXEL_PROXY_INDEX_VBO_ID] = vbo;
}

void GfxOpenGL2_draw_sixel(GfxOpenGL2* self, Vt* vt, VtSixelSurface* srf)
{
    if (unlikely(!srf->proxy.data[SIXEL_PROXY_INDEX_TEXTURE_ID])) {
        GfxOpenGL2_load_sixel(self, vt, srf);
    }

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    Shader_use(&self->image_shader);
    glBindTexture(GL_TEXTURE_2D, srf->proxy.data[SIXEL_PROXY_INDEX_TEXTURE_ID]);

    int64_t y_index  = srf->anchor_global_index - Vt_visual_top_line(vt);
    float   offset_x = self->sx * (srf->anchor_cell_idx * self->glyph_width_pixels);
    float   offset_y = -self->sy * (y_index * self->line_height_pixels);

    glBindBuffer(GL_ARRAY_BUFFER, srf->proxy.data[SIXEL_PROXY_INDEX_VBO_ID]);
    glVertexAttribPointer(self->image_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform2f(self->image_shader.uniforms[1].location, offset_x, offset_y);

    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
}

void GfxOpenGL2_draw_sixels(GfxOpenGL2* self, Vt* vt)
{
    for (RcPtr_VtSixelSurface* i = NULL;
         (i = Vector_iter_RcPtr_VtSixelSurface(&vt->scrolled_sixels, i));) {
        VtSixelSurface* ptr = RcPtr_get_VtSixelSurface(i);

        if (!ptr) {
            continue;
        }

        self->frame_overlay_damage->overlay_state = true;

        uint32_t six_ycells = Vt_pixels_to_cells(vt, 0, ptr->height).second + 1;
        if (ptr->anchor_global_index < Vt_visual_bottom_line(vt) &&
            ptr->anchor_global_index + six_ycells > Vt_visual_top_line(vt)) {
            GfxOpenGL2_draw_sixel(self, vt, ptr);
        }
    }
}

void GfxOpenGL2_draw_images(GfxOpenGL2* self, const Vt* vt, bool up_to_zero_z)
{
    for (VtLine* l = NULL; (l = Vector_iter_VtLine((Vector_VtLine*)&vt->lines, l));) {
        if (l->graphic_attachments && l->graphic_attachments->images) {
            for (RcPtr_VtImageSurfaceView* i = NULL;
                 (i = Vector_iter_RcPtr_VtImageSurfaceView(l->graphic_attachments->images, i));) {

                self->frame_overlay_damage->overlay_state = true;

                VtImageSurfaceView* view = RcPtr_get_VtImageSurfaceView(i);
                VtImageSurface*     surf = RcPtr_get_VtImageSurface(&view->source_image_surface);

                if (surf->state == VT_IMAGE_SURFACE_READY &&
                    ((view->z_layer >= 0 && !up_to_zero_z) ||
                     (view->z_layer < 0 && up_to_zero_z))) {
                    GfxOpenGL2_draw_image_view(self, vt, view);
                }
            }
        }
    }
}

static window_partial_swap_request_t* GfxOpenGL2_merge_into_modified_rect(GfxOpenGL2* self,
                                                                          rect_t      rect,
                                                                          uint8_t     idx)
{
    rect_t* tgt  = &self->modified_region.regions[idx];
    int32_t xmin = MIN(tgt->x, rect.x);
    int32_t xmax = MAX(tgt->x + tgt->w, rect.x + rect.w);
    int32_t ymin = MIN(tgt->y, rect.y);
    int32_t ymax = MAX(tgt->y + tgt->h, rect.y + rect.h);
    tgt->x       = xmin;
    tgt->w       = xmax - xmin;
    tgt->y       = ymin;
    tgt->h       = ymax - ymin;
    return &self->modified_region;
}

static inline window_partial_swap_request_t* GfxOpenGL2_try_push_modified_rect(GfxOpenGL2* self,
                                                                               rect_t      rect)
{
    if (self->modified_region.count >= WINDOW_MAX_SWAP_REGION_COUNT)
        return NULL;

    self->modified_region.regions[self->modified_region.count++] = rect;
    return &self->modified_region;
}

static inline window_partial_swap_request_t* GfxOpenGL2_merge_or_push_modified_rect(
  GfxOpenGL2* self,
  rect_t      rect)
{
    for (int i = 0; i < self->modified_region.count; ++i) {
        if (rect_intersects(&self->modified_region.regions[i], &rect)) {
            return GfxOpenGL2_merge_into_modified_rect(self, rect, i);
        }
    }

    return GfxOpenGL2_try_push_modified_rect(self, rect);
}

static window_partial_swap_request_t* GfxOpenGL2_merge_into_last_modified_rect(GfxOpenGL2* self,
                                                                               rect_t      rect)
{
    return GfxOpenGL2_merge_into_modified_rect(self, rect, self->modified_region.count - 1);
}

static rect_t GfxOpenGL2_translate_coords(GfxOpenGL2* self,
                                          int32_t     x,
                                          int32_t     y,
                                          int32_t     w,
                                          int32_t     h)
{
    return (rect_t){
        .x = x,
        .y = self->win_h - y - h,
        .w = w,
        .h = h,
    };
}

static bool GfxOpenGL2_get_accumulated_line_damaged(GfxOpenGL2* self,
                                                    uint16_t    line_index,
                                                    uint8_t     age)
{
    if (age < 2) {
        return true;
    }

    bool     rv               = false;
    bool     cursor_drawn_now = self->frame_overlay_damage->cursor_drawn;
    uint32_t cursor_now_x     = self->frame_overlay_damage->curosr_position_x;

    for (int i = 0;
         i < age && !rv && self->line_damage.n_lines > line_index + self->line_damage.n_lines * i;
         ++i) {
        rv |= (self->line_damage.damage_history[line_index + self->line_damage.n_lines * i] ||
               (self->frame_overlay_damage[i].line_index == line_index &&
                self->frame_overlay_damage[i].cursor_drawn != cursor_drawn_now &&
                self->frame_overlay_damage[i].curosr_position_x != cursor_now_x) ||
               self->frame_overlay_damage[i].overlay_state);
    }
    return rv;
}

static window_partial_swap_request_t* GfxOpenGL2_try_push_accumulated_cursor_damage(
  GfxOpenGL2* self,
  uint8_t     age)
{
    window_partial_swap_request_t* rv = NULL;
    for (int i = 0; i < age; ++i) {
        int32_t x = self->frame_overlay_damage[i].curosr_position_x;
        int32_t y = self->frame_overlay_damage[i].curosr_position_y;
        int32_t w = self->glyph_width_pixels;
        int32_t h = self->line_height_pixels;

        rv = GfxOpenGL2_merge_or_push_modified_rect(self,
                                                    GfxOpenGL2_translate_coords(self, x, y, w, h));
        if (!rv)
            break;
    }
    return rv;
}

static bool GfxOpenGL2_get_accumulated_overlay_damaged(GfxOpenGL2* self, uint8_t age)
{
    if (age > MAX_TRACKED_FRAME_DAMAGE || age < 2) {
        return true;
    }

    bool rv = false;
    for (int i = 0; i < age && !rv; ++i) {
        rv |= self->frame_overlay_damage[i].overlay_state;
    }
    return rv;
}

static bool GfxOpenGL2_is_framebuffer_dirty(Gfx* self, uint8_t buffer_age)
{
    GfxOpenGL2* gfx = gfxOpenGL2(self);
    return GfxOpenGL2_get_accumulated_overlay_damaged(gfx, buffer_age);
}

/* check if lines switching positions should generate fb damage */
static window_partial_swap_request_t* GfxOpenGL2_process_line_position_change_damage(
  GfxOpenGL2*                    self,
  window_partial_swap_request_t* swap_request,
  VtLine*                        line,
  size_t                         visual_index,
  uint8_t                        buffer_age,
  Ui*                            ui)
{
    bool repainted = self->line_damage.damage_history[visual_index];

    if (swap_request) {
        if (!repainted) /* just scrolling */ {
            uint32_t n_lines = self->line_damage.n_lines;
            uint32_t ix      = visual_index + 1 * n_lines + (buffer_age * n_lines);
            uint16_t len     = MAX(line->data.size, self->line_damage.line_length[ix]);

            if (swap_request && len > 0) {
                int32_t x = self->pixel_offset_x;
                int32_t y = self->pixel_offset_y + self->line_height_pixels * visual_index;
                int32_t w = self->glyph_width_pixels * len;
                int32_t h = self->line_height_pixels;

                rect_t dam_rect = GfxOpenGL2_translate_coords(self, x, y, w, h);
                swap_request    = GfxOpenGL2_merge_or_push_modified_rect(self, dam_rect);
            }
        } else /* scrolling, but replaced with shorter content */ {
            uint32_t n_lines = self->line_damage.n_lines;
            uint32_t ix      = visual_index + 1 * n_lines + (buffer_age * n_lines);
            uint16_t new_len = line->data.size;
            uint16_t old_len = self->line_damage.line_length[ix];

            if (swap_request && old_len > new_len) {
                int32_t x = self->pixel_offset_x;
                int32_t y = self->pixel_offset_y + self->line_height_pixels * visual_index;
                int32_t w = self->glyph_width_pixels * old_len;
                int32_t h = self->line_height_pixels;

                rect_t dam_rect = GfxOpenGL2_translate_coords(self, x, y, w, h);
                swap_request    = GfxOpenGL2_merge_or_push_modified_rect(self, dam_rect);
            }
        }
    }

    return swap_request;
}

/* GIMP RGBA C-Source image dump (close_titlebar_button_icon.c) */
static const struct
{
    unsigned int  width;
    unsigned int  height;
    unsigned int  bytes_per_pixel; /* 2:RGB16, 3:RGB, 4:RGBA */
    unsigned char pixel_data[8 * 8 * 4 + 1];
} close_titlebar_button_image = {
    8,
    8,
    4,
    "\377\377\377\377\377\377\377\303\377\377\377\025\000\000\000\000\000\000\000\000\377\377\377"
    "\023\377\377\377\277\377\377\377\377\377\377\377\325\377\377\377\377\377\377"
    "\377\322\377\377\377\025\377\377\377\025\377\377\377\317\377\377\377\377\377"
    "\377\377\327\377\377\377\033\377\377\377\333\377\377\377\377\377\377\377\322"
    "\377\377\377\320\377\377\377\377\377\377\377\337\377\377\377\040\000\000\000\000\377"
    "\377\377\033\377\377\377\333\377\377\377\377\377\377\377\377\377\377\377\341"
    "\377\377\377\"\000\000\000\000\000\000\000\000\377\377\377\026\377\377\377\322\377\377\377"
    "\377\377\377\377\377\377\377\377\331\377\377\377\032\000\000\000\000\377\377\377\031"
    "\377\377\377\325\377\377\377\377\377\377\377\327\377\377\377\327\377\377"
    "\377\377\377\377\377\327\377\377\377\031\377\377\377\316\377\377\377\377\377"
    "\377\377\327\377\377\377\031\377\377\377\031\377\377\377\327\377\377\377\377"
    "\377\377\377\317\377\377\377\377\377\377\377\317\377\377\377\031\000\000\000\000\000"
    "\000\000\000\377\377\377\031\377\377\377\317\377\377\377\377",
};

static void GfxOpenGL2_create_close_button_img(Gfx* self)
{
    GfxOpenGL2* gfx = gfxOpenGL2(self);

    Texture_destroy(&gfx->csd_close_button_texture);

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, close_titlebar_button_image.bytes_per_pixel);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 close_titlebar_button_image.width,
                 close_titlebar_button_image.height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 close_titlebar_button_image.pixel_data);

    gfx->csd_close_button_texture = (Texture){ .id     = tex,
                                               .format = TEX_FMT_RGBA,
                                               .w      = close_titlebar_button_image.width,
                                               .h      = close_titlebar_button_image.height };
}

static window_partial_swap_request_t*
GfxOpenGL2_maybe_draw_titlebar(Gfx* self, Ui* ui, window_partial_swap_request_t* swap_request)
{
    GfxOpenGL2* gfx = gfxOpenGL2(self);

    if (!Ui_csd_titlebar_visible(ui)) {
        return swap_request;
    }

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    static const float tb_clr_if[4]      = { 0.188f, 0.188f, 0.188f, 1.0f };
    static const float tb_clr_of[4]      = { 0.141f, 0.141f, 0.141f, 1.0f };
    static const float btn_clr_if[4]     = { 0.267f, 0.267f, 0.267f, 1.0f };
    static const float btn_clr_of[4]     = { 0.184f, 0.184f, 0.184f, 1.0f };
    static const float btn_clr_hi[4]     = { 0.310f, 0.310f, 0.310f, 1.0f };
    static const float btn_clr_sym_if[4] = { 0.996f, 0.996f, 0.996f, 1.0f };
    static const float btn_clr_sym_of[4] = { 0.569f, 0.569f, 0.569f, 1.0f };
    static const float tb_clr_bdr[4]     = { 0.243f, 0.243f, 0.243f, 1.0f };

    const float* tb_clr      = ui->window_in_focus ? tb_clr_if : tb_clr_of;
    const float* btn_clr     = ui->window_in_focus ? btn_clr_if : btn_clr_of;
    const float* btn_clr_sym = ui->window_in_focus ? btn_clr_sym_if : btn_clr_sym_of;

    Shader_use(&gfx->solid_fill_shader);

    glUniform4f(gfx->solid_fill_shader.uniforms[0].location,
                tb_clr_bdr[0],
                tb_clr_bdr[1],
                tb_clr_bdr[2],
                tb_clr_bdr[3]);
    glViewport(0, gfx->win_h - UI_CSD_TITLEBAR_HEIGHT_PX, gfx->win_w, UI_CSD_TITLEBAR_HEIGHT_PX);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->full_framebuffer_quad_vbo);
    glVertexAttribPointer(gfx->solid_fill_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);

    glUniform4f(gfx->solid_fill_shader.uniforms[0].location,
                tb_clr[1],
                tb_clr[1],
                tb_clr[2],
                tb_clr[3]);
    glViewport(1,
               gfx->win_h - UI_CSD_TITLEBAR_HEIGHT_PX + 1,
               gfx->win_w - 2,
               UI_CSD_TITLEBAR_HEIGHT_PX - 2);
    glVertexAttribPointer(gfx->solid_fill_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);

    if (ui->csd.mode == UI_CSD_MODE_FLOATING) {
        if (unlikely(!gfx->circle_shader.id)) {
            gfx->circle_shader =
              Shader_new(circle_vs_src, circle_fs_src, "pos", "clr", "bclr", "cir", NULL);
        }
        Shader_use(&gfx->circle_shader);
        glViewport(0,
                   gfx->win_h - UI_CSD_TITLEBAR_RADIUS_PX,
                   UI_CSD_TITLEBAR_RADIUS_PX,
                   UI_CSD_TITLEBAR_RADIUS_PX);
        glUniform4f(gfx->circle_shader.uniforms[0].location,
                    tb_clr_bdr[0],
                    tb_clr_bdr[1],
                    tb_clr_bdr[2],
                    tb_clr_bdr[3]);
        glUniform4f(gfx->circle_shader.uniforms[1].location, 0.0f, 0.0f, 0.0f, 0.0f);
        glUniform4f(gfx->circle_shader.uniforms[2].location,
                    1.0f,
                    -1.0f,
                    2.0f,
                    1.2f / UI_CSD_TITLEBAR_RADIUS_PX);
        glVertexAttribPointer(gfx->circle_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
        glViewport(gfx->win_w - UI_CSD_TITLEBAR_RADIUS_PX,
                   gfx->win_h - UI_CSD_TITLEBAR_RADIUS_PX,
                   UI_CSD_TITLEBAR_RADIUS_PX,
                   UI_CSD_TITLEBAR_RADIUS_PX);
        glUniform4f(gfx->circle_shader.uniforms[2].location,
                    -1.0f,
                    -1.0f,
                    2.0f,
                    1.2f / UI_CSD_TITLEBAR_RADIUS_PX);
        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
    }

    if (ui->csd.mode == UI_CSD_MODE_FLOATING) {
        if (unlikely(!gfx->circle_shader.id)) {
            gfx->circle_shader =
              Shader_new(circle_vs_src, circle_fs_src, "pos", "clr", "bclr", "cir", NULL);
        }
        Shader_use(&gfx->circle_shader);
        glViewport(1,
                   gfx->win_h - UI_CSD_TITLEBAR_RADIUS_PX,
                   UI_CSD_TITLEBAR_RADIUS_PX - 1,
                   UI_CSD_TITLEBAR_RADIUS_PX - 1);
        glUniform4f(gfx->circle_shader.uniforms[0].location,
                    tb_clr[0],
                    tb_clr[1],
                    tb_clr[2],
                    tb_clr[3]);
        glUniform4f(gfx->circle_shader.uniforms[1].location,
                    tb_clr_bdr[0],
                    tb_clr_bdr[1],
                    tb_clr_bdr[2],
                    0.0f);
        glUniform4f(gfx->circle_shader.uniforms[2].location,
                    1.0f,
                    -1.0f,
                    2.0f,
                    1.25f / (UI_CSD_TITLEBAR_RADIUS_PX - 1));
        glVertexAttribPointer(gfx->circle_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_SRC_ALPHA, GL_DST_ALPHA);
        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
        glViewport(gfx->win_w - UI_CSD_TITLEBAR_RADIUS_PX,
                   gfx->win_h - UI_CSD_TITLEBAR_RADIUS_PX,
                   UI_CSD_TITLEBAR_RADIUS_PX - 1,
                   UI_CSD_TITLEBAR_RADIUS_PX - 1);
        glUniform4f(gfx->circle_shader.uniforms[2].location,
                    -1.0f,
                    -1.0f,
                    2.0f,
                    1.25f / (UI_CSD_TITLEBAR_RADIUS_PX - 1));
        glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
        glDisable(GL_BLEND);
    } else /* no rounded corners */ {
        if (unlikely(!gfx->circle_shader.id)) {
            gfx->circle_shader =
              Shader_new(circle_vs_src, circle_fs_src, "pos", "clr", "bclr", "cir", NULL);
        }
        Shader_use(&gfx->circle_shader);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBuffer(GL_ARRAY_BUFFER, gfx->full_framebuffer_quad_vbo);
        glVertexAttribPointer(gfx->circle_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    }

    if (ui->csd.buttons.size) {
        uint32_t vp_w = UI_CSD_TITLEBAR_RADIUS_PX * 2 + 2;
        uint32_t vp_h = UI_CSD_TITLEBAR_RADIUS_PX * 2 + 2;

        uint32_t xoffset_px = gfx->win_w - UI_CSD_TITLEBAR_HEIGHT_PX / 2 - vp_w / 2;
        uint32_t yoffset_px = gfx->win_h - UI_CSD_TITLEBAR_HEIGHT_PX / 2 - vp_h / 2;

        glUniform4f(gfx->circle_shader.uniforms[0].location,
                    btn_clr[0],
                    btn_clr[1],
                    btn_clr[2],
                    btn_clr[3]);
        glUniform4f(gfx->circle_shader.uniforms[1].location,
                    tb_clr[0],
                    tb_clr[1],
                    tb_clr[2],
                    tb_clr[3]);
        glUniform4f(gfx->circle_shader.uniforms[2].location, 0.0f, 0.0f, 1.0f, 1.5f / vp_w);

        for (ui_csd_titlebar_button_info_t* i = NULL;
             (i = Vector_iter_ui_csd_titlebar_button_info_t(&ui->csd.buttons, i));) {
            float this_btn_clr[4];
            for (int j = 0; j < 4; j++) {
                this_btn_clr[j] = btn_clr[j] * (1.0 - i->highlight_fraction) +
                                  btn_clr_hi[j] * i->highlight_fraction;
            }

            glDisable(GL_BLEND);
            Shader_use(&gfx->circle_shader);
            glUniform4f(gfx->circle_shader.uniforms[0].location,
                        this_btn_clr[0],
                        this_btn_clr[1],
                        this_btn_clr[2],
                        this_btn_clr[3]);
            glViewport(xoffset_px, yoffset_px, vp_w, vp_h);
            glBindBuffer(GL_ARRAY_BUFFER, gfx->full_framebuffer_quad_vbo);
            glVertexAttribPointer(gfx->circle_shader.attribs->location,
                                  2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  0,
                                  0);
            glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);

            Shader_use(&gfx->solid_fill_shader);
            glVertexAttribPointer(gfx->solid_fill_shader.attribs->location,
                                  2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  0,
                                  0);
            glUniform4f(gfx->solid_fill_shader.uniforms[0].location,
                        btn_clr_sym[0],
                        btn_clr_sym[1],
                        btn_clr_sym[2],
                        btn_clr_sym[3]);
            switch (i->type) {
                case UI_CSD_TITLEBAR_BUTTON_CLOSE: {
                    glEnable(GL_BLEND);

                    glViewport(xoffset_px + (vp_w - gfx->csd_close_button_texture.w) / 2,
                               yoffset_px + (vp_h - gfx->csd_close_button_texture.h) / 2,
                               gfx->csd_close_button_texture.w,
                               gfx->csd_close_button_texture.h);

                    if (unlikely(!gfx->csd_close_button_texture.id)) {
                        GfxOpenGL2_create_close_button_img(self);
                    }

                    Shader_use(&gfx->image_tint_shader);
                    glBindTexture(GL_TEXTURE_2D, gfx->csd_close_button_texture.id);
                    glUniform3f(gfx->image_tint_shader.uniforms[1].location,
                                btn_clr_sym[0],
                                btn_clr_sym[1],
                                btn_clr_sym[2]);

                    if (!gfx->csd_close_button_vbo) {
                        glGenBuffers(1, &gfx->csd_close_button_vbo);

                        Vector_clear_vertex_t(&gfx->vec_vertex_buffer);
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ -1, -1 });
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 0, 0 });

                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ -1, 1 });
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 0, 1 });

                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 1, 1 });
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 1, 1 });

#ifdef GFX_GLES
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 1, 1 });
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 1, 1 });

                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ -1, -1 });
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 0, 0 });
#endif

                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 1, -1 });
                        Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ 1, 0 });

                        size_t new_size = sizeof(vertex_t) * gfx->vec_vertex_buffer.size;
                        glBindBuffer(GL_ARRAY_BUFFER, gfx->csd_close_button_vbo);
                        glBufferData(GL_ARRAY_BUFFER,
                                     new_size,
                                     gfx->vec_vertex_buffer.buf,
                                     GL_STATIC_DRAW);
                    } else {
                        glBindBuffer(GL_ARRAY_BUFFER, gfx->csd_close_button_vbo);
                    }

                    glVertexAttribPointer(gfx->font_shader.attribs->location,
                                          4,
                                          GL_FLOAT,
                                          GL_FALSE,
                                          0,
                                          0);
                    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
                } break;
                case UI_CSD_TITLEBAR_BUTTON_MAXIMIZE:
                    glUniform4f(gfx->solid_fill_shader.uniforms[0].location,
                                btn_clr_sym[0],
                                btn_clr_sym[1],
                                btn_clr_sym[2],
                                btn_clr_sym[3]);
                    glViewport(xoffset_px + 7, yoffset_px + 7, 8, 8);
                    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
                    glUniform4f(gfx->solid_fill_shader.uniforms[0].location,
                                this_btn_clr[0],
                                this_btn_clr[1],
                                this_btn_clr[2],
                                this_btn_clr[3]);
                    glViewport(xoffset_px + 9, yoffset_px + 9, 4, 4);
                    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
                    break;
                case UI_CSD_TITLEBAR_BUTTON_MINIMIZE:
                    glUniform4f(gfx->solid_fill_shader.uniforms[0].location,
                                btn_clr_sym[0],
                                btn_clr_sym[1],
                                btn_clr_sym[2],
                                btn_clr_sym[3]);
                    glViewport(xoffset_px + 7, yoffset_px + 7, 8, 2);
                    glDrawArrays(QUAD_DRAW_MODE, 0, QUAD_V_SZ);
                    break;
                default:
                    ASSERT_UNREACHABLE;
            }
            xoffset_px -= 37;
        }
    } /* buttons */

    glViewport(0, 0, gfx->win_w, gfx->win_h);

    rect_t dam_rect = GfxOpenGL2_translate_coords(gfx, 0, 0, gfx->win_w, UI_CSD_TITLEBAR_HEIGHT_PX);
    return swap_request ? GfxOpenGL2_merge_or_push_modified_rect(gfx, dam_rect) : NULL;
}

window_partial_swap_request_t* GfxOpenGL2_draw(Gfx* self, const Vt* vt, Ui* ui, uint8_t buffer_age)
{
    GfxOpenGL2* gfx = gfxOpenGL2(self);

    gfx->modified_region.count            = 0;
    window_partial_swap_request_t* retval = &gfx->modified_region;

    static uint8_t old_age = 0;
    if (buffer_age == 0 || buffer_age != old_age) {
        GfxOpenGL2_external_framebuffer_damage(self);
        old_age = buffer_age;
        retval  = NULL;
    }

    gfx->pixel_offset_x = ui->pixel_offset_x;
    gfx->pixel_offset_y = ui->pixel_offset_y;

    GfxOpenGL2_rotate_damage_record(gfx);

    gfx->frame_overlay_damage->overlay_state =
      Ui_any_overlay_element_visible(ui) || Vt_is_scrolling_visual(vt);

    if (GfxOpenGL2_get_accumulated_overlay_damaged(gfx, buffer_age) ||
        gfx->frame_overlay_damage->overlay_state) {
        retval = NULL;
    }

    VtLine *begin, *end;
    Vt_get_visible_lines(vt, &begin, &end);
    glDisable(GL_SCISSOR_TEST);

#ifndef GFX_GLES
    glDisable(GL_DEPTH_TEST);
#endif

    glViewport(0, 0, gfx->win_w, gfx->win_h);
    glClearColor(ColorRGBA_get_float(vt->colors.bg, 0),
                 ColorRGBA_get_float(vt->colors.bg, 1),
                 ColorRGBA_get_float(vt->colors.bg, 2),
                 ColorRGBA_get_float(vt->colors.bg, 3));

    glClear(GL_COLOR_BUFFER_BIT);

    for (VtLine* i = begin; i < end; ++i) {
        line_render_pass_args_t rp_args = {
            .gl2             = gfx,
            .vt              = vt,
            .vt_line         = i,
            .proxy           = NULL,
            .damage          = NULL,
            .visual_index    = i - begin,
            .is_for_cursor   = false,
            .is_for_blinking = false,
        };

        bool should_repaint = should_create_line_render_pass(&rp_args);
        if (rp_args.visual_index < gfx->line_damage.n_lines) {
            gfx->line_damage.damage_history[rp_args.visual_index] = should_repaint;
        }

        if (should_repaint) {
            gfx->bound_resources  = BOUND_RESOURCES_NONE;
            line_render_pass_t rp = create_line_render_pass(&rp_args);
            line_reder_pass_run_initial_setup(&rp);

            Pair_uint16_t damage = line_reder_pass_run_subpasses(&rp);

            uint16_t dam_len = (damage.second < damage.first) ? (uint16_t)i->data.size
                                                              : (damage.second - damage.first) + 1;

            bool surface_fragment_repaint =
              GfxOpenGL2_get_accumulated_line_damaged(gfx, i - begin, buffer_age);

            bool length_in_limit = dam_len < CELL_DAMAGE_TO_SURF_LIMIT;

            if (retval && surface_fragment_repaint && rp.n_queued_subpasses > 0 &&
                length_in_limit && !unlikely(rp.has_blinking_chars)) {
                int32_t x = gfx->pixel_offset_x + gfx->glyph_width_pixels * damage.first;
                int32_t y = gfx->pixel_offset_y + gfx->line_height_pixels * rp.args.visual_index;
                int32_t w = gfx->glyph_width_pixels * dam_len;
                int32_t h = gfx->line_height_pixels;

                rect_t dam_rect = GfxOpenGL2_translate_coords(gfx, x, y, w, h);

                if (retval) {
                    retval = GfxOpenGL2_merge_or_push_modified_rect(gfx, dam_rect);
                }
            } else {
                retval = NULL;
            }

            if (unlikely(rp.has_blinking_chars)) {
                self->has_blinking_text = true;
                gfx->bound_resources    = BOUND_RESOURCES_NONE;
                rp_args.is_for_blinking = true;
                line_render_pass_t rp_b = create_line_render_pass(&rp_args);
                line_reder_pass_run(&rp_b);
                rp_b.has_blinking_chars = true;
                line_render_pass_finalize(&rp_b);
                rp_args.is_for_blinking = false;
            }

            line_render_pass_finalize(&rp);
        }
    }

    for (VtLine* i = begin; i < end; ++i) {
        ptrdiff_t visual_index = i - begin;
        if (retval) {
            retval = GfxOpenGL2_process_line_position_change_damage(gfx,
                                                                    retval,
                                                                    i,
                                                                    visual_index,
                                                                    buffer_age,
                                                                    ui);
        }

        /* update damage history data */
        if (gfx->line_damage.n_lines > i - begin) {
            gfx->line_damage.proxy_color_component[i - begin] = i->proxy.data[PROXY_INDEX_TEXTURE];
            gfx->line_damage.line_length[i - begin]           = i->data.size;
        }
    }

    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    Pair_uint32_t chars = Gfx_get_char_size(
      self,
      (Pair_uint32_t){ gfx->win_w - settings.padding * 2, gfx->win_h - settings.padding * 2 });

    if (vt->scrolling_visual) {
        glScissor(gfx->pixel_offset_x,
                  gfx->pixel_offset_y - titlebar_height_px(ui),
                  chars.first * gfx->glyph_width_pixels,
                  gfx->win_h);
    } else {
        glScissor(gfx->pixel_offset_x,
                  gfxOpenGL2(self)->win_h - chars.second * gfx->line_height_pixels -
                    gfx->pixel_offset_y,
                  chars.first * gfx->glyph_width_pixels,
                  chars.second * gfx->line_height_pixels);
    }

    GfxOpenGL2_draw_images(gfx, vt, true);
    glBindBuffer(GL_ARRAY_BUFFER, gfx->line_quads_vbo);
    Shader_use(&gfx->image_shader);
    glUniform2f(gfx->image_shader.uniforms[1].location, 0, 0);
    glVertexAttribPointer(gfx->image_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glViewport(gfx->pixel_offset_x, -gfx->pixel_offset_y, gfx->win_w, gfx->win_h);

    for (VtLine* i = begin; i < end; ++i) {
        uint32_t vis_idx = i - begin;
        // TODO: maybe this is up to date and we can get away without drawing?
        GfxOpenGL2_draw_line_quads(gfx, ui, i, vis_idx);
    }

    GfxOpenGL2_draw_images(gfx, vt, false);
    GfxOpenGL2_draw_sixels(gfx, (Vt*)vt);
    GfxOpenGL2_draw_overlays(gfx, vt, ui);

    if (ui->flash_fraction != 0.0) {
        retval = NULL;
        if (Ui_csd_titlebar_visible(ui)) {
            glViewport(0, 0, gfx->win_w, gfx->win_h - UI_CSD_TITLEBAR_HEIGHT_PX);
        } else {
            glViewport(0, 0, gfx->win_w, gfx->win_h);
        }
        GfxOpenGL2_draw_flash(gfx, ui->flash_fraction);
    }

    if (unlikely(ui->draw_out_of_focus_tint && settings.dim_tint.a)) {
        retval = NULL;
        if (Ui_csd_titlebar_visible(ui)) {
            glViewport(0, 0, gfx->win_w, gfx->win_h - UI_CSD_TITLEBAR_HEIGHT_PX);
        } else {
            glViewport(0, 0, gfx->win_w, gfx->win_h);
        }
        GfxOpenGL2_draw_tint(gfx);
    }

    retval = GfxOpenGL2_maybe_draw_titlebar(self, ui, retval);

    if (retval) {
        retval = GfxOpenGL2_try_push_accumulated_cursor_damage(gfx, buffer_age);
    }

    if (unlikely(settings.debug_gfx)) {
        if (retval) {
            for (int8_t i = 0; i < retval->count; ++i) {
                rect_t* r = &retval->regions[i];
                Shader_use(&gfx->solid_fill_shader);
                glBindTexture(GL_TEXTURE_2D, 0);
                float vertex_data[] = {
                    -1.0f + r->x * gfx->sx + gfx->sx,    -1.0 + r->y * gfx->sy + gfx->sy,

                    -1.0f + r->x * gfx->sx + gfx->sx,    -1.0f + (r->y + r->h - 1) * gfx->sy,

                    -1.0f + (r->x + r->w - 1) * gfx->sx, -1.0f + (r->y + r->h - 1) * gfx->sy,

                    -1.0f + (r->x + r->w - 1) * gfx->sx, -1.0 + r->y * gfx->sy + gfx->sy,

                    -1.0f + r->x * gfx->sx + gfx->sx,    -1.0 + r->y * gfx->sy + gfx->sy,

                    -1.0f + (r->x + r->w - 1) * gfx->sx, -1.0f + (r->y + r->h - 1) * gfx->sy,

                };
                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                ARRAY_BUFFER_SUB_OR_SWAP(vertex_data, gfx->flex_vbo.size, (sizeof vertex_data));
                glVertexAttribPointer(gfx->solid_fill_shader.attribs->location,
                                      2,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);
                glUniform4f(gfx->solid_fill_shader.uniforms[0].location, 1.0f, 0.0f, 0.0f, 1.0f);
                glDrawArrays(GL_LINE_STRIP, 0, 6);
            }
            retval = NULL;
        }

        static bool repaint_indicator_visible = true;
        if (repaint_indicator_visible) {
            retval                                   = NULL;
            gfx->frame_overlay_damage->overlay_state = true;
            Shader_use(&gfx->solid_fill_shader);
            glBindTexture(GL_TEXTURE_2D, 0);

            float vertex_data[] = { -1.0f, 1.0f,  -1.0f + gfx->sx * 5.0f,
                                    1.0f,  -1.0f, 1.0f - gfx->sy * 5.0f };

            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
            ARRAY_BUFFER_SUB_OR_SWAP(vertex_data, gfx->flex_vbo.size, (sizeof vertex_data));

            glVertexAttribPointer(gfx->solid_fill_shader.attribs->location,
                                  2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  0,
                                  0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        repaint_indicator_visible = !repaint_indicator_visible;
    }

    return gfx->frame_overlay_damage->overlay_state ? NULL : retval;
}

void GfxOpenGL2_destroy_recycled(GfxOpenGL2* self)
{
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(self->recycled_textures); ++i) {
        if (self->recycled_textures[i].color_tex) {
            DBG_DELTEX
            glDeleteTextures(1, &self->recycled_textures[i].color_tex);

#ifndef GFX_GLES
            glDeleteRenderbuffers(1, &self->recycled_textures[i].depth_rb);
#endif
        }
        self->recycled_textures[i].color_tex = 0;

#ifndef GFX_GLES
        self->recycled_textures[i].depth_rb = 0;
#endif
    }
}

void GfxOpenGL2_push_recycled(GfxOpenGL2* self,
                              GLuint      tex_id

#ifndef GFX_GLES
                              ,
                              GLuint rb_id
#endif
)
{
    for (uint_fast8_t insert_point = 0; insert_point < N_RECYCLED_TEXTURES; ++insert_point) {
        if (!self->recycled_textures[insert_point].color_tex) {
            if (likely(ARRAY_LAST(self->recycled_textures).color_tex)) {
                DBG_DELTEX
                glDeleteTextures(1, &ARRAY_LAST(self->recycled_textures).color_tex);

#ifndef GFX_GLES
                ASSERT(ARRAY_LAST(self->recycled_textures).depth_rb,
                       "deleted texture has depth rb");
                glDeleteRenderbuffers(1, &ARRAY_LAST(self->recycled_textures).depth_rb);
#endif
            }

            void*  src = self->recycled_textures + insert_point;
            void*  dst = self->recycled_textures + insert_point + 1;
            size_t n = sizeof(*self->recycled_textures) * (N_RECYCLED_TEXTURES - insert_point - 1);
            memmove(dst, src, n);
            self->recycled_textures[insert_point].color_tex = tex_id;

#ifndef GFX_GLES
            self->recycled_textures[insert_point].depth_rb = rb_id;
#endif

            return;
        }
    }

    DBG_DELTEX
    glDeleteTextures(1, &tex_id);

#ifndef GFX_GLES
    glDeleteRenderbuffers(1, &rb_id);
#endif
}

Pair_GLuint GfxOpenGL2_pop_recycled(GfxOpenGL2* self)
{
    Pair_GLuint ret = { .first  = self->recycled_textures[0].color_tex,
                        .second = self->recycled_textures[0].depth_rb };

    memmove(self->recycled_textures,
            self->recycled_textures + 1,
            sizeof(*self->recycled_textures) * (N_RECYCLED_TEXTURES - 1));

    ARRAY_LAST(self->recycled_textures).color_tex = 0;
    ARRAY_LAST(self->recycled_textures).depth_rb  = 0;
    return ret;
}

void GfxOpenGL2_destroy_image_proxy(Gfx* self, uint32_t* proxy)
{
    if (proxy[IMG_PROXY_INDEX_TEXTURE_ID]) {
        glDeleteTextures(1, &proxy[IMG_PROXY_INDEX_TEXTURE_ID]);
        proxy[IMG_PROXY_INDEX_TEXTURE_ID] = 0;
    }
}

void GfxOpenGL2_destroy_sixel_proxy(Gfx* self, uint32_t* proxy)
{
    if (proxy[SIXEL_PROXY_INDEX_TEXTURE_ID]) {
        glDeleteTextures(1, &proxy[SIXEL_PROXY_INDEX_TEXTURE_ID]);
        glDeleteBuffers(1, &proxy[SIXEL_PROXY_INDEX_VBO_ID]);
        proxy[SIXEL_PROXY_INDEX_TEXTURE_ID] = 0;
        proxy[SIXEL_PROXY_INDEX_VBO_ID]     = 0;
    }
}

void GfxOpenGL2_destroy_image_view_proxy(Gfx* self, uint32_t* proxy)
{
    if (proxy[IMG_VIEW_PROXY_INDEX_VBO_ID]) {
        glDeleteBuffers(1, proxy);
        proxy[IMG_VIEW_PROXY_INDEX_VBO_ID] = 0;
    }
}

__attribute__((hot)) void GfxOpenGL2_destroy_proxy(Gfx* self, uint32_t* proxy)
{
    if (unlikely(proxy[PROXY_INDEX_TEXTURE])) {
        GfxOpenGL2_push_recycled(gfxOpenGL2(self),
                                 proxy[PROXY_INDEX_TEXTURE]
#ifndef GFX_GLES
                                 ,
                                 proxy[PROXY_INDEX_DEPTHBUFFER]
#endif
        );

        if (unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK])) {
            GfxOpenGL2_push_recycled(gfxOpenGL2(self),
                                     proxy[PROXY_INDEX_TEXTURE_BLINK]
#ifndef GFX_GLES
                                     ,
                                     proxy[PROXY_INDEX_DEPTHBUFFER_BLINK]
#endif
            );
        }
    } else if (likely(proxy[PROXY_INDEX_TEXTURE])) {
        /* delete starting from first */

        int del_num = unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK]) ? 2 : 1;

#ifdef DEBUG
        for (int i = 0; i < del_num; ++i) {
            DBG_DELTEX
        }
#endif

        glDeleteTextures(del_num, (GLuint*)&proxy[PROXY_INDEX_TEXTURE]);

#ifndef GFX_GLES
        ASSERT(proxy[PROXY_INDEX_DEPTHBUFFER], "deleted proxy texture has a renderbuffer");
        if (del_num == 2) {
            ASSERT(proxy[PROXY_INDEX_DEPTHBUFFER_BLINK],
                   "deleted proxy texture has a renderbuffer");
        }
        glDeleteRenderbuffers(del_num, (GLuint*)&proxy[PROXY_INDEX_DEPTHBUFFER]);
#endif

    } else if (unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK])) {
        ASSERT_UNREACHABLE;
        DBG_DELTEX
        glDeleteTextures(1, (GLuint*)&proxy[PROXY_INDEX_TEXTURE_BLINK]);

#ifndef GFX_GLES
        glDeleteRenderbuffers(1, (GLuint*)&proxy[PROXY_INDEX_DEPTHBUFFER_BLINK]);
#endif
    }

    proxy[PROXY_INDEX_TEXTURE]       = 0;
    proxy[PROXY_INDEX_TEXTURE_BLINK] = 0;

#ifndef GFX_GLES
    proxy[PROXY_INDEX_DEPTHBUFFER]       = 0;
    proxy[PROXY_INDEX_DEPTHBUFFER_BLINK] = 0;
#endif
}

void GfxOpenGL2_destroy(Gfx* self)
{
    glUseProgram(0);
    free(gfxOpenGL2(self)->line_damage.damage_history);
    free(gfxOpenGL2(self)->line_damage.proxy_color_component);
    free(gfxOpenGL2(self)->line_damage.line_length);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GfxOpenGL2_destroy_recycled(gfxOpenGL2(self));
    glDeleteTextures(1, &gfxOpenGL2(self)->squiggle_texture.id);
    if (gfxOpenGL2(self)->csd_close_button_texture.id) {
        glDeleteTextures(1, &gfxOpenGL2(self)->csd_close_button_texture.id);
        gfxOpenGL2(self)->csd_close_button_texture.id = 0;
    }
    if (gfxOpenGL2(self)->csd_close_button_vbo) {
        glDeleteBuffers(1, &gfxOpenGL2(self)->csd_close_button_vbo);
        gfxOpenGL2(self)->csd_close_button_vbo = 0;
    }
    glDeleteFramebuffers(1, &gfxOpenGL2(self)->line_framebuffer);
    VBO_destroy(&gfxOpenGL2(self)->flex_vbo);
    glDeleteBuffers(1, &gfxOpenGL2(self)->line_quads_vbo);
    glDeleteBuffers(1, &gfxOpenGL2(self)->full_framebuffer_quad_vbo);
    Shader_destroy(&gfxOpenGL2(self)->solid_fill_shader);
    Shader_destroy(&gfxOpenGL2(self)->font_shader);
    Shader_destroy(&gfxOpenGL2(self)->font_shader_gray);
    Shader_destroy(&gfxOpenGL2(self)->font_shader_blend);
    Shader_destroy(&gfxOpenGL2(self)->line_shader);
    Shader_destroy(&gfxOpenGL2(self)->image_shader);
    Shader_destroy(&gfxOpenGL2(self)->image_tint_shader);
    GlyphAtlas_destroy(&gfxOpenGL2(self)->glyph_atlas);
    Vector_destroy_vertex_t(&(gfxOpenGL2(self)->vec_vertex_buffer));
    Vector_destroy_vertex_t(&(gfxOpenGL2(self)->vec_vertex_buffer2));
    Vector_destroy_Vector_float(&(gfxOpenGL2(self))->float_vec);

#ifdef DEBUG
    INFO("proxy textures created: %zu, destroyed: %zu (total: %zu)\n",
         dbg_line_proxy_textures_created,
         dbg_line_proxy_textures_destroyed,
         (dbg_line_proxy_textures_created - dbg_line_proxy_textures_destroyed))
#endif
}
