#include "freetype.h"
#include "freetype/ftbitmap.h"
#include "freetype/ftimage.h"
#include "util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline FT_Render_Mode render_mode_for_output(enum FreetypeOutputTextureType output_type)
{
    switch (output_type) {
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_RGB_H:
            return FT_RENDER_MODE_LCD;

        case FT_OUTPUT_BGR_V:
        case FT_OUTPUT_RGB_V:
            return FT_RENDER_MODE_LCD_V;

        case FT_OUTPUT_GRAYSCALE:
        case FT_OUTPUT_COLOR_BGRA:
            return FT_RENDER_MODE_NORMAL;

        default:
            ASSERT_UNREACHABLE
    }
}

static inline FT_Int32 load_flags_for_output(enum FreetypeOutputTextureType output_type)
{
    switch (output_type) {
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_RGB_H:
            return FT_LOAD_TARGET_LCD;
        case FT_OUTPUT_BGR_V:
        case FT_OUTPUT_RGB_V:
            return FT_LOAD_TARGET_LCD_V;
        case FT_OUTPUT_GRAYSCALE:
            return FT_LOAD_TARGET_NORMAL;
        case FT_OUTPUT_COLOR_BGRA:
            return FT_LOAD_COLOR;
        default:
            ASSERT_UNREACHABLE
    }
}

static inline bool flip_order_for_output(enum FreetypeOutputTextureType output_type)
{
    switch (output_type) {
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_BGR_V:
            return true;
        default:
            return false;
    }
}

static inline int32_t width_factor_for_output(enum FreetypeOutputTextureType output_type)
{
    switch (output_type) {
        case FT_OUTPUT_BGR_H:
        case FT_OUTPUT_RGB_H:
            return 3;
        default:
            return 1;
    }
}

static inline int32_t height_factor_for_output(enum FreetypeOutputTextureType output_type)
{
    switch (output_type) {
        case FT_OUTPUT_BGR_V:
        case FT_OUTPUT_RGB_V:
            return 3;
        default:
            return 1;
    }
}

static void Freetype_convert_mono_bitmap_to_grayscale(Freetype* self, const FT_Bitmap* source)
{
    FT_Error e;
    if (unlikely(!self->conversion_bitmap_initialized)) {
        FT_Bitmap_Init(&self->converted_output_bitmap);
        self->conversion_bitmap_initialized = true;
    }
    if ((e = FT_Bitmap_Convert(self->ft, source, &self->converted_output_bitmap, 1))) {
        ERR("Bitmap conversion failed %s", ft_error_to_string(e));
    }
    uint_fast16_t pixel_count =
      self->converted_output_bitmap.width * self->converted_output_bitmap.rows;
    for (uint_fast16_t i = 0; i < pixel_count; ++i) {
        self->converted_output_bitmap.buffer[i] *= UINT8_MAX;
    }
}

static void Freetype_convert_vertical_pixel_data_layout(Freetype* self, FT_Bitmap* source)
{
    ASSERT(source->pixel_mode == FT_PIXEL_MODE_LCD_V, "is vertical layout");

    const uint_fast16_t TARGET_ROW_ALIGNMENT = 4;
    uint_fast16_t       pixel_width          = source->width;
    uint_fast16_t       pixel_height         = source->rows / 3;
    uint_fast16_t       target_width         = pixel_width * 3;
    uint_fast16_t       target_height        = pixel_height;
    uint_fast16_t       source_row_length    = source->pitch;
    uint_fast16_t       target_row_length    = target_width;
    uint_fast16_t       mod                  = target_row_length % TARGET_ROW_ALIGNMENT;
    uint_fast16_t       add                  = mod ? (TARGET_ROW_ALIGNMENT - mod) : 0;
    target_row_length += add;
    uint8_t* target = calloc(1, target_row_length * target_height);

    for (uint_fast16_t x = 0; x < pixel_width; ++x) {
        for (uint_fast16_t y = 0; y < pixel_height; ++y) {
            for (uint_fast8_t s = 0; s < 3; ++s) {
                uint_fast16_t src_off = source_row_length * (y * 3 + s) + x;
                uint_fast16_t tgt_off = target_row_length * y + 3 * x + s;
                target[tgt_off]       = source->buffer[src_off];
            }
        }
    }

    free(self->converted_output_pixels);
    self->converted_output_pixels = target;
}

void FreetypeFace_load(Freetype*                      freetype,
                       FreetypeFace*                  self,
                       int32_t                        size,
                       uint32_t                       dpi,
                       enum FreetypeOutputTextureType output_type,
                       bool                           warn_not_fixed)
{
    FT_F26Dot6 siz = MAX(size, 1) * 64;
    FT_UInt    res = dpi;
    FT_Error   e;
    self->output_type = output_type;

    if ((e = FT_New_Face(freetype->ft, self->file_name, 0, &self->face))) {
        ERR("failed to load font file %s %s", self->file_name, ft_error_to_string(e));
    }

    if ((e = FT_Set_Char_Size(self->face, siz, siz, res, res))) {
        if (!self->face->size->metrics.height) {
            if ((e = FT_Select_Size(self->face, self->face->num_fixed_sizes - 1))) {
                ERR("failed to select bitmap font size for %s %s",
                    self->file_name,
                    ft_error_to_string(e));
            }
        } else {
            ERR("failed to set font size for %s %s", self->file_name, ft_error_to_string(e));
        }
    }

    if (warn_not_fixed && !FT_IS_FIXED_WIDTH(self->face)) {
        WRN("face %s is not fixed-width\n", self->file_name);
    }

    if ((e = FT_Load_Char(self->face, '(', FT_LOAD_TARGET_NORMAL))) {
        ERR("failed to load font %s", self->file_name);
    }

    bool is_packed = self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO;

    if (is_packed && self->output_type != FT_OUTPUT_COLOR_BGRA) {
        self->output_type = FT_OUTPUT_GRAYSCALE;
    }

    self->glyph_width_pixels = self->face->glyph->advance.x / 64;
    self->line_height_pixels = self->face->size->metrics.height / 64;
    self->loaded             = true;
    self->load_flags         = load_flags_for_output(self->output_type);
    self->render_mode        = render_mode_for_output(self->output_type);
    self->rgb_flip           = flip_order_for_output(self->output_type);
}

void FreetypeFace_unload(FreetypeFace* self)
{
    if (self->loaded) {
        FT_Done_Face(self->face);
    }
    self->loaded             = false;
    self->glyph_width_pixels = 0;
    self->line_height_pixels = 0;
}

FreetypeOutput* FreetypeFace_load_glyph(Freetype* freetype, FreetypeFace* self, char32_t codepoint)
{
    ASSERT(freetype->initialized, "freetype is initialized before rendering")
    ASSERT(self->loaded, "face loaded before rendering")

    FT_Error e;
    if ((e = FT_Load_Char(self->face, codepoint, self->load_flags))) {
        WRN("glyph load error %c(%d) %s\n", codepoint, codepoint, ft_error_to_string(e));
    }
    if (self->face->glyph->glyph_index == 0) {
        return NULL;
    }

    freetype->output.ft_slot = self->face->glyph;
    freetype->output.width =
      self->face->glyph->bitmap.width / width_factor_for_output(self->output_type);
    freetype->output.height =
      self->face->glyph->bitmap.rows / height_factor_for_output(self->output_type);
    freetype->output.left   = self->face->glyph->bitmap_left;
    freetype->output.top    = self->face->glyph->bitmap_top;
    freetype->output.type   = FT_OUTPUT_GEOMETRY_ONLY;
    freetype->output.pixels = NULL;
    return &freetype->output;
}

FreetypeOutput* FreetypeFace_load_and_render_glyph(Freetype*     freetype,
                                                   FreetypeFace* self,
                                                   char32_t      codepoint)
{
    ASSERT(freetype->initialized, "freetype is initialized before rendering")
    ASSERT(self->loaded, "face loaded before rendering")

    FT_Error e;

    if ((e = FT_Load_Char(self->face, codepoint, self->load_flags))) {
        WRN("glyph load error %c(%d) %s\n", codepoint, codepoint, ft_error_to_string(e));
    }

    if ((e = FT_Render_Glyph(self->face->glyph, self->render_mode))) {
        WRN("glyph render error %c(%d) %s\n", codepoint, codepoint, ft_error_to_string(e));
    }

    if (self->face->glyph->glyph_index == 0) {
        return NULL;
    }

    bool is_packed = self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO;
    if (is_packed) {
        Freetype_convert_mono_bitmap_to_grayscale(freetype, &self->face->glyph->bitmap);
        freetype->output.width =
          freetype->converted_output_bitmap.width / width_factor_for_output(self->output_type);
        freetype->output.height =
          freetype->converted_output_bitmap.rows / height_factor_for_output(self->output_type);
        freetype->output.pixels = freetype->converted_output_bitmap.buffer;
    } else {
        freetype->output.ft_slot = self->face->glyph;
        freetype->output.width =
          self->face->glyph->bitmap.width / width_factor_for_output(self->output_type);
        freetype->output.height =
          self->face->glyph->bitmap.rows / height_factor_for_output(self->output_type);
        freetype->output.pixels = self->face->glyph->bitmap.buffer;
    }

    freetype->output.left = self->face->glyph->bitmap_left;
    freetype->output.top  = self->face->glyph->bitmap_top;

    if (is_packed || self->output_type == FT_OUTPUT_GRAYSCALE) {
        freetype->output.type      = FT_OUTPUT_GRAYSCALE;
        freetype->output.alignment = 1;
    } else if (self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        freetype->output.type      = FT_OUTPUT_COLOR_BGRA;
        freetype->output.alignment = 4;
    } else if (self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_LCD_V) {
        Freetype_convert_vertical_pixel_data_layout(freetype, &self->face->glyph->bitmap);
        freetype->output.pixels    = freetype->converted_output_pixels;
        freetype->output.type      = self->output_type;
        freetype->output.alignment = 4;
    } else {
        freetype->output.type      = self->output_type;
        freetype->output.alignment = 4;
    }
    return &freetype->output;
}

/**
 * Create a new styled font family from file names */
FreetypeStyledFamily FreetypeStyledFamily_new(const char*                    regular_file,
                                              const char*                    opt_bold_file,
                                              const char*                    opt_italic_file,
                                              const char*                    opt_bold_italic_file,
                                              Vector_Pair_char32_t*          opt_codepoint_ranges,
                                              int16_t                        size_offset,
                                              enum FreetypeOutputTextureType output_type)
{
    ASSERT(regular_file, "regular file not null");

    LOG("ft::StyledFamily_new{ r: %s, b: %s, i: %s, bi: %s, so: %d, ot: %d }\n",
        regular_file,
        opt_bold_file,
        opt_italic_file,
        opt_bold_italic_file,
        size_offset,
        output_type);

    FreetypeStyledFamily self = { .faces            = Vector_new_FreetypeFace(),
                                  .regular          = NULL,
                                  .bold             = NULL,
                                  .italic           = NULL,
                                  .bold_italic      = NULL,
                                  .codepoint_ranges = opt_codepoint_ranges,
                                  .output_type      = output_type };

    Vector_push_FreetypeFace(&self.faces,
                             (FreetypeFace){ .loaded           = false,
                                             .file_name        = regular_file,
                                             .size_offset      = size_offset,
                                             .codepoint_ranges = opt_codepoint_ranges });
    self.regular = Vector_last_FreetypeFace(&self.faces);

    if (opt_bold_file) {
        Vector_push_FreetypeFace(&self.faces,
                                 (FreetypeFace){ .loaded           = false,
                                                 .file_name        = opt_bold_file,
                                                 .size_offset      = size_offset,
                                                 .codepoint_ranges = opt_codepoint_ranges });
        self.bold = Vector_last_FreetypeFace(&self.faces);
    } else {
        self.bold = NULL;
    }

    if (opt_italic_file) {
        Vector_push_FreetypeFace(&self.faces,
                                 (FreetypeFace){ .loaded           = false,
                                                 .file_name        = opt_italic_file,
                                                 .size_offset      = size_offset,
                                                 .codepoint_ranges = opt_codepoint_ranges });
        self.italic = Vector_last_FreetypeFace(&self.faces);
    } else {
        self.italic = NULL;
    }

    if (opt_bold_italic_file) {
        Vector_push_FreetypeFace(&self.faces,
                                 (FreetypeFace){ .loaded           = false,
                                                 .file_name        = opt_bold_italic_file,
                                                 .size_offset      = size_offset,
                                                 .codepoint_ranges = opt_codepoint_ranges });
        self.bold_italic = Vector_last_FreetypeFace(&self.faces);
    } else {
        self.bold_italic = NULL;
    }

    return self;
}

void FreetypeStyledFamily_load(Freetype*             freetype,
                               FreetypeStyledFamily* self,
                               uint32_t              size,
                               uint32_t              dpi)
{
    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->faces, i));) {
        FreetypeFace_load(freetype, i, size + i->size_offset, dpi, self->output_type, true);
    }

    self->output_type = Vector_first_FreetypeFace(&self->faces)->output_type;
}

void FreetypeStyledFamily_unload(FreetypeStyledFamily* self)
{
    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->faces, i));) {
        FreetypeFace_unload(i);
    }
}

FreetypeFace* FreetypeStyledFamily_select_face(FreetypeStyledFamily*   self,
                                               enum FreetypeFontStyle  style,
                                               enum FreetypeFontStyle* opt_out_style)
{
    if (opt_out_style) {
        *opt_out_style = FT_STYLE_REGULAR;
    }
    switch (style) {
        case FT_STYLE_REGULAR:
            return self->regular;

        case FT_STYLE_BOLD:
            if (self->bold) {
                if (opt_out_style) {
                    *opt_out_style = FT_STYLE_BOLD;
                }
                return self->bold;
            } else {
                return self->regular;
            }

        case FT_STYLE_ITALIC:
            if (self->italic) {
                if (opt_out_style) {
                    *opt_out_style = FT_STYLE_ITALIC;
                }
                return self->italic;
            } else {
                return self->regular;
            }

        case FT_STYLE_BOLD_ITALIC:
            if (self->bold_italic) {
                if (opt_out_style) {
                    *opt_out_style = FT_STYLE_BOLD_ITALIC;
                }
                return self->bold_italic;
            } else if (self->italic) {
                if (opt_out_style) {
                    *opt_out_style = FT_STYLE_ITALIC;
                }
                return self->italic;
            } else if (self->bold) {
                if (opt_out_style) {
                    *opt_out_style = FT_STYLE_BOLD;
                }
                return self->bold;
            } else {
                return self->regular;
            }

        default:
            ASSERT_UNREACHABLE
    }
}

FreetypeOutput* FreetypeStyledFamily_load_glyph(Freetype*              freetype,
                                                FreetypeStyledFamily*  self,
                                                char32_t               codepoint,
                                                enum FreetypeFontStyle style)
{
    enum FreetypeFontStyle final_style;
    FreetypeFace*   source_face = FreetypeStyledFamily_select_face(self, style, &final_style);
    FreetypeOutput* output      = FreetypeFace_load_glyph(freetype, source_face, codepoint);
    if (output) {
        output->style = final_style;
    }
    return output;
}

FreetypeOutput* FreetypeStyledFamily_load_and_render_glyph(Freetype*              freetype,
                                                           FreetypeStyledFamily*  self,
                                                           char32_t               codepoint,
                                                           enum FreetypeFontStyle style)
{
    enum FreetypeFontStyle final_style;
    FreetypeFace*   source_face = FreetypeStyledFamily_select_face(self, style, &final_style);
    FreetypeOutput* output = FreetypeFace_load_and_render_glyph(freetype, source_face, codepoint);

    if (output) {
        output->style = final_style;
    }
    return output;
}

static bool FreetypeFace_applies_to(FreetypeFace* self, char32_t codepoint)
{
    if (!self->codepoint_ranges) {
        return true;
    }
    for (Pair_char32_t* i = NULL; (i = Vector_iter_Pair_char32_t(self->codepoint_ranges, i));) {
        if (i->first <= codepoint && i->second >= codepoint) {
            return true;
        }
    }
    return false;
}

static bool FreetypeStyledFamily_applies_to(FreetypeStyledFamily* self, char32_t codepoint)
{
    if (!self->codepoint_ranges) {
        return true;
    }
    for (Pair_char32_t* i = NULL; (i = Vector_iter_Pair_char32_t(self->codepoint_ranges, i));) {
        if (i->first <= codepoint && i->second >= codepoint) {
            return true;
        }
    }
    return false;
}

Freetype Freetype_new()
{
    Freetype self;
    memset(&self, 0, sizeof(self));
    self.primaries    = Vector_new_FreetypeStyledFamily();
    self.symbol_faces = Vector_new_FreetypeFace();
    self.color_faces  = Vector_new_FreetypeFace();
    FT_Error e        = 0;

    if (!self.initialized) {
        if ((e = FT_Init_FreeType(&self.ft))) {
            ERR("Failed to initialize freetype %s", ft_error_to_string(e));
        }
        if ((e = FT_Library_SetLcdFilter(self.ft, FT_LCD_FILTER_DEFAULT))) {
            WRN("Freetype has no clear type support %s\n", ft_error_to_string(e));
        }
        self.initialized = true;
    }

    enum FreetypeOutputTextureType output_type;
    switch (settings.lcd_filter) {
        case LCD_FILTER_V_BGR:
            output_type = FT_OUTPUT_BGR_V;
            break;
        case LCD_FILTER_V_RGB:
            output_type = FT_OUTPUT_RGB_V;
            break;
        case LCD_FILTER_H_BGR:
            output_type = FT_OUTPUT_BGR_H;
            break;
        case LCD_FILTER_H_RGB:
            output_type = FT_OUTPUT_RGB_H;
            break;
        case LCD_FILTER_NONE:
            output_type = FT_OUTPUT_GRAYSCALE;
            break;
        default:
            ASSERT_UNREACHABLE
    }
    self.target_output_type = output_type;

    for (StyledFontInfo* i = NULL; (i = Vector_iter_StyledFontInfo(&settings.styled_fonts, i));) {
        if (i->regular_file_name) {
            Vector_push_FreetypeStyledFamily(
              &self.primaries,
              FreetypeStyledFamily_new(i->regular_file_name,
                                       i->bold_file_name,
                                       i->italic_file_name,
                                       i->bold_italic_file_name,
                                       i->codepoint_ranges.size ? &i->codepoint_ranges : NULL,
                                       i->size_offset,
                                       self.target_output_type));
        }
    }

    for (UnstyledFontInfo* i = NULL;
         (i = Vector_iter_UnstyledFontInfo(&settings.symbol_fonts, i));) {
        if (i->file_name) {
            Vector_push_FreetypeFace(&self.symbol_faces,
                                     (FreetypeFace){ .loaded           = false,
                                                     .file_name        = i->file_name,
                                                     .size_offset      = i->size_offset,
                                                     .codepoint_ranges = i->codepoint_ranges.size
                                                                           ? &i->codepoint_ranges
                                                                           : NULL });
        }
    }

    for (UnstyledFontInfo* i = NULL;
         (i = Vector_iter_UnstyledFontInfo(&settings.color_fonts, i));) {
        if (i->file_name) {
            Vector_push_FreetypeFace(&self.color_faces,
                                     (FreetypeFace){ .loaded           = false,
                                                     .file_name        = i->file_name,
                                                     .size_offset      = i->size_offset,
                                                     .codepoint_ranges = i->codepoint_ranges.size
                                                                           ? &i->codepoint_ranges
                                                                           : NULL });
        }
    }

    Freetype_load_fonts(&self);
    return self;
}

void Freetype_load_fonts(Freetype* self)
{

    if (settings.defer_font_loading) {
        FreetypeStyledFamily_load(self,
                                  Vector_first_FreetypeStyledFamily(&self->primaries),
                                  settings.font_size,
                                  settings.font_dpi);

        self->primary_output_type =
          Vector_first_FreetypeStyledFamily(&self->primaries)->output_type;
    } else {
        for (FreetypeStyledFamily* i = NULL;
             (i = Vector_iter_FreetypeStyledFamily(&self->primaries, i));) {
            FreetypeStyledFamily_load(self, i, settings.font_size, settings.font_dpi);
        }

        self->primary_output_type =
          Vector_first_FreetypeStyledFamily(&self->primaries)->output_type;

        for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->symbol_faces, i));) {
            FreetypeFace_load(self,
                              i,
                              settings.font_size + i->size_offset,
                              settings.font_dpi,
                              self->target_output_type,
                              false);
        }

        for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->color_faces, i));) {
            FreetypeFace_load(self,
                              i,
                              settings.font_size + i->size_offset,
                              settings.font_dpi,
                              FT_OUTPUT_COLOR_BGRA,
                              false);
        }
    }

    self->glyph_width_pixels =
      Vector_first_FreetypeStyledFamily(&self->primaries)->regular->glyph_width_pixels;

    self->line_height_pixels =
      Vector_first_FreetypeStyledFamily(&self->primaries)->regular->line_height_pixels;
}

void Freetype_unload_fonts(Freetype* self)
{
    for (FreetypeStyledFamily* i = NULL;
         (i = Vector_iter_FreetypeStyledFamily(&self->primaries, i));) {
        FreetypeStyledFamily_unload(i);
    }

    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->symbol_faces, i));) {
        FreetypeFace_unload(i);
    }

    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->color_faces, i));) {
        FreetypeFace_unload(i);
    }
}

void Freetype_reload_fonts(Freetype* self)
{
    Freetype_unload_fonts(self);
    Freetype_load_fonts(self);
}

FreetypeOutput* Freetype_load_ascii_glyph(Freetype* self, char code, enum FreetypeFontStyle style)
{
    return FreetypeStyledFamily_load_glyph(self,
                                           Vector_first_FreetypeStyledFamily(&self->primaries),
                                           code,
                                           style);
}

FreetypeOutput* Freetype_load_and_render_ascii_glyph(Freetype*              self,
                                                     char                   code,
                                                     enum FreetypeFontStyle style)
{
    return FreetypeStyledFamily_load_and_render_glyph(
      self,
      Vector_first_FreetypeStyledFamily(&self->primaries),
      code,
      style);
}

FreetypeOutput* Freetype_load_and_render_glyph(Freetype*              self,
                                               char32_t               codepoint,
                                               enum FreetypeFontStyle style)
{
    FreetypeOutput* output;
    for (FreetypeStyledFamily* i = NULL;
         (i = Vector_iter_FreetypeStyledFamily(&self->primaries, i));) {
        if (FreetypeStyledFamily_applies_to(i, codepoint)) {
            if (unlikely(!i->regular->loaded)) {
                FreetypeStyledFamily_load(self, i, settings.font_size, settings.font_dpi);
            }

            output = FreetypeStyledFamily_load_and_render_glyph(self, i, codepoint, style);
            if (output) {
                return output;
            }
        }
    }
    if (self->symbol_faces.size) {
        for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->symbol_faces, i));) {
            if (FreetypeFace_applies_to(i, codepoint)) {
                if (unlikely(!i->loaded)) {
                    FreetypeFace_load(self,
                                      i,
                                      settings.font_size + i->size_offset,
                                      settings.font_dpi,
                                      self->target_output_type,
                                      false);
                }

                output = FreetypeFace_load_and_render_glyph(self, i, codepoint);
                if (output) {
                    output->style = FT_STYLE_NONE;
                    return output;
                }
            }
        }
    }
    if (self->color_faces.size) {
        for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->color_faces, i));) {
            if (FreetypeFace_applies_to(i, codepoint)) {
                if (unlikely(!i->loaded)) {
                    FreetypeFace_load(self,
                                      i,
                                      settings.font_size + i->size_offset,
                                      settings.font_dpi,
                                      FT_OUTPUT_COLOR_BGRA,
                                      false);
                }

                output = FreetypeFace_load_and_render_glyph(self, i, codepoint);
                if (output) {
                    output->style = FT_STYLE_NONE;
                    return output;
                }
            }
        }
    }
    return NULL;
}

void Freetype_destroy(Freetype* self)
{
    if (!self->initialized) {
        return;
    }
    Freetype_unload_fonts(self);
    Vector_destroy_FreetypeStyledFamily(&self->primaries);
    Vector_destroy_FreetypeFace(&self->symbol_faces);
    Vector_destroy_FreetypeFace(&self->color_faces);
    free(self->converted_output_pixels);
    self->converted_output_pixels = NULL;
    if (self->conversion_bitmap_initialized) {
        FT_Bitmap_Done(self->ft, &self->converted_output_bitmap);
        self->conversion_bitmap_initialized = false;
    }
    FT_Done_FreeType(self->ft);
    self->initialized = false;
}
