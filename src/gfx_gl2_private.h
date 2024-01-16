#pragma once

#define _GNU_SOURCE

#include "gl2_util.h"
#include "colors.h"
#include "ui.h"
#include "vt.h"
#include "gfx_gl2.h"

#include "freetype.h"
#include "fterrors.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "map.h"
#include "util.h"
#include "wcwidth/wcwidth.h"


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


DEF_PAIR(GLuint);
DEF_VECTOR(float, NULL);

typedef struct
{
    float x, y;
} vertex_t;

DEF_VECTOR(Vector_float, Vector_destroy_float);

DEF_VECTOR(vertex_t, NULL);

enum GlyphColor
{
    GLYPH_COLOR_MONO,
    GLYPH_COLOR_LCD,
    GLYPH_COLOR_COLOR,
};

typedef struct
{
    float fade_fraction;
} cursor_color_animation_override_t;

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

#define ATLAS_RENDERABLE_START ' '
#define ATLAS_RENDERABLE_END   CHAR_MAX

typedef struct
{
    GLuint color_tex;
    GLuint depth_rb;
} LineTexture;

#ifdef DEBUG
static size_t dbg_line_proxy_textures_created   = 0;
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
#define DBG_DELTEX  ;
#define DBG_MAKETEX ;
#endif

static void LineTexture_destroy(LineTexture* self)
{
    if (self->color_tex) {
        DBG_DELTEX
        glDeleteTextures(1, &self->color_tex);
        self->color_tex = 0;

#ifndef GFX_GLES
        ASSERT(self->depth_rb, "deleted texture has depth renderbuffer");
        glDeleteRenderbuffers_(1, &self->depth_rb);
        self->depth_rb = 0;
#endif
    }
}

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
    Shader line_shader_alpha;
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
