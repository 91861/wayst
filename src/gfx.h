/* See LICENSE for license information. */


#pragma once

//#include <GL/glew.h>

#include "util.h"
#include "colors.h"
#include "vector.h"
#include "gl.h"


typedef struct _IRendererBase {
    
} RendererBase;

/**
 * set renderer viewport size and recalculate transformations */
void
gfx_resize(uint32_t w, uint32_t h);

/**
 * @return number of rows and columns */
Pair_uint32_t
gfx_get_char_size(void*);


/**
 * Load font */
void
gfx_load_font();


/**
 * Load OpenGL resources, needs a gl context */
void
gfx_init();


/**
 * Check animation timers */
bool
gfx_update_timers();

/**
 * Notify the renderer that there was some activity */
void
gfx_notify_action(void*);

bool
gfx_set_focus(bool focus);

/**
 * Flash bell */
void
gfx_flash();

static inline void
gfx_init_with_size(const Pair_uint32_t res)
{
    gfx_init();

    gfx_resize(res.first, res.second);
}

/**
 * size of viewport in pixels required to fit given number of character cells */
Pair_uint32_t
gfx_pixels(void*, uint32_t r, uint32_t c);


struct _Vt;
/**
 * draw vt state */
void
gfx_draw_vt(struct _Vt* pty);


void
gfx_cleanup();

void
gfx_destroy_line_proxy(int32_t proxy[static 4]);
