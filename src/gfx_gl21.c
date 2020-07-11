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

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_TRUETYPE_TABLES_H
#include FT_BITMAP_H
#include FT_FONT_FORMATS_H

#include "fterrors.h"
#include "gl.h"
#include "shaders.h"
#include "util.h"
#include "wcwidth/wcwidth.h"

/* number of buckets in the non-ascii glyph map */
#define NUM_BUCKETS 64

#ifndef ATLAS_SIZE_LIMIT
#define ATLAS_SIZE_LIMIT INT32_MAX
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
    GLint max_tex_res;

    Vector_vertex_t vec_vertex_buffer;
    Vector_vertex_t vec_vertex_buffer2;

    Vector_GlyphBufferData _vec_glyph_buffer;
    Vector_GlyphBufferData _vec_glyph_buffer_italic;
    Vector_GlyphBufferData _vec_glyph_buffer_bold;

    Vector_GlyphBufferData* vec_glyph_buffer;
    Vector_GlyphBufferData* vec_glyph_buffer_italic;
    Vector_GlyphBufferData* vec_glyph_buffer_bold;

    VBO flex_vbo;
    VBO flex_vbo_italic;
    VBO flex_vbo_bold;

    /* pen position to begin drawing font */
    float pen_begin;
    float pen_begin_pixels;

    uint32_t win_w, win_h;

    FT_Library   ft;
    FT_Face      face;
    FT_Face      face_bold;
    FT_Face      face_italic;
    FT_Face      face_fallback;
    FT_Face      face_fallback2;
    FT_GlyphSlot g;

    float    line_height, glyph_width;
    uint16_t line_height_pixels, glyph_width_pixels;
    size_t   max_cells_in_line;

    float    sx, sy;
    uint32_t gw;

    Framebuffer line_framebuffer;

    VBO font_vao;
    VBO bg_vao;
    VBO line_vao;

    VBO line_bg_vao;

    Shader font_shader;
    Shader font_shader_gray;
    Shader bg_shader;
    Shader line_shader;
    Shader image_shader;
    Shader image_tint_shader;

    ColorRGB  color;
    ColorRGBA bg_color;

    GlyphMap _cache;
    GlyphMap _cache_bold;
    GlyphMap _cache_italic;

    GlyphMap* cache;
    GlyphMap* cache_bold;
    GlyphMap* cache_italic;

    Atlas  _atlas;
    Atlas  _atlas_bold;
    Atlas  _atlas_italic;
    Atlas* atlas;
    Atlas* atlas_bold;
    Atlas* atlas_italic;

    // keep textures for reuse in order of length
    Texture recycled_line_textures[3];

    Texture squiggle_texture;

    bool      has_blinking_text;
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

    int scrollbar_fade;

    Timer flash_timer;
    float flash_fraction;

} GfxOpenGL21;

#define gfxOpenGL21(gfx) ((GfxOpenGL21*)&gfx->extend_data)

void GfxOpenGL21_load_font(Gfx* self);
void GfxOpenGL21_destroy_recycled_proxies(GfxOpenGL21* self);

void          GfxOpenGL21_destroy(Gfx* self);
void          GfxOpenGL21_draw(Gfx* self, const Vt* vt, Ui* ui);
Pair_uint32_t GfxOpenGL21_get_char_size(Gfx* self);
void          GfxOpenGL21_resize(Gfx* self, uint32_t w, uint32_t h);
void          GfxOpenGL21_init_with_context_activated(Gfx* self);
bool          GfxOpenGL21_update_timers(Gfx* self, Vt* vt, Ui* ui);
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

Gfx* Gfx_new_OpenGL21()
{
    Gfx* self       = calloc(1, sizeof(Gfx) + sizeof(GfxOpenGL21) - sizeof(uint8_t));
    self->interface = &gfx_interface_opengl21;

    gfxOpenGL21(self)->is_main_font_rgb = settings.lcd_filter != LCD_FILTER_NONE;
    GfxOpenGL21_load_font(self);

    return self;
}

#define ARRAY_BUFFER_SUB_OR_SWAP(_buf, _size, _newsize)                                            \
    if ((_newsize) > _size) {                                                                      \
        _size = (_newsize);                                                                        \
        glBufferData(GL_ARRAY_BUFFER, (_newsize), _buf, GL_STREAM_DRAW);                           \
    } else {                                                                                       \
        glBufferSubData(GL_ARRAY_BUFFER, 0, (_newsize), (_buf));                                   \
    }

void GfxOpenGL21_flash(Gfx* self)
{
    if (!settings.no_flash)
        gfxOpenGL21(self)->flash_timer = Timer_from_now_to_ms_from_now(300);
}

/**
 * @return offset into info buffer */
__attribute__((always_inline, hot)) static inline int32_t Atlas_select(Atlas* self, char32_t code)
{
    if (code < 32 || code > CHAR_MAX)
        return -1;
    else {
        glBindTexture(GL_TEXTURE_2D, self->tex);
        return code - 32;
    }
}

static Atlas Atlas_new(GfxOpenGL21* gfx, FT_Face face_)
{
    Atlas self;

    self.w = self.h = 0;
    uint32_t wline = 0, hline = 0, limit = MIN(gfx->max_tex_res, ATLAS_SIZE_LIMIT);

    uint32_t max_char_height  = 0;
    bool     bitmap_is_packed = false;

    for (int i = ATLAS_RENDERABLE_START + 1; i < ATLAS_RENDERABLE_END; i++) {
        if (FT_Load_Char(face_, i,
                         gfx->is_main_font_rgb ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_MONO))
            WRN("glyph load error\n");

        gfx->g = face_->glyph;

        uint32_t char_width  = gfx->g->bitmap.width / (gfx->is_main_font_rgb ? 3 : 1);
        uint32_t char_height = gfx->g->bitmap.rows;

        max_char_height = MAX(max_char_height, char_height);

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

    // monochrome bitmap uses 1 bit per pixel
    bitmap_is_packed = gfx->g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO;

    if (wline > self.w)
        self.w = wline;

    self.h += hline;

    if (self.h > (uint32_t)gfx->max_tex_res)
        ERR("Failed to generate font atlas, target texture to small");

    glActiveTexture(GL_TEXTURE0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, gfx->is_main_font_rgb ? 4 : 1);
    glGenTextures(1, &self.tex);
    glBindTexture(GL_TEXTURE_2D, self.tex);

    // faster than clamp
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(GL_TEXTURE_2D, 0, gfx->is_main_font_rgb ? GL_RGBA : GL_RED, self.w, self.h, 0,
                 gfx->is_main_font_rgb ? GL_RGB : GL_RED, GL_UNSIGNED_BYTE, 0);

    hline                 = 0;
    uint32_t offset_x_pix = 0, offset_y_pix = 0;

    FT_Bitmap conversion_map;

    if (bitmap_is_packed) {
        FT_Bitmap_Init(&conversion_map);
    }

    for (int i = ATLAS_RENDERABLE_START + 1; i < ATLAS_RENDERABLE_END; i++) {

        if (FT_Load_Char(face_, i,
                         gfx->is_main_font_rgb ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL)) {
            WRN("glyph render error\n");
        }

        if (FT_Render_Glyph(face_->glyph,
                            gfx->is_main_font_rgb ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL)) {
            WRN("glyph render error\n");
        }

        gfx->g = face_->glyph;

        if (bitmap_is_packed) {
            if (FT_Bitmap_Convert(gfx->ft, &gfx->g->bitmap, &conversion_map, 1)) {
                WRN("FT bitmap conversion failed\n");
            }

            // why does FT not do that?!
            for (uint32_t i = 0; i < (conversion_map.width * conversion_map.rows); ++i) {
                if (conversion_map.buffer[i])
                    conversion_map.buffer[i] = UINT8_MAX;
            }
        }

        uint32_t char_width = (bitmap_is_packed ? conversion_map.width : gfx->g->bitmap.width) /
                              (gfx->is_main_font_rgb ? 3 : 1);
        uint32_t char_height = bitmap_is_packed ? conversion_map.rows : gfx->g->bitmap.rows;

        if (offset_x_pix + char_width > self.w) {
            offset_y_pix += hline;
            offset_x_pix = 0;
            hline        = char_height;
        } else {
            hline = char_height > hline ? char_height : hline;
        }

        glTexSubImage2D(GL_TEXTURE_2D, 0, offset_x_pix, offset_y_pix, char_width, char_height,
                        gfx->is_main_font_rgb ? GL_RGB : GL_RED, GL_UNSIGNED_BYTE,
                        bitmap_is_packed ? conversion_map.buffer : gfx->g->bitmap.buffer);

        self.char_info[i - ATLAS_RENDERABLE_START] = (struct AtlasCharInfo){
            .rows       = char_height,
            .width      = char_width,
            .left       = (float)gfx->g->bitmap_left,
            .top        = (float)gfx->g->bitmap_top,
            .tex_coords = { ((float)offset_x_pix + 0.01) / self.w,

                            1.0f - (((float)self.h - ((float)offset_y_pix + 0.01)) / self.h),

                            ((float)offset_x_pix + 0.01) / self.w + (float)char_width / self.w,

                            1.0f - (((float)self.h - ((float)offset_y_pix + 0.01)) / self.h -
                                    (float)char_height / self.h) }
        };

        offset_x_pix += char_width;
    }

    if (bitmap_is_packed) {
        FT_Bitmap_Done(gfx->ft, &conversion_map);
    }

    return self;
}

static void GlyphMapEntry_destroy(GlyphMapEntry* self)
{
    Texture_destroy(&self->tex);
}

static void Cache_init(GlyphMap* c)
{
    for (size_t i = 0; i < NUM_BUCKETS; ++i)
        c->buckets[i] = Vector_new_GlyphMapEntry();
}

static inline Vector_GlyphMapEntry* Cache_select_bucket(GlyphMap* self, uint32_t code)
{
    return &self->buckets[(NUM_BUCKETS - 1) % code];
}

static inline void Cache_destroy(GlyphMap* self)
{
    for (int i = 0; i < NUM_BUCKETS; ++i)
        Vector_destroy_GlyphMapEntry(&self->buckets[i]);
}

__attribute__((hot, flatten)) static GlyphMapEntry* Cache_get_glyph(GfxOpenGL21* gfx,
                                                                    GlyphMap*    self,
                                                                    FT_Face      face,
                                                                    char32_t     code)
{
    Vector_GlyphMapEntry* block = Cache_select_bucket(self, code);

    GlyphMapEntry* found = NULL;
    while ((found = Vector_iter_GlyphMapEntry(block, found))) {
        if (found->code == code) {
            glBindTexture(GL_TEXTURE_2D, found->tex.id);
            return found;
        }
    }

    bool     bitmap_is_packed = false;
    FT_Error e;
    bool     color         = false;
    bool     fallback_font = false;
    int32_t  index         = FT_Get_Char_Index(face, code);

    if (FT_Load_Glyph(face, index,
                      gfx->is_main_font_rgb ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL) ||
        FT_Render_Glyph(face->glyph,
                        gfx->is_main_font_rgb ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL)) {
        WRN("Glyph error in main font %d \n", code);
    } else if (face->glyph->glyph_index == 0 && gfx->face_fallback) {
        // Glyph is missing im main font

        index = FT_Get_Char_Index(gfx->face_fallback, code);
        if (FT_Load_Glyph(gfx->face_fallback, index, FT_LOAD_TARGET_LCD) ||
            FT_Render_Glyph(gfx->face_fallback->glyph, FT_RENDER_MODE_LCD)) {
            WRN("Glyph error in fallback font %d \n", code);
        } else if (gfx->face_fallback->glyph->glyph_index == 0 && gfx->face_fallback2) {
            color = true;
            self  = gfx->cache; // put this in the 'Regular style' map
            index = FT_Get_Char_Index(gfx->face_fallback2, code);

            if ((e = FT_Load_Glyph(gfx->face_fallback2, index, FT_LOAD_COLOR))) {
                WRN("Glyph load error2 %d %s | %d (%d)\n", e, stringify_ft_error(e), code, index);
            } else if ((e = FT_Render_Glyph(gfx->face_fallback2->glyph, FT_RENDER_MODE_NORMAL)))
                WRN("Glyph render error2 %d %s | %d (%d)\n", e, stringify_ft_error(e), code, index);
            if (gfx->face_fallback2->glyph->glyph_index == 0) {
                WRN("Missing glyph %d\n", code);
            }
            gfx->g        = gfx->face_fallback2->glyph;
            fallback_font = true;
        } else {
            gfx->g        = gfx->face_fallback->glyph;
            fallback_font = true;
        }
    } else
        gfx->g = face->glyph;

    // In general color characters don't scale
    // exclude box drawing characters
    if (gfx->g->bitmap.rows > gfx->line_height_pixels && (code < 0x2500 || code > 0x257f)) {
        color = true;
    }

    bitmap_is_packed = gfx->g->bitmap.pixel_mode == FT_PIXEL_MODE_MONO;

    // rgb textures are packed the same as rgba
    int alignment_unpack = (gfx->is_main_font_rgb || fallback_font || color) ? 4 : 1;

    // rgb texture width i 1/3 of its bitmap width
    int actual_width =
      gfx->g->bitmap.width / (!color && (fallback_font || gfx->is_main_font_rgb) ? 3 : 1);

    FT_Bitmap conversion_map;
    if (bitmap_is_packed) {
        FT_Bitmap_Init(&conversion_map);

        if (FT_Bitmap_Convert(gfx->ft, &gfx->g->bitmap, &conversion_map, 1)) {
            WRN("Conversion failed\n");
        }

        // why does FT not do that?!
        for (uint32_t i = 0; i < (conversion_map.width * conversion_map.rows); ++i) {
            if (conversion_map.buffer[i])
                conversion_map.buffer[i] = UINT8_MAX;
        }
    }

    Texture tex = { .id = 0,
                    .format =
                      color ? TEX_FMT_RGB : gfx->is_main_font_rgb ? TEX_FMT_RGBA : TEX_FMT_MONO,
                    .w = actual_width,
                    .h = gfx->g->bitmap.rows };

    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment_unpack);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    color ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0,

                 color ? GL_RGBA : (fallback_font || gfx->is_main_font_rgb) ? GL_RGB : GL_RED,

                 (tex.w = actual_width), (tex.h = gfx->g->bitmap.rows), 0,

                 // FT always renders in BGR,
                 // TODO: we can use RGB to flip subpixel order
                 color ? GL_BGRA : (fallback_font || gfx->is_main_font_rgb) ? GL_RGB : GL_RED,
                 GL_UNSIGNED_BYTE,
                 bitmap_is_packed ? conversion_map.buffer : gfx->g->bitmap.buffer);

    if (bitmap_is_packed) {
        FT_Bitmap_Done(gfx->ft, &conversion_map);
    }

    if (color) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }

    Vector_push_GlyphMapEntry(
      block,
      (GlyphMapEntry){
        .code  = code,
        .color = color ? GLYPH_COLOR_COLOR : bitmap_is_packed ? GLYPH_COLOR_MONO : GLYPH_COLOR_LCD,
        .left  = (float)gfx->g->bitmap_left,
        .top   = (float)gfx->g->bitmap_top,
        .tex   = tex,
      });

    return Vector_at_GlyphMapEntry(block, block->size - 1);
}

// Generate a sinewave image and store it as an OpenGL texture
__attribute__((cold)) static Texture create_squiggle_texture(uint32_t w,
                                                             uint32_t h,
                                                             uint32_t thickness)
{
    GLuint tex;
    glEnable(GL_TEXTURE_2D);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    uint8_t* fragments = calloc(w * h * 4, 1);

    double pixel_size                = 2.0 / h;
    double stroke_width              = thickness * pixel_size;
    double stroke_fade               = pixel_size * 1.5;
    double distance_limit_full_alpha = POW2(stroke_width / 2.0);
    double distance_limit_zero_alpha = POW2(stroke_width / 2.0 + stroke_fade);

    for (uint_fast32_t x = 0; x < w; ++x)
        for (uint_fast32_t y = 0; y < h; ++y) {
            uint8_t* fragment = &fragments[(y * w + x) * 4];

#define DISTANCE_SQR(_x, _y, _x2, _y2) (pow((_x2) - (_x), 2) + pow((_y2) - (_y), 2))

            double x_frag = (double)x / (double)w * 2.0 * M_PI;
            double y_frag = (double)y / (double)h * (2.0 + stroke_width * 2.0 + stroke_fade * 2.0) -
                            1.0 - stroke_width - stroke_fade;

            double y_curve = sin(x_frag);

            // d/dx -> in what dir is closest point
            double dx_frag          = cos(x_frag);
            double y_dist           = y_frag - y_curve;
            double closest_distance = DISTANCE_SQR(x_frag, y_frag, x_frag, y_curve);

            double step = dx_frag * y_dist < 0.0 ? 0.01 : -0.01;

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

    return (Texture){ .id = tex, .format = TEX_FMT_RGBA, .w = w, .h = h };
}

void GfxOpenGL21_resize(Gfx* self, uint32_t w, uint32_t h)
{
    GfxOpenGL21_destroy_recycled_proxies(gfxOpenGL21(self));

    if (FT_Load_Char(gfxOpenGL21(self)->face, '>', FT_LOAD_TARGET_LCD) ||
        FT_Render_Glyph(gfxOpenGL21(self)->face->glyph, FT_RENDER_MODE_LCD)) {
        WRN("Glyph error\n");
    }

    if (!gfxOpenGL21(self)->face->size->metrics.height ||
        !gfxOpenGL21(self)->face->glyph->advance.x) {
        ERR("Font error, reported size is NULL");
    }

    gfxOpenGL21(self)->win_w = w;
    gfxOpenGL21(self)->win_h = h;

    glViewport(0, 0, w, h);

    gfxOpenGL21(self)->sx = 2.0f / gfxOpenGL21(self)->win_w;
    gfxOpenGL21(self)->sy = 2.0f / gfxOpenGL21(self)->win_h;

    uint32_t height = gfxOpenGL21(self)->face->size->metrics.height + 128;
    uint32_t hber   = gfxOpenGL21(self)->face->glyph->metrics.horiBearingY - 128;

    gfxOpenGL21(self)->line_height_pixels = height / 64.0;

    gfxOpenGL21(self)->line_height = (float)height * gfxOpenGL21(self)->sy / 64.0;

    gfxOpenGL21(self)->pen_begin =
      gfxOpenGL21(self)->sy * (height / 64.0 / 2.0) + gfxOpenGL21(self)->sy * ((hber) / 2.0 / 64.0);
    gfxOpenGL21(self)->pen_begin_pixels =
      (float)(height / 64.0 / 1.75) + (float)((hber) / 2.0 / 64.0);

    gfxOpenGL21(self)->gw                 = gfxOpenGL21(self)->face->glyph->advance.x;
    gfxOpenGL21(self)->glyph_width_pixels = gfxOpenGL21(self)->gw / 64;
    gfxOpenGL21(self)->glyph_width        = gfxOpenGL21(self)->gw * gfxOpenGL21(self)->sx / 64.0;

    LOG("glyph box size GL: %fx%f, pixels: %dx%d\n", gfxOpenGL21(self)->glyph_width,
        gfxOpenGL21(self)->line_height, gfxOpenGL21(self)->glyph_width_pixels,
        gfxOpenGL21(self)->line_height_pixels);

    gfxOpenGL21(self)->max_cells_in_line =
      gfxOpenGL21(self)->win_w / gfxOpenGL21(self)->glyph_width_pixels;

    // update dynamic bg buffer
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->bg_vao.vbo);

    glVertexAttribPointer(gfxOpenGL21(self)->bg_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0,
                          0);

    float bg_box[] = {
        0.0f,
        0.0f,
        0.0f,
        gfxOpenGL21(self)->line_height,
        gfxOpenGL21(self)->glyph_width,
        gfxOpenGL21(self)->line_height,
        gfxOpenGL21(self)->glyph_width,
        0.0f,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof bg_box, bg_box, GL_STREAM_DRAW);
}

Pair_uint32_t GfxOpenGL21_get_char_size(Gfx* self)
{
    return (Pair_uint32_t){ .first  = 2.0f / gfxOpenGL21(self)->glyph_width,
                            .second = 2.0f / gfxOpenGL21(self)->line_height };
}

Pair_uint32_t GfxOpenGL21_pixels(Gfx* self, uint32_t c, uint32_t r)
{
    if (!gfxOpenGL21(self)->gw) {
        if (FT_Load_Char(gfxOpenGL21(self)->face, '>', FT_LOAD_TARGET_LCD) ||
            FT_Render_Glyph(gfxOpenGL21(self)->face->glyph, FT_RENDER_MODE_LCD)) {
            WRN("Glyph load error\n");
        }
        gfxOpenGL21(self)->gw = gfxOpenGL21(self)->face->glyph->advance.x;
    }

    float x, y;
    x = c * gfxOpenGL21(self)->gw + (gfxOpenGL21(self)->is_main_font_rgb ? 0 : 64);
    y = r * (gfxOpenGL21(self)->face->size->metrics.height + 128);

    return (Pair_uint32_t){ .first = x / 64.0, .second = y / 64.0 };
}

void GfxOpenGL21_load_font(Gfx* self)
{
    static bool ft_lib_was_initialized = false;
    FT_Error    ft_err                 = 0;
    int         strike_idx             = -1;

    if (!ft_lib_was_initialized) {
        if ((ft_err = FT_Init_FreeType(&gfxOpenGL21(self)->ft))) {
            ERR("Failed to initialize freetype %s", stringify_ft_error(ft_err));
        }
    }

    if ((ft_err = FT_New_Face(gfxOpenGL21(self)->ft, settings.font_file_name_regular.str, 0,
                              &gfxOpenGL21(self)->face))) {
        ERR("Font error, failed to load font file: %s. error: %s",
            settings.font_file_name_regular.str, stringify_ft_error(ft_err));
    }

    if ((ft_err =
           FT_Set_Char_Size(gfxOpenGL21(self)->face, settings.font_size * 64,
                            settings.font_size * 64, settings.font_dpi, settings.font_dpi))) {

        if (!gfxOpenGL21(self)->face->size->metrics.height) {
            for (strike_idx = 0; strike_idx < gfxOpenGL21(self)->face->num_fixed_sizes;
                 ++strike_idx) {
            }

            if ((ft_err = FT_Select_Size(gfxOpenGL21(self)->face, strike_idx - 1))) {
                ERR("Failed to set main font bitmap strike, file %s. error: %s\n",
                    settings.font_file_name_regular.str, stringify_ft_error(ft_err));
            }
        }
    }

    if (!FT_IS_FIXED_WIDTH(gfxOpenGL21(self)->face)) {
        WRN("Main font is not fixed width\n");
    }

    if (settings.font_file_name_bold.str) {
        if ((ft_err = FT_New_Face(gfxOpenGL21(self)->ft, settings.font_file_name_bold.str, 0,
                                  &gfxOpenGL21(self)->face_bold)))
            ERR("Font error, failed to load font file: %s. error: %s",
                settings.font_file_name_bold.str, stringify_ft_error(ft_err));

        if (strike_idx >= 0) {
            if ((ft_err = FT_Select_Size(gfxOpenGL21(self)->face_bold, strike_idx - 1))) {
                WRN("Failed to set main font bitmap strike, file %s. error: %s\n",
                    settings.font_file_name_bold.str, stringify_ft_error(ft_err));
            }
        } else {
            if ((ft_err = FT_Set_Char_Size(gfxOpenGL21(self)->face_bold, settings.font_size * 64,
                                           settings.font_size * 64, settings.font_dpi,
                                           settings.font_dpi))) {
                WRN("Failed to set font size, file %s. error: %s\n",
                    settings.font_file_name_bold.str, stringify_ft_error(ft_err));
            }
        }

        if (!FT_IS_FIXED_WIDTH(gfxOpenGL21(self)->face_bold)) {
            WRN("Bold font is not fixed width\n");
        }
    }

    if (settings.font_file_name_italic.str) {
        if ((ft_err = FT_New_Face(gfxOpenGL21(self)->ft, settings.font_file_name_italic.str, 0,
                                  &gfxOpenGL21(self)->face_italic)))
            ERR("Font error, failed to load font file: %s. error: %s",
                settings.font_file_name_italic.str, stringify_ft_error(ft_err));

        if (strike_idx >= 0) {
            if ((ft_err = FT_Select_Size(gfxOpenGL21(self)->face_italic, strike_idx - 1))) {
                WRN("Failed to set font bitmap strike, file %s. error: %s\n",
                    settings.font_file_name_italic.str, stringify_ft_error(ft_err));
            }
        } else {
            if ((ft_err = FT_Set_Char_Size(gfxOpenGL21(self)->face_italic, settings.font_size * 64,
                                           settings.font_size * 64, settings.font_dpi,
                                           settings.font_dpi))) {
                WRN("Failed to set font size, file %s. error: %s\n",
                    settings.font_file_name_italic.str, stringify_ft_error(ft_err));
            }
        }

        if (!FT_IS_FIXED_WIDTH(gfxOpenGL21(self)->face_italic)) {
            WRN("Italic font is not fixed width\n");
        }
    }

    if (settings.font_file_name_fallback.str) {
        if (FT_New_Face(gfxOpenGL21(self)->ft, settings.font_file_name_fallback.str, 0,
                        &gfxOpenGL21(self)->face_fallback))
            WRN("Font error, failed to load font file: %s", settings.font_file_name_fallback.str);

        if ((ft_err =
               FT_Set_Char_Size(gfxOpenGL21(self)->face_fallback, settings.font_size * 64,
                                settings.font_size * 64, settings.font_dpi, settings.font_dpi))) {
            WRN("Failed to set font size, file %s. error: %s\n",
                settings.font_file_name_fallback.str, stringify_ft_error(ft_err));
        }
    }

    if (settings.font_file_name_fallback2.str) {
        if (FT_New_Face(gfxOpenGL21(self)->ft, settings.font_file_name_fallback2.str, 0,
                        &gfxOpenGL21(self)->face_fallback2))
            ERR("Font error, failed to load font file: %s", settings.font_file_name_fallback2.str);

        if ((ft_err = FT_Select_Size(gfxOpenGL21(self)->face_fallback2, 0))) {
            WRN("Failed to set font size, file %s. error: %s\n",
                settings.font_file_name_fallback2.str, stringify_ft_error(ft_err));
        }
    }

    if (!ft_lib_was_initialized) {

        // we can only do this once per ft instance
        if ((ft_err = FT_Library_SetLcdFilter(gfxOpenGL21(self)->ft, FT_LCD_FILTER_DEFAULT))) {
        } else {
            WRN("LCD filtering not avaliable %s\n", stringify_ft_error(ft_err));
        }

        ft_lib_was_initialized = true;
    }
}

void GfxOpenGL21_init_with_context_activated(Gfx* self)
{
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

    glClearColor(ColorRGBA_get_float(settings.bg, 0), ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2), ColorRGBA_get_float(settings.bg, 3));

    gfxOpenGL21(self)->font_shader =
      Shader_new(font_vs_src, font_fs_src, "coord", "tex", "clr", "bclr", NULL);

    gfxOpenGL21(self)->font_shader_gray =
      Shader_new(font_vs_src, font_gray_fs_src, "coord", "tex", "clr", NULL);

    gfxOpenGL21(self)->bg_shader = Shader_new(bg_vs_src, bg_fs_src, "pos", "mv", "clr", NULL);

    gfxOpenGL21(self)->line_shader = Shader_new(line_vs_src, line_fs_src, "pos", "clr", NULL);

    gfxOpenGL21(self)->image_shader =
      Shader_new(image_rgb_vs_src, image_rgb_fs_src, "coord", "tex", NULL);

    gfxOpenGL21(self)->image_tint_shader =
      Shader_new(image_rgb_vs_src, image_tint_rgb_fs_src, "coord", "tex", "tint", NULL);

    gfxOpenGL21(self)->bg_vao = VBO_new(2, 1, gfxOpenGL21(self)->bg_shader.attribs);

    gfxOpenGL21(self)->line_bg_vao = VBO_new(2, 1, gfxOpenGL21(self)->bg_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->line_bg_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gfxOpenGL21(self)->font_vao = VBO_new(4, 1, gfxOpenGL21(self)->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->font_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gfxOpenGL21(self)->line_vao = VBO_new(2, 1, gfxOpenGL21(self)->line_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->line_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 8, NULL, GL_STREAM_DRAW);

    gfxOpenGL21(self)->flex_vbo = VBO_new(4, 1, gfxOpenGL21(self)->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->flex_vbo.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gfxOpenGL21(self)->flex_vbo_italic = VBO_new(4, 1, gfxOpenGL21(self)->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->flex_vbo_italic.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    gfxOpenGL21(self)->flex_vbo_bold = VBO_new(4, 1, gfxOpenGL21(self)->font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, gfxOpenGL21(self)->flex_vbo_bold.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 4 * 4, NULL, GL_STREAM_DRAW);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gfxOpenGL21(self)->max_tex_res);

    gfxOpenGL21(self)->color    = settings.fg;
    gfxOpenGL21(self)->bg_color = settings.bg;

    Shader_use(&gfxOpenGL21(self)->font_shader);
    glUniform3f(gfxOpenGL21(self)->font_shader.uniforms[1].location,
                ColorRGB_get_float(settings.fg, 0), ColorRGB_get_float(settings.fg, 1),
                ColorRGB_get_float(settings.fg, 2));

    gfxOpenGL21(self)->_atlas = Atlas_new(gfxOpenGL21(self), gfxOpenGL21(self)->face);
    gfxOpenGL21(self)->atlas  = &gfxOpenGL21(self)->_atlas;

    Cache_init(&gfxOpenGL21(self)->_cache);
    gfxOpenGL21(self)->cache = &gfxOpenGL21(self)->_cache;

    gfxOpenGL21(self)->line_framebuffer = Framebuffer_new();

    gfxOpenGL21(self)->in_focus           = true;
    gfxOpenGL21(self)->recent_action      = true;
    gfxOpenGL21(self)->draw_blinking      = true;
    gfxOpenGL21(self)->draw_blinking_text = true;

    gfxOpenGL21(self)->blink_switch      = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
    gfxOpenGL21(self)->blink_switch_text = TimePoint_now();

    gfxOpenGL21(self)->_vec_glyph_buffer = Vector_new_with_capacity_GlyphBufferData(80);
    gfxOpenGL21(self)->vec_glyph_buffer  = &gfxOpenGL21(self)->_vec_glyph_buffer;

    gfxOpenGL21(self)->_vec_glyph_buffer_bold = Vector_new_with_capacity_GlyphBufferData(20);

    gfxOpenGL21(self)->_vec_glyph_buffer_italic = Vector_new_with_capacity_GlyphBufferData(20);

    // if font styles don't exist point their resources to deaults
    if (settings.font_file_name_bold.str) {
        gfxOpenGL21(self)->_atlas_bold = Atlas_new(gfxOpenGL21(self), gfxOpenGL21(self)->face_bold);
        gfxOpenGL21(self)->atlas_bold  = &gfxOpenGL21(self)->_atlas_bold;

        Cache_init(&gfxOpenGL21(self)->_cache_bold);
        gfxOpenGL21(self)->cache_bold = &gfxOpenGL21(self)->_cache_bold;

        gfxOpenGL21(self)->vec_glyph_buffer_bold = &gfxOpenGL21(self)->_vec_glyph_buffer_bold;
    } else {
        gfxOpenGL21(self)->atlas_bold = &gfxOpenGL21(self)->_atlas;
        gfxOpenGL21(self)->cache_bold = &gfxOpenGL21(self)->_cache;

        gfxOpenGL21(self)->vec_glyph_buffer_bold = &gfxOpenGL21(self)->_vec_glyph_buffer;
    }

    if (settings.font_file_name_italic.str) {
        gfxOpenGL21(self)->_atlas_italic =
          Atlas_new(gfxOpenGL21(self), gfxOpenGL21(self)->face_italic);
        gfxOpenGL21(self)->atlas_italic = &gfxOpenGL21(self)->_atlas_italic;

        Cache_init(&gfxOpenGL21(self)->_cache_italic);
        gfxOpenGL21(self)->cache_italic = &gfxOpenGL21(self)->_cache_italic;

        gfxOpenGL21(self)->vec_glyph_buffer_italic = &gfxOpenGL21(self)->_vec_glyph_buffer_italic;
    } else {
        gfxOpenGL21(self)->atlas_italic = &gfxOpenGL21(self)->_atlas;
        gfxOpenGL21(self)->cache_italic = &gfxOpenGL21(self)->_cache;

        gfxOpenGL21(self)->vec_glyph_buffer_italic = &gfxOpenGL21(self)->_vec_glyph_buffer;
    }

    gfxOpenGL21(self)->vec_vertex_buffer  = Vector_new_vertex_t();
    gfxOpenGL21(self)->vec_vertex_buffer2 = Vector_new_vertex_t();

    GfxOpenGL21_notify_action(self);

    float height                          = gfxOpenGL21(self)->face->size->metrics.height + 64;
    gfxOpenGL21(self)->line_height_pixels = height / 64.0;

    uint32_t t_height = CLAMP(gfxOpenGL21(self)->line_height_pixels / 8.0 + 2, 4, UINT8_MAX);

    gfxOpenGL21(self)->squiggle_texture =
      create_squiggle_texture(t_height * M_PI / 2.0, t_height, CLAMP(t_height / 3, 1, 10));
}

void GfxOpenGL21_reload_font(Gfx* self)
{
    if (!gfxOpenGL21(self)->is_main_font_rgb) {
        WRN("Resizing is not available for bitmap fonts\n");
        return;
    }

    Atlas_destroy(&gfxOpenGL21(self)->_atlas);
    if (gfxOpenGL21(self)->atlas != gfxOpenGL21(self)->atlas_bold)
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_bold);
    if (gfxOpenGL21(self)->atlas != gfxOpenGL21(self)->atlas_italic)
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_italic);

    Cache_destroy(&gfxOpenGL21(self)->_cache);
    if (gfxOpenGL21(self)->cache != gfxOpenGL21(self)->cache_bold)
        Cache_destroy(&gfxOpenGL21(self)->_cache_bold);
    if (gfxOpenGL21(self)->cache != gfxOpenGL21(self)->cache_italic)
        Cache_destroy(&gfxOpenGL21(self)->_cache_italic);

    FT_Done_Face(gfxOpenGL21(self)->face);
    if (settings.font_file_name_bold.str)
        FT_Done_Face(gfxOpenGL21(self)->face_bold);
    if (settings.font_file_name_italic.str)
        FT_Done_Face(gfxOpenGL21(self)->face_italic);
    if (settings.font_file_name_fallback.str)
        FT_Done_Face(gfxOpenGL21(self)->face_fallback);
    if (settings.font_file_name_fallback2.str)
        FT_Done_Face(gfxOpenGL21(self)->face_fallback2);

    GfxOpenGL21_load_font(self);

    GfxOpenGL21_resize(self, gfxOpenGL21(self)->win_w, gfxOpenGL21(self)->win_h);

    gfxOpenGL21(self)->_atlas = Atlas_new(gfxOpenGL21(self), gfxOpenGL21(self)->face);
    Cache_init(&gfxOpenGL21(self)->_cache);

    if (settings.font_file_name_bold.str) {
        gfxOpenGL21(self)->_atlas_bold = Atlas_new(gfxOpenGL21(self), gfxOpenGL21(self)->face_bold);
        Cache_init(&gfxOpenGL21(self)->_cache_bold);
    }

    if (settings.font_file_name_italic.str) {
        gfxOpenGL21(self)->_atlas_italic =
          Atlas_new(gfxOpenGL21(self), gfxOpenGL21(self)->face_italic);
        Cache_init(&gfxOpenGL21(self)->_cache_italic);
    }

    // regenerate the squiggle texture
    glDeleteTextures(1, &gfxOpenGL21(self)->squiggle_texture.id);
    uint32_t t_height = CLAMP(gfxOpenGL21(self)->line_height_pixels / 8.0 + 2, 4, UINT8_MAX);
    gfxOpenGL21(self)->squiggle_texture =
      create_squiggle_texture(t_height * M_PI / 2.0, t_height, CLAMP(t_height / 7, 1, 10));

    GfxOpenGL21_notify_action(self);
}

bool GfxOpenGL21_set_focus(Gfx* self, bool focus)
{
    bool ret = false;
    if (gfxOpenGL21(self)->in_focus && !focus)
        ret = true;
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

bool GfxOpenGL21_update_timers(Gfx* self, Vt* vt, Ui* ui)
{
    bool repaint = false;

    if (TimePoint_passed(gfxOpenGL21(self)->blink_switch_text)) {
        gfxOpenGL21(self)->blink_switch_text =
          TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
        gfxOpenGL21(self)->draw_blinking_text = !gfxOpenGL21(self)->draw_blinking_text;
        if (unlikely(gfxOpenGL21(self)->has_blinking_text)) {
            repaint = true;
        }
    }

    if (!gfxOpenGL21(self)->in_focus && !gfxOpenGL21(self)->has_blinking_text) {
        return false;
    }

    float fraction = Timer_get_fraction_clamped_now(&gfxOpenGL21(self)->flash_timer);
    if (fraction != gfxOpenGL21(self)->flash_fraction) {
        gfxOpenGL21(self)->flash_fraction = fraction;
        repaint                           = true;
    }

    if (gfxOpenGL21(self)->recent_action && TimePoint_passed(gfxOpenGL21(self)->action)) {
        // start blinking cursor
        gfxOpenGL21(self)->recent_action = false;
        gfxOpenGL21(self)->blink_switch  = TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
        gfxOpenGL21(self)->draw_blinking = !gfxOpenGL21(self)->draw_blinking;
        repaint                          = true;
    }

    if (TimePoint_passed(gfxOpenGL21(self)->inactive) && gfxOpenGL21(self)->draw_blinking &&
        settings.cursor_blink_end_s >= 0) {
        gfxOpenGL21(self)->is_inactive = true;
    } else {
        if (TimePoint_passed(gfxOpenGL21(self)->blink_switch)) {
            gfxOpenGL21(self)->blink_switch =
              TimePoint_ms_from_now(settings.cursor_blink_interval_ms);
            gfxOpenGL21(self)->draw_blinking = !gfxOpenGL21(self)->draw_blinking;
            if (!(gfxOpenGL21(self)->recent_action && !gfxOpenGL21(self)->draw_blinking) &&
                (settings.enable_cursor_blink || gfxOpenGL21(self)->has_blinking_text)) {
                repaint = true;
            }
        }
    }

    return repaint;
}

static void GfxOpenGL21_generate_line_quads(GfxOpenGL21*  gfx,
                                            VtLine* const vt_line,
                                            uint_fast16_t line_index)
{
    if (vt_line->proxy.data[PROXY_INDEX_TEXTURE] ||
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]) {

        if (vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]) {
            gfx->has_blinking_text = true;
        }

        float tex_end_x   = -1.0f + vt_line->data.size * gfx->glyph_width_pixels * gfx->sx;
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

/**
 * (Re)generate 'proxy' texture(s) for a given VtLine
 *
 * TODO:
 * - Extract some of this, one big magical function that does everything seems like a bad idea
 **/
__attribute__((hot)) static inline void GfxOpenGL21_rasterize_line(GfxOpenGL21*    gfx,
                                                                   const Vt* const vt,
                                                                   VtLine*         vt_line,
                                                                   const size_t visual_line_index,
                                                                   bool         is_for_blinking)
{
    if (likely(!is_for_blinking)) {
        if (likely(!vt_line->damaged || !vt_line->data.size)) {
            return;
        }
    }

    const size_t length               = vt_line->data.size;
    bool         has_blinking_chars   = false;
    float        texture_width        = vt_line->data.size * gfx->glyph_width_pixels;
    float        actual_texture_width = texture_width;
    float        texture_height       = gfx->line_height_pixels;

    // Try to reuse the texture that is already there
    Texture recovered = {
        .id =
          vt_line->proxy.data[is_for_blinking ? PROXY_INDEX_TEXTURE_BLINK : PROXY_INDEX_TEXTURE],
        .w = vt_line->proxy.data[PROXY_INDEX_TEXTURE_SIZE],
    };

    bool can_reuse = recovered.id && recovered.w >= texture_width;

    if (can_reuse) {
        actual_texture_width = recovered.w;
        Framebuffer_attach_as_color(&gfx->line_framebuffer, &recovered, recovered.w,
                                    texture_height);
    } else {
        if (!is_for_blinking) {
            GfxOpenGL21_destroy_proxy((Gfx*)gfx - offsetof(Gfx, extend_data), vt_line->proxy.data);
        }

        // TODO: try to recycle
        Framebuffer_generate_color_attachment(&gfx->line_framebuffer, texture_width,
                                              texture_height);
    }

    Framebuffer_assert_complete(&gfx->line_framebuffer);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glClearColor(ColorRGBA_get_float(settings.bg, 0), ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2), ColorRGBA_get_float(settings.bg, 3));

    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    static float buffer[] = {
        -1.0f, -1.0f, -1.0f, 1.0f,
        0.0f, // overwritten
        1.0f,
        0.0f, // overwritten
        -1.0f,
    };

#define BOUND_RESOURCES_NONE      0
#define BOUND_RESOURCES_BG        1
#define BOUND_RESOURCES_FONT      2
#define BOUND_RESOURCES_LINES     3
#define BOUND_RESOURCES_IMAGE     4
#define BOUND_RESOURCES_FONT_MONO 5
    int_fast8_t bound_resources = BOUND_RESOURCES_NONE;

    /* Scale from pixels to GL coordinates */
    float scalex = 2.0f / texture_width;
    float scaley = 2.0f / texture_height;

    ColorRGBA bg_color = settings.bg;

    VtRune *each_rune = vt_line->data.buf, *same_bg_block_begin_rune = vt_line->data.buf;

    for (size_t idx_each_rune = 0; idx_each_rune <= vt_line->data.size; ++idx_each_rune) {
        each_rune = vt_line->data.buf + idx_each_rune;

        if (likely(idx_each_rune != vt_line->data.size) && unlikely(each_rune->blinkng)) {
            has_blinking_chars = true;
        }

#define L_CALC_BG_COLOR                                                                            \
    Vt_is_cell_selected(vt, idx_each_rune, visual_line_index) ? settings.bghl : each_rune->bg

        if (idx_each_rune == length || !ColorRGBA_eq(L_CALC_BG_COLOR, bg_color)) {

            int extra_width = 0;

            // fb was cleared with settings.bg
            if (!ColorRGBA_eq(bg_color, settings.bg)) {

                if (idx_each_rune > 1) {
                    extra_width = wcwidth(vt_line->data.buf[idx_each_rune - 1].code) - 1;
                }

                buffer[4] = buffer[6] = -1.0f + (idx_each_rune + extra_width) * scalex *
                                                  gfx->glyph_width_pixels; // set buffer end

                if (bound_resources != BOUND_RESOURCES_BG) {
                    glBindBuffer(GL_ARRAY_BUFFER, gfx->line_bg_vao.vbo);
                    glVertexAttribPointer(gfx->bg_shader.attribs->location, 2, GL_FLOAT, GL_FALSE,
                                          0, 0);
                    Shader_use(&gfx->bg_shader);
                    glUniform2f(gfx->bg_shader.uniforms[0].location, 0.0f, 0.0f);

                    bound_resources = BOUND_RESOURCES_BG;
                }

                glBufferData(GL_ARRAY_BUFFER, sizeof buffer, buffer, GL_STREAM_DRAW);

                glUniform4f(gfx->bg_shader.uniforms[1].location, ColorRGBA_get_float(bg_color, 0),
                            ColorRGBA_get_float(bg_color, 1), ColorRGBA_get_float(bg_color, 2),
                            ColorRGBA_get_float(bg_color, 3));

                glDrawArrays(GL_QUADS, 0, 4);
            }

            { // for each block of characters with the same background color
                ColorRGB      fg_color                     = settings.fg;
                const VtRune* same_colors_block_begin_rune = same_bg_block_begin_rune;

                for (const VtRune* each_rune_same_bg = same_bg_block_begin_rune;
                     each_rune_same_bg != each_rune + 1; ++each_rune_same_bg) {

// it's very unlikely that this will be needed as selected region changes the bg color, but
// technically the bg highlight color could be exactly the same as the background
#define L_CALC_FG_COLOR                                                                            \
    !settings.highlight_change_fg                                                                  \
      ? each_rune_same_bg->fg                                                                      \
      : unlikely(                                                                                  \
          Vt_is_cell_selected(vt, each_rune_same_bg - vt_line->data.buf, visual_line_index))       \
          ? settings.fghl                                                                          \
          : each_rune_same_bg->fg

                    if (each_rune_same_bg == each_rune || !ColorRGB_eq(L_CALC_FG_COLOR, fg_color)) {

                        { // for each block with the same fg color
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_italic);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_bold);

                            static VtRune        same_color_blank_space;
                            static const VtRune* each_rune_same_colors_filtered_blink;

                            for (const VtRune* each_rune_same_colors = same_colors_block_begin_rune;
                                 each_rune_same_colors != each_rune_same_bg;
                                 ++each_rune_same_colors) {

                                size_t column = each_rune_same_colors - vt_line->data.buf;

                                if (is_for_blinking && each_rune_same_colors->blinkng) {
                                    same_color_blank_space = *each_rune_same_colors_filtered_blink;
                                    same_color_blank_space.code          = ' ';
                                    each_rune_same_colors_filtered_blink = &same_color_blank_space;
                                } else {
                                    each_rune_same_colors_filtered_blink = each_rune_same_colors;
                                }

                                if (each_rune_same_colors_filtered_blink->code >
                                      ATLAS_RENDERABLE_START &&
                                    each_rune_same_colors_filtered_blink->code <=
                                      ATLAS_RENDERABLE_END) {
                                    // pull data from font atlas
                                    struct AtlasCharInfo* g;
                                    int32_t               atlas_offset = -1;

                                    Vector_GlyphBufferData* target       = gfx->vec_glyph_buffer;
                                    Atlas*                  source_atlas = gfx->atlas;

                                    switch (expect(each_rune_same_colors_filtered_blink->state,
                                                   VT_RUNE_NORMAL)) {
                                        case VT_RUNE_ITALIC:
                                            target       = gfx->vec_glyph_buffer_italic;
                                            source_atlas = gfx->atlas_italic;
                                            break;

                                        case VT_RUNE_BOLD:
                                            target       = gfx->vec_glyph_buffer_bold;
                                            source_atlas = gfx->atlas_bold;
                                            break;

                                        default:;
                                    }

                                    atlas_offset = Atlas_select(
                                      source_atlas, each_rune_same_colors_filtered_blink->code);

                                    g       = &source_atlas->char_info[atlas_offset];
                                    float h = (float)g->rows * scaley;
                                    float w = (float)g->width * scalex;
                                    float t = (float)g->top * scaley;
                                    float l = (float)g->left * scalex;

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

                            if (gfx->vec_glyph_buffer->size || gfx->vec_glyph_buffer_italic->size ||
                                gfx->vec_glyph_buffer_bold->size) {

                                bound_resources = gfx->is_main_font_rgb ? BOUND_RESOURCES_FONT
                                                                        : BOUND_RESOURCES_FONT_MONO;

                                if (gfx->is_main_font_rgb) {
                                    glUseProgram(gfx->font_shader.id);
                                    glUniform3f(gfx->font_shader.uniforms[1].location,
                                                ColorRGB_get_float(fg_color, 0),
                                                ColorRGB_get_float(fg_color, 1),
                                                ColorRGB_get_float(fg_color, 2));
                                    glUniform4f(gfx->font_shader.uniforms[2].location,
                                                ColorRGBA_get_float(bg_color, 0),
                                                ColorRGBA_get_float(bg_color, 1),
                                                ColorRGBA_get_float(bg_color, 2),
                                                ColorRGBA_get_float(bg_color, 3));
                                } else {
                                    glUseProgram(gfx->font_shader_gray.id);
                                    glUniform3f(gfx->font_shader_gray.uniforms[1].location,
                                                ColorRGB_get_float(fg_color, 0),
                                                ColorRGB_get_float(fg_color, 1),
                                                ColorRGB_get_float(fg_color, 2));
                                }

                                // normal
                                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                                glVertexAttribPointer(gfx->font_shader.attribs->location, 4,
                                                      GL_FLOAT, GL_FALSE, 0, 0);

                                size_t newsize =
                                  gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);

                                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer->buf,
                                                         gfx->flex_vbo.size, newsize);

                                glBindTexture(GL_TEXTURE_2D, gfx->atlas->tex);

                                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                                glDrawArrays(GL_QUADS, 0, gfx->vec_glyph_buffer->size * 4);

                                // italic
                                if (gfx->vec_glyph_buffer_italic != gfx->vec_glyph_buffer) {
                                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo_italic.vbo);
                                    glVertexAttribPointer(gfx->font_shader.attribs->location, 4,
                                                          GL_FLOAT, GL_FALSE, 0, 0);

                                    size_t newsize =
                                      gfx->vec_glyph_buffer_italic->size * sizeof(GlyphBufferData);

                                    ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer_italic->buf,
                                                             gfx->flex_vbo_italic.size, newsize);

                                    glBindTexture(GL_TEXTURE_2D, gfx->atlas_italic->tex);

                                    glDrawArrays(GL_QUADS, 0,
                                                 gfx->vec_glyph_buffer_italic->size * 4);
                                }

                                // bold
                                if (gfx->vec_glyph_buffer_bold != gfx->vec_glyph_buffer) {
                                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo_bold.vbo);
                                    glVertexAttribPointer(gfx->font_shader.attribs->location, 4,
                                                          GL_FLOAT, GL_FALSE, 0, 0);

                                    size_t newsize =
                                      gfx->vec_glyph_buffer_bold->size * sizeof(GlyphBufferData);

                                    ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer_bold->buf,
                                                             gfx->flex_vbo_bold.size, newsize);

                                    glBindTexture(GL_TEXTURE_2D, gfx->atlas_bold->tex);

                                    glDrawArrays(GL_QUADS, 0, gfx->vec_glyph_buffer_bold->size * 4);
                                }

                            } // end if there are atlas chars to draw

                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_italic);
                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer_bold);
                            for (const VtRune* z = same_colors_block_begin_rune;
                                 z != each_rune_same_bg; ++z) {
                                if (unlikely(z->code > ATLAS_RENDERABLE_END)) {
                                    size_t    column = z - vt_line->data.buf;
                                    GlyphMap* ca     = gfx->cache;
                                    FT_Face   fc     = gfx->face;

                                    switch (z->state) {
                                        case VT_RUNE_ITALIC:
                                            ca = gfx->cache_italic;
                                            fc = gfx->face_italic;
                                            break;
                                        case VT_RUNE_BOLD:
                                            ca = gfx->cache_bold;
                                            fc = gfx->face_bold;
                                            break;
                                        default:;
                                    }

                                    GlyphMapEntry* g = NULL;
                                    g                = Cache_get_glyph(gfx, ca, fc, z->code);

                                    if (!g)
                                        continue;

                                    float h = scaley * g->tex.h;
                                    float w = scalex * g->tex.w;
                                    float l = scalex * g->left;
                                    float t = scaley * g->top;

                                    /* why does this even work?! */
                                    float gsx = -0.1 / g->tex.w;
                                    float gsy = -0.05 / g->tex.h;

                                    if (unlikely(h > 2.0f)) {
                                        const float scale = h / 2.0f;
                                        h /= scale;
                                        w /= scale;
                                        t /= scale;
                                        l /= scale;
                                        gsx = 0;
                                        gsy = 0;
                                    }

                                    float x3 = -1.0f +
                                               (double)column * gfx->glyph_width_pixels * scalex +
                                               l + (scalex * 0.5);
                                    float y3 = -1.0f + (double)gfx->pen_begin_pixels * scaley - t +
                                               (scaley * 0.5);

                                    Vector_GlyphBufferData* target = &gfx->_vec_glyph_buffer;

                                    if (g->color == GLYPH_COLOR_COLOR)
                                        target = &gfx->_vec_glyph_buffer_bold;
                                    else if (g->color == GLYPH_COLOR_MONO)
                                        target = &gfx->_vec_glyph_buffer_italic;

                                    // Very often characters like this are
                                    // used to draw tables and borders. Render
                                    // all repeating characters in one call.
                                    Vector_push_GlyphBufferData(
                                      target, (GlyphBufferData){
                                                { { x3, y3, 0.0f - gsx, 0.0f - gsy },
                                                  { x3 + w, y3, 1.0f - gsx, 0.0f - gsy },
                                                  { x3 + w, y3 + h, 1.0f - gsx, 1.0f - gsy },
                                                  { x3, y3 + h, 0.0f - gsx, 1.0f - gsy } } });

                                    // needs to change texture on next iteration
                                    if ((z + 1 != each_rune_same_bg && z->code != (z + 1)->code) ||
                                        z + 1 == each_rune_same_bg) {

                                        // Draw noramal characters
                                        if (gfx->vec_glyph_buffer->size) {
                                            if (bound_resources != BOUND_RESOURCES_FONT) {
                                                glUseProgram(gfx->font_shader.id);
                                                bound_resources = BOUND_RESOURCES_FONT;
                                            }

                                            glUniform3f(gfx->font_shader.uniforms[1].location,
                                                        ColorRGB_get_float(fg_color, 0),
                                                        ColorRGB_get_float(fg_color, 1),
                                                        ColorRGB_get_float(fg_color, 2));

                                            glUniform4f(gfx->font_shader.uniforms[2].location,
                                                        ColorRGBA_get_float(bg_color, 0),
                                                        ColorRGBA_get_float(bg_color, 1),
                                                        ColorRGBA_get_float(bg_color, 2),
                                                        ColorRGBA_get_float(bg_color, 3));

                                            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);

                                            glVertexAttribPointer(
                                              gfx->font_shader.attribs->location, 4, GL_FLOAT,
                                              GL_FALSE, 0, 0);

                                            size_t newsize =
                                              gfx->vec_glyph_buffer->size * sizeof(GlyphBufferData);

                                            ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_glyph_buffer->buf,
                                                                     gfx->flex_vbo.size, newsize);

                                            glDrawArrays(GL_QUADS, 0,
                                                         gfx->vec_glyph_buffer->size * 4);

                                            Vector_clear_GlyphBufferData(gfx->vec_glyph_buffer);
                                        }

                                        // Draw color characters
                                        if (gfx->_vec_glyph_buffer_bold.size) {
                                            if (likely(bound_resources != BOUND_RESOURCES_IMAGE)) {
                                                glUseProgram(gfx->image_shader.id);
                                                bound_resources = BOUND_RESOURCES_IMAGE;
                                            }

                                            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);

                                            glVertexAttribPointer(
                                              gfx->image_shader.attribs->location, 4, GL_FLOAT,
                                              GL_FALSE, 0, 0);

                                            size_t newsize = gfx->_vec_glyph_buffer_bold.size *
                                                             sizeof(GlyphBufferData);

                                            ARRAY_BUFFER_SUB_OR_SWAP(
                                              gfx->_vec_glyph_buffer_bold.buf, gfx->flex_vbo.size,
                                              newsize);

                                            glDrawArrays(GL_QUADS, 0,
                                                         gfx->_vec_glyph_buffer_bold.size * 4);

                                            gfx->_vec_glyph_buffer_bold.size = 0;
                                        }

                                        // draw mono characters
                                        if (gfx->_vec_glyph_buffer_italic.size) {
                                            if (likely(bound_resources !=
                                                       BOUND_RESOURCES_FONT_MONO)) {
                                                glUseProgram(gfx->font_shader_gray.id);

                                                bound_resources = BOUND_RESOURCES_FONT_MONO;
                                            }

                                            glUniform3f(gfx->font_shader_gray.uniforms[1].location,
                                                        ColorRGB_get_float(fg_color, 0),
                                                        ColorRGB_get_float(fg_color, 1),
                                                        ColorRGB_get_float(fg_color, 2));

                                            glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);

                                            glVertexAttribPointer(
                                              gfx->font_shader_gray.attribs->location, 4, GL_FLOAT,
                                              GL_FALSE, 0, 0);

                                            size_t newsize = gfx->_vec_glyph_buffer_italic.size *
                                                             sizeof(GlyphBufferData);

                                            ARRAY_BUFFER_SUB_OR_SWAP(
                                              gfx->_vec_glyph_buffer_italic.buf, gfx->flex_vbo.size,
                                              newsize);

                                            glDrawArrays(GL_QUADS, 0,
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
                                unlikely(Vt_is_cell_selected(
                                  vt, each_rune_same_bg - vt_line->data.buf, visual_line_index))) {
                                fg_color = settings.fghl;
                            } else {
                                fg_color = each_rune_same_bg->fg;
                            }
                        }
                    } // end for each block with the same color
                }     // end for each char
            }         // end for each block with the same bg

            // set background buffer start
            buffer[0] = buffer[2] =
              -1.0f + (idx_each_rune + extra_width) * scalex * gfx->glyph_width_pixels;

            int clip_begin = idx_each_rune * gfx->glyph_width_pixels;
            glEnable(GL_SCISSOR_TEST);
            glScissor(clip_begin, 0, texture_width, texture_height);

            if (idx_each_rune != vt_line->data.size) {
                same_bg_block_begin_rune = each_rune;

                // update active bg color;
                if (unlikely(Vt_is_cell_selected(vt, idx_each_rune, visual_line_index))) {
                    bg_color = settings.bghl;
                } else {
                    bg_color = each_rune->bg;
                }
            }
        } // end if bg color changed
    }     // end for each VtRune

    // draw lines
    static float begin[5]   = { -1, -1, -1, -1, -1 };
    static float end[5]     = { 1, 1, 1, 1, 1 };
    static bool  drawing[5] = { 0 };

    // lines are drawn in the same color as the character, unless line color was
    // explicitly set
    ColorRGB line_color =
      vt_line->data.buf->linecolornotdefault ? vt_line->data.buf->line : vt_line->data.buf->fg;

    for (const VtRune* each_rune = vt_line->data.buf;
         each_rune <= vt_line->data.buf + vt_line->data.size; ++each_rune) {
        size_t column = each_rune - vt_line->data.buf;

        ColorRGB nc = {};
        if (each_rune != vt_line->data.buf + vt_line->data.size)
            nc = each_rune->linecolornotdefault ? each_rune->line : each_rune->fg;

        // State has changed
        if (!ColorRGB_eq(line_color, nc) || each_rune == vt_line->data.buf + vt_line->data.size ||
            each_rune->underlined != drawing[0] || each_rune->doubleunderline != drawing[1] ||
            each_rune->strikethrough != drawing[2] || each_rune->overline != drawing[3] ||
            each_rune->curlyunderline != drawing[4]) {
            if (each_rune == vt_line->data.buf + vt_line->data.size) {
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
                float cw      = gfx->glyph_width_pixels * scalex;
                int   n_cells = round((end[4] - begin[4]) / cw);
                float t_y     = 1.0f - gfx->squiggle_texture.h * scaley;

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
                if (bound_resources != BOUND_RESOURCES_LINES) {
                    bound_resources = BOUND_RESOURCES_LINES;
                    Shader_use(&gfx->line_shader);
                    glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                    glVertexAttribPointer(gfx->line_shader.attribs->location, 2, GL_FLOAT, GL_FALSE,
                                          0, 0);
                }

                glUniform3f(gfx->line_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0), ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));

                size_t new_size = sizeof(vertex_t) * gfx->vec_vertex_buffer.size;

                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer.buf, gfx->flex_vbo.size, new_size);

                glDrawArrays(GL_LINES, 0, gfx->vec_vertex_buffer.size);
            }

            if (gfx->vec_vertex_buffer2.size) {
                bound_resources = BOUND_RESOURCES_NONE;
                Shader_use(&gfx->image_tint_shader);
                glBindTexture(GL_TEXTURE_2D, gfx->squiggle_texture.id);

                glUniform3f(gfx->image_tint_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0), ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));

                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);

                glVertexAttribPointer(gfx->font_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0,
                                      0);

                size_t new_size = sizeof(vertex_t) * gfx->vec_vertex_buffer2.size;

                ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer2.buf, gfx->flex_vbo.size, new_size);

                glDrawArrays(GL_QUADS, 0, gfx->vec_vertex_buffer2.size / 2);
            }

#define L_SET_BOUNDS_BEGIN(what, index)                                                            \
    if (what) {                                                                                    \
        begin[index] = -1.0f + (float)column * scalex * (float)gfx->glyph_width_pixels;            \
    }

            if (each_rune != vt_line->data.buf + vt_line->data.size) {
                L_SET_BOUNDS_BEGIN(each_rune->underlined, 0);
                L_SET_BOUNDS_BEGIN(each_rune->doubleunderline, 1);
                L_SET_BOUNDS_BEGIN(each_rune->strikethrough, 2);
                L_SET_BOUNDS_BEGIN(each_rune->overline, 3);
                L_SET_BOUNDS_BEGIN(each_rune->curlyunderline, 4);
            }

            if (each_rune != vt_line->data.buf + vt_line->data.size) {
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
    } // end drawing lines

    // set proxy data to generated texture
    if (unlikely(is_for_blinking)) {
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] =
          Framebuffer_extract_color_texture(&gfx->line_framebuffer).id;
    } else {
        vt_line->proxy.data[PROXY_INDEX_TEXTURE] =
          Framebuffer_extract_color_texture(&gfx->line_framebuffer).id;
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_SIZE] = actual_texture_width;
        vt_line->damaged                              = false;
    }

    static float debug_tint = 0.0f;
    if (unlikely(settings.debug_gfx)) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBegin(GL_QUADS);
        if (can_reuse)
            glColor4f(0, 0, 0, 0);
        else
            glColor4f(fabs(sin(debug_tint)), fabs(cos(debug_tint)), sin(debug_tint), 0.3);
        glVertex2f(1, 1);
        glVertex2f(-1, 1);
        glColor4f(fabs(sin(debug_tint)), fabs(cos(debug_tint)), sin(debug_tint), 0.3);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glEnd();
        glDisable(GL_BLEND);
        debug_tint += 0.5f;
        if (debug_tint > M_PI)
            debug_tint -= M_PI;
    }

    Framebuffer_use(NULL);
    glViewport(0, 0, gfx->win_w, gfx->win_h);
    glDisable(GL_SCISSOR_TEST);

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

            glUniform3f(gfx->line_shader.uniforms[1].location, ColorRGB_get_float(*clr, 0),
                        ColorRGB_get_float(*clr, 1), ColorRGB_get_float(*clr, 2));

            size_t newsize = gfx->vec_vertex_buffer.size * sizeof(vertex_t);

            ARRAY_BUFFER_SUB_OR_SWAP(gfx->vec_vertex_buffer.buf, gfx->flex_vbo.size, newsize);

            glDrawArrays(gfx->vec_vertex_buffer.size == 2 ? GL_LINES : GL_LINE_LOOP, 0,
                         gfx->vec_vertex_buffer.size);
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(col * gfx->glyph_width_pixels,
                      gfx->win_h - (row + 1) * gfx->line_height_pixels, gfx->glyph_width_pixels,
                      gfx->line_height_pixels);
            glClearColor(ColorRGB_get_float(*clr, 0), ColorRGB_get_float(*clr, 1),
                         ColorRGB_get_float(*clr, 2), 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (cursor_char && cursor_char->code > ' ') {
                glBindBuffer(GL_ARRAY_BUFFER, gfx->flex_vbo.vbo);
                glVertexAttribPointer(gfx->font_shader.attribs->location, 4, GL_FLOAT, GL_FALSE, 0,
                                      0);

                Atlas*          source_atlas = gfx->atlas;
                GlyphMap*       source_cache = gfx->cache;
                FT_Face         source_face  = gfx->face;
                enum GlyphColor color;
                switch (cursor_char->state) {
                    case VT_RUNE_ITALIC:
                        source_atlas = gfx->atlas_italic;
                        source_cache = gfx->cache_italic;
                        source_face  = gfx->face_italic;
                        break;
                    case VT_RUNE_BOLD:
                        source_atlas = gfx->atlas_bold;
                        source_cache = gfx->cache_bold;
                        source_face  = gfx->face_bold;
                        break;
                    default:;
                }

                float h, w, t, l, gsx = 0.0, gsy = 0.0;
                float tc[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

                int32_t atlas_offset = Atlas_select(source_atlas, cursor_char->code);
                if (atlas_offset >= 0) {
                    struct AtlasCharInfo* g = &source_atlas->char_info[atlas_offset];
                    h                       = (float)g->rows * gfx->sy;
                    w                       = (float)g->width * gfx->sx;
                    t                       = (float)g->top * gfx->sy;
                    l                       = (float)g->left * gfx->sx;
                    memcpy(tc, g->tex_coords, sizeof tc);
                    color = gfx->is_main_font_rgb ? GLYPH_COLOR_LCD : GLYPH_COLOR_MONO;
                } else {
                    GlyphMapEntry* g =
                      Cache_get_glyph(gfx, source_cache, source_face, cursor_char->code);
                    h = (float)g->tex.h * gfx->sy;
                    w = (float)g->tex.w * gfx->sx;
                    t = (float)g->top * gfx->sy;
                    l = (float)g->left * gfx->sx;

                    gsx = -0.1 / g->tex.w;
                    gsy = -0.05 / g->tex.h;

                    color = g->color;
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
                                ColorRGB_get_float(*clr_bg, 0), ColorRGB_get_float(*clr_bg, 1),
                                ColorRGB_get_float(*clr_bg, 2));
                    glUniform4f(gfx->font_shader.uniforms[2].location, ColorRGB_get_float(*clr, 0),
                                ColorRGB_get_float(*clr, 1), ColorRGB_get_float(*clr, 2), 1.0f);
                } else if (color == GLYPH_COLOR_MONO) {
                    glUseProgram(gfx->font_shader_gray.id);
                    glUniform3f(gfx->font_shader_gray.uniforms[1].location,
                                ColorRGB_get_float(*clr_bg, 0), ColorRGB_get_float(*clr_bg, 1),
                                ColorRGB_get_float(*clr_bg, 2));
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

    size_t row = vt->cursor.row - Vt_visual_top_line(vt), col = begin;

    glEnable(GL_SCISSOR_TEST);
    glScissor(col * gfx->glyph_width_pixels, gfx->win_h - (row + 1) * gfx->line_height_pixels,
              gfx->glyph_width_pixels * (vt->unicode_input.buffer.size + 1),
              gfx->line_height_pixels);

    glClearColor(ColorRGBA_get_float(settings.bg, 0), ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2), ColorRGBA_get_float(settings.bg, 3));

    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(gfx->font_shader.id);
    glUniform3f(gfx->font_shader.uniforms[1].location, ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1), ColorRGB_get_float(settings.fg, 2));

    glUniform4f(gfx->font_shader.uniforms[2].location, ColorRGBA_get_float(settings.bg, 0),
                ColorRGBA_get_float(settings.bg, 1), ColorRGBA_get_float(settings.bg, 2),
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

    Vector_push_GlyphBufferData(gfx->vec_glyph_buffer, (GlyphBufferData){ {
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

        Vector_push_GlyphBufferData(gfx->vec_glyph_buffer, (GlyphBufferData){ {
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

    glUniform3f(gfx->line_shader.uniforms[1].location, ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1), ColorRGB_get_float(settings.fg, 2));

    glDrawArrays(GL_LINES, 0, 2);

    glDisable(GL_SCISSOR_TEST);
}

static void GfxOpenGL21_draw_scrollbar(GfxOpenGL21* self, const Scrollbar* scrollbar)
{
    // TODO: use VBOs
    Shader_use(NULL);
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
    } else {
        GfxOpenGL21_draw_cursor(self, vt, ui);
    }

    if (ui->scrollbar.visible) {
        GfxOpenGL21_draw_scrollbar(self, &ui->scrollbar);
    }
}

static void GfxOpenGL21_draw_flash(GfxOpenGL21* self, float fraction)
{
    // TODO: use VBOs
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
    GfxOpenGL21* gfx = gfxOpenGL21(self);

    VtLine *begin, *end;
    Vt_get_visible_lines(vt, &begin, &end);

    glClearColor(ColorRGBA_get_float(settings.bg, 0), ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2), ColorRGBA_get_float(settings.bg, 3));

    glClear(GL_COLOR_BUFFER_BIT);
    for (VtLine* i = begin; i < end; ++i) {
        GfxOpenGL21_rasterize_line(gfx, vt, i, i - begin, false);
    }
    glDisable(GL_BLEND);

    glEnable(GL_SCISSOR_TEST);
    Pair_uint32_t chars = Gfx_get_char_size(self);
    if (vt->scrolling_visual) {
        glScissor(0, 0, chars.first * gfx->glyph_width_pixels, gfx->win_h);
    } else {
        glScissor(0, gfxOpenGL21(self)->win_h - chars.second * gfx->line_height_pixels,
                  chars.first * gfx->glyph_width_pixels, chars.second * gfx->line_height_pixels);
    }

    glLoadIdentity();

    Vector_clear_GlyphBufferData(gfxOpenGL21(self)->vec_glyph_buffer);
    gfxOpenGL21(self)->has_blinking_text = false;
    for (VtLine* i = begin; i < end; ++i) {
        GfxOpenGL21_generate_line_quads(gfx, i, i - begin);
    }

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
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(self->recycled_line_textures); ++i) {
        Texture_destroy(&self->recycled_line_textures[i]);
    }
}

__attribute__((hot)) void GfxOpenGL21_destroy_proxy(Gfx* self, int32_t* proxy)
{
    // TODO: store for reuse
    if (likely(proxy[PROXY_INDEX_TEXTURE])) {
        glDeleteTextures(unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK]) ? 2 : 1,
                         (GLuint*)&proxy[PROXY_INDEX_TEXTURE]);
        proxy[PROXY_INDEX_TEXTURE]       = 0;
        proxy[PROXY_INDEX_TEXTURE_BLINK] = 0;
        proxy[PROXY_INDEX_TEXTURE_SIZE]  = 0;
    } else if (unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK])) {
        glDeleteTextures(1, (GLuint*)&proxy[PROXY_INDEX_TEXTURE_BLINK]);
        proxy[PROXY_INDEX_TEXTURE]       = 0;
        proxy[PROXY_INDEX_TEXTURE_BLINK] = 0;
        proxy[PROXY_INDEX_TEXTURE_SIZE]  = 0;
    }
}

void GfxOpenGL21_destroy(Gfx* self)
{
    GfxOpenGL21_destroy_recycled_proxies(gfxOpenGL21(self));

    Cache_destroy(gfxOpenGL21(self)->cache);
    Atlas_destroy(gfxOpenGL21(self)->atlas);

    if (settings.font_file_name_bold.str) {
        Cache_destroy(&gfxOpenGL21(self)->_cache_bold);
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_bold);
        FT_Done_Face(gfxOpenGL21(self)->face_bold);
    }

    if (settings.font_file_name_italic.str) {
        Cache_destroy(&gfxOpenGL21(self)->_cache_italic);
        Atlas_destroy(&gfxOpenGL21(self)->_atlas_italic);
        FT_Done_Face(gfxOpenGL21(self)->face_italic);
    }

    VBO_destroy(&gfxOpenGL21(self)->font_vao);
    VBO_destroy(&gfxOpenGL21(self)->bg_vao);

    Shader_destroy(&gfxOpenGL21(self)->font_shader);
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

    Vector_destroy_vertex_t(&(gfxOpenGL21(self)->vec_vertex_buffer));
    Vector_destroy_vertex_t(&(gfxOpenGL21(self)->vec_vertex_buffer2));

    FT_Done_Face(gfxOpenGL21(self)->face);

    if (settings.font_file_name_fallback.str) {
        FT_Done_Face(gfxOpenGL21(self)->face_fallback);
    }

    if (settings.font_file_name_fallback2.str) {
        FT_Done_Face(gfxOpenGL21(self)->face_fallback2);
    }

    FT_Done_FreeType(gfxOpenGL21(self)->ft);
}
