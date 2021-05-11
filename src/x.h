/* See LICENSE for license information. */

/**
 * WindowX11 - window interface implementation for X11
 */

#ifndef NOX

#pragma once

#include "window.h"
#include "util.h"

struct WindowBase* Window_new_x11(Pair_uint32_t res, Pair_uint32_t cell_dims);

#endif
