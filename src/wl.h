/* See LICENSE for license information. */

/**
 * WindowWl - window interface implementation for wayland
 */

#ifndef NOWL

#pragma once

#include "window.h"
#include "util.h"

struct WindowBase* Window_new_wayland(Pair_uint32_t res);

#endif
