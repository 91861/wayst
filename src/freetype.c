#include "freetype.h"

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

static void Freetype_convert_bitmap(Freetype* self, const FT_Bitmap* source)
{
    FT_Error e;
    if (unlikely(!self->conversion_bitmap_initialized)) {
        FT_Bitmap_Init(&self->converted_output_bitmap);
        self->conversion_bitmap_initialized = true;
    }
    if ((e = FT_Bitmap_Convert(self->ft, source, &self->converted_output_bitmap, 1))) {
        ERR("Bitmap conversion failed %s", ft_error_to_string(e));
    }
    uint32_t pixel_count = self->converted_output_bitmap.width * self->converted_output_bitmap.rows;
    for (uint_fast32_t i = 0; i < pixel_count; ++i) {
        self->converted_output_bitmap.buffer[i] *= UINT8_MAX;
    }
}

void FreetypeFace_load(Freetype*                      freetype,
                       FreetypeFace*                  self,
                       uint32_t                       size,
                       uint32_t                       dpi,
                       enum FreetypeOutputTextureType output_type,
                       bool                           warn_not_fixed)
{
    FT_F26Dot6 siz = size * 64;
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
        WRN("face %s is not fixed width\n", self->file_name);
    }
    if ((e = FT_Load_Char(self->face, '(', FT_LOAD_TARGET_NORMAL))) {
        ERR("failed to load font %s", self->file_name);
    }
    bool is_packed = self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO;
    if (is_packed) {
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
    FT_Done_Face(self->face);
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
        Freetype_convert_bitmap(freetype, &self->face->glyph->bitmap);
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
    if (is_packed) {
        freetype->output.type      = FT_OUTPUT_GRAYSCALE;
        freetype->output.alignment = 1;
    } else if (self->face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        freetype->output.type      = FT_OUTPUT_COLOR_BGRA;
        freetype->output.alignment = 4;
    } else {
        freetype->output.type      = self->output_type;
        freetype->output.alignment = 4;
    }

    return &freetype->output;
}

/**
 * Create a new styled font family from file names */
FreetypeStyledFamily FreetypeStyledFamily_new(const char* regular_file,
                                              const char* opt_bold_file,
                                              const char* opt_italic_file,
                                              const char* opt_bold_italic_file,
                                              char32_t    opt_codepoint_range_begin,
                                              char32_t    opt_codepoint_range_end,
                                              enum FreetypeOutputTextureType output_type)
{
    ASSERT(regular_file, "regular file not null");

    FreetypeStyledFamily self = { .faces                 = Vector_new_FreetypeFace(),
                                  .regular               = NULL,
                                  .bold                  = NULL,
                                  .italic                = NULL,
                                  .bold_italic           = NULL,
                                  .codepoint_range_begin = opt_codepoint_range_begin,
                                  .codepoint_range_end   = opt_codepoint_range_end,
                                  .output_type           = output_type };

    Vector_push_FreetypeFace(&self.faces,
                             (FreetypeFace){ .loaded = false, .file_name = regular_file });
    self.regular = Vector_last_FreetypeFace(&self.faces);

    if (opt_bold_file) {
        Vector_push_FreetypeFace(&self.faces,
                                 (FreetypeFace){ .loaded = false, .file_name = opt_bold_file });
        self.bold = Vector_last_FreetypeFace(&self.faces);
    } else {
        self.bold = self.regular;
    }

    if (opt_italic_file) {
        Vector_push_FreetypeFace(&self.faces,
                                 (FreetypeFace){ .loaded = false, .file_name = opt_italic_file });
        self.italic = Vector_last_FreetypeFace(&self.faces);
    } else {
        self.italic = self.regular;
    }

    if (opt_bold_italic_file) {
        Vector_push_FreetypeFace(
          &self.faces,
          (FreetypeFace){ .loaded = false, .file_name = opt_bold_italic_file });
        self.bold_italic = Vector_last_FreetypeFace(&self.faces);
    } else {
        self.bold_italic = OR(self.italic, OR(self.bold, self.regular));
    }

    return self;
}

void FreetypeStyledFamily_load(Freetype*             freetype,
                               FreetypeStyledFamily* self,
                               uint32_t              size,
                               uint32_t              dpi)
{
    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->faces, i));) {
        FreetypeFace_load(freetype, i, size, dpi, self->output_type, true);
    }
    self->output_type = Vector_first_FreetypeFace(&self->faces)->output_type;
}

void FreetypeStyledFamily_unload(FreetypeStyledFamily* self)
{
    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->faces, i));) {
        FreetypeFace_unload(i);
    }
}

FreetypeFace* FreetypeStyledFamily_select_face(FreetypeStyledFamily*  self,
                                               enum FreetypeFontStyle style)
{
    switch (style) {
        case FT_STYLE_REGULAR:
            return self->regular;
        case FT_STYLE_BOLD:
            return self->bold;
        case FT_STYLE_ITALIC:
            return self->italic;
        case FT_STYLE_BOLD_ITALIC:
            return self->bold_italic;
        default:
            ASSERT_UNREACHABLE
    }
}

FreetypeOutput* FreetypeStyledFamily_load_glyph(Freetype*              freetype,
                                                FreetypeStyledFamily*  self,
                                                char32_t               codepoint,
                                                enum FreetypeFontStyle style)
{
    FreetypeFace* source_face = FreetypeStyledFamily_select_face(self, style);
    return FreetypeFace_load_glyph(freetype, source_face, codepoint);
}

FreetypeOutput* FreetypeStyledFamily_load_and_render_glyph(Freetype*              freetype,
                                                           FreetypeStyledFamily*  self,
                                                           char32_t               codepoint,
                                                           enum FreetypeFontStyle style)
{
    FreetypeFace* source_face = FreetypeStyledFamily_select_face(self, style);
    return FreetypeFace_load_and_render_glyph(freetype, source_face, codepoint);
}

bool FreetypeStyledFamily_applies_to(FreetypeStyledFamily* self, char32_t codepoint)
{
    if (self->codepoint_range_begin && self->codepoint_range_begin > codepoint) {
        return false;
    }
    if (self->codepoint_range_end && self->codepoint_range_end < codepoint) {
        return false;
    }
    return true;
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
        FT_Library_SetLcdFilter(self.ft, FT_LCD_FILTER_DEFAULT);
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
    /* for (...) */ {
        Vector_push_FreetypeStyledFamily(
          &self.primaries,
          FreetypeStyledFamily_new(settings.font_file_name_regular.str,
                                   settings.font_file_name_bold.str,
                                   settings.font_file_name_italic.str,
                                   settings.font_file_name_bold_italic.str,
                                   0,
                                   0,
                                   output_type));
    }
    self.primary_output_type = Vector_first_FreetypeStyledFamily(&self.primaries)->output_type;
    /* for (...) */ {
        if (settings.font_file_name_fallback.str) {
            Vector_push_FreetypeFace(
              &self.symbol_faces,
              (FreetypeFace){ .loaded = false, .file_name = settings.font_file_name_fallback.str });
        }
    }
    /* for (...) */ {
        if (settings.font_file_name_fallback2.str) {
            Vector_push_FreetypeFace(
              &self.color_faces,
              (FreetypeFace){ .loaded    = false,
                              .file_name = settings.font_file_name_fallback2.str });
        }
    }
    Freetype_load_fonts(&self);
    return self;
}

void Freetype_load_fonts(Freetype* self)
{
    for (FreetypeStyledFamily* i = NULL;
         (i = Vector_iter_FreetypeStyledFamily(&self->primaries, i));) {
        FreetypeStyledFamily_load(self, i, settings.font_size, settings.font_dpi);
    }
    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->symbol_faces, i));) {
        FreetypeFace_load(self, i, settings.font_size, settings.font_dpi, FT_OUTPUT_RGB_H, false);
    }
    for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->color_faces, i));) {
        FreetypeFace_load(self,
                          i,
                          settings.font_size,
                          settings.font_dpi,
                          FT_OUTPUT_COLOR_BGRA,
                          false);
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
            output = FreetypeStyledFamily_load_and_render_glyph(self, i, codepoint, style);
            if (output) {
                return output;
            }
        }
    }

    if (self->symbol_faces.size) {
        for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->symbol_faces, i));) {
            output = FreetypeFace_load_and_render_glyph(self, i, codepoint);
            if (output) {
                return output;
            }
        }
    }
    if (self->color_faces.size) {
        for (FreetypeFace* i = NULL; (i = Vector_iter_FreetypeFace(&self->color_faces, i));) {
            output = FreetypeFace_load_and_render_glyph(self, i, codepoint);
            if (output) {
                return output;
            }
        }
    }
    return NULL;
}

void Freetype_destroy(Freetype* self)
{
    Freetype_unload_fonts(self);
    Vector_destroy_FreetypeStyledFamily(&self->primaries);
    Vector_destroy_FreetypeFace(&self->symbol_faces);
    Vector_destroy_FreetypeFace(&self->color_faces);
    FT_Done_FreeType(self->ft);
    self->initialized = false;
}
