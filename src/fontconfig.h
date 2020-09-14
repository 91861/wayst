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

char* FontconfigContext_get_file(FontconfigContext*   self,
                                 const char* restrict opt_family,
                                 const char* restrict opt_style,
                                 uint32_t             opt_size,
                                 bool*                opt_out_is_bitmap,
                                 bool*                opt_out_is_exact);

void FontconfigContext_destroy(FontconfigContext* self);
