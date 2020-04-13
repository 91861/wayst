/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "vt.h"
#include "gfx.h"

#include <math.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <GL/gl.h>

#include <freetype/ftlcdfil.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_TRUETYPE_TABLES_H
#include FT_FONT_FORMATS_H

#include "gl.h"
#include "shaders.h"
#include "util.h"
#include "fterrors.h"
#include "wcwidth/wcwidth.h"

/* number of buckets in the non-ascii glyph map */
#define NUM_BUCKETS 64

#define ATLAS_SIZE_LIMIT INT32_MAX

/* time to stop cursor blinking after inaction */
#define ACTION_SUSPEND_BLINK_MS 500

/* time to suspend cursor blinking for after action */
#define ACTION_END_BLINK_S       10

#define SCROLLBAR_FADE_MAX 100
#define SCROLLBAR_FADE_MIN   0
#define SCROLLBAR_FADE_INC   1
#define SCROLLBAR_FADE_DEC   1

#define PROXY_INDEX_TEXTURE       0
#define PROXY_INDEX_TEXTURE_BLINK 1
#define PROXY_INDEX_TEXTURE_SIZE  2

typedef struct
{
    uint32_t code;
    float left, top;
    bool is_color;
    Texture tex;

} GlyphUnitCache;


static void
GlyphUnitCache_destroy(GlyphUnitCache* self);

DEF_VECTOR(GlyphUnitCache, GlyphUnitCache_destroy)

typedef struct
{
    Vector_GlyphUnitCache buckets[NUM_BUCKETS];

} Cache;


struct Atlas_char_info
{
    float left, top;
    int32_t rows, width;
    float tex_coords[4];
};


#define ATLAS_RENDERABLE_START 32
#define ATLAS_RENDERABLE_END CHAR_MAX
typedef struct
{
    GLuint tex;
    uint32_t w, h;

    GLuint vbo;
    GLuint ibo;

    struct Atlas_char_info char_info[ATLAS_RENDERABLE_END +1 - ATLAS_RENDERABLE_START];

} Atlas;


/* GLOBALS */
static GLint max_tex_res;

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

Vector_vertex_t vec_vertex_buffer;
Vector_vertex_t vec_vertex_buffer2;

Vector_GlyphBufferData _vec_glyph_buffer;
Vector_GlyphBufferData _vec_glyph_buffer_italic;
Vector_GlyphBufferData _vec_glyph_buffer_bold;

Vector_GlyphBufferData* vec_glyph_buffer;
Vector_GlyphBufferData* vec_glyph_buffer_italic;
Vector_GlyphBufferData* vec_glyph_buffer_bold;

VBO flex_vbo = { 0 };
VBO flex_vbo_italic = { 0 };
VBO flex_vbo_bold = { 0 };

/* pen position to begin drawing font */
float pen_begin;
float pen_begin_pixels;

static bool lcd_filter;

static uint32_t win_w, win_h;

static FT_Library ft;
static FT_Face face;
static FT_Face face_bold;
static FT_Face face_italic;
static FT_Face face_fallback;
static FT_Face face_fallback2;
static FT_GlyphSlot g;

static float line_height, glyph_width;
static uint16_t line_height_pixels, glyph_width_pixels;
static size_t max_cells_in_line;

static float sx, sy;

static Framebuffer line_fb;

static VBO font_vao;
static VBO bg_vao;
static VBO line_vao;

static VBO line_bg_vao;

static Shader font_shader;
static Shader bg_shader;
static Shader line_shader;
static Shader image_shader;
static Shader image_tint_shader;

static ColorRGB color;
static ColorRGBA bg_color;

static Cache _cache;
static Cache _cache_bold;
static Cache _cache_italic;

static Cache* cache;
static Cache* cache_bold;
static Cache* cache_italic;

static Atlas _atlas;
static Atlas _atlas_bold;
static Atlas _atlas_italic;
static Atlas* atlas;
static Atlas* atlas_bold;
static Atlas* atlas_italic;

static Texture squiggle_texture;

static bool has_blinking_text;
static TimePoint blink_switch;
static TimePoint blink_switch_text;
static TimePoint action;
static TimePoint inactive;

static bool in_focus;
static bool draw_blinking;
static bool draw_blinking_text;
static bool recent_action;
static bool is_inactive;

static int scrollbar_fade = SCROLLBAR_FADE_MIN; // start with scrollbar hidden

static Timer flash_timer = {{0,0}, {0,0}};
static float flash_fraction = 1.0f;

void
gl_flash()
{
    if (!settings.no_flash)
        flash_timer = Timer_from_now_to_ms_from_now(300);
}


void
Atlas_destroy(Atlas* self)
{
    glDeleteTextures(1, &self->tex);
}


/**
 * @return offset into info buffer */
__attribute__((always_inline, hot))
static inline int32_t
Atlas_select(Atlas* self, uint32_t code)
{
    if (code < 32 || code > CHAR_MAX)
        return -1;
    else {
        glBindTexture(GL_TEXTURE_2D, self->tex);
        return code -32;
    }
}


Atlas
Atlas_new(FT_Face face_)
{

    Atlas self;

    self.w = self.h = 0;
    uint32_t wline = 0, hline = 0, limit = MIN(max_tex_res, ATLAS_SIZE_LIMIT);

    for (int i = ATLAS_RENDERABLE_START; i <= ATLAS_RENDERABLE_END; i++) {
        if (FT_Load_Char(face_, i, FT_LOAD_TARGET_LCD))
            WRN("font error");

        g = face_->glyph;

        uint32_t char_width = g->bitmap.width / (lcd_filter ? 3 : 1);
        uint32_t char_height = g->bitmap.rows;

        if (wline + char_width < (uint32_t) limit) {
            wline += char_width;
            hline = char_height > hline ? char_height : hline;
        } else {
            self.h += hline;
            self.w = wline > self.w ? wline : self.w;
            hline = char_height;
            wline = char_width;
        }
    }

    if (wline > self.w)
        self.w = wline;

    self.h += hline +1;

    if (self.h > (uint32_t) max_tex_res)
        ERR("Failed to generate font atlas, target texture to small");

    glGenTextures(1, &self.tex);
    glBindTexture(GL_TEXTURE_2D, self.tex);

    // faster than clamp
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA, self.w, self.h, 0, GL_BGR, GL_UNSIGNED_BYTE, 0);

    hline = 0;
    uint32_t ox = 0, oy = 0;
    for (int i = ATLAS_RENDERABLE_START; i < ATLAS_RENDERABLE_END; i++) {
        if (FT_Load_Char(face_, i, lcd_filter ?
                         FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_MONO) ||
            FT_Render_Glyph(face_->glyph, lcd_filter ?
                            FT_RENDER_MODE_LCD : FT_RENDER_MODE_MONO))
        {
            WRN("font error");
        }
        
        g = face_->glyph;
        
        uint32_t char_width = g->bitmap.width / (lcd_filter ? 3 : 1);
        uint32_t char_height = g->bitmap.rows;
        
        if (ox + char_width > self.w) {
            oy += hline;
            ox = 0;
            hline = char_height;
        } else
            hline = char_height > hline ? char_height : hline;
        
        glTexSubImage2D(GL_TEXTURE_2D, 0, ox, oy,
                        char_width, char_height,
                        GL_RGB, GL_UNSIGNED_BYTE,
                        g->bitmap.buffer);
        
        self.char_info[i -ATLAS_RENDERABLE_START] = (struct Atlas_char_info) {
            .rows = g->bitmap.rows,
            .width = g->bitmap.width,
            .left = g->bitmap_left,
            .top = g->bitmap_top,
            .tex_coords = {
                (float) ox / self.w,
                
                1.0f - ((float) (self.h - oy) / self.h),
                
                (float) ox / self.w + (float) char_width / self.w,
                
                1.0f - ((float) (self.h - oy) / self.h -
                        (float) char_height / self.h)
            }
        };

        ox += char_width;
    }

    return self;
}


static void
GlyphUnitCache_destroy(GlyphUnitCache* self)
{
    Texture_destroy(&self->tex);
}


static void
Cache_init(Cache* c)
{
    for (size_t i = 0; i < NUM_BUCKETS; ++i)
        c->buckets[i] = Vector_new_GlyphUnitCache();
}


static inline Vector_GlyphUnitCache*
Cache_select_bucket(Cache* self, uint32_t code)
{
    return &self->buckets[(NUM_BUCKETS -1) % code];
}


static inline void
Cache_destroy(Cache* self)
{
    for (int i = 0; i < NUM_BUCKETS; ++i)
        Vector_destroy_GlyphUnitCache(&self->buckets[i]);
}


__attribute__((hot, flatten))
static GlyphUnitCache*
Cache_get_glyph(Cache* self, FT_Face face, Rune code)
{
    Vector_GlyphUnitCache* block = Cache_select_bucket(self, code);

    GlyphUnitCache* found = NULL;
    while ((found = Vector_iter_GlyphUnitCache(block, found))) {
        if (found->code == code) {
            glBindTexture(GL_TEXTURE_2D, found->tex.id);
            return found;
        }
    }

    FT_Error e;
    bool color = false;
    int32_t index = FT_Get_Char_Index(face, code);

    if (FT_Load_Char(face, code, FT_LOAD_TARGET_LCD) ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD))
    {
        WRN("Glyph error in main font %d \n", code);
    } else if (face->glyph->glyph_index == 0) {
        /* Glyph is missing im main font */
        index = FT_Get_Char_Index(face_fallback, code);
        if (FT_Load_Glyph(face_fallback, index, FT_LOAD_TARGET_LCD) ||
            FT_Render_Glyph(face_fallback->glyph, FT_RENDER_MODE_LCD))
        {
            WRN("Glyph error in fallback font %d \n", code);
        } else if (face_fallback->glyph->glyph_index == 0) {
            color = true;
            self = cache; // put this in the 'Regular style' map
            index = FT_Get_Char_Index(face_fallback2, code);

            if ((e = FT_Load_Glyph(face_fallback2, index, FT_LOAD_COLOR))) {
                WRN("Glyph load error2 %d %s | %d (%d)\n", e, stringify_ft_error(e), code, index);
            } else if ((e = FT_Render_Glyph(face_fallback2->glyph, FT_RENDER_MODE_NORMAL)))
                WRN("Glyph render error2 %d %s | %d (%d)\n", e, stringify_ft_error(e), code, index);
            if (face_fallback2->glyph->glyph_index == 0) {
                WRN("Missing glyph %d\n", code);
            }
            g = face_fallback2->glyph;
        } else
            g = face_fallback->glyph;
    } else
        g = face->glyph;

    // In general color characters don't scale
    if (g->bitmap.rows > line_height_pixels) {
        color = true;
    }

    Texture tex = {
        .id = 0,
        .has_alpha = false,
        .w = g->bitmap.width / 3,
        .h = g->bitmap.rows
    };

    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D,
                    GL_TEXTURE_MIN_FILTER,
                    color ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 color ? GL_RGBA : GL_RGB,
                 (tex.w = g->bitmap.width / (lcd_filter && !color ? 3 : 1)),
                 (tex.h = g->bitmap.rows),
                 0,
                 color ? GL_RGBA : GL_RGB,
                 GL_UNSIGNED_BYTE,
                 g->bitmap.buffer);

    if (color) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    
    Vector_push_GlyphUnitCache(block, (GlyphUnitCache) {
        .code = code,
        .is_color = color,
        .left = (float) g->bitmap_left,
        .top  = (float) g->bitmap_top,
        .tex  = tex,
    });

    return Vector_at_GlyphUnitCache(block, block->size - 1);
}


// Generate a sinewave image and store it as an OpenGL texture
__attribute__((cold))
Texture
create_squiggle_texture(uint32_t w, uint32_t h, uint32_t thickness)
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

    double pixel_size = 2.0 / h;
    double stroke_width = thickness * pixel_size;
    double stroke_fade = pixel_size * 2.0;
    double distance_limit_full_alpha = stroke_width / 2.0;
    double distance_limit_zero_alpha = stroke_width / 2.0 + stroke_fade;
    
    for (int_fast32_t x = 0; x < w; ++x) for (int_fast32_t y = 0; y < h; ++y) {
        uint8_t* fragment = &fragments[(y * w + x) * 4];

        #define DISTANCE(_x, _y, _x2, _y2)\
            (sqrt(pow((_x2) - (_x), 2) + pow((_y2) - (_y), 2)))

        double x_frag = (double) x / (double) w * 2.0 * M_PI;
        double y_frag = (double) y / (double) h *
            (2.0 + stroke_width * 2.0 + stroke_fade * 2.0)
            - 1.0 - stroke_width - stroke_fade;

        double y_curve = sin(x_frag);
        double dx_frag = cos(x_frag); // x/xd -> in what dir is closest point
        double y_dist = y_frag - y_curve;
        double closest_distance = DISTANCE(x_frag, y_frag, x_frag, y_curve);

        double step = dx_frag * y_dist < 0.0 ? 0.001 : -0.001;

        for (double i = x_frag + step;; i += step) {
            double i_distance = DISTANCE(x_frag, y_frag, i, sin(i));
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
            double alpha = 1.0 - (closest_distance - distance_limit_full_alpha) /
                (distance_limit_zero_alpha - distance_limit_full_alpha);

            fragment[0] = fragment[1] = fragment[2] = UINT8_MAX;
            fragment[3] = CLAMP(alpha * UINT8_MAX , 0, UINT8_MAX);
        }
    }


    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, fragments);

    free(fragments);

    return (Texture) {
        .id = tex,
        .has_alpha = true,
        .w = w,
        .h = h
    };
}


void
gl_set_size(uint32_t w, uint32_t h)
{
    win_w = w;
    win_h = h;

    glViewport(0, 0, w, h);

    sx = 2.0f / win_w;
    sy = 2.0f / win_h;

    static uint32_t height = 0, hber;

    if (!height) {
        // add one pixel to hide lcd filter artefacting
        height = face->size->metrics.height +64;
        hber = face->glyph->metrics.horiBearingY;
    }

    /* there seems to be no better way of calculating this */
    line_height_pixels = height / 64.0;

    line_height = (float) height *sy /64.0 ;

    pen_begin = +sy * (height /64 /1.75)
        +sy * ((hber +184) /2 /64);
    pen_begin_pixels = (float)(height /64 /1.75)
        + (float) ((hber +184) /2 /64);

    static uint32_t gw = 0;
    if (!gw) {
        gw = face->glyph->advance.x;
    }

    glyph_width_pixels = gw / 64;
    glyph_width = gw * sx / 64.0;

    LOG("glyph box size: %fx%f\n", glyph_width, line_height);

    max_cells_in_line = win_w / glyph_width_pixels;

    // update dynamic bg buffer
    glBindBuffer(GL_ARRAY_BUFFER, bg_vao.vbo);

    glVertexAttribPointer(
      bg_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);

    float bg_box[] = {
        0.0f,        0.0f,
        0.0f,        line_height,
        glyph_width, line_height,
        glyph_width, 0.0f,
    };

    glBufferData(GL_ARRAY_BUFFER, sizeof bg_box, bg_box, GL_STREAM_DRAW);
}


__attribute__((always_inline))
inline Pair_uint32_t
gl_get_char_size()
{
    return (Pair_uint32_t) { .first = 2.0f / glyph_width,
                             .second = 2.0f / line_height };
}


Pair_uint32_t
gl_pixels(uint32_t c, uint32_t r)
{
    static uint32_t gw = 0;
    if (!gw) {
        gw = face->glyph->advance.x;
    }

    float x, y;
    x = c * gw;
    y = r * (face->size->metrics.height +64);

    return (Pair_uint32_t) { .first = x / 64.0,
                             .second = y / 64.0 };
}


void
gl_init_font()
{
    if (FT_Init_FreeType(&ft) ||
        FT_New_Face(ft, settings.font_name, 0, &face))
    {
        ERR("Font error, font file: %s", settings.font_name);
    }

    if (FT_Set_Char_Size(face,
                         settings.font_size * 64,
                         settings.font_size * 64,
                         settings.font_dpi,
                         settings.font_dpi))
    {
        LOG("Failed to set font size\n");
    }

    if (!FT_IS_FIXED_WIDTH(face)) {
        WRN("main font is not fixed width");
    }

    if (settings.font_name_bold) {
        if (FT_New_Face(ft, settings.font_name_bold, 0, &face_bold))
            ERR("Font error, font file: %s", settings.font_name_bold);

        if (FT_Set_Char_Size(face_bold,
                             settings.font_size * 64,
                             settings.font_size * 64,
                             settings.font_dpi,
                             settings.font_dpi))
        {
            LOG("Failed to set font size\n");
        }

        if (!FT_IS_FIXED_WIDTH(face_bold)) {
            WRN("bold font is not fixed width");
        }
    }


    if (settings.font_name_italic) {
        if (FT_New_Face(ft, settings.font_name_italic, 0, &face_italic))
            ERR("Font error, font file: %s", settings.font_name_italic);

        if (FT_Set_Char_Size(face_italic,
                             settings.font_size * 64,
                             settings.font_size * 64,
                             settings.font_dpi,
                             settings.font_dpi))
        {
            LOG("Failed to set font size\n");
        }

        if (!FT_IS_FIXED_WIDTH(face_italic)) {
            WRN("italic font is not fixed width");
        }
    }


    if (settings.font_name_fallback) {
        if (FT_New_Face(ft, settings.font_name_fallback, 0, &face_fallback))
            ERR("Font error, font file: %s", settings.font_name_fallback);

        if (FT_Set_Char_Size(face_fallback,
                             settings.font_size * 64,
                             settings.font_size * 64,
                             settings.font_dpi,
                             settings.font_dpi))
        {
            LOG("Failed to set font size\n");
        }
    }

    if (settings.font_name_fallback2) {
        if (FT_New_Face(ft, settings.font_name_fallback2, 0, &face_fallback2))
            ERR("Font error, font file: %s", settings.font_name_fallback2);

        FT_Select_Size(face_fallback2, 0 );
    }

    const char* fmt = FT_Get_Font_Format(face);
    if (strcmp(fmt, "TrueType") &&
        strcmp(fmt, "CFF"))
    {
        ERR("Font format \"%s\" not supported", fmt);
    }

    if (FT_Library_SetLcdFilter(ft, FT_LCD_FILTER_DEFAULT))
        lcd_filter = true;
    else {
        WRN("LCD filtering not avaliable\n");
        lcd_filter = false;
    }

    /**
     * Load a character we will be centering the entire text to. */
    if (FT_Load_Char(face, '|', FT_LOAD_TARGET_LCD) ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_LCD))
    {
        WRN("Glyph error\n");
    }
}


void
gl_init_renderer()
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

    glEnable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));

    font_shader =
      Shader_new(font_vs_src, font_fs_src,
                 "coord", "tex", "clr", "bclr", NULL);

    bg_shader = Shader_new(bg_vs_src, bg_fs_src, "pos", "mv", "clr", NULL);

    line_shader = Shader_new(line_vs_src, line_fs_src, "pos", "clr", NULL);

    image_shader = Shader_new(image_rgb_vs_src, image_rgb_fs_src,
                              "coord", "tex", NULL);

    image_tint_shader = Shader_new(image_rgb_vs_src, image_tint_rgb_fs_src,
                                   "coord", "tex", "tint", NULL);

    bg_vao = VBO_new(2, 1, bg_shader.attribs);

    line_bg_vao = VBO_new(2, 1, bg_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, line_bg_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) *4 *4, NULL, GL_STREAM_DRAW);

    font_vao = VBO_new(4, 1, font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, font_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) *4 *4, NULL, GL_STREAM_DRAW);

    line_vao = VBO_new(2, 1, line_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, line_vao.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) *8, NULL, GL_STREAM_DRAW);

    flex_vbo = VBO_new(4, 1, font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) *4 *4, NULL, GL_STREAM_DRAW);
    
    flex_vbo_italic = VBO_new(4, 1, font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, flex_vbo_italic.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) *4 *4, NULL, GL_STREAM_DRAW);

    flex_vbo_bold = VBO_new(4, 1, font_shader.attribs);
    glBindBuffer(GL_ARRAY_BUFFER, flex_vbo_bold.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) *4 *4, NULL, GL_STREAM_DRAW);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_res);

    color = settings.fg;
    bg_color = settings.bg;

    Shader_use(&font_shader);
    glUniform3f(font_shader.uniforms[1].location,
                ColorRGB_get_float(settings.fg, 0),
                ColorRGB_get_float(settings.fg, 1),
                ColorRGB_get_float(settings.fg, 2));


    _atlas = Atlas_new(face);
    atlas = &_atlas;

    Cache_init(&_cache);
    cache = &_cache;

    line_fb = Framebuffer_new();

    in_focus = true;
    recent_action = true;
    draw_blinking = true;
    draw_blinking_text = true;

    blink_switch = TimePoint_ms_from_now(settings.text_blink_interval);
    blink_switch_text = TimePoint_now();


    _vec_glyph_buffer = Vector_new_with_capacity_GlyphBufferData(80);
    vec_glyph_buffer = & _vec_glyph_buffer;
    
    /* if font styles don't exist point their resources to deaults */
    if (settings.font_name_bold) {
        _atlas_bold = Atlas_new(face_bold);
        atlas_bold = &_atlas_bold;

        Cache_init(&_cache_bold);
        cache_bold = &_cache_bold;

        _vec_glyph_buffer_bold = Vector_new_with_capacity_GlyphBufferData(20);
        vec_glyph_buffer_bold = &_vec_glyph_buffer_bold;
    } else {
        atlas_bold = &_atlas;
        cache_bold = &_cache;

        vec_glyph_buffer_bold = &_vec_glyph_buffer;
    }

    if (settings.font_name_italic) {
        _atlas_italic = Atlas_new(face_italic);
        atlas_italic = &_atlas_italic;

        Cache_init(&_cache_italic);
        cache_italic = &_cache_italic;

        _vec_glyph_buffer_italic = Vector_new_with_capacity_GlyphBufferData(20);
        vec_glyph_buffer_italic = &_vec_glyph_buffer_italic;
    } else {
        atlas_italic = &_atlas;
        cache_italic = &_cache;

        vec_glyph_buffer_italic = &_vec_glyph_buffer;
    }

    vec_vertex_buffer = Vector_new_vertex_t();
    vec_vertex_buffer2 = Vector_new_vertex_t();


    gl_reset_action_timer();

    float height = face->size->metrics.height +64;
    line_height_pixels = height / 64.0;

    uint32_t t_height = CLAMP(line_height_pixels /8.0 +2, 4, UINT8_MAX);

    squiggle_texture = create_squiggle_texture(t_height * M_PI / 2.0,
                                               t_height,
                                               CLAMP(t_height / 5,1,10));
}


bool
gl_set_focus(const bool focus)
{
    bool ret = false;
    if (in_focus && !focus)
        ret = true;
    in_focus = focus;
    return ret;
}


void
gl_reset_action_timer()
{
    blink_switch = TimePoint_ms_from_now(settings.text_blink_interval);
    draw_blinking = true;
    recent_action = true;
    action = TimePoint_ms_from_now(settings.text_blink_interval +
            ACTION_SUSPEND_BLINK_MS);

    inactive = TimePoint_s_from_now(ACTION_END_BLINK_S);
}


bool
gl_check_timers(Vt* vt)
{
    bool repaint = false;


    if (TimePoint_passed(blink_switch_text)) {
        blink_switch_text = TimePoint_ms_from_now(settings.text_blink_interval);
        draw_blinking_text = !draw_blinking_text;
        if (unlikely(has_blinking_text)) {
            repaint = true;
        }
    }

    if (!in_focus && !has_blinking_text){
        return false;
    }

    float fraction = Timer_get_fraction_clamped_now(&flash_timer);
    if (fraction != flash_fraction) {
        flash_fraction = fraction;
        repaint = true;
    }

    if (vt->scrollbar.visible) {
        if (scrollbar_fade < SCROLLBAR_FADE_MAX) {
            scrollbar_fade = MIN(scrollbar_fade + SCROLLBAR_FADE_INC, SCROLLBAR_FADE_MAX);
            repaint = true;
        }
    } else {
        if (scrollbar_fade > SCROLLBAR_FADE_MIN) {
            scrollbar_fade = MAX(scrollbar_fade - SCROLLBAR_FADE_DEC, SCROLLBAR_FADE_MIN);
            repaint = true;
        }
    }


    if (recent_action && TimePoint_passed(action)) {
        /* start blinking cursor */
        recent_action = false;
        blink_switch = TimePoint_ms_from_now(settings.text_blink_interval);
        draw_blinking = !draw_blinking;
        repaint = true;
    }


    if (TimePoint_passed(inactive) &&

        // all animations finished
        ((vt->scrollbar.visible && scrollbar_fade == SCROLLBAR_FADE_MAX) ||
         (!vt->scrollbar.visible && scrollbar_fade == SCROLLBAR_FADE_MIN)) &&

        draw_blinking)
    {
        // dsiable cursor blinking
        is_inactive = true;
    } else {
        if (TimePoint_passed(blink_switch)) {
            blink_switch = TimePoint_ms_from_now(settings.text_blink_interval);
            draw_blinking = !draw_blinking;
            if (!(recent_action && !draw_blinking))
                repaint = true;
        }
    }

    return repaint;
}


__attribute__((hot, always_inline))
static inline void
gfx_push_line_quads(VtLine* const vt_line, uint_fast16_t line_index)
{

    if (vt_line->proxy.data[0]) {
        float tex_end_x = -1.0f + vt_line->data.size * glyph_width_pixels * sx;
        float tex_begin_y = 1.0f - line_height_pixels * (line_index +1) * sy;
        Vector_push_GlyphBufferData(
            vec_glyph_buffer,
            (GlyphBufferData) {{
                    {
                        -1.0f,
                        tex_begin_y + line_height,
                        0.0f,
                        0.0f
                    }, {
                        -1.0,
                        tex_begin_y,
                        0.0f,
                        1.0f
                    }, {
                        tex_end_x,
                        tex_begin_y,
                        1.0f,
                        1.0f
                    }, {
                        tex_end_x,
                        tex_begin_y + line_height,
                        1.0f,
                        0.0f
                    },
        }});
    }
}


static uint_fast32_t quad_index;
__attribute__((hot, always_inline))
static inline void
gfx_draw_line_quads(VtLine* const vt_line)
{

    if (vt_line->proxy.data[0]) {

        if (vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK])
            has_blinking_text = true;

        glBindTexture(GL_TEXTURE_2D,
                      vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK]
                      && !draw_blinking_text ?
                        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] :
                        vt_line->proxy.data[PROXY_INDEX_TEXTURE]);
        glDrawArrays(GL_QUADS, quad_index *4, 4);
        ++quad_index;
    }
}


__attribute__((hot))
static inline void
gfx_rasterize_line(const Vt* const vt,
                   VtLine* vt_line,
                   const size_t line,
                   bool is_for_blinking)
{
    const size_t length = vt_line->data.size;
    bool has_blinking_chars = false;

    
    if (!is_for_blinking) {
        if (likely(!vt_line->damaged || !vt_line->data.size))
            return;

        if (likely(vt_line->proxy.data[0])) {
            gfx_destroy_line_proxy(vt_line->proxy.data);
        }
    }

    #define BOUND_RESOURCES_NONE 0
    #define BOUND_RESOURCES_BG 1
    #define BOUND_RESOURCES_FONT 2
    #define BOUND_RESOURCES_LINES 3
    #define BOUND_RESOURCES_IMAGE 4
    int_fast8_t bound_resources = BOUND_RESOURCES_NONE;

    float texture_width = vt_line->data.size * glyph_width_pixels;
    float texture_height = line_height_pixels;

    float scalex = 2.0f / texture_width;
    float scaley = 2.0f / texture_height;

    Framebuffer_generate_texture_attachment(&line_fb,
                                            texture_width,
                                            texture_height);

    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));

    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);

    static float buffer[] = {
        -1.0f,               -1.0f,
        -1.0f,                1.0f,
        0.0f/*overwritten*/,  1.0f,
        0.0f/*overwritten*/, -1.0f,
    };

    ColorRGBA bg_color = settings.bg;
    
    VtRune *c = vt_line->data.buf, *c_begin = vt_line->data.buf;

    for (size_t i = 0; i <= vt_line->data.size; ++i) {
        c = vt_line->data.buf + i;

        if (likely(i != vt_line->data.size) && unlikely(c->blinkng))
            has_blinking_chars = true;

        if (i == length ||
            !ColorRGBA_eq(Vt_selection_should_highlight_char(vt, i, line)?
                          settings.bghl : c->bg, bg_color))
        {
            int extra_width = 0;

            if (!ColorRGBA_eq(bg_color, settings.bg)) {

                if (i > 1)
                    extra_width = wcwidth(vt_line->data.buf[i -1].code) -1;
                
                buffer[4] = buffer[6] = -1.0f + (i + extra_width)
                    * scalex * glyph_width_pixels; // set buffer end 

                if (bound_resources != BOUND_RESOURCES_BG) {
                    glBindBuffer(GL_ARRAY_BUFFER, line_bg_vao.vbo);
                    glVertexAttribPointer(bg_shader.attribs->location, 2, GL_FLOAT, GL_FALSE, 0, 0);
                    Shader_use(&bg_shader);
                    glUniform2f(bg_shader.uniforms[0].location, 0.0f, 0.0f);

                    bound_resources = BOUND_RESOURCES_BG;
                }

                glBufferData(GL_ARRAY_BUFFER, sizeof buffer, buffer, GL_STREAM_DRAW);

                glUniform4f(bg_shader.uniforms[1].location,
                            ColorRGBA_get_float(bg_color, 0),
                            ColorRGBA_get_float(bg_color, 1),
                            ColorRGBA_get_float(bg_color, 2),
                            ColorRGBA_get_float(bg_color, 3));

                glDrawArrays(GL_QUADS, 0, 4);
            }

            { // for each block with the same background color
                ColorRGB fg_color = settings.fg;
                const VtRune* r_begin = c_begin;

                for (const VtRune* r = c_begin; r != c +1; ++r) {
                    if (r == c || !ColorRGB_eq(fg_color, r->fg)) {

                        { //for each block with the same fg color
                            vec_glyph_buffer->size = 0;
                            vec_glyph_buffer_italic->size = 0;
                            vec_glyph_buffer_bold->size = 0;

                            static VtRune k_cpy;
                            static const VtRune* z;
                            for (const VtRune* k = r_begin; k != r; ++k) {
                                size_t column = k - vt_line->data.buf;

                                if (is_for_blinking && z->blinkng) {
                                    k_cpy = *z;
                                    k_cpy.code = ' ';
                                    z = &k_cpy;
                                } else {
                                    z = k;
                                }

                                if (z->code > ATLAS_RENDERABLE_START &&
                                    z->code <= ATLAS_RENDERABLE_END)
                                {
                                    // pull data from font atlas
                                    struct Atlas_char_info* g;
                                    int32_t atlas_offset = -1;

                                    Vector_GlyphBufferData* target =
                                        vec_glyph_buffer;
                                    Atlas* source_atlas = atlas;

                                    switch (expect(z->state, VT_RUNE_NORMAL)) {
                                    case VT_RUNE_ITALIC:
                                        target = vec_glyph_buffer_italic;
                                        source_atlas = atlas_italic;
                                        break;
                                        
                                    case VT_RUNE_BOLD:
                                        target = vec_glyph_buffer_bold;
                                        source_atlas = atlas_bold;
                                        break;
                                        
                                    default:;
                                    }

                                    atlas_offset = Atlas_select(source_atlas,
                                                                z->code);

                                    g = &source_atlas->char_info[atlas_offset];
                                    float h = (float) g->rows * scaley;
                                    float w = (float) g->width /
                                        (lcd_filter ? 3.0f : 1.0f) * scalex;
                                    float t = (float) g->top * scaley;
                                    float l = (float) g->left * scalex;

                                    float x3 = -1.0f
                                        + (float) column * glyph_width_pixels
                                            * scalex
                                        + l;
                                    float y3 = -1.0f + pen_begin_pixels *
                                        scaley -t;

                                    Vector_push_GlyphBufferData(
                                        target,
                                        (GlyphBufferData) {{
                                            {
                                                x3,
                                                y3,
                                                g->tex_coords[0],
                                                g->tex_coords[1]
                                            }, {
                                                x3 + w,
                                                y3,
                                                g->tex_coords[2],
                                                g->tex_coords[1]
                                            }, {
                                                x3 +w,
                                                y3 +h,
                                                g->tex_coords[2],
                                                g->tex_coords[3]
                                            }, {
                                                x3,
                                                y3 +h,
                                                g->tex_coords[0],
                                                g->tex_coords[3]
                                            },
                                    }});
                                }
                            }

                            if (vec_glyph_buffer->size ||
                                vec_glyph_buffer_italic->size ||
                                vec_glyph_buffer_bold->size)
                            {
                                bound_resources = BOUND_RESOURCES_FONT;

                                glUseProgram(font_shader.id);

                                glUniform3f(font_shader.uniforms[1].location,
                                            ColorRGB_get_float(fg_color, 0),
                                            ColorRGB_get_float(fg_color, 1),
                                            ColorRGB_get_float(fg_color, 2));

                                glUniform3f(font_shader.uniforms[2].location,
                                            ColorRGBA_get_float(bg_color, 0),
                                            ColorRGBA_get_float(bg_color, 1),
                                            ColorRGBA_get_float(bg_color, 2));

                                // normal
                                glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);
                                glVertexAttribPointer(
                                    font_shader.attribs->location,
                                    4, GL_FLOAT, GL_FALSE, 0, 0);

                                size_t newsize = vec_glyph_buffer->size *
                                    sizeof(GlyphBufferData);

                                if (newsize > flex_vbo.size) {
                                    flex_vbo.size = newsize;
                                    glBufferData(GL_ARRAY_BUFFER,
                                                 newsize,
                                                 vec_glyph_buffer->buf,
                                                 GL_STREAM_DRAW);
                                } else {
                                    glBufferSubData(GL_ARRAY_BUFFER,
                                                    0,
                                                    newsize,
                                                    vec_glyph_buffer->buf);
                                }

                                glBindTexture(GL_TEXTURE_2D, atlas->tex);
                                glDrawArrays(GL_QUADS,
                                             0,
                                             vec_glyph_buffer->size * 4);

                                // italic
                                if (vec_glyph_buffer_italic != vec_glyph_buffer) {
                                    glBindBuffer(GL_ARRAY_BUFFER, flex_vbo_italic.vbo);
                                    glVertexAttribPointer(
                                        font_shader.attribs->location,
                                        4, GL_FLOAT, GL_FALSE, 0, 0);

                                    size_t newsize = vec_glyph_buffer_italic->size *
                                        sizeof(GlyphBufferData);

                                    if (newsize > flex_vbo_italic.size) {
                                        flex_vbo_italic.size = newsize;
                                        glBufferData(GL_ARRAY_BUFFER,
                                                    newsize,
                                                    vec_glyph_buffer_italic->buf,
                                                    GL_STREAM_DRAW);
                                    } else {
                                        glBufferSubData(GL_ARRAY_BUFFER,
                                                        0,
                                                        newsize,
                                                        vec_glyph_buffer_italic->buf);
                                    }
                                    glBindTexture(GL_TEXTURE_2D, atlas_italic->tex);
                                    glDrawArrays(GL_QUADS,
                                                 0,
                                                 vec_glyph_buffer_italic->size * 4);
                                }

                                //bold
                                if (vec_glyph_buffer_bold != vec_glyph_buffer) {
                                    glBindBuffer(GL_ARRAY_BUFFER, flex_vbo_bold.vbo);
                                    glVertexAttribPointer(
                                        font_shader.attribs->location,
                                        4, GL_FLOAT, GL_FALSE, 0, 0);

                                    size_t newsize = vec_glyph_buffer_bold->size *
                                        sizeof(GlyphBufferData);

                                    if (newsize > flex_vbo_bold.size) {
                                        flex_vbo_bold.size = newsize;
                                        glBufferData(GL_ARRAY_BUFFER,
                                                    newsize,
                                                    vec_glyph_buffer_bold->buf,
                                                    GL_STREAM_DRAW);
                                    } else {
                                        glBufferSubData(GL_ARRAY_BUFFER,
                                                        0,
                                                        newsize,
                                                        vec_glyph_buffer_bold->buf);
                                    }
                                    glBindTexture(GL_TEXTURE_2D, atlas_bold->tex);
                                    glDrawArrays(GL_QUADS,
                                                 0,
                                                 vec_glyph_buffer_bold->size * 4);
                                }

                            }// end if there are atlas chars to draw

                            vec_glyph_buffer->size = 0;
                            vec_glyph_buffer_bold->size = 0;
                            for (const VtRune* z = r_begin; z != r; ++z) {
                                if (unlikely(z->code > ATLAS_RENDERABLE_END)) {
                                    size_t column = z - vt_line->data.buf;
                                    Cache* ca = cache;
                                    FT_Face fc = face;

                                    switch (z->state) {
                                    case VT_RUNE_ITALIC:
                                        ca = cache_italic;
                                        fc = face_italic;
                                        break;
                                    case VT_RUNE_BOLD:
                                        ca = cache_bold;
                                        fc = face_bold;
                                        break;
                                    default:
                                        ;
                                    }
                                        
                                    GlyphUnitCache* g = NULL;
                                    g = Cache_get_glyph(ca, fc, z->code);

                                    if (!g)
                                        continue;

                                    float h = scaley * g->tex.h;
                                    float w = scalex * g->tex.w; 
                                    float t = scaley * g->top;
                                    float l = scalex * g->left;

                                    if (unlikely(h > 2.0f)) {
                                        const float scale = h / 2.0f;
                                        h /= scale;
                                        w /= scale;
                                        t /= scale;
                                        l /= scale;
                                    }

                                    float x3 = -1.0f
                                        + (double) column *
                                            glyph_width_pixels * scalex 
                                        + l;
                                    float y3 = -1.0f + pen_begin_pixels *
                                        scaley -t;

                                    // Very often characters like this are used
                                    // to draw tables and borders. Render all
                                    // repeating characters in one call.
                                    Vector_push_GlyphBufferData(
                                        unlikely(g->is_color) ?
                                        vec_glyph_buffer_bold :
                                        vec_glyph_buffer,
                                        (GlyphBufferData) {{
                                            {
                                                x3,
                                                y3,
                                                0.0f,
                                                0.0f
                                            }, {
                                                x3 + w,
                                                y3,
                                                1.0f,
                                                0.0f
                                            }, {
                                                x3 +w,
                                                y3 +h,
                                                1.0f,
                                                1.0f
                                            }, {
                                                x3,
                                                y3 +h,
                                                0.0f,
                                                1.0f
                                            }
                                        }});


                                    // needs to change texture on next iteration
                                    if (z+1 != r && z->code != (z+1)->code) {

                                        // Draw noramal characters
                                        if (vec_glyph_buffer->size) {
                                            if (bound_resources != BOUND_RESOURCES_FONT) {
                                                glUseProgram(font_shader.id);
                                                bound_resources = BOUND_RESOURCES_FONT;
                                            }

                                            glUniform3f(font_shader.uniforms[1].location,
                                                        ColorRGB_get_float(fg_color, 0),
                                                        ColorRGB_get_float(fg_color, 1),
                                                        ColorRGB_get_float(fg_color, 2));

                                            glUniform3f(font_shader.uniforms[2].location,
                                                        ColorRGBA_get_float(bg_color, 0),
                                                        ColorRGBA_get_float(bg_color, 1),
                                                        ColorRGBA_get_float(bg_color, 2));

                                            glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);

                                            glVertexAttribPointer(
                                                font_shader.attribs->location,
                                                4, GL_FLOAT, GL_FALSE, 0, 0);

                                            size_t newsize = vec_glyph_buffer->size * sizeof(GlyphBufferData);
                                            if (flex_vbo.size < newsize) {
                                                flex_vbo.size = newsize;
                                                glBufferData(GL_ARRAY_BUFFER,
                                                            newsize,
                                                            vec_glyph_buffer->buf,
                                                            GL_STREAM_DRAW);
                                            } else {
                                                glBufferSubData(GL_ARRAY_BUFFER,
                                                                0,
                                                                newsize,
                                                                vec_glyph_buffer->buf);
                                            }

                                            glDrawArrays(GL_QUADS, 0, vec_glyph_buffer->size *4);

                                            vec_glyph_buffer->size = 0;
                                        }

                                        // Draw color characters
                                        if (vec_glyph_buffer_bold->size) {
                                            if (likely(bound_resources != BOUND_RESOURCES_IMAGE)) {
                                                glUseProgram(image_shader.id);
                                                bound_resources = BOUND_RESOURCES_IMAGE;
                                            }

                                            glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);

                                            glVertexAttribPointer(image_shader.attribs->location,
                                                                4, GL_FLOAT, GL_FALSE, 0, 0);

                                            size_t newsize = vec_glyph_buffer_bold->size * sizeof(GlyphBufferData);
                                            if (flex_vbo.size < newsize) {
                                                flex_vbo.size = newsize;
                                                glBufferData(GL_ARRAY_BUFFER,
                                                            newsize,
                                                            vec_glyph_buffer_bold->buf,
                                                            GL_STREAM_DRAW);
                                            } else {
                                                glBufferSubData(GL_ARRAY_BUFFER,
                                                                0,
                                                                newsize,
                                                                vec_glyph_buffer_bold->buf);
                                            }

                                            glDrawArrays(GL_QUADS, 0, vec_glyph_buffer_bold->size *4);

                                            vec_glyph_buffer_bold->size = 0;
                                        }
                                    }
                                } // end if out of atlas range
                            } // end for each separate texture glyph 
                        } // end for each block with the same bg and fg

                        if (r != c) {
                            r_begin = r;
                            fg_color = r->fg;
                        }
                    } // END for each block with the same color
                } // END for each char
            } // END for each block with the same bg

            buffer[0] = buffer[2] = -1.0f + (i+extra_width) * scalex * glyph_width_pixels; // set background buffer start

            if (i != vt_line->data.size) {
                c_begin = c;

                if (unlikely(Vt_selection_should_highlight_char(vt, i, line))) {
                    bg_color = settings.bghl;
                } else {
                    bg_color = c->bg;
                }
            }
        } // end if bg color changed
    } // end for each VtRune

    // draw lines
    static float begin[5] = { -1, -1, -1, -1, -1 };
    static float end[5] = { 1, 1, 1, 1, 1 };
    static bool drawing[5] = { 0 };

    ColorRGB line_color = vt_line->data.buf->linecolornotdefault ?
        vt_line->data.buf->line : vt_line->data.buf->fg;

    for (const VtRune* z = vt_line->data.buf;
         z <= vt_line->data.buf + vt_line->data.size; ++z)
    {
        size_t column = z - vt_line->data.buf;

        ColorRGB nc = {};
        if (z != vt_line->data.buf + vt_line->data.size)
             nc = z->linecolornotdefault ? z->line : z->fg;

        // State has changed
        if (!ColorRGB_eq(line_color, nc) ||
            z == vt_line->data.buf + vt_line->data.size ||
            z->underlined      != drawing[0] ||
            z->doubleunderline != drawing[1] ||
            z->strikethrough   != drawing[2] ||
            z->overline        != drawing[3] ||
            z->curlyunderline  != drawing[4] )
        {
            if (z == vt_line->data.buf + vt_line->data.size ) {
                for (int_fast8_t tmp = 0; tmp < 5; tmp++) {
                    end[tmp] = -1.0f + (float) column * scalex *
                        (float) glyph_width_pixels;
                }
            } else {

                #define SET_BOUNDS_END(what, index)                     \
                    if (drawing[index]) {                               \
                        end[index] = -1.0f + (float) column * scalex *  \
                            (float) glyph_width_pixels;                 \
                    }

                SET_BOUNDS_END(z->underlined,      0);
                SET_BOUNDS_END(z->doubleunderline, 1);
                SET_BOUNDS_END(z->strikethrough,   2);
                SET_BOUNDS_END(z->overline,        3);
                SET_BOUNDS_END(z->curlyunderline,  4);
            }

            vec_vertex_buffer.size  = 0;
            vec_vertex_buffer2.size = 0;

            if (drawing[0]) {
                Vector_push_vertex_t(&vec_vertex_buffer,
                                 (vertex_t) { begin[0], 1.0f - scaley });
                Vector_push_vertex_t(&vec_vertex_buffer,
                                 (vertex_t) { end[0], 1.0f - scaley });
            }

            if (drawing[1]) {
                Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[1], 1.0f });
                Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { end[1], 1.0f });
                Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[1], 1.0f -2 * scaley });
                Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { end[1],   1.0f -2 * scaley });
            }

            if (drawing[2]) {
                Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[2], .2f });
                Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { end[2], .2f });
            }

            if (drawing[3]) {
                Vector_push_vertex_t(&vec_vertex_buffer,
                                 (vertex_t) { begin[3], -1.0f + scaley });
                Vector_push_vertex_t(&vec_vertex_buffer,
                                 (vertex_t) { end[3], -1.0f + scaley });
            }

            if (drawing[4]) {

                float cw = glyph_width_pixels * scalex;
                int n_cells = round((end[4] - begin[4]) / cw);
                float t_y = 1.0f - squiggle_texture.h * scaley;

                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { begin[4], t_y });
                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { 0.0f, 0.0f });

                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { begin[4], 1.0f });
                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { 0.0f, 1.0f });

                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { end[4], 1.0f });
                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { 1.0f * n_cells, 1.0f });

                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { end[4], t_y });
                Vector_push_vertex_t(&vec_vertex_buffer2,
                                     (vertex_t) { 1.0f * n_cells, 0.0f });
            /*
                float cw = glyph_width_pixels * scalex;

                int n_cells = round((end[4] - begin[4]) / cw);
                for (int i = 0; i < n_cells; ++i) {

                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i,
                                                 1.0f - 2.0f * scaley });
                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i + cw / 4.0f,
                                                 1.0f - 2.0f * scaley });
                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i + cw / 4.0f,
                                                 1.0f - scaley });
                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i + cw * 0.5f,
                                                 1.0f - scaley });

                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i + cw * 0.5f,
                                                 1.0f - 2.0f * scaley });
                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i + cw / 4.0f * 3.0f,
                                                 1.0f - 2.0f * scaley });
                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * i + cw / 4.0f * 3.0f,
                                                 1.0f - scaley });
                    Vector_push_vertex_t(&vec_vertex_buffer,
                                     (vertex_t) { begin[4] + cw * (i +1),
                                                 1.0f - scaley });
                }
            */
            }

            if (vec_vertex_buffer.size) {
                if (bound_resources != BOUND_RESOURCES_LINES) {
                    bound_resources = BOUND_RESOURCES_LINES;
                    Shader_use(&line_shader);
                    glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);
                    glVertexAttribPointer(line_shader.attribs->location,
                                        2, GL_FLOAT, GL_FALSE, 0, 0);
                }

                //glBindTexture(GL_TEXTURE_2D, 0);

                glUniform3f(line_shader.uniforms[1].location,
                            ColorRGB_get_float(line_color, 0),
                            ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));

                size_t new_size = sizeof(vertex_t) * vec_vertex_buffer.size; 
                if (flex_vbo.size < new_size) {
                    flex_vbo.size = new_size;
                    glBufferData(GL_ARRAY_BUFFER,
                                new_size,
                                vec_vertex_buffer.buf,
                                GL_STREAM_DRAW);
                } else {
                    glBufferSubData(GL_ARRAY_BUFFER,
                                    0,
                                    new_size,
                                    vec_vertex_buffer.buf);
                }

                glDrawArrays(GL_LINES, 0, vec_vertex_buffer.size);
            }

            if (vec_vertex_buffer2.size) {
                bound_resources = BOUND_RESOURCES_NONE;
                Shader_use(&image_tint_shader);
                glBindTexture(GL_TEXTURE_2D, squiggle_texture.id);

                glUniform3f(font_shader.uniforms[2].location,
                            ColorRGB_get_float(line_color, 0),
                            ColorRGB_get_float(line_color, 1),
                            ColorRGB_get_float(line_color, 2));

                glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);

                glVertexAttribPointer(
                    font_shader.attribs->location,
                    4, GL_FLOAT, GL_FALSE, 0, 0);

                size_t new_size = sizeof(vertex_t) * vec_vertex_buffer2.size; 
                if (flex_vbo.size < new_size) {
                    flex_vbo.size = new_size;
                    glBufferData(GL_ARRAY_BUFFER,
                                new_size,
                                vec_vertex_buffer2.buf,
                                GL_STREAM_DRAW);
                } else {
                    glBufferSubData(GL_ARRAY_BUFFER,
                                    0,
                                    new_size,
                                    vec_vertex_buffer2.buf);
                }

                glDrawArrays(GL_QUADS, 0, vec_vertex_buffer2.size /2);
            }

            #define SET_BOUNDS_BEGIN(what, index)                    \
                if (what) {                                          \
                    begin[index] = -1.0f + (float) column * scalex * \
                        (float) glyph_width_pixels;                  \
                }

            SET_BOUNDS_BEGIN(z->underlined,      0);
            SET_BOUNDS_BEGIN(z->doubleunderline, 1);
            SET_BOUNDS_BEGIN(z->strikethrough,   2);
            SET_BOUNDS_BEGIN(z->overline,        3);
            SET_BOUNDS_BEGIN(z->curlyunderline,  4);


            if (z != vt_line->data.buf + vt_line->data.size) {
                drawing[0] = z->underlined;
                drawing[1] = z->doubleunderline;
                drawing[2] = z->strikethrough;
                drawing[3] = z->overline;
                drawing[4] = z->curlyunderline;
            } else {
                drawing[0] = false;
                drawing[1] = false;
                drawing[2] = false;
                drawing[3] = false;
                drawing[4] = false;
            }

            line_color = nc;
        }
    }// END drawing lines

    gl_check_error();

    if (is_for_blinking) {
        vt_line->proxy.data[PROXY_INDEX_TEXTURE_BLINK] =
            Framebuffer_get_color_texture(&line_fb).id;
    } else {
        vt_line->proxy.data[PROXY_INDEX_TEXTURE] =
            Framebuffer_get_color_texture(&line_fb).id;
        vt_line->damaged = false;
    }

    Framebuffer_use(NULL);
    glViewport(0, 0, win_w, win_h);

    if (unlikely(has_blinking_chars && !is_for_blinking)) {
        // render version with blinking chars hidden
        gfx_rasterize_line(vt, vt_line, line, true);
    }
}


__attribute__((always_inline))
static inline void
gfx_draw_cursor(Vt* vt)
{
    if (!vt->cursor.hidden &&
        ((vt->cursor.blinking && in_focus) ? draw_blinking : true || recent_action))
    {
        size_t row = vt->active_line - Vt_visual_top_line(vt),
               col = vt->cursor_pos;
        bool filled_block = false;

        vec_vertex_buffer.size = 0;

        switch (vt->cursor.type) {
        case CURSOR_BEAM:
            Vector_pushv_vertex_t(&vec_vertex_buffer, (vertex_t[2]) {{
                            .x = -1.0f + (1 + col * glyph_width_pixels) * sx,
                            .y = 1.0f - row * line_height_pixels * sy
                        }, {
                            .x = -1.0f + (1 + col * glyph_width_pixels) * sx,
                            .y = 1.0f - (row +1)* line_height_pixels * sy
                        }
                    }, 2);
            break;

        case CURSOR_UNDERLINE:
            Vector_pushv_vertex_t(&vec_vertex_buffer, (vertex_t[2]) {{
                            .x = -1.0f + col * glyph_width_pixels * sx,
                            .y = 1.0f - ((row + 1) * line_height_pixels) * sy
                        }, {
                            .x = -1.0f + (col + 1) * glyph_width_pixels * sx,
                            .y = 1.0f - ((row + 1) * line_height_pixels) * sy
                        }
                    }, 2);
            break;

        case CURSOR_BLOCK:
            if (!in_focus)
                Vector_pushv_vertex_t(&vec_vertex_buffer, (vertex_t[4]) {{
                                .x = -1.0f + (col * glyph_width_pixels)       * sx +0.9f * sx,
                                .y =  1.0f - ((row + 1) * line_height_pixels) * sy +0.5f * sy
                            }, {
                                .x = -1.0f + ((col + 1) * glyph_width_pixels) * sx,
                                .y =  1.0f - ((row + 1) * line_height_pixels) * sy +0.5f * sy
                            }, {
                                .x = -1.0f + ((col + 1) * glyph_width_pixels) * sx,
                                .y =  1.0f - (row * line_height_pixels)       * sy -0.5f * sy
                            }, {
                                .x = -1.0f + (col * glyph_width_pixels)       * sx +0.9f * sx,
                                .y =  1.0f - (row * line_height_pixels)       * sy
                            }
                        }, 4);
            else
                filled_block = true;
            break;
        }

        ColorRGB *clr, *clr_bg;
        VtRune* cursor_char = NULL;
        if (vt->lines.size > vt->active_line &&
            vt->lines.buf[vt->active_line].data.size > col)
        {
            clr = &vt->lines.buf[vt->active_line].data.buf[col].fg;
            clr_bg = (ColorRGB*)&vt->lines.buf[vt->active_line].data.buf[col].bg;
            cursor_char = &vt->lines.buf[vt->active_line].data.buf[col];
        } else {
            clr = &settings.fg;
        }


        if (!filled_block) {
            Shader_use(&line_shader);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);
            glVertexAttribPointer(line_shader.attribs->location,
                                2, GL_FLOAT, GL_FALSE, 0, 0);

            glUniform3f(line_shader.uniforms[1].location,
                        ColorRGB_get_float(*clr, 0),
                        ColorRGB_get_float(*clr, 1),
                        ColorRGB_get_float(*clr, 2));

            size_t newsize = vec_vertex_buffer.size * sizeof(vertex_t);
            if (newsize > flex_vbo.size) {
                flex_vbo.size = newsize;
                glBufferData(GL_ARRAY_BUFFER,
                                newsize,
                                vec_vertex_buffer.buf,
                                GL_STREAM_DRAW);
            } else {
                glBufferSubData(GL_ARRAY_BUFFER,
                                0,
                                newsize,
                                vec_vertex_buffer.buf);
            }
            glDrawArrays(vec_vertex_buffer.size == 2 ? GL_LINES : GL_LINE_LOOP,
                         0,
                         vec_vertex_buffer.size);
        } else {
            glEnable(GL_SCISSOR_TEST);
            glScissor(col * glyph_width_pixels,
                      win_h - (row + 1) * line_height_pixels,
                      glyph_width_pixels,
                      line_height_pixels);
            glClearColor(ColorRGB_get_float(*clr, 0),
                         ColorRGB_get_float(*clr, 1),
                         ColorRGB_get_float(*clr, 2),
                         1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (cursor_char) {
                glUseProgram(font_shader.id);
                glUniform3f(font_shader.uniforms[1].location,
                            ColorRGB_get_float(*clr_bg, 0),
                            ColorRGB_get_float(*clr_bg, 1),
                            ColorRGB_get_float(*clr_bg, 2));

                glUniform3f(font_shader.uniforms[2].location,
                            ColorRGB_get_float(*clr, 0),
                            ColorRGB_get_float(*clr, 1),
                            ColorRGB_get_float(*clr, 2));

                glBindBuffer(GL_ARRAY_BUFFER, flex_vbo.vbo);
                glVertexAttribPointer(
                    font_shader.attribs->location,
                    4, GL_FLOAT, GL_FALSE, 0, 0);

                Atlas* source_atlas = atlas;
                Cache* source_cache = cache;
                FT_Face source_face = face;
                switch (cursor_char->state) {
                case VT_RUNE_ITALIC:
                    source_atlas = atlas_italic;
                    source_cache = cache_italic;
                    source_face = face_italic;
                    break;
                case VT_RUNE_BOLD:
                    source_atlas = atlas_bold;
                    source_cache = cache_bold;
                    source_face = face_bold;
                    break;
                default:
                    ;
                }

                float h, w, t, l;
                float tc[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

                int32_t atlas_offset = Atlas_select(source_atlas, cursor_char->code);
                if (atlas_offset >= 0) {
                    struct Atlas_char_info* g = &source_atlas->char_info[atlas_offset];
                    h = (float) g->rows * sy;
                    w = (float) g->width / (lcd_filter ? 3.0f : 1.0f) * sx;
                    t = (float) g->top * sy;
                    l = (float) g->left * sx;
                    memcpy(tc, g->tex_coords, sizeof tc);
                } else {
                    GlyphUnitCache* g = Cache_get_glyph(source_cache, source_face, cursor_char->code);
                    h = (float) g->tex.h * sy;
                    w = (float) g->tex.w * sx;
                    t = (float) g->top * sy;
                    l = (float) g->left * sx;
                    if (unlikely(h > line_height)) {
                        const float scale = h / line_height;
                        h /= scale;
                        w /= scale;
                        t /= scale;
                        l /= scale;
                    }
                }

                float x3 = -1.0f
                    + (float) col * glyph_width_pixels * sx
                    + l;
                float y3 = + 1.0f - pen_begin_pixels * sy - (float) row * line_height_pixels * sy +t;

                vec_glyph_buffer->size = 0;
                Vector_push_GlyphBufferData(
                    vec_glyph_buffer,
                    (GlyphBufferData) {{{
                            x3,
                            y3,
                            tc[0],
                            tc[1]
                        }, {
                            x3 + w,
                            y3,
                            tc[2],
                            tc[1]
                        }, {
                            x3 + w,
                            y3 - h,
                            tc[2],
                            tc[3]
                        }, {
                            x3,
                            y3 - h,
                            tc[0],
                            tc[3]
                        },
                    }});

                size_t newsize = vec_glyph_buffer->size * sizeof(GlyphBufferData);
                if (newsize > flex_vbo.size) {
                    flex_vbo.size = newsize;
                    glBufferData(GL_ARRAY_BUFFER,
                                 newsize,
                                 vec_glyph_buffer->buf,
                                 GL_STREAM_DRAW);
                } else {
                    glBufferSubData(GL_ARRAY_BUFFER,
                                    0,
                                    newsize,
                                    vec_glyph_buffer->buf);
                }

                glDrawArrays(GL_QUADS, 0, 4);
            }
            glDisable(GL_SCISSOR_TEST);
        }
    }
}


/**
 * Main repaint function
 */
void
gfx_draw_vt(Vt* vt)
{
    VtLine *begin, *end;
    Vt_get_visible_lines(vt, &begin, &end);

    glClearColor(ColorRGBA_get_float(settings.bg, 0),
                 ColorRGBA_get_float(settings.bg, 1),
                 ColorRGBA_get_float(settings.bg, 2),
                 ColorRGBA_get_float(settings.bg, 3));

    glClear(GL_COLOR_BUFFER_BIT);
    for (VtLine* i = begin; i < end; ++i)
        gfx_rasterize_line(vt, i, i - begin, false);
    glDisable(GL_BLEND);

    glEnable(GL_SCISSOR_TEST);
    glScissor(0,
              win_h - gl_get_char_size().second * line_height_pixels,
              gl_get_char_size().first * glyph_width_pixels,
              gl_get_char_size().second * line_height_pixels);
    
    quad_index = 0;
    glLoadIdentity();

    vec_glyph_buffer->size = 0;

    for (VtLine* i = begin; i < end; ++i)
        gfx_push_line_quads(i, i - begin);

    if (vec_glyph_buffer->size) {
        glBindBuffer(GL_ARRAY_BUFFER,flex_vbo.vbo);
        size_t newsize = vec_glyph_buffer->size *
            sizeof(GlyphBufferData);

        if (newsize > flex_vbo.size) {
            flex_vbo.size = newsize;
            glBufferData(GL_ARRAY_BUFFER,
                            newsize,
                            vec_glyph_buffer->buf,
                            GL_STREAM_DRAW);
        } else {
            glBufferSubData(GL_ARRAY_BUFFER,
                            0,
                            newsize,
                            vec_glyph_buffer->buf);
        }

        Shader_use(&image_shader);
        glVertexAttribPointer(image_shader.attribs->location,
                              4, GL_FLOAT, GL_FALSE, 0, 0);

        has_blinking_text = false;
        for (VtLine* i = begin; i < end; ++i)
            gfx_draw_line_quads(i);
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    gfx_draw_cursor(vt);

    // TODO: use vbo
    if (vt->scrollbar.visible ||
        scrollbar_fade != SCROLLBAR_FADE_MIN)
    {
        Shader_use(NULL);
        glBindTexture(GL_TEXTURE_2D, 0);

        float length = vt->scrollbar.length;
        float begin = vt->scrollbar.top;
        float width = sx * vt->scrollbar.width;
            
        glBegin(GL_QUADS);
        glColor4f(1,1,1, vt->scrollbar.dragging ? 0.8f :
                  ((float) scrollbar_fade / 100.0 * 0.5f));
        glVertex2f(1.0f - width, 1.0f -begin);
        glVertex2f(1.0f        , 1.0f -begin);
        glVertex2f(1.0f        , 1.0f -length -begin);
        glVertex2f(1.0f - width, 1.0f -length -begin);
        glEnd();
    }

    if (flash_fraction != 1.0) {
        Shader_use(NULL);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBegin(GL_QUADS);
        glColor4f(1,1,1,sinf((1.0 - flash_fraction) * M_1_PI ));
        glVertex2f(1, 1);
        glVertex2f(-1, 1);
        glVertex2f(-1, -1);
        glVertex2f(1, -1);
        glEnd();
    }
}


void
gfx_destroy_line_proxy(int32_t proxy[static 4])
{
    if (likely(proxy[0])) {
        glDeleteTextures(unlikely(proxy[PROXY_INDEX_TEXTURE_BLINK]) ? 2 : 1,
                         (GLuint*) &proxy[0]);
        proxy[PROXY_INDEX_TEXTURE] = 0;
        proxy[PROXY_INDEX_TEXTURE_BLINK] = 0;
    }
}


void
gfx_cleanup()
{
    Cache_destroy(cache);
    Atlas_destroy(atlas);

    if (settings.font_name_bold) {
        Cache_destroy(&_cache_bold);
        Atlas_destroy(&_atlas_bold);
        FT_Done_Face(face_bold);
    }

    if (settings.font_name_italic) {
        Cache_destroy(&_cache_italic);
        Atlas_destroy(&_atlas_italic);
        FT_Done_Face(face_italic);
    }

    VBO_destroy(&font_vao);
    VBO_destroy(&bg_vao);

    Shader_destroy(&font_shader);
    Shader_destroy(&bg_shader);

    FT_Done_Face(face);

    if (settings.font_name_fallback)
        FT_Done_Face(face_fallback);

    if (settings.font_name_fallback2)
        FT_Done_Face(face_fallback2);

    FT_Done_FreeType(ft);
}

