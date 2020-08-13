/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "gfx_gl21.h"
#include "vt.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <GL/gl.h>

#include "freetype.h"
#include "fterrors.h"
#include "gl.h"
#include "map.h"
#include "shaders.h"
#include "util.h"
#include "wcwidth/wcwidth.h"

#define NUM_BUCKETS 256

#ifndef ATLAS_SIZE_LIMIT
#define ATLAS_SIZE_LIMIT INT32_MAX
#endif

#ifndef FLASH_DURATION_MS
#define FLASH_DURATION_MS 300
#endif

#ifndef DIM_COLOR_BLEND_FACTOR
#define DIM_COLOR_BLEND_FACTOR 0.4f
#endif

#ifndef N_RECYCLED_TEXTURES
#define N_RECYCLED_TEXTURES 5
#endif

#define PROXY_INDEX_TEXTURE       0
#define PROXY_INDEX_TEXTURE_BLINK 1
#define PROXY_INDEX_TEXTURE_SIZE  2

enum GlyphColor
{
    GLYPH_COLOR_MONO,
    GLYPH_COLOR_LCD,
    GLYPH_COLOR_COLOR,
};

typedef struct
{
    uint32_t        code;
    float           left, top;
    enum GlyphColor color;
    Texture         tex;

} GlyphMapEntry;

static void GlyphMapEntry_destroy(GlyphMapEntry* self);

DEF_VECTOR(GlyphMapEntry, GlyphMapEntry_destroy)
DEF_VECTOR(Texture, Texture_destroy)

static inline size_t Rune_hash(const Rune* self)
{
    return self->code;
}

static inline size_t Rune_eq(const Rune* self, const Rune* other)
{
    return !memcmp(self, other, sizeof(Rune));
}

DEF_MAP(Rune, GlyphMapEntry, Rune_hash, Rune_eq, GlyphMapEntry_destroy)

typedef struct
{
    Vector_GlyphMapEntry buckets[NUM_BUCKETS];
} GlyphMap;

struct AtlasCharInfo
{
    float   left, top;
    int32_t rows, width;
    float   tex_coords[4];
};

#define ATLAS_RENDERABLE_START ' '
#define ATLAS_RENDERABLE_END   CHAR_MAX
typedef struct
{
    GLuint   tex;
    uint32_t w, h;

    GLuint vbo;
    GLuint ibo;

    struct AtlasCharInfo char_info[ATLAS_RENDERABLE_END + 1 - ATLAS_RENDERABLE_START];

} Atlas;

static void Atlas_destroy(Atlas* self)
{
    glDeleteTextures(1, &self->tex);
}

typedef struct __attribute__((packed)) _GlyphBufferData
{
    GLfloat data[4][4];
} GlyphBufferData;

typedef struct __attribute__((packed)) _vertex_t
{
    float x, y;
} vertex_t;

DEF_VECTOR(GlyphBufferData, NULL);

DEF_VECTOR(vertex_t, NULL);

typedef struct
{
    GLuint  id;
    int32_t width;
} LineTexture;

static void LineTexture_destroy(LineTexture* self)
{
    if (self->id) {
        glDeleteTextures(1, &self->id);
    }
    self->id = 0;
}

typedef struct
{
    GLint max_tex_res;

    Vector_vertex_t vec_vertex_buffer;
    Vector_vertex_t vec_vertex_buffer2;

    Vector_GlyphBufferData  _vec_glyph_buffer;
    Vector_GlyphBufferData  _vec_glyph_buffer_italic;
    Vector_GlyphBufferData  _vec_glyph_buffer_bold;
    Vector_GlyphBufferData  _vec_glyph_buffer_bold_italic;
    Vector_GlyphBufferData* vec_glyph_buffer;
    Vector_GlyphBufferData* vec_glyph_buffer_italic;
    Vector_GlyphBufferData* vec_glyph_buffer_bold;
    Vector_GlyphBufferData* vec_glyph_buffer_bold_italic;

    VBO flex_vbo;
    VBO flex_vbo_italic;
    VBO flex_vbo_bold;
    VBO flex_vbo_bold_italic;

    /* pen position to begin drawing font */
    float    pen_begin;
    float    pen_begin_pixels;
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

    VBO font_vao;
    VBO bg_vao;
    VBO line_vao;
    VBO line_bg_vao;

    Shader font_shader;
    Shader font_shader_blend;
    Shader font_shader_gray;
    Shader bg_shader;
    Shader line_shader;
    Shader image_shader;
    Shader image_tint_shader;

    ColorRGB               color;
    ColorRGBA              bg_color;
    Map_Rune_GlyphMapEntry glyph_cache;

    Atlas  _atlas;
    Atlas  _atlas_bold;
    Atlas  _atlas_italic;
    Atlas  _atlas_bold_italic;
    Atlas* atlas;
    Atlas* atlas_bold;
    Atlas* atlas_italic;
    Atlas* atlas_bold_italic;

    // keep textures for reuse in order of length
    LineTexture recycled_textures[5];

    Texture squiggle_texture;

    bool has_blinking_text;

    TimePoint blink_switch;
    TimePoint blink_switch_text;
    TimePoint action;
    TimePoint inactive;

    bool in_focus;
    bool draw_blinking;
    bool draw_blinking_text;
    bool recent_action;
    bool is_inactive;
    bool is_main_font_rgb;

    int   scrollbar_fade;
    Timer flash_timer;
    float flash_fraction;

    Freetype* freetype;

} GfxOpenGL21;

#define gfxOpenGL21(gfx) ((GfxOpenGL21*)&gfx->extend_data)

GLuint GfxOpenGL21_pop_recycled_texture(GfxOpenGL21* self);
void   GfxOpenGL21_load_font(Gfx* self);
void   GfxOpenGL21_destroy_recycled_proxies(GfxOpenGL21* self);

void          GfxOpenGL21_destroy(Gfx* self);
void          GfxOpenGL21_draw(Gfx* self, const Vt* vt, Ui* ui);
Pair_uint32_t GfxOpenGL21_get_char_size(Gfx* self);
void          GfxOpenGL21_resize(Gfx* self, uint32_t w, uint32_t h);
void          GfxOpenGL21_init_with_context_activated(Gfx* self);
bool          GfxOpenGL21_update_timers(Gfx* self, Vt* vt, Ui* ui, TimePoint** out_pending);
void          GfxOpenGL21_notify_action(Gfx* self);
bool          GfxOpenGL21_set_focus(Gfx* self, bool focus);
void          GfxOpenGL21_flash(Gfx* self);
Pair_uint32_t GfxOpenGL21_pixels(Gfx* self, uint32_t c, uint32_t r);
void          GfxOpenGL21_destroy_proxy(Gfx* self, int32_t* proxy);
void          GfxOpenGL21_reload_font(Gfx* self);

static struct IGfx gfx_interface_opengl21 = {
    .draw                        = GfxOpenGL21_draw,
    .resize                      = GfxOpenGL21_resize,
    .get_char_size               = GfxOpenGL21_get_char_size,
    .init_with_context_activated = GfxOpenGL21_init_with_context_activated,
    .reload_font                 = GfxOpenGL21_reload_font,
    .update_timers               = GfxOpenGL21_update_timers,
    .notify_action               = GfxOpenGL21_notify_action,
    .set_focus                   = GfxOpenGL21_set_focus,
    .flash                       = GfxOpenGL21_flash,
    .pixels                      = GfxOpenGL21_pixels,
    .destroy                     = GfxOpenGL21_destroy,
    .destroy_proxy               = GfxOpenGL21_destroy_proxy,
};

Gfx* Gfx_new_OpenGL21(Freetype* freetype)
{
    Gfx* self                   = calloc(1, sizeof(Gfx) + sizeof(GfxOpenGL21) - sizeof(uint8_t));
    self->interface             = &gfx_interface_opengl21;
    gfxOpenGL21(self)->freetype = freetype;
    gfxOpenGL21(self)->is_main_font_rgb = !(freetype->primary_output_type == FT_OUTPUT_GRAYSCALE);
    GfxOpenGL21_load_font(self);
    return self;
}

#define ARRAY_BUFFER_SUB_OR_SWAP(_buf, _size, _newsize)                                            \
    if ((_newsize) > _size) {                                                                      \
        _size = (_newsize);                                                                        \
        glBufferData(GL_ARRAY_BUFFER, (_newsize), (_buf), GL_STREAM_DRAW);                         \
    } else {                                                                                       \
        glBufferSubData(GL_ARRAY_BUFFER, 0, (_newsize), (_buf));                                   \
    }

void GfxOpenGL21_flash(Gfx* self)
{
    if (!settings.no_flash)
        gfxOpenGL21(self)->flash_timer = Timer_from_now_to_ms_from_now(FLASH_DURATION_MS);
}

/**
 * @return offset into info buffer */
__attribute__((always_inline, hot)) static inline int32_t Atlas_select(Atlas* self, char32_t code)
{
    if (unlikely(code < ATLAS_RENDERABLE_START || code > ATLAS_RENDERABLE_END)) {
        return -1;
    } else {
        glBindTexture(GL_TEXTURE_2D, self->tex);
        return code - ATLAS_RENDERABLE_START;
    }
}

static Atlas Atlas_new(GfxOpenGL21* gfx, enum FreetypeFontStyle style)
{
    Atlas self;
    self.w = self.h = 0;
    uint32_t wline = 0, hline = 0, limit = MIN(gfx->max_tex_res, ATLAS_SIZE_LIMIT);
    uint32_t max_char_height = 0;
    for (int i = ATLAS_RENDERABLE_START + 1; i < ATLAS_RENDERABLE_END; i++) {
        FreetypeOutput* output      = Freetype_load_ascii_glyph(gfx->freetype, i, style);
        uint32_t        char_width  = output->width;
        uint32_t        char_height = output->height;
        max_char_height             = MAX(max_char_height, char_height);
        if (wline + char_width < limit) {
            wline += char_width;
            hline = char_height > hline ? char_height : hline;
        } else {
            self.h += hline;
            self.w = wline > (self.w ? wline : self.w);
            hline  = char_height;
            wline  = char_width;
        }
    }
    if (wline > self.w) {
        self.w = wline;
    }
    self.h += hline;
    if (self.h > (uint32_t)gfx->max_tex_res) {
        ERR("Failed to generate font atlas, target texture to small");
    }

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &self.tex);
    glBindTexture(GL_TEXTURE_2D, self.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum internal_format;
    GLenum format;
    switch (gfx->freetype->primary_output_type) {
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_BGR_V:
        case FT_OUTPUT_RGB_H:
        case FT_OUTPUT_RGB_V:
            internal_format = GL_RGB;
            format          = GL_RGBA;
            break;
        case FT_OUTPUT_GRAYSCALE:
            internal_format = GL_RGB;
            format          = GL_RED;
            break;
        default:
            ASSERT_UNREACHABLE
    }
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, self.w, self.h, 0, format, GL_UNSIGNED_BYTE, 0);
    hline             = 0;
    uint32_t offset_x = 0, offset_y = 0;

    for (int i = ATLAS_RENDERABLE_START + 1; i < ATLAS_RENDERABLE_END; i++) {
        FreetypeOutput* output = Freetype_load_and_render_ascii_glyph(gfx->freetype, i, style);
        ASSERT(output, "has output");
        uint32_t width  = output->width;
        uint32_t height = output->height;
        if (offset_x + width > self.w) {
            offset_y += hline;
            offset_x = 0;
            hline    = height;
        } else {
            hline = height > hline ? height : hline;
        }
        GLenum format;
        switch (output->type) {
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
            default:
                ASSERT_UNREACHABLE
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, output->alignment);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        offset_x,
                        offset_y,
                        width,
                        height,
                        format,
                        GL_UNSIGNED_BYTE,
                        output->pixels);

        self.char_info[i - ATLAS_RENDERABLE_START] = (struct AtlasCharInfo){
            .rows       = height,
            .width      = width,
            .left       = output->left,
            .top        = output->top,
            .tex_coords = { ((float)offset_x + 0.01) / self.w,

                            1.0f - (((float)self.h - ((float)offset_y + 0.01)) / self.h),

                            ((float)offset_x + 0.01) / self.w + (float)width / self.w,

                            1.0f - (((float)self.h - ((float)offset_y + 0.01)) / self.h -
                                    (float)height / self.h) }
        };
        offset_x += width;
    }
    return self;
}

static void GlyphMapEntry_destroy(GlyphMapEntry* self)
{
    Texture_destroy(&self->tex);
}

__attribute__((hot)) static GlyphMapEntry* GfxOpenGL21_get_cached_glyph(GfxOpenGL21* gfx,
                                                                        const Rune*  rune)
{
    GlyphMapEntry* entry = Map_get_Rune_GlyphMapEntry(&gfx->glyph_cache, rune);
    if (likely(entry)) {
        glBindTexture(GL_TEXTURE_2D, entry->tex.id);
        return entry;
    }
    Rune alt  = *rune;
    alt.style = TV_RUNE_UNSTYLED;
    entry     = Map_get_Rune_GlyphMapEntry(&gfx->glyph_cache, &alt);
    if (likely(entry)) {
        glBindTexture(GL_TEXTURE_2D, entry->tex.id);
        return entry;
    }
    char32_t               code  = rune->code;
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
    FreetypeOutput* output = Freetype_load_and_render_glyph(gfx->freetype, code, style);
    if (!output) {
        WRN("Missing glyph u+%X\n", code)
        return NULL;
    }
    bool               scale = false;
    enum TextureFormat texture_format;
    enum GlyphColor    glyph_color;
    GLenum             internal_format;
    GLenum             load_format;
    switch (output->type) {
        case FT_OUTPUT_RGB_H:
            texture_format  = TEX_FMT_RGBA;
            glyph_color     = GLYPH_COLOR_LCD;
            internal_format = GL_RGB;
            load_format     = GL_RGB;
            break;
        case FT_OUTPUT_BGR_H:
            texture_format  = TEX_FMT_RGBA;
            glyph_color     = GLYPH_COLOR_LCD;
            internal_format = GL_RGB;
            load_format     = GL_BGR;
            break;
        case FT_OUTPUT_RGB_V:
            texture_format  = TEX_FMT_RGBA;
            glyph_color     = GLYPH_COLOR_LCD;
            internal_format = GL_RGB;
            load_format     = GL_RGB;
            break;
        case FT_OUTPUT_BGR_V:
            texture_format  = TEX_FMT_RGBA;
            glyph_color     = GLYPH_COLOR_LCD;
            internal_format = GL_RGB;
            load_format     = GL_BGR;
            break;
        case FT_OUTPUT_GRAYSCALE:
            texture_format  = TEX_FMT_MONO;
            glyph_color     = GLYPH_COLOR_MONO;
            internal_format = GL_RED;
            load_format     = GL_RED;
            break;
        case FT_OUTPUT_COLOR_BGRA:
            texture_format  = TEX_FMT_RGB;
            glyph_color     = GLYPH_COLOR_COLOR;
            scale           = true;
            internal_format = GL_RGBA;
            load_format     = GL_BGRA;
            break;
        default:
            ASSERT_UNREACHABLE
    }

    Rune key = *rune;
    if (output->style == FT_STYLE_NONE) {
        key.style = TV_RUNE_UNSTYLED;
    }
    GlyphMapEntry new_entry;

    if (unlikely(rune->combine[0])) {
        /* Contains accent characters. Generate a texture combining them with the main `base` glyph.
         * This has to be done here, freetype does not support this directly and blending in
         * freetype requires RGBA as the target format and will remove lcd antialiasing.
         */

        int32_t base_left = output->left;
        Texture tex       = {
            .id     = 0,
            .format = texture_format,
            .w      = MAX(gfx->glyph_width_pixels, output->width),
            .h      = MAX(gfx->line_height_pixels, output->height),
        };
        float scalex = 2.0 / tex.w;
        float scaley = 2.0 / tex.h;

        glGenTextures(1, &tex.id);
        glBindTexture(GL_TEXTURE_2D, tex.id);
        glTexParameteri(GL_TEXTURE_2D,
                        GL_TEXTURE_MIN_FILTER,
                        scale ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glPixelStorei(GL_UNPACK_ALIGNMENT, output->alignment);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     internal_format,
                     tex.w,
                     tex.h,
                     0,
                     load_format,
                     GL_UNSIGNED_BYTE,
                     NULL);

        GLint     old_fb;
        GLint     old_shader;
        GLboolean old_depth_test;
        glGetBooleanv(GL_DEPTH_TEST, &old_depth_test);
        GLint old_viewport[4];
        glGetIntegerv(GL_VIEWPORT, old_viewport);
        glGetIntegerv(GL_CURRENT_PROGRAM, &old_shader);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_fb);

        GLuint tmp_rb;
        glGenRenderbuffers(1, &tmp_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, tmp_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, tex.w, tex.h);

        GLuint tmp_fb;
        glGenFramebuffers(1, &tmp_fb);
        glBindFramebuffer(GL_FRAMEBUFFER, tmp_fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex.id, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, tmp_rb);

        glViewport(0, 0, tex.w, tex.h);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
        glDepthRange(0.0f, 1.0f);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        GLuint tmp_vbo;
        glGenBuffers(1, &tmp_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, tmp_vbo);
        glUseProgram(gfx->font_shader_blend.id);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float[4][4]), NULL, GL_STREAM_DRAW);
        glVertexAttribPointer(gfx->font_shader_blend.attribs->location,
                              4,
                              GL_FLOAT,
                              GL_FALSE,
                              0,
                              0);

        gl_check_error();

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

            float vbo_data[4][4] = {
                { x, y, 0.0f, 1.0f },
                { x + w, y, 1.0f, 1.0f },
                { x + w, y - h, 1.0f, 0.0f },
                { x, y - h, 0.0f, 0.0f },
            };
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vbo_data), vbo_data);
            glDrawArrays(GL_QUADS, 0, 4);

            glDeleteTextures(1, &tmp_tex);
            gl_check_error();
        }

        glDepthFunc(GL_LESS);

        /* restore initial state */
        glUseProgram(old_shader);
        glBindFramebuffer(GL_FRAMEBUFFER, old_fb);
        glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
        glDeleteFramebuffers(1, &tmp_fb);
        glDeleteRenderbuffers(1, &tmp_rb);
        glDeleteBuffers(1, &tmp_vbo);
        if (!old_depth_test) {
            glDisable(GL_DEPTH_TEST);
        }

        new_entry = (GlyphMapEntry){
            .code  = code,
            .color = glyph_color,
            .left  = MIN(0, base_left),
            .top   = tex.h,
            .tex   = tex,
        };
        glBindTexture(GL_TEXTURE_2D, tex.id);
    } else {
        Texture tex = {
            .id     = 0,
            .format = texture_format,
            .w      = output->width,
            .h      = output->height,
        };
        glGenTextures(1, &tex.id);
        glBindTexture(GL_TEXTURE_2D, tex.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, output->alignment);
        glTexParameteri(GL_TEXTURE_2D,
                        GL_TEXTURE_MIN_FILTER,
                        scale ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     internal_format,
                     output->width,
                     output->height,
                     0,
                     load_format,
                     GL_UNSIGNED_BYTE,
                     output->pixels);
        if (scale) {
            glGenerateMipmap(GL_TEXTURE_2D);
        }
        new_entry = (GlyphMapEntry){
            .code  = code,
            .color = glyph_color,
            .left  = output->left,
            .top   = output->top,
            .tex   = tex,
        };
    }
    return Map_insert_Rune_GlyphMapEntry(&gfx->glyph_cache, key, new_entry);
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
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    uint8_t* fragments                 = calloc(w * h * 4, 1);
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

void GfxOpenGL21_resize(Gfx* self, uint32_t w, uint32_t h)
{
    GfxOpenGL21* gl21 = gfxOpenGL21(self);
    GfxOpenGL21_destroy_recycled_proxies(gl21);

    gl21->win_w              = w;
    gl21->win_h              = h;
    gl21->sx                 = 2.0f / gl21->win_w;
    gl21->sy                 = 2.0f / gl21->win_h;
    gl21->line_height_pixels = gl21->freetype->line_height_pixels + settings.padd_glyph_y;
    gl21->glyph_width_pixels = gl21->freetype->glyph_width_pixels + settings.padd_glyph_x;
    gl21->gw                 = gl21->freetype->gw;
    FreetypeOutput* output   = Freetype_load_ascii_glyph(gl21->freetype, '(', FT_STYLE_REGULAR);
    uint32_t        hber     = output->ft_slot->metrics.horiBearingY / 64 / 2 / 2 + 1;
    gl21->pen_begin          = gl21->sy * (gl21->line_height_pixels / 2.0) + gl21->sy * hber;
    gl21->pen_begin_pixels   = (float)(gl21->line_height_pixels / 1.75) + (float)hber;
    uint32_t height          = (gl21->line_height_pixels + settings.padd_glyph_y) * 64;
    gl21->line_height        = (float)height * gl21->sy / 64.0;
    gl21->glyph_width        = gl21->glyph_width_pixels * gl21->sx;
    gl21->max_cells_in_line  = gl21->win_w / gl21->glyph_width_pixels;

    // update dynamic bg buffer
    glBindBuffer(GL_ARRAY_BUFFER, gl21->bg_vao.vbo);
    glVertexAttribPointer(gl21->bg_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);

    float bg_box[] = {
        0.0f,
        0.0f,
        0.0f,
        gl21->line_height,
        gl21->glyph_width,
        gl21->line_height,
        gl21->glyph_width,
        0.0f,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof bg_box, bg_box, GL_STREAM_DRAW);

    Pair_uint32_t cells = GfxOpenGL21_get_char_size(self);
    cells               = GfxOpenGL21_pixels(self, cells.first, cells.second);

    glViewport(0, 0, gl21->win_w, gl21->win_h);
}

Pair_uint32_t GfxOpenGL21_get_char_size(Gfx* self)
{
    GfxOpenGL21* gl21 = gfxOpenGL21(self);

    int32_t cols = MAX((gl21->win_w - 2 * settings.padding) /
                         (gfxOpenGL21(self)->freetype->glyph_width_pixels + settings.padd_glyph_x),
                       0);
    int32_t rows = MAX((gl21->win_h - 2 * settings.padding) /
                         (gfxOpenGL21(self)->freetype->line_height_pixels + settings.padd_glyph_y),
                       0);

    return (Pair_uint32_t){ .first = cols, .second = rows };
}

Pair_uint32_t GfxOpenGL21_pixels(Gfx* self, uint32_t c, uint32_t r)
{
    float x, y;
    x = c * (gfxOpenGL21(self)->freetype->glyph_width_pixels + settings.padd_glyph_x);
    y = r * (gfxOpenGL21(self)->freetype->line_height_pixels + settings.padd_glyph_y);
    return (Pair_uint32_t){ .first = x + 2 * settings.padding, .second = y + 2 * settings.padding };
}

void GfxOpenGL21_load_font(Gfx* self) {}

void GfxOpenGL21_init_with_context_activated(Gfx* self)
{
    GfxOpenGL21* gl21 = gfxOpenGL21(self);

    gl_load_exts();

#ifdef DEBUG
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(on_gl_error, NULL);
#endif

    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glDisable(GL_FRAMEBUFFER_SRGB);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));

    gl21->font_shader = Shader_new(font_vs_src, font_fs_src, "coord", "tex", "clr", "bclr", NULL);

    gl21->font_shader_gray =
      Shader_new(font_vs_src, font_gray_fs_src, "coord", "tex", "clr", "bclr", NULL);

    gl21->font_shader_blend =
      Shader_new(font_vs_src, font_depth_blend_fs_src, "coord", "tex", NULL);

    gl21->bg_shader = Shader_new(bg_vs_src, bg_fs_src, "pos", "mv", "clr", NULL);

    gl21->line_shader = Shader_new(line_vs_src, line_fs_src, "pos", "clr", NULL);

    gfxOpenGL21(self)->image_shader =
      Shader_new(image_rgb_vs_src, image_rgb_fs_src, "coord", "tex", NULL);

    gl21->image_tint_shader =
      Shader_new(image_rgb_vs_src, image_tint_rgb_fs_src, "coord", "tex", "tint", NULL);

    gl21->bg_vao = VBO_new(2, 1, gl21->bg_shader.attribs);

    gl21->line_bg_vao = VBO_new(2, 1, gl21->bg_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gl21->line_bg_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gl21->font_vao = VBO_new(4, 1, gl21->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gl21->font_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gl21->line_vao = VBO_new(2, 1, gl21->line_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->line_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8, NULL, GL_STREAM_DRAW);

    gl21->flex_vbo = VBO_new(4, 1, gl21->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gl21->flex_vbo.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gl21->flex_vbo_italic = VBO_new(4, 1, gl21->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gl21->flex_vbo_italic.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gl21->flex_vbo_bold = VBO_new(4, 1, gl21->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gl21->flex_vbo_bold.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gl21->flex_vbo_bold_italic = VBO_new(4, 1, gl21->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gl21->flex_vbo_bold_italic.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gl21->max_tex_res);

    gl21->color    = settings.fg;
    gl21->bg_color = settings.bg;

    Shader_use(&gl21->font_shader);
    glUniform3f(gl21->font_shader.uniforms[1].location,
                ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1),
                ColorRGB_get_float(settings.fg, 2));

    gl21->_atlas = Atlas_new(gl21, FT_STYLE_REGULAR);
    gl21->atlas  = &gl21->_atlas;

    glGenFramebuffers(1, &gl21->line_framebuffer);

    gl21->in_focus           = true;
    gl21->recent_action      = true;
    gl21->draw_blinking      = true;
    gl21->draw_blinking_text = true;

    gl21->blink_switch      = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
    gl21->blink_switch_text = TimePoint_now();

    gl21->_vec_glyph_buffer = Vector_new_with_capacity_GlyphBufferData(80);
    gl21->vec_glyph_buffer  = &gl21->_vec_glyph_buffer;

    gl21->_vec_glyph_buffer_italic = Vector_new_GlyphBufferData();
    gl21->_vec_glyph_buffer_bold   = Vector_new_GlyphBufferData();

    // if font styles don't exist point their resources to deaults
    if (settings.font_file_name_bold.str) {
        gl21->_atlas_bold           = Atlas_new(gl21, FT_STYLE_BOLD);
        gl21->atlas_bold            = &gl21->_atlas_bold;
        gl21->vec_glyph_buffer_bold = &gl21->_vec_glyph_buffer_bold;
    } else {
        gl21->atlas_bold            = &gl21->_atlas;
        gl21->vec_glyph_buffer_bold = &gl21->_vec_glyph_buffer;
    }

    if (settings.font_file_name_italic.str) {
        gl21->_atlas_italic           = Atlas_new(gl21, FT_STYLE_ITALIC);
        gl21->atlas_italic            = &gl21->_atlas_italic;
        gl21->vec_glyph_buffer_italic = &gl21->_vec_glyph_buffer_italic;
    } else {
        gl21->atlas_italic            = &gl21->_atlas;
        gl21->vec_glyph_buffer_italic = &gl21->_vec_glyph_buffer;
    }

    if (settings.font_file_name_bold_italic.str) {
        gl21->_atlas_bold_italic            = Atlas_new(gfxOpenGL21(self), FT_STYLE_BOLD_ITALIC);
        gl21->atlas_bold_italic             = &gl21->_atlas_bold_italic;
        gl21->vec_glyph_buffer_bold_italic  = &gl21->_vec_glyph_buffer_bold_italic;
        gl21->_vec_glyph_buffer_bold_italic = Vector_new_GlyphBufferData();
    } else {
        if (settings.font_file_name_italic.str) {
            gl21->atlas_bold_italic            = &gfxOpenGL21(self)->_atlas_italic;
            gl21->vec_glyph_buffer_bold_italic = &gl21->_vec_glyph_buffer_italic;
        } else if (settings.font_file_name_bold.str) {
            gl21->atlas_bold_italic            = &gl21->_atlas_bold;
            gl21->vec_glyph_buffer_bold_italic = &gl21->_vec_glyph_buffer_bold;
        } else {
            gl21->atlas_bold_italic            = &gl21->_atlas;
            gl21->vec_glyph_buffer_bold_italic = &gl21->_vec_glyph_buffer;
        }
    }

    gl21->glyph_cache = Map_new_Rune_GlyphMapEntry(NUM_BUCKETS);

    gl21->vec_vertex_buffer  = Vector_new_vertex_t();
    gl21->vec_vertex_buffer2 = Vector_new_vertex_t();

    GfxOpenGL21_notify_action(self);

    Freetype* ft = gl21->freetype;

    gl21->line_height_pixels = ft->line_height_pixels + settings.padd_glyph_y;
    gl21->glyph_width_pixels = ft->glyph_width_pixels + settings.padd_glyph_x;
    uint32_t t_height        = CLAMP(gl21->line_height_pixels / 8.0 + 2, 4, UINT8_MAX);
    gl21->squiggle_texture =
      create_squiggle_texture(t_height * M_PI / 2.0, t_height, CLAMP((t_height / 4), 1, 20));
}

void GfxOpenGL21_reload_font(Gfx* self)
{
    GfxOpenGL21* gl21 = gfxOpenGL21(self);

    Atlas_destroy(&gl21->_atlas);
    if (gl21->atlas != gfxOpenGL21(self)->atlas_bold) {
        Atlas_destroy(&gl21->_atlas_bold);
    }
    if (gl21->atlas != gl21->atlas_italic) {
        Atlas_destroy(&gl21->_atlas_italic);
    }
    if (gl21->atlas != gl21->atlas_bold_italic && gl21->atlas_bold != gl21->atlas_bold_italic &&
        gl21->atlas_italic != gl21->atlas_bold_italic) {
        Atlas_destroy(&gl21->_atlas_bold_italic);
    }

    Map_destroy_Rune_GlyphMapEntry(&gl21->glyph_cache);
    gl21->glyph_cache = Map_new_Rune_GlyphMapEntry(NUM_BUCKETS);

    GfxOpenGL21_load_font(self);
    GfxOpenGL21_resize(self, gl21->win_w, gl21->win_h);

    gfxOpenGL21(self)->_atlas = Atlas_new(gl21, FT_STYLE_REGULAR);

    if (settings.font_file_name_bold.str) {
        gl21->_atlas_bold = Atlas_new(gl21, FT_STYLE_BOLD);
    }
    if (settings.font_file_name_italic.str) {
        gl21->_atlas_italic = Atlas_new(gl21, FT_STYLE_ITALIC);
    }
    if (settings.font_file_name_bold_italic.str) {
        gl21->_atlas_bold_italic = Atlas_new(gl21, FT_STYLE_BOLD_ITALIC);
    }

    // regenerate the squiggle texture
    glDeleteTextures(1, &gl21->squiggle_texture.id);
    uint32_t t_height = CLAMP(gl21->line_height_pixels / 8.0 + 2, 4, UINT8_MAX);
    gl21->squiggle_texture =
      create_squiggle_texture(t_height * M_PI / 2.0, t_height, CLAMP(t_height / 4, 1, 20));

    GfxOpenGL21_notify_action(self);
}

bool GfxOpenGL21_set_focus(Gfx* self, bool focus)
{
    bool ret = false;
    if (gfxOpenGL21(self)->in_focus && !focus) {
        ret = true;
    }
    gfxOpenGL21(self)->in_focus = focus;
    return ret;
}

void GfxOpenGL21_notify_action(Gfx* self)
{
    gfxOpenGL21(self)->blink_switch  = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
    gfxOpenGL21(self)->draw_blinking = true;
    gfxOpenGL21(self)->recent_action = true;
    gfxOpenGL21(self)->action =
      TimePoint_ms_from_now(settings.cursor_blink_interval_ms + settings.cursor_blink_suspend_ms);
    gfxOpenGL21(self)->inactive = TimePoint_s_from_now(settings.cursor_blink_end_s);
}

bool GfxOpenGL21_update_timers(Gfx* self, Vt* vt, Ui* ui, TimePoint** out_pending)
{
    bool repaint = false;

    GfxOpenGL21* gl21 = gfxOpenGL21(self);

    TimePoint* closest = NULL;
    if (gl21->has_blinking_text) {
        closest = &gl21->blink_switch_text;
    }
    if (!(gl21->recent_action && !gl21->draw_blinking) && settings.enable_cursor_blink) {
        if (!closest || (!TimePoint_passed(gl21->blink_switch) &&
                         TimePoint_is_earlier(gl21->blink_switch, *closest))) {
            closest = &gl21->blink_switch;
        }
    }
    *out_pending = closest;

    if (TimePoint_passed(gl21->blink_switch_text)) {
        if (unlikely(gl21->has_blinking_text)) {
            gl21->draw_blinking_text = !gl21->draw_blinking_text;
            gl21->blink_switch_text  = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
            repaint                  = true;
        }
    }

    if (!gl21->in_focus && !gl21->has_blinking_text) {
        return false;
    }

    float fraction = Timer_get_fraction_clamped_now(&gl21->flash_timer);
    if (fraction != gl21->flash_fraction) {
        gl21->flash_fraction = fraction;
        repaint              = true;
    }

    if (gl21->recent_action && TimePoint_passed(gfxOpenGL21(self)->action)) {
        // start blinking cursor
        gl21->recent_action = false;
        gl21->blink_switch  = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
        gl21->draw_blinking = !gl21->draw_blinking;
        repaint             = true;
    }

    if (TimePoint_passed(gl21->inactive) && gl21->draw_blinking &&
        settings.cursor_blink_end_s >= 0) {
        gl21->is_inactive = true;
    } else {
        if (TimePoint_passed(gl21->blink_switch)) {
            gl21->blink_switch  = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
            gl21->draw_blinking = !gl21->draw_blinking;

            if (!(gl21->recent_action && !gl21->draw_blinking) && settings.enable_cursor_blink) {
                repaint = true;
            }
        }
    }

    return repaint;
}

/**
 * Generate vertex data for drawing lines on the backbuffer */
static void GfxOpenGL21_generate_line_quads(GfxOpenGL21*  gfx,
                                            VtLine* const vt_line,
                                            uint_fast16_t line_index)
{
    if (vt_line->proxy.data[PROXY_INDEX_TEXTURE] ||
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]) {

        if (vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]) {
            gfx->has_blinking_text = true;
        }

        float tex_end_x   = -1.0f + vt_line->proxy.data[PROXY_INDEX_TEXTURE_SIZE] * gfx->sx;
        float tex_begin_y = 1.0f - gfx->line_height_pixels * (line_index + 1) * gfx->sy;
        Vector_push_GlyphBufferData(gfx->vec_glyph_buffer,
                                    (GlyphBufferData){ {
                                      { -1.0f, tex_begin_y + gfx->line_height, 0.0f, 0.0f },
                                      { -1.0, tex_begin_y, 0.0f, 1.0f },
                                      { tex_end_x, tex_begin_y, 1.0f, 1.0f },
                                      { tex_end_x, tex_begin_y + gfx->line_height, 1.0f, 0.0f },
                                    } });
    }
}

/**
 * Draw lines generated by GfxOpenGL21_generate_line_quads() */
static uint_fast32_t GfxOpenGL21_draw_line_quads(GfxOpenGL21*  gfx,
                                                 VtLine* const vt_line,
                                                 uint_fast32_t quad_index)
{
    if (vt_line->proxy.data[PROXY_INDEX_TEXTURE] ||
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]) {

        if (vt_line->proxy.data[PROXY_INDEX_TEXTURE] || !gfx->draw_blinking_text) {
            glBindTexture(GL_TEXTURE_2D,
                          vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] && !gfx->draw_blinking_text
                            ? vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]
                            : vt_line->proxy.data[PROXY_INDEX_TEXTURE]);
            glDrawArrays(GL_QUADS, quad_index * 4, 4);
        }
        ++quad_index;
    }

    return quad_index;
}

#define BOUND_RESOURCES_NONE      0
#define BOUND_RESOURCES_BG        1
#define BOUND_RESOURCES_FONT      2
#define BOUND_RESOURCES_LINES     3
#define BOUND_RESOURCES_IMAGE     4
#define BOUND_RESOURCES_FONT_MONO 5

/**
 * Draw a range of character *lines of a given VtLine
 *
 * Should only be called by GfxOpenGL21_rasterize_line() */
__attribute__((hot)) static inline void _GfxOpenGL21_rasterize_line_underline_range(
  GfxOpenGL21* gfx,
  VtLine*      vt_line,
  Pair_size_t  range,
  int_fast8_t* bound_resources,
  Pair_int32_t texture_dims)
{
    /* Scale from pixels to GL coordinates */
    const double scalex = 2.0f / texture_dims.first;
    const double scaley = 2.0f / texture_dims.second;

    // draw lines
    static float begin[5]   = { -1, -1, -1, -1, -1 };
    static float end[5]     = { 1, 1, 1, 1, 1 };
    static bool  drawing[5] = { 0 };

    if (unlikely(range.first)) {
        const float init_coord =
          unlikely(range.second) ? -1.0f + gfx->glyph_width_pixels * scalex * range.first : 0.0f;
        for (uint_fast8_t i = 0; i < ARRAY_SIZE(begin); ++i) {
            begin[i] = init_coord;
        }
    }

    // lines are drawn in the same color as the character, unless the line color was explicitly set
    ColorRGB line_color = vt_line->data.buf[range.first].linecolornotdefault
                            ? vt_line->data.buf[range.first].line
                            : vt_line->data.buf[range.first].fg;

    glDisable(GL_SCISSOR_TEST);

    for (const VtRune* each_rune = vt_line->data.buf + range.first;
         each_rune <= vt_line->data.buf + range.second;
         ++each_rune) {

        /* text column where this should be drawn */
        size_t   column = each_rune - vt_line->data.buf;
        ColorRGB nc     = {};
        if (each_rune != vt_line->data.buf + range.second) {
            nc = each_rune->linecolornotdefault ? each_rune->line : each_rune->fg;
        }
        // State has changed
        if (!ColorRGB_eq(line_color, nc) || each_rune == vt_line->data.buf + range.second ||
            each_rune->underlined != drawing[0] || each_rune->doubleunderline != drawing[1] ||
            each_rune->strikethrough != drawing[2] || each_rune->overline != drawing[3] ||
            each_rune->curlyunderline != drawing[4]) {
            if (each_rune == vt_line->data.buf + range.second) {
                for (int_fast8_t tmp = 0; tmp < 5; tmp++) {
                    end[tmp] = -1.0f + (float)column * scalex * (float)gfx->glyph_width_pixels;
                }
            } else {
#define L_SET_BOUNDS_END(what, index)                                                              \
    if (drawing[index]) {                                                                          \
        end[index] = -1.0f + (float)column * scalex * (float)gfx->glyph_width_pixels;              \
    }
                L_SET_BOUNDS_END(z->underlined, 0);
                L_SET_BOUNDS_END(z->doubleunderline, 1);
                L_SET_BOUNDS_END(z->strikethrough, 2);
                L_SET_BOUNDS_END(z->overline, 3);
                L_SET_BOUNDS_END(z->curlyunderline, 4);
            }
            Vector_clear_vertex_t(&gfx->vec_vertex_buffer);
            Vector_clear_vertex_t(&gfx->vec_vertex_buffer2);
            if (drawing[0]) {
                Vector_push_vertex_t(&gfx->vec_vertex_buffer,
                                     (vertex_t){ begin[0], 1.0f - scaley });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ end[0], 1.0f - scaley });
            }
            if (drawing[1]) {
                Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ begin[1], 1.0f });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ end[1], 1.0f });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer,
                                     (vertex_t){ begin[1], 1.0f - 2 * scaley });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer,
                                     (vertex_t){ end[1], 1.0f - 2 * scaley });
            }
            if (drawing[2]) {
                Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ begin[2], .2f });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ end[2], .2f });
            }
            if (drawing[3]) {
                Vector_push_vertex_t(&gfx->vec_vertex_buffer,
                                     (vertex_t){ begin[3], -1.0f + scaley });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer, (vertex_t){ end[3], -1.0f + scaley });
            }
            if (drawing[4]) {
                float   cw      = gfx->glyph_width_pixels * scalex;
                int32_t n_cells = round((end[4] - begin[4]) / cw);
                float   t_y     = 1.0f - gfx->squiggle_texture.h * scaley;

                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ begin[4], t_y });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ 0.0f, 0.0f });

                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ begin[4], 1.0f });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ 0.0f, 1.0f });

                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ end[4], 1.0f });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ 1.0f * n_cells, 1.0f });

                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ end[4], t_y });
                Vector_push_vertex_t(&gfx->vec_vertex_buffer2, (vertex_t){ 1.0f * n_cells, 0.0f });
            }
            if (gfx->vec_vertex_buffer.size) {
                if (*bound_resources != BOUND_RESOURCES_LINES) {
                    *bound_resources = BOUND_RESOURCES_LINES;
                    Shader_use(&gfx->line_shader);
                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                    glVertexAttribPointer(gfx->line_shader.attribs->location,
                                          2,
                                          GL_FLOAT,
                                          GL_FALSE,
                                          0,
                                          0);
                }
                glUniform3f(gfx->line_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0),
                            ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));
                size_t new_size = sizeof(vertex_t) * gfx->vec_vertex_buffer.size;
                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer.buf, gfx->flex_vbo.size, new_size);
                glDrawArrays(GL_LINES, 0, gfx->vec_vertex_buffer.size);
            }
            if (gfx->vec_vertex_buffer2.size) {
                *bound_resources = BOUND_RESOURCES_NONE;
                Shader_use(&gfx->image_tint_shader);
                glBindTexture(GL_TEXTURE_2D, gfx->squiggle_texture.id);
                glUniform3f(gfx->image_tint_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0),
                            ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));
                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                glVertexAttribPointer(gfx->font_shader.attribs->location,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);
                size_t new_size = sizeof(vertex_t) * gfx->vec_vertex_buffer2.size;
                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer2.buf, gfx->flex_vbo.size, new_size);
                glDrawArrays(GL_QUADS, 0, gfx->vec_vertex_buffer2.size / 2);
            }

#define L_SET_BOUNDS_BEGIN(what, index)                                                            \
    if (what) {                                                                                    \
        begin[index] = -1.0f + (float)column * scalex * (float)gfx->glyph_width_pixels;            \
    }

            if (each_rune != vt_line->data.buf + range.second) {
                L_SET_BOUNDS_BEGIN(each_rune->underlined, 0);
                L_SET_BOUNDS_BEGIN(each_rune->doubleunderline, 1);
                L_SET_BOUNDS_BEGIN(each_rune->strikethrough, 2);
                L_SET_BOUNDS_BEGIN(each_rune->overline, 3);
                L_SET_BOUNDS_BEGIN(each_rune->curlyunderline, 4);
            }
            if (each_rune != vt_line->data.buf + range.second) {
                drawing[0] = each_rune->underlined;
                drawing[1] = each_rune->doubleunderline;
                drawing[2] = each_rune->strikethrough;
                drawing[3] = each_rune->overline;
                drawing[4] = each_rune->curlyunderline;
            } else {
                drawing[0] = false;
                drawing[1] = false;
                drawing[2] = false;
                drawing[3] = false;
                drawing[4] = false;
            }
            line_color = nc;
        }
    }
}

/**
 * Draw a range of characters of a given VtLine
 *
 * Should only be called by GfxOpenGL21_rasterize_line() */
__attribute__((hot)) static inline void _GfxOpenGL21_rasterize_line_range(
  GfxOpenGL21*    gfx,
  const Vt* const vt,
  VtLine*         vt_line,
  Pair_size_t     range,
  size_t          visual_line_index,
  bool            is_for_blinking,
  int_fast8_t*    bound_resources,
  Pair_int32_t    texture_dims,
  bool*           has_blinking_chars,
  bool*           has_underlined_chars)
{
    /* Scale from pixels to GL coordinates */
    const double scalex = 2.0f / texture_dims.first;
    const double scaley = 2.0f / texture_dims.second;

    GLint     bg_pixels_begin          = range.first * gfx->glyph_width_pixels, bg_pixels_end;
    ColorRGBA active_bg_color          = settings.bg;
    VtRune*   each_rune                = vt_line->data.buf + range.first;
    VtRune*   same_bg_block_begin_rune = each_rune;

    for (size_t idx_each_rune = range.first; idx_each_rune <= range.second;) {

        each_rune = vt_line->data.buf + idx_each_rune;
        if (likely(idx_each_rune != range.second)) {
            if (unlikely(each_rune->blinkng)) {
                *has_blinking_chars = true;
            }
            if (!*has_underlined_chars &&
                unlikely(each_rune->underlined || each_rune->strikethrough ||
                         each_rune->doubleunderline || each_rune->curlyunderline ||
                         each_rune->overline)) {
                *has_underlined_chars = true;
            }
        }

#define L_CALC_BG_COLOR                                                                            \
    Vt_is_cell_selected(vt, idx_each_rune, visual_line_index) ? settings.bghl : each_rune->bg

        if (idx_each_rune == range.second || !ColorRGBA_eq(L_CALC_BG_COLOR, active_bg_color)) {
            int32_t extra_width = 0;

            if (idx_each_rune > 1) {
                extra_width = MAX(wcwidth(vt_line->data.buf[idx_each_rune - 1].rune.code) - 2, 0);
            }

            bg_pixels_end = (idx_each_rune + extra_width) * gfx->glyph_width_pixels;

            glEnable(GL_SCISSOR_TEST);
            glScissor(bg_pixels_begin, 0, bg_pixels_end - bg_pixels_begin, texture_dims.second);

            glClearColor(ColorRGBA_get_float(active_bg_color, 0),
                         ColorRGBA_get_float(active_bg_color, 1),
                         ColorRGBA_get_float(active_bg_color, 2),
                         ColorRGBA_get_float(active_bg_color, 3));
            glClear(GL_COLOR_BUFFER_BIT);

            { // for each block of characters with the same background color
                ColorRGB      active_fg_color              = settings.fg;
                const VtRune* same_colors_block_begin_rune = same_bg_block_begin_rune;

                for (const VtRune* each_rune_same_bg = same_bg_block_begin_rune;
                     each_rune_same_bg != each_rune + 1;
                     ++each_rune_same_bg) {

#define L_CALC_DIM_BLEND_COLOR                                                                     \
    (unlikely(each_rune_same_bg->dim)                                                              \
       ? ColorRGB_new_from_blend(each_rune_same_bg->fg,                                            \
                                 ColorRGB_from_RGBA(active_bg_color),                              \
                                 DIM_COLOR_BLEND_FACTOR)                                           \
       : each_rune_same_bg->fg)

// it's very unlikely that this will be needed as selected region changes the bg color, but
// technically the bg highlight color could be exactly the same as the background
#define L_CALC_FG_COLOR                                                                            \
    !settings.highlight_change_fg                                                                  \
      ? L_CALC_DIM_BLEND_COLOR                                                                     \
      : unlikely(                                                                                  \
          Vt_is_cell_selected(vt, each_rune_same_bg - vt_line->data.buf, visual_line_index))       \
          ? settings.fghl                                                                          \
          : L_CALC_DIM_BLEND_COLOR

                    if (each_rune_same_bg == each_rune ||
                        !ColorRGB_eq(L_CALC_FG_COLOR, active_fg_color)) {
                        { // Text color has changed

                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_italic);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_bold);

                            if (settings.font_file_name_bold_italic.str) {
                                Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_bold_italic);
                            }
                            /* Dummy value with we can point to to filter out a character */
                            VtRune        same_color_blank_space;
                            const VtRune* each_rune_filtered_visible;

                            /* Go through each character and collect all that come from the font
                             * atlas */
                            for (const VtRune* each_rune_same_colors = same_colors_block_begin_rune;
                                 each_rune_same_colors != each_rune_same_bg;
                                 ++each_rune_same_colors) {
                                size_t column = each_rune_same_colors - vt_line->data.buf;

                                /* Filter out stuff that should be hidden on this pass */
                                if (unlikely((is_for_blinking && each_rune_same_colors->blinkng) ||
                                             each_rune_same_colors->hidden)) {
                                    same_color_blank_space           = *each_rune_same_colors;
                                    same_color_blank_space.rune.code = ' ';
                                    each_rune_filtered_visible       = &same_color_blank_space;
                                } else {
                                    each_rune_filtered_visible = each_rune_same_colors;
                                }
                                if (each_rune_filtered_visible->rune.code >
                                      ATLAS_RENDERABLE_START &&
                                    each_rune_filtered_visible->rune.code <= ATLAS_RENDERABLE_END &&
                                    !each_rune_filtered_visible->rune.combine[0]) {
                                    // pull data from font atlas
                                    struct AtlasCharInfo*   g;
                                    int32_t                 atlas_offset = -1;
                                    Vector_GlyphBufferData* target       = gfx->vec_glyph_buffer;
                                    Atlas*                  source_atlas = gfx->atlas;
                                    switch (expect(each_rune_filtered_visible->rune.style,
                                                   VT_RUNE_NORMAL)) {
                                        case VT_RUNE_ITALIC:
                                            target       = gfx->vec_glyph_buffer_italic;
                                            source_atlas = gfx->atlas_italic;
                                            break;
                                        case VT_RUNE_BOLD:
                                            target       = gfx->vec_glyph_buffer_bold;
                                            source_atlas = gfx->atlas_bold;
                                            break;
                                        case VT_RUNE_BOLD_ITALIC:
                                            target       = gfx->vec_glyph_buffer_bold_italic;
                                            source_atlas = gfx->atlas_bold_italic;
                                        default:;
                                    }
                                    atlas_offset =
                                      Atlas_select(source_atlas,
                                                   each_rune_filtered_visible->rune.code);
                                    g        = &source_atlas->char_info[atlas_offset];
                                    double h = (double)g->rows * scaley;
                                    double w = (double)g->width * scalex;
                                    double t = (double)g->top * scaley;
                                    double l = (double)g->left * scalex;

                                    /* (scalex/y * 0.5) at the end to put it in the middle of the
                                     * pixel */
                                    float x3 = -1.0f +
                                               (float)column * gfx->glyph_width_pixels * scalex +
                                               l + (scalex * 0.5);
                                    float y3 =
                                      -1.0f + gfx->pen_begin_pixels * scaley - t + (scaley * 0.5);
                                    Vector_push_GlyphBufferData(
                                      target,
                                      (GlyphBufferData){ {
                                        { x3, y3, g->tex_coords[0], g->tex_coords[1] },
                                        { x3 + w, y3, g->tex_coords[2], g->tex_coords[1] },
                                        { x3 + w, y3 + h, g->tex_coords[2], g->tex_coords[3] },
                                        { x3, y3 + h, g->tex_coords[0], g->tex_coords[3] },
                                      } });
                                }
                            }
                            // Draw atlas characters we collected if any
                            if (gfx->vec_glyph_buffer->size || gfx->vec_glyph_buffer_italic->size ||
                                gfx->vec_glyph_buffer_bold->size) {

                                /* Set up the scissor box for this block */
                                GLint clip_begin =
                                  (same_colors_block_begin_rune - vt_line->data.buf) *
                                  gfx->glyph_width_pixels;
                                GLsizei clip_end =
                                  (each_rune_same_bg - vt_line->data.buf) * gfx->glyph_width_pixels;
                                // glEnable(GL_SCISSOR_TEST);
                                glScissor(clip_begin,
                                          0,
                                          clip_end - clip_begin,
                                          texture_dims.second);

                                *bound_resources = gfx->is_main_font_rgb
                                                     ? BOUND_RESOURCES_FONT
                                                     : BOUND_RESOURCES_FONT_MONO;
                                if (gfx->is_main_font_rgb) {
                                    glUseProgram(gfx->font_shader.id);
                                    glUniform3f(gfx->font_shader.uniforms[1].location,
                                                ColorRGB_get_float(active_fg_color, 0),
                                                ColorRGB_get_float(active_fg_color, 1),
                                                ColorRGB_get_float(active_fg_color, 2));
                                    glUniform4f(gfx->font_shader.uniforms[2].location,
                                                ColorRGBA_get_float(active_bg_color, 0),
                                                ColorRGBA_get_float(active_bg_color, 1),
                                                ColorRGBA_get_float(active_bg_color, 2),
                                                ColorRGBA_get_float(active_bg_color, 3));
                                } else {
                                    glUseProgram(gfx->font_shader_gray.id);
                                    glUniform3f(gfx->font_shader_gray.uniforms[1].location,
                                                ColorRGB_get_float(active_fg_color, 0),
                                                ColorRGB_get_float(active_fg_color, 1),
                                                ColorRGB_get_float(active_fg_color, 2));

                                    glUniform4f(gfx->font_shader_gray.uniforms[2].location,
                                                ColorRGBA_get_float(active_bg_color, 0),
                                                ColorRGBA_get_float(active_bg_color, 1),
                                                ColorRGBA_get_float(active_bg_color, 2),
                                                ColorRGBA_get_float(active_bg_color, 3));
                                }
                                // normal
                                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                                glVertexAttribPointer(gfx->font_shader.attribs->location,
                                                      4,
                                                      GL_FLOAT,
                                                      GL_FALSE,
                                                      0,
                                                      0);
                                size_t newsize =
                                  gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);
                                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer->buf,
                                                         gfx->flex_vbo.size,
                                                         newsize);
                                glBindTexture(GL_TEXTURE_2D, gfx->atlas->tex);
                                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                                glDrawArrays(GL_QUADS, 0, gfx->vec_glyph_buffer->size * 4);
                                // bold
                                if (gfx->vec_glyph_buffer_bold != gfx->vec_glyph_buffer) {
                                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo_bold.vbo);
                                    glVertexAttribPointer(gfx->font_shader.attribs->location,
                                                          4,
                                                          GL_FLOAT,
                                                          GL_FALSE,
                                                          0,
                                                          0);
                                    size_t newsize =
                                      gfx->vec_glyph_buffer_bold->size * sizeof(GlyphBufferData);
                                    ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer_bold->buf,
                                                             gfx->flex_vbo_bold.size,
                                                             newsize);
                                    glBindTexture(GL_TEXTURE_2D, gfx->atlas_bold->tex);
                                    glDrawArrays(GL_QUADS, 0, gfx->vec_glyph_buffer_bold->size * 4);
                                }
                                // italic
                                if (gfx->vec_glyph_buffer_italic != gfx->vec_glyph_buffer) {
                                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo_italic.vbo);
                                    glVertexAttribPointer(gfx->font_shader.attribs->location,
                                                          4,
                                                          GL_FLOAT,
                                                          GL_FALSE,
                                                          0,
                                                          0);
                                    size_t newsize =
                                      gfx->vec_glyph_buffer_italic->size * sizeof(GlyphBufferData);
                                    ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer_italic->buf,
                                                             gfx->flex_vbo_italic.size,
                                                             newsize);
                                    glBindTexture(GL_TEXTURE_2D, gfx->atlas_italic->tex);
                                    glDrawArrays(GL_QUADS,
                                                 0,
                                                 gfx->vec_glyph_buffer_italic->size * 4);
                                }
                                // bold italic
                                if (gfx->vec_glyph_buffer_bold_italic != gfx->vec_glyph_buffer &&
                                    gfx->vec_glyph_buffer_bold_italic !=
                                      gfx->vec_glyph_buffer_bold &&
                                    gfx->vec_glyph_buffer_bold_italic !=
                                      gfx->vec_glyph_buffer_italic) {
                                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo_bold_italic.vbo);
                                    glVertexAttribPointer(gfx->font_shader.attribs->location,
                                                          4,
                                                          GL_FLOAT,
                                                          GL_FALSE,
                                                          0,
                                                          0);
                                    size_t newsize = gfx->vec_glyph_buffer_bold_italic->size *
                                                     sizeof(GlyphBufferData);
                                    ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer_bold_italic->buf,
                                                             gfx->flex_vbo_bold_italic.size,
                                                             newsize);
                                    glBindTexture(GL_TEXTURE_2D, gfx->atlas_bold_italic->tex);
                                    glDrawArrays(GL_QUADS,
                                                 0,
                                                 gfx->vec_glyph_buffer_bold_italic->size * 4);
                                }
                            } // end if there are atlas chars to draw

                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_italic);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_bold);

                            // Go through each character again draw all that come from the glyphmap
                            glDisable(GL_SCISSOR_TEST);
                            for (const VtRune* z = same_colors_block_begin_rune;
                                 z != each_rune_same_bg;
                                 ++z) {

                                if (unlikely(z->rune.code > ATLAS_RENDERABLE_END ||
                                             z->rune.combine[0])) {

                                    size_t         column = z - vt_line->data.buf;
                                    GlyphMapEntry* g = GfxOpenGL21_get_cached_glyph(gfx, &z->rune);
                                    if (!g) {
                                        continue;
                                    }

                                    double h   = scaley * g->tex.h;
                                    double w   = scalex * g->tex.w;
                                    double l   = scalex * g->left;
                                    double t   = scaley * g->top;
                                    double gsx = -0.1 / g->tex.w;
                                    double gsy = -0.05 / g->tex.h;
                                    if (g->color == GLYPH_COLOR_COLOR && unlikely(h > 2.0f)) {
                                        const float scale = h / 2.0f;
                                        h /= scale;
                                        w /= scale;
                                        t /= scale;
                                        l /= scale;
                                        gsx = 0;
                                        gsy = 0;
                                    }
                                    float x3 = -1.0f +
                                               (double)(column * gfx->glyph_width_pixels) * scalex +
                                               l + (scalex * 0.5);
                                    float y3 = -1.0f + (double)gfx->pen_begin_pixels * scaley - t +
                                               (scaley * 0.5);

                                    Vector_GlyphBufferData* target = &gfx->_vec_glyph_buffer;

                                    if (g->color == GLYPH_COLOR_COLOR) {
                                        target = &gfx->_vec_glyph_buffer_bold;
                                    } else if (g->color == GLYPH_COLOR_MONO) {
                                        target = &gfx->_vec_glyph_buffer_italic;
                                    }
                                    /* Very often characters like this are used to draw tables and
                                     * borders. Render all repeating characters in one call. */
                                    Vector_push_GlyphBufferData(
                                      target,
                                      (GlyphBufferData){
                                        { { x3, y3, 0.0f - gsx, 0.0f - gsy },
                                          { x3 + w, y3, 1.0f - gsx, 0.0f - gsy },
                                          { x3 + w, y3 + h, 1.0f - gsx, 1.0f - gsy },
                                          { x3, y3 + h, 0.0f - gsx, 1.0f - gsy } } });

                                    bool next_iteration_changes_texture =
                                      (z + 1 != each_rune_same_bg &&
                                       z->rune.code != (z + 1)->rune.code) ||
                                      z + 1 == each_rune_same_bg;

                                    if (next_iteration_changes_texture) {
                                        /* Set up the scissor box for this block */
                                        GLint clip_begin =
                                          (same_colors_block_begin_rune - vt_line->data.buf) *
                                          gfx->glyph_width_pixels;
                                        GLsizei clip_end = (each_rune_same_bg - vt_line->data.buf) *
                                                           gfx->glyph_width_pixels;
                                        glEnable(GL_SCISSOR_TEST);
                                        glScissor(clip_begin,
                                                  0,
                                                  clip_end - clip_begin,
                                                  texture_dims.second);

                                        // Draw noramal characters
                                        if (gfx->vec_glyph_buffer->size) {
                                            if (*bound_resources != BOUND_RESOURCES_FONT) {
                                                glUseProgram(gfx->font_shader.id);
                                                *bound_resources = BOUND_RESOURCES_FONT;
                                            }
                                            glUniform3f(gfx->font_shader.uniforms[1].location,
                                                        ColorRGB_get_float(active_fg_color, 0),
                                                        ColorRGB_get_float(active_fg_color, 1),
                                                        ColorRGB_get_float(active_fg_color, 2));
                                            glUniform4f(gfx->font_shader.uniforms[2].location,
                                                        ColorRGBA_get_float(active_bg_color, 0),
                                                        ColorRGBA_get_float(active_bg_color, 1),
                                                        ColorRGBA_get_float(active_bg_color, 2),
                                                        ColorRGBA_get_float(active_bg_color, 3));
                                            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                                            glVertexAttribPointer(
                                              gfx->font_shader.attribs->location,
                                              4,
                                              GL_FLOAT,
                                              GL_FALSE,
                                              0,
                                              0);
                                            size_t newsize =
                                              gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);
                                            ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer->buf,
                                                                     gfx->flex_vbo.size,
                                                                     newsize);
                                            glDrawArrays(GL_QUADS,
                                                         0,
                                                         gfx->vec_glyph_buffer->size * 4);
                                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                                        }
                                        // Draw color characters
                                        if (gfx->_vec_glyph_buffer_bold.size) {
                                            if (likely(*bound_resources != BOUND_RESOURCES_IMAGE)) {
                                                glUseProgram(gfx->image_shader.id);
                                                *bound_resources = BOUND_RESOURCES_IMAGE;
                                            }
                                            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                                            glVertexAttribPointer(
                                              gfx->image_shader.attribs->location,
                                              4,
                                              GL_FLOAT,
                                              GL_FALSE,
                                              0,
                                              0);
                                            size_t newsize = gfx->_vec_glyph_buffer_bold.size *
                                                             sizeof(GlyphBufferData);
                                            ARRAY_BUFFER_SUB_OR_SWAP(
                                              gfx->_vec_glyph_buffer_bold.buf,
                                              gfx->flex_vbo.size,
                                              newsize);
                                            glDrawArrays(GL_QUADS,
                                                         0,
                                                         gfx->_vec_glyph_buffer_bold.size * 4);
                                            gfx->_vec_glyph_buffer_bold.size = 0;
                                        }
                                        // draw mono characters
                                        if (gfx->_vec_glyph_buffer_italic.size) {
                                            if (likely(*bound_resources !=
                                                       BOUND_RESOURCES_FONT_MONO)) {
                                                glUseProgram(gfx->font_shader_gray.id);
                                                *bound_resources = BOUND_RESOURCES_FONT_MONO;
                                            }
                                            glUniform3f(gfx->font_shader_gray.uniforms[1].location,
                                                        ColorRGB_get_float(active_fg_color, 0),
                                                        ColorRGB_get_float(active_fg_color, 1),
                                                        ColorRGB_get_float(active_fg_color, 2));
                                            glUniform4f(gfx->font_shader_gray.uniforms[2].location,
                                                        ColorRGBA_get_float(active_bg_color, 0),
                                                        ColorRGBA_get_float(active_bg_color, 1),
                                                        ColorRGBA_get_float(active_bg_color, 2),
                                                        ColorRGBA_get_float(active_bg_color, 3));

                                            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                                            glVertexAttribPointer(
                                              gfx->font_shader_gray.attribs->location,
                                              4,
                                              GL_FLOAT,
                                              GL_FALSE,
                                              0,
                                              0);

                                            size_t newsize = gfx->_vec_glyph_buffer_italic.size *
                                                             sizeof(GlyphBufferData);
                                            ARRAY_BUFFER_SUB_OR_SWAP(
                                              gfx->_vec_glyph_buffer_italic.buf,
                                              gfx->flex_vbo.size,
                                              newsize);
                                            glDrawArrays(GL_QUADS,
                                                         0,
                                                         gfx->_vec_glyph_buffer_italic.size * 4);
                                            gfx->_vec_glyph_buffer_italic.size = 0;
                                        }
                                    }
                                } // end if out of atlas range
                            }     // end for each separate texture glyph
                        }         // end for each block with the same bg and fg

                        if (each_rune_same_bg != each_rune) {
                            same_colors_block_begin_rune = each_rune_same_bg;
                            // update active fg color;
                            if (settings.highlight_change_fg &&
                                unlikely(Vt_is_cell_selected(vt,
                                                             each_rune_same_bg - vt_line->data.buf,
                                                             visual_line_index))) {
                                active_fg_color = settings.fghl;
                            } else {
                                active_fg_color = L_CALC_DIM_BLEND_COLOR;
                            }
                        }
                    } // end for each block with the same color
                }     // end for each char
            }         // end for each block with the same bg

            bg_pixels_begin = (idx_each_rune + extra_width) * gfx->glyph_width_pixels;

            int clip_begin = idx_each_rune * gfx->glyph_width_pixels;
            glEnable(GL_SCISSOR_TEST);
            glScissor(clip_begin, 0, texture_dims.first, texture_dims.second);

            if (idx_each_rune != range.second) {
                same_bg_block_begin_rune = each_rune;
                // update active bg color;
                if (unlikely(Vt_is_cell_selected(vt, idx_each_rune, visual_line_index))) {
                    active_bg_color = settings.bghl;
                } else {
                    active_bg_color = each_rune->bg;
                }
            }
        } // end if bg color changed

        int w = likely(idx_each_rune != range.second)
                  ? wcwidth(vt_line->data.buf[idx_each_rune].rune.code)
                  : 1;
        idx_each_rune =
          CLAMP(idx_each_rune + (unlikely(w > 1) ? w : 1), range.first, (vt_line->data.size + 1));
    } // end for each VtRune
}

/**
 * (Re)generate 'proxy' texture(s) for a given VtLine
 *
 * @param is_for_blinking - this func calls itself with this set to true if the line contains
 * blinking characters
 *
 */
__attribute__((hot)) static inline void GfxOpenGL21_rasterize_line(GfxOpenGL21*    gfx,
                                                                   const Vt* const vt,
                                                                   VtLine*         vt_line,
                                                                   size_t visual_line_index,
                                                                   bool   is_for_blinking)
{
    if (likely(!is_for_blinking && vt_line->damage.type == VT_LINE_DAMAGE_NONE)) {
        return;
    }

    const size_t length               = vt_line->data.size;
    bool         has_blinking_chars   = false;
    uint32_t     texture_width        = length * gfx->glyph_width_pixels;
    uint32_t     actual_texture_width = texture_width;
    uint32_t     texture_height       = gfx->line_height_pixels;
    bool         has_underlined_chars = false;
    GLuint       final_texture        = 0;

    // Try to reuse the texture that is already there
    Texture recovered = {
        .id =
          vt_line->proxy.data[is_for_blinking ? PROXY_INDEX_TEXTURE_BLINK : PROXY_INDEX_TEXTURE],
        .w = vt_line->proxy.data[PROXY_INDEX_TEXTURE_SIZE],
    };

    bool can_reuse = recovered.id && recovered.w >= texture_width;

    /* TODO:
     * render the recovered texture onto the new one and set damage mode to remaining range? */
    if (!can_reuse) {
        vt_line->damage.type = VT_LINE_DAMAGE_FULL;
    }

    if (can_reuse) {
        final_texture        = recovered.id;
        actual_texture_width = recovered.w;
        glBindFramebuffer(GL_FRAMEBUFFER, gfx->line_framebuffer);
        glBindTexture(GL_TEXTURE_2D, recovered.id);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               recovered.id,
                               0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        glViewport(0, 0, recovered.w, recovered.h);
        gl_check_error();
    } else {
        if (!vt_line->data.size) {
            return;
        }
        if (!is_for_blinking) {
            GfxOpenGL21_destroy_proxy((void*)gfx - offsetof(Gfx, extend_data), vt_line->proxy.data);
        }

        GLuint  recycle_id    = gfx->recycled_textures[0].id;
        int32_t recycle_width = gfx->recycled_textures[0].width;
        if (recycle_id && recycle_width >= (int32_t)texture_width) {
            final_texture        = GfxOpenGL21_pop_recycled_texture(gfx);
            actual_texture_width = recycle_width;
            glBindFramebuffer(GL_FRAMEBUFFER, gfx->line_framebuffer);
            glBindTexture(GL_TEXTURE_2D, final_texture);
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D,
                                   final_texture,
                                   0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
            gl_check_error();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, gfx->line_framebuffer);
            glGenTextures(1, &final_texture);
            glBindTexture(GL_TEXTURE_2D, final_texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA,
                         texture_width,
                         texture_height,
                         0,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         0);
            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D,
                                   final_texture,
                                   0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
            gl_check_error();
        }
    }

    assert_farmebuffer_complete();

    glViewport(0, 0, texture_width, texture_height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));

    if (vt_line->damage.type == VT_LINE_DAMAGE_RANGE) {
        glEnable(GL_SCISSOR_TEST);
        size_t begin_px = gfx->glyph_width_pixels * vt_line->damage.front;
        size_t width_px =
          ((vt_line->damage.end + 1) - vt_line->damage.front) * gfx->glyph_width_pixels;
        glScissor(begin_px, 0, width_px, texture_height);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    // TODO: VT_LINE_DAMAGE_SHIFT

    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Keep track of gl state to avoid unnececery changes */
    int_fast8_t bound_resources = BOUND_RESOURCES_NONE;

    switch (vt_line->damage.type) {
        case VT_LINE_DAMAGE_RANGE: {
            size_t range_begin_idx = vt_line->damage.front;
            size_t range_end_idx   = vt_line->damage.end + 1;

            while (range_begin_idx) {
                char32_t this_char = vt_line->data.buf[range_begin_idx].rune.code;
                char32_t prev_char = vt_line->data.buf[range_begin_idx - 1].rune.code;
                if (this_char == ' ' && !unicode_is_private_use_area(prev_char) &&
                    wcwidth(prev_char) < 2) {
                    break;
                }
                --range_begin_idx;
            }

            while (range_end_idx < vt_line->data.size && range_end_idx &&
                   vt_line->data.buf[range_end_idx - 1].rune.code != ' ') {
                ++range_end_idx;
            }

            _GfxOpenGL21_rasterize_line_range(gfx,
                                              vt,
                                              vt_line,
                                              (Pair_size_t){ range_begin_idx, range_end_idx },
                                              visual_line_index,
                                              is_for_blinking,
                                              &bound_resources,
                                              (Pair_int32_t){ texture_width, texture_height },
                                              &has_blinking_chars,
                                              &has_underlined_chars);
            if (has_underlined_chars) {
                _GfxOpenGL21_rasterize_line_underline_range(
                  gfx,
                  vt_line,
                  (Pair_size_t){ range_begin_idx, range_end_idx },
                  &bound_resources,
                  (Pair_int32_t){ texture_width, texture_height });
            }
        } break;

        case VT_LINE_DAMAGE_SHIFT:
            // TODO:
        case VT_LINE_DAMAGE_FULL: {
            _GfxOpenGL21_rasterize_line_range(gfx,
                                              vt,
                                              vt_line,
                                              (Pair_size_t){ 0, length },
                                              visual_line_index,
                                              is_for_blinking,
                                              &bound_resources,
                                              (Pair_int32_t){ texture_width, texture_height },
                                              &has_blinking_chars,
                                              &has_underlined_chars);
            if (has_underlined_chars) {
                _GfxOpenGL21_rasterize_line_underline_range(
                  gfx,
                  vt_line,
                  (Pair_size_t){ 0, length },
                  &bound_resources,
                  (Pair_int32_t){ texture_width, texture_height });
            }
        } break;

        default:
            ASSERT_UNREACHABLE
    }

    // set proxy data to generated texture
    if (unlikely(is_for_blinking)) {
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] = final_texture;
    } else {
        vt_line->proxy.data[PROXY_INDEX_TEXTURE]      = final_texture;
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_SIZE] = actual_texture_width;
        vt_line->damage.type                          = VT_LINE_DAMAGE_NONE;
        vt_line->damage.shift                         = 0;
        vt_line->damage.front                         = 0;
        vt_line->damage.end                           = 0;
    }

    static float debug_tint = 0.0f;
    if (unlikely(settings.debug_gfx)) {
        glDisable(GL_SCISSOR_TEST);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        if (can_reuse) {
            glColor4f(0, 0, 0, 0);
        } else {
            glColor4f(fabs(sin(debug_tint)), fabs(cos(debug_tint)), sin(debug_tint), 0.1);
        }
        glVertex2f(1, 1);
        glVertex2f(-1, 1);
        glColor4f(fabs(sin(debug_tint)), fabs(cos(debug_tint)), sin(debug_tint), 0.1);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glEnd();
        glDisable(GL_BLEND);
        debug_tint += 0.5f;
        if (debug_tint > M_PI) {
            debug_tint -= M_PI;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gfx->win_w, gfx->win_h);

    if (!has_blinking_chars && vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]) {
        // TODO: recycle
        glDeleteTextures(1, (GLuint*)&vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]);
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] = 0;
    }

    if (unlikely(has_blinking_chars && !is_for_blinking)) {
        GfxOpenGL21_rasterize_line(gfx, vt, vt_line, visual_line_index, true);
    }
}

static inline void GfxOpenGL21_draw_cursor(GfxOpenGL21* gfx, const Vt* vt, const Ui* ui)
{
    if ((!vt->cursor.hidden &&
         (((ui->cursor->blinking && gfx->in_focus) ? gfx->draw_blinking
                                                   : true || gfx->recent_action) ||
          !settings.enable_cursor_blink))) {
        size_t row = ui->cursor->row - Vt_visual_top_line(vt), col = ui->cursor->col;
        bool   filled_block = false;
        Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
        Vector_clear_vertex_t(&gfx->vec_vertex_buffer);

        switch (vt->cursor.type) {
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
                if (!gfx->in_focus)
                    Vector_pushv_vertex_t(
                      &gfx->vec_vertex_buffer,
                      (vertex_t[4]){
                        { .x = -1.0f + (col * gfx->glyph_width_pixels) * gfx->sx + 0.9f * gfx->sx,
                          .y = 1.0f - ((row + 1) * gfx->line_height_pixels) * gfx->sy +
                               0.5f * gfx->sy },
                        { .x = -1.0f + ((col + 1) * gfx->glyph_width_pixels) * gfx->sx,
                          .y = 1.0f - ((row + 1) * gfx->line_height_pixels) * gfx->sy +
                               0.5f * gfx->sy },
                        { .x = -1.0f + ((col + 1) * gfx->glyph_width_pixels) * gfx->sx,
                          .y = 1.0f - (row * gfx->line_height_pixels) * gfx->sy - 0.5f * gfx->sy },
                        { .x = -1.0f + (col * gfx->glyph_width_pixels) * gfx->sx + 0.9f * gfx->sx,
                          .y = 1.0f - (row * gfx->line_height_pixels) * gfx->sy } },
                      4);
                else
                    filled_block = true;
                break;
        }

        ColorRGB *clr, *clr_bg;
        VtRune*   cursor_char = NULL;
        if (vt->lines.size > ui->cursor->row && vt->lines.buf[ui->cursor->row].data.size > col) {
            clr         = &vt->lines.buf[ui->cursor->row].data.buf[col].fg;
            clr_bg      = (ColorRGB*)&vt->lines.buf[ui->cursor->row].data.buf[col].bg;
            cursor_char = &vt->lines.buf[ui->cursor->row].data.buf[col];
        } else {
            clr = &settings.fg;
        }

        if (!filled_block) {
            Shader_use(&gfx->line_shader);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
            glVertexAttribPointer(gfx->line_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
            glUniform3f(gfx->line_shader.uniforms[1].location,
                        ColorRGB_get_float(*clr, 0),
                        ColorRGB_get_float(*clr, 1),
                        ColorRGB_get_float(*clr, 2));
            size_t newsize = gfx->vec_vertex_buffer.size * sizeof(vertex_t);
            ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer.buf, gfx->flex_vbo.size, newsize);
            glDrawArrays(gfx->vec_vertex_buffer.size == 2 ? GL_LINES : GL_LINE_LOOP,
                         0,
                         gfx->vec_vertex_buffer.size);
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(col * gfx->glyph_width_pixels + gfx->pixel_offset_x,
                      gfx->win_h - (row + 1) * gfx->line_height_pixels - gfx->pixel_offset_y,
                      gfx->glyph_width_pixels,
                      gfx->line_height_pixels);
            glClearColor(ColorRGB_get_float(*clr, 0),
                         ColorRGB_get_float(*clr, 1),
                         ColorRGB_get_float(*clr, 2),
                         1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (cursor_char && cursor_char->rune.code > ' ') {
                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                glVertexAttribPointer(gfx->font_shader.attribs->location,
                                      4,
                                      GL_FLOAT,
                                      GL_FALSE,
                                      0,
                                      0);
                Atlas* source_atlas = gfx->atlas;
                switch (expect(cursor_char->rune.style, VT_RUNE_NORMAL)) {
                    case VT_RUNE_ITALIC:
                        source_atlas = gfx->atlas_italic;
                        break;
                    case VT_RUNE_BOLD:
                        source_atlas = gfx->atlas_bold;
                        break;
                    case VT_RUNE_BOLD_ITALIC:
                        source_atlas = gfx->atlas_bold_italic;
                    default:;
                }
                enum GlyphColor color;
                float           h, w, t, l, gsx = 0.0, gsy = 0.0;
                float           tc[4]        = { 0.0f, 0.0f, 1.0f, 1.0f };
                int32_t         atlas_offset = Atlas_select(source_atlas, cursor_char->rune.code);
                if (atlas_offset >= 0) {
                    struct AtlasCharInfo* g = &source_atlas->char_info[atlas_offset];
                    h                       = (float)g->rows * gfx->sy;
                    w                       = (float)g->width * gfx->sx;
                    t                       = (float)g->top * gfx->sy;
                    l                       = (float)g->left * gfx->sx;
                    memcpy(tc, g->tex_coords, sizeof tc);
                    color = gfx->is_main_font_rgb ? GLYPH_COLOR_LCD : GLYPH_COLOR_MONO;
                } else {
                    GlyphMapEntry* g = GfxOpenGL21_get_cached_glyph(gfx, &cursor_char->rune);
                    h                = (float)g->tex.h * gfx->sy;
                    w                = (float)g->tex.w * gfx->sx;
                    t                = (float)g->top * gfx->sy;
                    l                = (float)g->left * gfx->sx;
                    gsx              = -0.1 / g->tex.w;
                    gsy              = -0.05 / g->tex.h;
                    color            = g->color;
                    if (unlikely(h > gfx->line_height)) {
                        const float scale = h / gfx->line_height;
                        h /= scale;
                        w /= scale;
                        t /= scale;
                        l /= scale;
                    }
                }
                float x3 =
                  -1.0f + (float)col * gfx->glyph_width_pixels * gfx->sx + l + (gfx->sx * 0.5);
                float y3 = +1.0f - gfx->pen_begin_pixels * gfx->sy -
                           (float)row * gfx->line_height_pixels * gfx->sy + t - (gfx->sy * 0.5);
                Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                Vector_push_GlyphBufferData(gfx->vec_glyph_buffer,
                                            (GlyphBufferData){ {
                                              { x3, y3, tc[0] - gsx, tc[1] - gsy },
                                              { x3 + w, y3, tc[2] - gsx, tc[1] - gsy },
                                              { x3 + w, y3 - h, tc[2] - gsx, tc[3] - gsy },
                                              { x3, y3 - h, tc[0] - gsx, tc[3] - gsy },
                                            } });
                size_t newsize = gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);
                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer->buf, gfx->flex_vbo.size, newsize);
                if (color == GLYPH_COLOR_LCD) {
                    glUseProgram(gfx->font_shader.id);
                    glUniform3f(gfx->font_shader.uniforms[1].location,
                                ColorRGB_get_float(*clr_bg, 0),
                                ColorRGB_get_float(*clr_bg, 1),
                                ColorRGB_get_float(*clr_bg, 2));
                    glUniform4f(gfx->font_shader.uniforms[2].location,
                                ColorRGB_get_float(*clr, 0),
                                ColorRGB_get_float(*clr, 1),
                                ColorRGB_get_float(*clr, 2),
                                1.0f);
                } else if (color == GLYPH_COLOR_MONO) {
                    glUseProgram(gfx->font_shader_gray.id);
                    glUniform3f(gfx->font_shader_gray.uniforms[1].location,
                                ColorRGB_get_float(*clr_bg, 0),
                                ColorRGB_get_float(*clr_bg, 1),
                                ColorRGB_get_float(*clr_bg, 2));
                    glUniform4f(gfx->font_shader_gray.uniforms[2].location,
                                ColorRGB_get_float(*clr, 0),
                                ColorRGB_get_float(*clr, 1),
                                ColorRGB_get_float(*clr, 2),
                                1.0f);
                } else {
                    glUseProgram(gfx->image_shader.id);
                }
                glDrawArrays(GL_QUADS, 0, 4);
            }
            glDisable(GL_SCISSOR_TEST);
        }
    }
}

static void GfxOpenGL21_draw_unicode_input(GfxOpenGL21* gfx, const Vt* vt)
{
    size_t begin = MIN(vt->cursor.col, vt->ws.ws_col - vt->unicode_input.buffer.size - 1);
    size_t row   = vt->cursor.row - Vt_visual_top_line(vt);
    size_t col   = begin;

    glEnable(GL_SCISSOR_TEST);
    glScissor(col * gfx->glyph_width_pixels + gfx->pixel_offset_x,
              gfx->win_h - (row + 1) * gfx->line_height_pixels - gfx->pixel_offset_y,
              gfx->glyph_width_pixels * (vt->unicode_input.buffer.size + 1),
              gfx->line_height_pixels);

    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gfx->font_shader.id);
    glUniform3f(gfx->font_shader.uniforms[1].location,
                ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1),
                ColorRGB_get_float(settings.fg, 2));
    glUniform4f(gfx->font_shader.uniforms[2].location,
                ColorRGBA_get_float(settings.bg, 0),
                ColorRGBA_get_float(settings.bg, 1),
                ColorRGBA_get_float(settings.bg, 2),
                ColorRGBA_get_float(settings.bg, 3));

    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
    glVertexAttribPointer(gfx->font_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);

    float tc[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float h, w, t, l;
    Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);

    int32_t               atlas_offset = Atlas_select(gfx->atlas, 'u');
    struct AtlasCharInfo* g            = &gfx->atlas->char_info[atlas_offset];

    h = (float)g->rows * gfx->sy;
    w = (float)g->width * gfx->sx;
    t = (float)g->top * gfx->sy;
    l = (float)g->left * gfx->sx;
    memcpy(tc, g->tex_coords, sizeof tc);

    float x3 = -1.0f + (float)col * gfx->glyph_width_pixels * gfx->sx + l;
    float y3 =
      +1.0f - gfx->pen_begin_pixels * gfx->sy - (float)row * gfx->line_height_pixels * gfx->sy + t;

    Vector_push_GlyphBufferData(gfx->vec_glyph_buffer,
                                (GlyphBufferData){ {
                                  { x3, y3, tc[0], tc[1] },
                                  { x3 + w, y3, tc[2], tc[1] },
                                  { x3 + w, y3 - h, tc[2], tc[3] },
                                  { x3, y3 - h, tc[0], tc[3] },
                                } });
    vertex_t lnbuf[] = {
        { -1.0f + (float)col * gfx->glyph_width_pixels * gfx->sx,
          +1.0f - gfx->pen_begin_pixels * gfx->sy -
            (float)row * gfx->line_height_pixels * gfx->sy },
        { -1.0f + (float)(col + 1) * gfx->glyph_width_pixels * gfx->sx,
          +1.0f - gfx->pen_begin_pixels * gfx->sy -
            (float)row * gfx->line_height_pixels * gfx->sy },
    };

    for (size_t i = 0; i < vt->unicode_input.buffer.size; ++i) {
        atlas_offset = Atlas_select(gfx->atlas, vt->unicode_input.buffer.buf[i]);
        g            = &gfx->atlas->char_info[atlas_offset];
        h            = (float)g->rows * gfx->sy;
        w            = (float)g->width * gfx->sx;
        t            = (float)g->top * gfx->sy;
        l            = (float)g->left * gfx->sx;
        memcpy(tc, g->tex_coords, sizeof tc);

        x3 = -1.0f + (float)(col + i + 1) * gfx->glyph_width_pixels * gfx->sx + l;
        y3 = +1.0f - gfx->pen_begin_pixels * gfx->sy -
             (float)row * gfx->line_height_pixels * gfx->sy + t;

        Vector_push_GlyphBufferData(gfx->vec_glyph_buffer,
                                    (GlyphBufferData){ {
                                      { x3, y3, tc[0], tc[1] },
                                      { x3 + w, y3, tc[2], tc[1] },
                                      { x3 + w, y3 - h, tc[2], tc[3] },
                                      { x3, y3 - h, tc[0], tc[3] },
                                    } });
    }

    size_t newsize = gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);
    if (newsize > gfx->flex_vbo.size) {
        gfx->flex_vbo.size = newsize;
        glBufferData(GL_ARRAY_BUFFER, newsize, gfx->vec_glyph_buffer->buf, GL_STREAM_DRAW);
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, newsize, gfx->vec_glyph_buffer->buf);
    }
    glDrawArrays(GL_QUADS, 0, 4 * (vt->unicode_input.buffer.size + 1));
    glVertexAttribPointer(gfx->line_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
    newsize = sizeof(lnbuf);
    if (newsize > gfx->flex_vbo.size) {
        gfx->flex_vbo.size = newsize;
        glBufferData(GL_ARRAY_BUFFER, newsize, lnbuf, GL_STREAM_DRAW);
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, newsize, lnbuf);
    }
    glUseProgram(gfx->line_shader.id);
    glUniform3f(gfx->line_shader.uniforms[1].location,
                ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1),
                ColorRGB_get_float(settings.fg, 2));
    glDrawArrays(GL_LINES, 0, 2);
    glDisable(GL_SCISSOR_TEST);
}

static void GfxOpenGL21_draw_scrollbar(GfxOpenGL21* self, const Scrollbar* scrollbar)
{
    // TODO: use VBOs
    Shader_use(NULL);
    glViewport(0, 0, self->win_w, self->win_h);
    glBindTexture(GL_TEXTURE_2D, 0);

    float length = scrollbar->length;
    float begin  = scrollbar->top;
    float width  = self->sx * scrollbar->width;

    glBegin(GL_QUADS);
    glColor4f(1, 1, 1, scrollbar->dragging ? 0.8f : scrollbar->opacity * 0.5f);
    glVertex2f(1.0f - width, 1.0f - begin);
    glVertex2f(1.0f, 1.0f - begin);
    glVertex2f(1.0f, 1.0f - length - begin);
    glVertex2f(1.0f - width, 1.0f - length - begin);
    glEnd();
}

static void GfxOpenGL21_draw_overlays(GfxOpenGL21* self, const Vt* vt, const Ui* ui)
{
    if (vt->unicode_input.active) {
        GfxOpenGL21_draw_unicode_input(self, vt);
    } else if (!vt->scrolling_visual) {
        GfxOpenGL21_draw_cursor(self, vt, ui);
    }
    if (ui->scrollbar.visible) {
        GfxOpenGL21_draw_scrollbar(self, &ui->scrollbar);
    }
}

static void GfxOpenGL21_draw_flash(GfxOpenGL21* self, float fraction)
{
    // TODO: use VBOs
    glViewport(0, 0, self->win_w, self->win_h);
    Shader_use(NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBegin(GL_QUADS);
    glColor4f(1, 1, 1, sinf((1.0 - fraction) * M_1_PI) / 4.0);
    glVertex2f(1, 1);
    glVertex2f(-1, 1);
    glVertex2f(-1, -1);
    glVertex2f(1, -1);
    glEnd();
}

void GfxOpenGL21_draw(Gfx* self, const Vt* vt, Ui* ui)
{
    GfxOpenGL21* gfx    = gfxOpenGL21(self);
    gfx->pixel_offset_x = ui->pixel_offset_x;
    gfx->pixel_offset_y = ui->pixel_offset_y;

    VtLine *begin, *end;
    Vt_get_visible_lines(vt, &begin, &end);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, gfx->win_w, gfx->win_h);
    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));
    glClear(GL_COLOR_BUFFER_BIT);
    for (VtLine* i = begin; i < end; ++i) {
        GfxOpenGL21_rasterize_line(gfx, vt, i, i - begin, false);
    }
    glDisable(GL_BLEND);
    glEnable(GL_SCISSOR_TEST);
    Pair_uint32_t chars = Gfx_get_char_size(self);
    if (vt->scrolling_visual) {
        glScissor(gfx->pixel_offset_x,
                  gfx->pixel_offset_y,
                  chars.first * gfx->glyph_width_pixels,
                  gfx->win_h);
    } else {
        glScissor(gfx->pixel_offset_x,
                  gfxOpenGL21(self)->win_h - chars.second * gfx->line_height_pixels -
                    gfx->pixel_offset_y,
                  chars.first * gfx->glyph_width_pixels,
                  chars.second * gfx->line_height_pixels);
    }
    glLoadIdentity();
    Vector_clear_GlyphBufferData(gfxOpenGL21(self)->vec_glyph_buffer);
    gfxOpenGL21(self)->has_blinking_text = false;
    for (VtLine* i = begin; i < end; ++i) {
        GfxOpenGL21_generate_line_quads(gfx, i, i - begin);
    }
    glViewport(gfx->pixel_offset_x, -gfx->pixel_offset_y, gfx->win_w, gfx->win_h);
    if (gfxOpenGL21(self)->vec_glyph_buffer->size) {
        glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
        size_t newsize = gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);
        if (newsize > gfx->flex_vbo.size) {
            gfx->flex_vbo.size = newsize;
            glBufferData(GL_ARRAY_BUFFER, newsize, gfx->vec_glyph_buffer->buf, GL_STREAM_DRAW);
        } else {
            glBufferSubData(GL_ARRAY_BUFFER, 0, newsize, gfx->vec_glyph_buffer->buf);
        }
        Shader_use(&gfx->image_shader);
        glVertexAttribPointer(gfx->image_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0, 0);
        uint_fast32_t quad_index = 0;
        for (VtLine* i = begin; i < end; ++i) {
            quad_index = GfxOpenGL21_draw_line_quads(gfx, i, quad_index);
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    GfxOpenGL21_draw_overlays(gfx, vt, ui);
    if (gfx->flash_fraction != 1.0) {
        GfxOpenGL21_draw_flash(gfx, gfx->flash_fraction);
    }
    static bool repaint_indicator_visible = true;
    if (unlikely(settings.debug_gfx)) {
        if (repaint_indicator_visible) {
            Shader_use(NULL);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBegin(GL_TRIANGLES);
            glColor4f(1, 1, 1, 0.7);
            glVertex2f(-1.0, 1);
            glColor4f(1, 1, 1, 0.0);
            glVertex2f(-1.0 + gfx->sx * 50, 1);
            glVertex2f(-1.0, 1.0 - gfx->sy * 50);
            glEnd();
        }
        repaint_indicator_visible = !repaint_indicator_visible;
    }
}

void GfxOpenGL21_destroy_recycled_proxies(GfxOpenGL21* self)
{
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(self->recycled_textures); ++i) {
        if (self->recycled_textures[i].id) {
            glDeleteTextures(1, &self->recycled_textures[i].id);
        }
        self->recycled_textures[i].id    = 0;
        self->recycled_textures[i].width = 0;
    }
}

void GfxOpenGL21_push_recycled_texture(GfxOpenGL21* self, GLuint tex_id, int32_t width)
{
    uint_fast8_t insert_point;
    for (insert_point = 0; insert_point < N_RECYCLED_TEXTURES; ++insert_point) {
        if (width > self->recycled_textures[insert_point].width) {
            if (likely(ARRAY_LAST(self->recycled_textures).id)) {
                glDeleteTextures(1, &ARRAY_LAST(self->recycled_textures).id);
            }
            void*  src = self->recycled_textures + insert_point;
            void*  dst = self->recycled_textures + insert_point + 1;
            size_t n = sizeof(*self->recycled_textures) * (N_RECYCLED_TEXTURES - insert_point - 1);
            memmove(dst, src, n);
            self->recycled_textures[insert_point].id    = tex_id;
            self->recycled_textures[insert_point].width = width;
            return;
        }
    }
    glDeleteTextures(1, &tex_id);
}

GLuint GfxOpenGL21_pop_recycled_texture(GfxOpenGL21* self)
{
    GLuint ret = self->recycled_textures[0].id;
    memmove(self->recycled_textures,
            self->recycled_textures + 1,
            sizeof(*self->recycled_textures) * (N_RECYCLED_TEXTURES - 1));
    ARRAY_LAST(self->recycled_textures).id    = 0;
    ARRAY_LAST(self->recycled_textures).width = 0;

    return ret;
}

__attribute__((hot)) void GfxOpenGL21_destroy_proxy(Gfx* self, int32_t* proxy)
{
    /* try to store for reuse */
    if (unlikely(proxy[PROXY_INDEX_TEXTURE] &&
                 ARRAY_LAST(gfxOpenGL21(self)->recycled_textures).width <
                   proxy[PROXY_INDEX_TEXTURE_SIZE])) {

        GfxOpenGL21_push_recycled_texture(gfxOpenGL21(self),
                                          proxy[PROXY_INDEX_TEXTURE],
                                          proxy[PROXY_INDEX_TEXTURE_SIZE]);

        if (unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK])) {
            GfxOpenGL21_push_recycled_texture(gfxOpenGL21(self),
                                              proxy[PROXY_INDEX_TEXTURE_BLINK],
                                              proxy[PROXY_INDEX_TEXTURE_SIZE]);
        }
    } else if (likely(proxy[PROXY_INDEX_TEXTURE])) {
        /* delete starting from first */
        glDeleteTextures(unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK]) ? 2 : 1,
                         (GLuint*)&proxy[PROXY_INDEX_TEXTURE]);
    } else if (unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK])) {
        glDeleteTextures(1, (GLuint*)&proxy[PROXY_INDEX_TEXTURE_BLINK]);
    }
    proxy[PROXY_INDEX_TEXTURE]       = 0;
    proxy[PROXY_INDEX_TEXTURE_BLINK] = 0;
    proxy[PROXY_INDEX_TEXTURE_SIZE]  = 0;
}

void GfxOpenGL21_destroy(Gfx* self)
{
    GfxOpenGL21_destroy_recycled_proxies(gfxOpenGL21(self));

    Atlas_destroy(gfxOpenGL21(self)->atlas);
    if (settings.font_file_name_bold.str) {
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_bold);
    }
    if (settings.font_file_name_italic.str) {
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_italic);
    }
    if (settings.font_file_name_bold_italic.str) {
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_bold_italic);
    }

    Map_destroy_Rune_GlyphMapEntry(&(gfxOpenGL21(self)->glyph_cache));

    glDeleteFramebuffers(1, &gfxOpenGL21(self)->line_framebuffer);

    VBO_destroy(&gfxOpenGL21(self)->font_vao);
    VBO_destroy(&gfxOpenGL21(self)->bg_vao);

    Shader_destroy(&gfxOpenGL21(self)->font_shader);
    Shader_destroy(&gfxOpenGL21(self)->font_shader_blend);
    Shader_destroy(&gfxOpenGL21(self)->bg_shader);
    Shader_destroy(&gfxOpenGL21(self)->line_shader);
    Shader_destroy(&gfxOpenGL21(self)->image_shader);
    Shader_destroy(&gfxOpenGL21(self)->image_tint_shader);

    Vector_destroy_GlyphBufferData(&gfxOpenGL21(self)->_vec_glyph_buffer);

    if (gfxOpenGL21(self)->vec_glyph_buffer_bold) {
        Vector_destroy_GlyphBufferData(&gfxOpenGL21(self)->_vec_glyph_buffer_bold);
    }

    if (gfxOpenGL21(self)->vec_glyph_buffer_italic) {
        Vector_destroy_GlyphBufferData(&gfxOpenGL21(self)->_vec_glyph_buffer_italic);
    }

    if (gfxOpenGL21(self)->vec_glyph_buffer_bold_italic) {
        Vector_destroy_GlyphBufferData(&gfxOpenGL21(self)->_vec_glyph_buffer_bold_italic);
    }

    Vector_destroy_vertex_t(&(gfxOpenGL21(self)->vec_vertex_buffer));
    Vector_destroy_vertex_t(&(gfxOpenGL21(self)->vec_vertex_buffer2));
}
