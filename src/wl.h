/* See LICENSE for license information. */

/**
 * WindowWl - window interface implementation for wayland
 */

#ifndef NOWL

#pragma once

#include "util.h"
#include "window.h"

struct WindowBase* Window_new_wayland(Pair_uint32_t res,
                                      Pair_uint32_t cell_dims,
                                      gfx_api_t     gfx_api,
                                      Ui*           ui);

#endif
