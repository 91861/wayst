/* See LICENSE for license information. */

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "fontconfig.h"
#include "settings.h"
#include "util.h"
#include "vector.h"

DEF_VECTOR(char, NULL)

FontconfigContext FontconfigContext_new()
{
    FontconfigContext self = { .cfg = FcInitLoadConfigAndFonts() };
    if (!self.cfg) {
        ERR("Failed to load fontconfig configuration");
    }
    return self;
}

char* FontconfigContext_get_file(FontconfigContext*   self,
                                 const char* restrict opt_family,
                                 const char* restrict opt_style,
                                 uint32_t             opt_size,
                                 bool*                opt_out_is_bitmap,
                                 bool*                opt_out_is_exact)
{
    ASSERT(self && self->cfg, "config loaded");

    char*       retval                 = NULL;
    Vector_char pattern_string_builder = Vector_new_char();
    opt_family                         = OR(opt_family, "");
    Vector_pushv_char(&pattern_string_builder, opt_family, strlen(opt_family));
    if (opt_size) {
        char tmp[64];
        int  len = snprintf(tmp, sizeof(tmp), "-%u", opt_size);
        Vector_pushv_char(&pattern_string_builder, tmp, len);
    }
    if (opt_style) {
        Vector_push_char(&pattern_string_builder, ':');
        Vector_pushv_char(&pattern_string_builder, opt_style, strlen(opt_style) + 1);
    } else {
        Vector_push_char(&pattern_string_builder, '\0');
    }

    if (unlikely(settings.debug_font)) {
        printf("Match result for \'%s\':\n", pattern_string_builder.buf);
    }

    FcPattern* pattern = FcNameParse((FcChar8*)pattern_string_builder.buf);
    FcConfigSubstitute(self->cfg, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcFontSet* font_set = FcFontSetCreate();
    FcResult   result;
    FcPattern* match = FcFontMatch(self->cfg, pattern, &result);
    if (match) {
        FcFontSetAdd(font_set, match);
    }
    FcPatternDestroy(pattern);

    if (font_set) {
        for (int i = 0; i < font_set->nfont; ++i) {
            FcPattern* filtered = FcPatternFilter(font_set->fonts[i], NULL);
            FcChar8*   file;
            FcPatternGetString(filtered, FC_FILE, 0, &file);
            free(retval);
            retval = strdup((char*)file);

            if (opt_out_is_exact && opt_family) {
                *opt_out_is_exact = !strcasecmp(retval, opt_family);
            }

            FcBool is_scalable;
            FcPatternGetBool(filtered, FC_SCALABLE, 0, &is_scalable);
            if (opt_out_is_bitmap) {
                *opt_out_is_bitmap = !is_scalable;
            }

            FcPatternDestroy(filtered); // frees file
        }
    }
    FcPatternDestroy(match);
    if (font_set) {
        FcFontSetDestroy(font_set);
    }
    Vector_destroy_char(&pattern_string_builder);

    if (unlikely(settings.debug_font)) {
        printf("  %s\n", retval);
    }

    return retval;
}

void FontconfigContext_destroy(FontconfigContext* self)
{
    ASSERT(self && self->cfg, "config loaded");

    FcConfigDestroy(self->cfg);
    self->cfg = NULL;
}
