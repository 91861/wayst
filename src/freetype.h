#pragma once

#define _GNU_SOURCE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <uchar.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_TRUETYPE_TABLES_H
#include FT_BITMAP_H
#include FT_FONT_FORMATS_H

#include "fterrors.h"
#include "settings.h"
#include "vector.h"

enum FreetypeFontStyle
{
    FT_STYLE_NONE,
    FT_STYLE_REGULAR,
    FT_STYLE_BOLD,
    FT_STYLE_ITALIC,
    FT_STYLE_BOLD_ITALIC,
};

enum FreetypeOutputTextureType
{
    FT_OUTPUT_GEOMETRY_ONLY = 0,
    FT_OUTPUT_GRAYSCALE,
    FT_OUTPUT_RGB_H,
    FT_OUTPUT_BGR_H,
    FT_OUTPUT_RGB_V,
    FT_OUTPUT_BGR_V,
    FT_OUTPUT_COLOR_BGRA,
};

typedef struct
{
    FT_GlyphSlotRec*               ft_slot;
    int16_t                        width, height, left, top;
    int8_t                         alignment;
    void*                          pixels;
    enum FreetypeOutputTextureType type;
    bool                           rgb_flip;
    enum FreetypeFontStyle         style;
} FreetypeOutput;

static void FreetypeOutput_print(FreetypeOutput* self)
{
    printf("FreetypeOutput {\n"
           "    width:  %d\n"
           "    height: %d\n"
           "    type:   %d\n"
           "}\n",
           self->width,
           self->height,
           self->type);
}

static enum FreetypeOutputTextureType output_texture_type_from_lcd_filter(lcd_filter_e lcd_filter)
{
    enum FreetypeOutputTextureType ltbl[] = { [LCD_FILTER_V_BGR] = FT_OUTPUT_BGR_V,
                                              [LCD_FILTER_V_RGB] = FT_OUTPUT_RGB_V,
                                              [LCD_FILTER_H_BGR] = FT_OUTPUT_BGR_H,
                                              [LCD_FILTER_H_RGB] = FT_OUTPUT_RGB_H,
                                              [LCD_FILTER_NONE]  = FT_OUTPUT_GRAYSCALE };

    return ltbl[lcd_filter];
}

typedef struct
{
    FT_Face                        face;
    bool                           loaded;
    const char*                    file_name;
    uint16_t                       line_height_pixels, glyph_width_pixels;
    enum FreetypeOutputTextureType output_type;
    bool                           rgb_flip;
    FT_Render_Mode                 render_mode;
    FT_Int32                       load_flags;
    Vector_Pair_char32_t*          codepoint_ranges;
    int16_t                        size_offset;
} FreetypeFace;

static void FreetypeFace_destroy(FreetypeFace* self)
{
    if (self->loaded) {
        FT_Done_Face(self->face);
    }
}

DEF_VECTOR(FreetypeFace, FreetypeFace_destroy);

typedef struct
{
    Vector_FreetypeFace            faces;
    FreetypeFace *                 regular, *bold, *italic, *bold_italic;
    Vector_Pair_char32_t*          codepoint_ranges;
    enum FreetypeOutputTextureType output_type;
} FreetypeStyledFamily;

static void FreetypeStyledFamily_destroy(FreetypeStyledFamily* self)
{
    Vector_destroy_FreetypeFace(&self->faces);
}

DEF_VECTOR(FreetypeStyledFamily, FreetypeStyledFamily_destroy);

typedef struct
{
    bool                           initialized;
    FT_Library                     ft;
    Vector_FreetypeStyledFamily    primaries;
    Vector_FreetypeFace            symbol_faces;
    Vector_FreetypeFace            color_faces;
    enum FreetypeOutputTextureType primary_output_type;
    enum FreetypeOutputTextureType target_output_type;
    int32_t                        gw;
    int16_t                        line_height_pixels, glyph_width_pixels;
    bool                           blend_bitmap_initialized;
    FT_Bitmap                      blend_output_bitmap;
    bool                           conversion_bitmap_initialized;
    FT_Bitmap                      converted_output_bitmap;
    void*                          converted_output_pixels;
    FreetypeOutput                 output;
} Freetype;

void FreetypeFace_load(Freetype*                      freetype,
                       FreetypeFace*                  self,
                       int32_t                        size,
                       uint32_t                       dpi,
                       enum FreetypeOutputTextureType output_type,
                       bool                           warn_not_fixed);

void FreetypeFace_unload(FreetypeFace* self);

FreetypeStyledFamily FreetypeStyledFamily_new(const char*           regular_file,
                                              const char*           opt_bold_file,
                                              const char*           opt_italic_file,
                                              const char*           opt_bold_italic_file,
                                              Vector_Pair_char32_t* opt_codepoint_range_begin,
                                              int16_t               size_offset,
                                              enum FreetypeOutputTextureType output_type);

void FreetypeStyledFamily_load(Freetype*             freetype,
                               FreetypeStyledFamily* self,
                               uint32_t              size,
                               uint32_t              dpi);

void FreetypeStyledFamily_unload(FreetypeStyledFamily* self);

Freetype Freetype_new();

FreetypeOutput* Freetype_load_ascii_glyph(Freetype* self, char code, enum FreetypeFontStyle style);

FreetypeOutput* Freetype_load_and_render_ascii_glyph(Freetype*              self,
                                                     char                   code,
                                                     enum FreetypeFontStyle style);

FreetypeOutput* Freetype_load_and_render_glyph(Freetype*              self,
                                               char32_t               codepoint,
                                               enum FreetypeFontStyle style);

void Freetype_unload_fonts(Freetype* self);
void Freetype_load_fonts(Freetype* self);

void Freetype_reload_fonts(Freetype* self);

static void Freetype_reload_fonts_with_output_type(Freetype*                      self,
                                                   enum FreetypeOutputTextureType output_type)
{

    Freetype_unload_fonts(self);

    self->target_output_type = output_type;

    for (FreetypeStyledFamily* i = NULL;
         (i = Vector_iter_FreetypeStyledFamily(&self->primaries, i));) {
        i->output_type = output_type;

        for (FreetypeFace* j = NULL; (j = Vector_iter_FreetypeFace(&i->faces, j));) {
            j->output_type = output_type;
        }
    }

    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->symbol_faces, i));) {
        i->output_type = output_type;
    }

    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->color_faces, i));) {
        i->output_type = output_type;
    }

    Freetype_load_fonts(self);
}

void Freetype_destroy(Freetype* self);
