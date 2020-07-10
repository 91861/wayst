/* See LICENSE for license information. */

/**
 * Simplified interface for fontconfig
 */

#pragma once

#include <fontconfig/fontconfig.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    FcConfig* cfg;
} FontconfigContext;

FontconfigContext FontconfigContext_new();

char* FontconfigContext_get_file(FontconfigContext* self,
                                 const char*        opt_family,
                                 const char*        opt_style,
                                 uint32_t           opt_size,
                                 bool*              out_is_bitmap);

void FontconfigContext_destroy(FontconfigContext* self);
