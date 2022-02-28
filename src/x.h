/* See LICENSE for license information. */

/**
 * WindowX11 - window interface implementation for X11
 */

#ifndef NOX

#pragma once

#include "util.h"
#include "window.h"

struct WindowBase* Window_new_x11(Pair_uint32_t res, Pair_uint32_t cell_dims, gfx_api_t gfx_api);

#endif
