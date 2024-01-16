/* See LICENSE for license information. */

/**
 * renderer implementations for for OpenGL 2.1 and OpenGL ES 2.0
 */

#pragma once

#define _GNU_SOURCE
#include "gfx.h"
#include "freetype.h"

Gfx* Gfx_new_OpenGL2(Freetype* freetype);
