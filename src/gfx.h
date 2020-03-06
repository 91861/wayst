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
gl_set_size(uint32_t w, uint32_t h);

/**
 * @return number of rows and columns */
Pair_uint32_t
gl_get_char_size();


/**
 * Load font */
void
gl_init_font();


/**
 * Load OpenGL resources, needs a gl context */
void
gl_init_renderer();


/**
 * Check animation timers */
bool
gl_check_timers();

/**
 * Notify the renderer that there was some activity */
void
gl_reset_action_timer();

bool
gl_set_focus(bool focus);

/**
 * Flash bell */
void
gl_flash();

static inline void
gl_init_renderer_with_size(const Pair_uint32_t res)
{
    gl_init_renderer();

    gl_set_size(res.first, res.second);
}

/**
 * size of viewport in pixels required to fit given number of character cells */
Pair_uint32_t
gl_pixels(uint32_t r, uint32_t c);


struct _Vt;
/**
 * draw vt state */
void
gfx_draw_vt(struct _Vt* pty);


void
gfx_cleanup();

void
gfx_destroy_line_proxy(int32_t proxy[static 4]);
