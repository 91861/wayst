#define _GNU_SOURCE
#include "gfx_gl2_boxdraw_page.h"
#include <stdint.h>

/* Generate a private atlas page with consistant looking block elements from unicode block
 * "Block Elements" and mirrored equivalents from "Symbols for Legacy Computing". */
__attribute__((cold)) void GfxOpenGL2_maybe_generate_boxdraw_atlas_page(GfxOpenGL2* gfx)
{
    if (settings.font_box_drawing_chars) {
        return;
    }

    if (gfx->glyph_width_pixels < 5 || gfx->line_height_pixels < 5) {
        return;
    }

    uint32_t page_width  = gfx->glyph_width_pixels * 2;
    uint32_t page_height = gfx->line_height_pixels * 12 + 1;

    GlyphAtlasPage self = (GlyphAtlasPage){
        .height_px       = page_height,
        .width_px        = page_width,
        .texture_id      = 0,
        .texture_format  = TEX_FMT_MONO,
        .internal_format = GL_RED,
        .page_id         = gfx->glyph_atlas.pages.size,
    };

    self.sx = 2.0 / self.width_px;
    self.sy = 2.0 / self.height_px;

    float scale_tex_u = 1.0 / self.width_px;
    float scale_tex_v = 1.0 / self.height_px;

    size_t bs = self.height_px * self.width_px * sizeof(uint8_t) * 4;

    uint8_t* fragments = _calloc(1, bs);

#define L_OFFSET(_x, _y) (self.width_px * (_y) + (_x))

    /* texture layout:
       +--------+
       |LMD     | section 0, h = 1px; light, medium, dark
       +--------+
       |####    | section 1, h = 2 * glyph.h; solid blocks
       |####    |
       |####    |
       |    ####|
       |    ####|
       |    ####|
       |    ####|
       +--------+
       |##  #   | section 2, h = 1 * glyph.h; arrows
       |########|
       |########|
       |##  #   |
       +--------+
       |##      | section 3, h = 1 * glyph.h; slant
       |###     |
       |####    |
       +--------+
       | │   │  | section 4.1, h = 1 * glyph.h; light and heavy box drawing
       |─┼───┼──|
       | │   │  |
       | │      | section 4.2, h = 1 * glyph.h; double cross, double vert-left
       |─┼───┼──|
       | │   │  |
       | │   │  | section 4.3, h = 1 * glyph.h; double |-|
       | ┼───┼  |
       | │   │  |
       | │      | section 4.4, h = 1 * glyph.h; double inverted T + rounded corners combo
       | ┼─     |
       |        |
       |        | section 4.5, h = 1 * glyph.h; top corners
       | ┼───┼  |
       | │   │  |
       | │   │  | section 4.6, h = 1 * glyph.h; bottom corners
       | ┼───┼  |
       |        |
       | │   │  | section 4.7, h = 1 * glyph.h; double-thin mix +, T's and corners
       |─┼───┼──|
       | │   │  |
       | │   │  | section 4.8, h = 1 * glyph.h; double-thin mix +, T's and corners (clear middle)
       |─┼───┼──|
       | │   │  |
       +--------+

    */

    /* section 0 */
    fragments[L_OFFSET(0, 0)] = 50;  // LIGHT SHADE
    fragments[L_OFFSET(1, 0)] = 100; // MEDIUM SHADE
    fragments[L_OFFSET(2, 0)] = 200; // DARK SHADE

    /* section 1 */
    for (int32_t x = 0; x < gfx->glyph_width_pixels; ++x) {
        for (int32_t y = 1; y < (gfx->line_height_pixels + 1); ++y) {
            fragments[L_OFFSET(x, y)] = UINT8_MAX;
        }
    }
    for (uint32_t x = gfx->glyph_width_pixels; x < (gfx->glyph_width_pixels * 2); ++x) {
        for (int32_t y = (gfx->line_height_pixels + 1); y < (gfx->line_height_pixels * 2 + 1);
             ++y) {
            fragments[L_OFFSET(x, y)] = UINT8_MAX;
        }
    }

    /* section 2 */
    /*  and  from private-use area (filled triangles) */
    const float    sx      = 1.0 / gfx->glyph_width_pixels;
    const uint32_t yoffset = 1 + gfx->line_height_pixels * 2;
    const uint32_t xoffset = gfx->glyph_width_pixels;
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        for (int32_t dy = 0; dy <= gfx->line_height_pixels; ++dy) {
            double x     = ((double)dx + 0.5) / (double)gfx->glyph_width_pixels;
            double y     = ((double)dy + 0.5) / ((double)gfx->line_height_pixels / 2.0) - 1.0;
            double sd    = CLAMP((x - fabs(y)), -sx, sx);
            double value = (sd / (2.0 * sx)) + 0.5;
            fragments[L_OFFSET(xoffset + dx, yoffset + dy)] = value * UINT8_MAX;
        }
    }

    /*  and  from private-use area (filled semielipses) */
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
            int32_t  y_out = 1 + gfx->line_height_pixels * 2 + dy;
            uint32_t x_out = dx;
            double   x     = (double)dx / gfx->glyph_width_pixels;
            double   y     = (double)((double)dy + 0.5 - (double)gfx->line_height_pixels / 2.0) /
                       gfx->line_height_pixels * 2.0;
            float   x2 = x * x;
            float   y2 = y * y;
            float   w2 = gfx->glyph_width_pixels * gfx->glyph_width_pixels;
            float   h2 = (gfx->line_height_pixels) * (gfx->line_height_pixels);
            float   f  = sqrt(x * x + y * y);
            float   sd = (f - 1.0) * f / (2.0 * sqrt(x2 / w2 + y2 / h2));
            uint8_t value;
            if (sd > 0.5) {
                value = 0;
            } else if (sd > -0.5) {
                value = (0.5 - sd) * UINT8_MAX;
            } else if (sd > +0.5) {
                value = UINT8_MAX;
            } else if (sd > -0.5) {
                value = (sd + 0.5) * UINT8_MAX;
            } else {
                value = UINT8_MAX;
            }

            fragments[L_OFFSET(x_out, y_out)] = value;
        }
    }

    /* section 3 */
    /*  and  from private-use area (half-cell triangles) */
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        for (int32_t dy = 0; dy <= gfx->line_height_pixels; ++dy) {
            int32_t  y_out = 1 + gfx->line_height_pixels * 3 + dy;
            uint32_t x_out = dx;
            double   x     = ((double)dx + 0.5) / (double)gfx->glyph_width_pixels;
            double   y     = ((double)dy + 0.5) / (double)gfx->line_height_pixels;
            double   sd    = CLAMP((x - fabs(y)), -sx, sx);
            double   value = (sd / (2.0 * sx)) + 0.5;

            fragments[L_OFFSET(x_out, y_out)] = value * value * UINT8_MAX;
        }
    }

    /* section 4 */
    {
        int32_t vert_bar_x  = gfx->glyph_width_pixels / 2;
        int32_t hori_bar_y  = 1 + gfx->line_height_pixels * 4 + gfx->line_height_pixels / 2;
        int32_t vert_bar_x2 = gfx->glyph_width_pixels + gfx->glyph_width_pixels / 2;

        /* thin */
        for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
            int32_t y_out                          = 1 + gfx->line_height_pixels * 4 + dy;
            fragments[L_OFFSET(vert_bar_x, y_out)] = UINT8_MAX;
        }
        for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
            fragments[L_OFFSET(dx, hori_bar_y)] = UINT8_MAX;
        }

        /* fat */
        for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
            int32_t y_out                               = 1 + gfx->line_height_pixels * 4 + dy;
            fragments[L_OFFSET(vert_bar_x2, y_out)]     = UINT8_MAX;
            fragments[L_OFFSET(vert_bar_x2 + 1, y_out)] = UINT8_MAX;
        }
        for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
            int32_t x_out                              = gfx->glyph_width_pixels + dx;
            fragments[L_OFFSET(x_out, hori_bar_y)]     = UINT8_MAX;
            fragments[L_OFFSET(x_out, hori_bar_y + 1)] = UINT8_MAX;
        }
    }

    uint32_t dl_spread = MAX(1, (gfx->glyph_width_pixels / 5));
    /* double line */
    int32_t hori_bar_y  = 1 + gfx->line_height_pixels * 5 + gfx->line_height_pixels / 2;
    int32_t hori_bar_y2 = 1 + gfx->line_height_pixels * 6 + gfx->line_height_pixels / 2;
    int32_t hori_bar_y3 = 1 + gfx->line_height_pixels * 7 + gfx->line_height_pixels / 2;

    int32_t vert_bar_x  = gfx->glyph_width_pixels / 2;
    int32_t vert_bar_x2 = gfx->glyph_width_pixels + gfx->glyph_width_pixels / 2;

    // +
    for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
        int32_t y_out                                      = 1 + gfx->line_height_pixels * 5 + dy;
        fragments[L_OFFSET(vert_bar_x + dl_spread, y_out)] = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x - dl_spread, y_out)] = UINT8_MAX;
    }
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        fragments[L_OFFSET(dx, hori_bar_y + dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx, hori_bar_y - dl_spread)] = UINT8_MAX;
    }
    for (uint32_t x = vert_bar_x - dl_spread + 1; x < vert_bar_x + dl_spread; ++x) {
        fragments[L_OFFSET(x, hori_bar_y + dl_spread)] = 0;
        fragments[L_OFFSET(x, hori_bar_y - dl_spread)] = 0;
    }
    for (uint32_t y = hori_bar_y - dl_spread + 1; y < hori_bar_y + dl_spread; ++y) {
        fragments[L_OFFSET(vert_bar_x + dl_spread, y)] = 0;
        fragments[L_OFFSET(vert_bar_x - dl_spread, y)] = 0;
    }

    // T
    for (uint32_t dx = gfx->glyph_width_pixels; dx < gfx->glyph_width_pixels * 2; ++dx) {
        fragments[L_OFFSET(dx, hori_bar_y + dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx, hori_bar_y - dl_spread)] = UINT8_MAX;
    }
    for (int32_t dy = hori_bar_y + dl_spread; dy < 1 + gfx->line_height_pixels * 6; ++dy) {
        fragments[L_OFFSET(vert_bar_x2 + dl_spread, dy)] = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x2 - dl_spread, dy)] = UINT8_MAX;
    }
    for (uint32_t x = vert_bar_x2 - dl_spread + 1; x < vert_bar_x2 + dl_spread; ++x) {
        fragments[L_OFFSET(x, hori_bar_y + dl_spread)] = 0;
    }

    // |-|
    for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
        int32_t y_out                                       = 1 + gfx->line_height_pixels * 6 + dy;
        fragments[L_OFFSET(vert_bar_x + dl_spread, y_out)]  = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x - dl_spread, y_out)]  = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x2 + dl_spread, y_out)] = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x2 - dl_spread, y_out)] = UINT8_MAX;
    }
    for (uint32_t dx = vert_bar_x + dl_spread; dx < vert_bar_x2 - dl_spread; ++dx) {
        fragments[L_OFFSET(dx, hori_bar_y2 + dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx, hori_bar_y2 - dl_spread)] = UINT8_MAX;
    }
    for (uint32_t y = hori_bar_y2 - dl_spread + 1; y < hori_bar_y2 + dl_spread; ++y) {
        fragments[L_OFFSET(vert_bar_x + dl_spread, y)]  = 0;
        fragments[L_OFFSET(vert_bar_x2 - dl_spread, y)] = 0;
    }

    // inverted T
    for (uint32_t dy = 0; dy < (gfx->line_height_pixels / 2) - dl_spread; ++dy) {
        int32_t y_out                                      = 1 + gfx->line_height_pixels * 7 + dy;
        fragments[L_OFFSET(vert_bar_x + dl_spread, y_out)] = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x - dl_spread, y_out)] = UINT8_MAX;
    }
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        fragments[L_OFFSET(dx, hori_bar_y3 + dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx, hori_bar_y3 - dl_spread)] = UINT8_MAX;
    }
    for (uint32_t x = vert_bar_x - dl_spread + 1; x < vert_bar_x + dl_spread; ++x) {
        fragments[L_OFFSET(x, hori_bar_y - dl_spread)] = 0;
    }

    // rounded corners
    int32_t rc_base_x     = gfx->glyph_width_pixels;
    int32_t rc_base_y     = 1 + gfx->line_height_pixels * 7;
    int32_t corner_radius = MIN(gfx->glyph_width_pixels / 2, gfx->line_height_pixels / 2);

    int32_t cen_left = gfx->glyph_width_pixels / 2 - corner_radius;
    for (int32_t dx = 0; dx < cen_left; ++dx)
        fragments[L_OFFSET(rc_base_x + dx, hori_bar_y3)] = UINT8_MAX;
    int32_t cen_right = gfx->glyph_width_pixels / 2 + corner_radius;
    for (int32_t dx = cen_right; dx < gfx->glyph_width_pixels; ++dx)
        fragments[L_OFFSET(rc_base_x + dx, hori_bar_y3)] = UINT8_MAX;
    int32_t cen_top = (gfx->line_height_pixels / 2) - corner_radius;
    for (int32_t dy = 0; dy < cen_top; ++dy)
        fragments[L_OFFSET(rc_base_x + gfx->glyph_width_pixels / 2, rc_base_y + dy)] = UINT8_MAX;
    int32_t cen_bot = gfx->line_height_pixels / 2 + corner_radius;
    for (int32_t dy = cen_bot; dy < gfx->line_height_pixels; ++dy)
        fragments[L_OFFSET(rc_base_x + gfx->glyph_width_pixels / 2, rc_base_y + dy)] = UINT8_MAX;

    int32_t rc_t = gfx->line_height_pixels / 2;
    int32_t rc_b = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 1;
    int32_t rc_l = gfx->glyph_width_pixels / 2;
    int32_t rc_r = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 1;

#define L_DIST(x1, y1, x2, y2) sqrt(pow(x1 - x2, 2.0) + pow(y1 - y2, 2));

    // draw circles
    int32_t center_x = cen_right, center_y = cen_bot;
    for (int32_t c_dx = 0; c_dx <= corner_radius; ++c_dx) {
        for (int32_t c_dy = 0; c_dy <= corner_radius; ++c_dy) {
            int32_t x    = center_x - c_dx;
            int32_t y    = center_y - c_dy;
            double  dis  = L_DIST((double)x, (double)y, (double)center_x, (double)center_y);
            double  diff = CLAMP(ABS(dis - (double)corner_radius), 0.0, 1.0);
            fragments[L_OFFSET(x + rc_base_x, y + rc_base_y)] = UINT8_MAX * (1.0 - diff);
        }
    }
    center_x = cen_right, center_y = cen_top;
    for (int32_t c_dx = 0; c_dx <= corner_radius; ++c_dx) {
        for (int32_t c_dy = 0; c_dy <= corner_radius; ++c_dy) {
            int32_t x    = center_x - c_dx;
            int32_t y    = center_y + c_dy;
            double  dis  = L_DIST((double)x, (double)y, (double)center_x, (double)center_y);
            double  diff = CLAMP(ABS(dis - (double)corner_radius), 0.0, 1.0);
            fragments[L_OFFSET(x + rc_base_x, y + rc_base_y)] = UINT8_MAX * (1.0 - diff);
        }
    }
    center_x = cen_left, center_y = cen_top;
    for (int32_t c_dx = 0; c_dx <= corner_radius; ++c_dx) {
        for (int32_t c_dy = 0; c_dy <= corner_radius; ++c_dy) {
            int32_t x    = center_x + c_dx;
            int32_t y    = center_y + c_dy;
            double  dis  = L_DIST((double)x, (double)y, (double)center_x, (double)center_y);
            double  diff = CLAMP(ABS(dis - (double)corner_radius), 0.0, 1.0);
            fragments[L_OFFSET(x + rc_base_x, y + rc_base_y)] = UINT8_MAX * (1.0 - diff);
        }
    }
    center_x = cen_left, center_y = cen_bot;
    for (int32_t c_dx = 0; c_dx <= corner_radius; ++c_dx) {
        for (int32_t c_dy = 0; c_dy <= corner_radius; ++c_dy) {
            int32_t x    = center_x + c_dx;
            int32_t y    = center_y - c_dy;
            double  dis  = L_DIST((double)x, (double)y, (double)center_x, (double)center_y);
            double  diff = CLAMP(ABS(dis - (double)corner_radius), 0.0, 1.0);
            fragments[L_OFFSET(x + rc_base_x, y + rc_base_y)] = UINT8_MAX * (1.0 - diff);
        }
    }

    // conrner box
    for (uint32_t dx = vert_bar_x - dl_spread; dx < vert_bar_x2 + dl_spread; ++dx) {
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 8 + gfx->line_height_pixels / 2 -
                             dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 9 + gfx->line_height_pixels / 2 +
                             dl_spread)] = UINT8_MAX;
    }
    for (uint32_t dx = vert_bar_x + dl_spread; dx < vert_bar_x2 - dl_spread; ++dx) {
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 8 + gfx->line_height_pixels / 2 +
                             dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 9 + gfx->line_height_pixels / 2 -
                             dl_spread)] = UINT8_MAX;
    }
    for (uint32_t y = 1 + gfx->line_height_pixels * 8 + gfx->line_height_pixels / 2 - dl_spread;
         y < 2 + gfx->line_height_pixels * 9 + gfx->line_height_pixels / 2 + dl_spread;
         ++y) {
        fragments[L_OFFSET(vert_bar_x - dl_spread, y)]  = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x2 + dl_spread, y)] = UINT8_MAX;
    }
    for (uint32_t y = 1 + gfx->line_height_pixels * 8 + gfx->line_height_pixels / 2 + dl_spread;
         y < 2 + gfx->line_height_pixels * 9 + gfx->line_height_pixels / 2 - dl_spread;
         ++y) {
        fragments[L_OFFSET(vert_bar_x + dl_spread, y)]  = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x2 - dl_spread, y)] = UINT8_MAX;
    }

    // double-thin mixed +'s
    for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
        int32_t y_out                                      = 1 + gfx->line_height_pixels * 10 + dy;
        fragments[L_OFFSET(vert_bar_x + dl_spread, y_out)] = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x - dl_spread, y_out)] = UINT8_MAX;
    }
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        fragments[L_OFFSET(dx, 1 + gfx->line_height_pixels * 10 + gfx->line_height_pixels / 2)] =
          UINT8_MAX;
    }

    for (uint32_t dx = gfx->glyph_width_pixels; dx < gfx->glyph_width_pixels * 2.0; ++dx) {
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 10 + gfx->line_height_pixels / 2 +
                             dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 10 + gfx->line_height_pixels / 2 -
                             dl_spread)] = UINT8_MAX;
    }

    for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
        int32_t y_out                           = 1 + gfx->line_height_pixels * 10 + dy;
        fragments[L_OFFSET(vert_bar_x2, y_out)] = UINT8_MAX;
    }
    // the same but clear the middle bits
    for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
        int32_t y_out                                      = 1 + gfx->line_height_pixels * 11 + dy;
        fragments[L_OFFSET(vert_bar_x + dl_spread, y_out)] = UINT8_MAX;
        fragments[L_OFFSET(vert_bar_x - dl_spread, y_out)] = UINT8_MAX;
    }
    for (uint32_t dx = 0; dx < gfx->glyph_width_pixels; ++dx) {
        fragments[L_OFFSET(dx, 1 + gfx->line_height_pixels * 11 + gfx->line_height_pixels / 2)] =
          UINT8_MAX;
    }

    for (uint32_t dx = gfx->glyph_width_pixels; dx < gfx->glyph_width_pixels * 2.0; ++dx) {
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 11 + gfx->line_height_pixels / 2 +
                             dl_spread)] = UINT8_MAX;
        fragments[L_OFFSET(dx,
                           1 + gfx->line_height_pixels * 11 + gfx->line_height_pixels / 2 -
                             dl_spread)] = UINT8_MAX;
    }

    for (int32_t dy = 0; dy < gfx->line_height_pixels; ++dy) {
        int32_t y_out                           = 1 + gfx->line_height_pixels * 11 + dy;
        fragments[L_OFFSET(vert_bar_x2, y_out)] = UINT8_MAX;
    }

    for (uint32_t dx = gfx->glyph_width_pixels / 2 - dl_spread + 1;
         dx < gfx->glyph_width_pixels - (gfx->glyph_width_pixels / 2) + dl_spread;
         ++dx) {
        fragments[L_OFFSET(dx, 1 + gfx->line_height_pixels * 11 + gfx->line_height_pixels / 2)] = 0;
    }
    for (uint32_t dy = gfx->line_height_pixels / 2 - dl_spread + 1;
         dy < gfx->line_height_pixels - (gfx->line_height_pixels / 2) + dl_spread;
         ++dy) {
        int32_t y_out                           = 1 + gfx->line_height_pixels * 11 + dy;
        fragments[L_OFFSET(vert_bar_x2, y_out)] = 0;
    }

#undef L_OFFSET

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &self.texture_id);
    glBindTexture(GL_TEXTURE_2D, self.texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RED,
                 self.width_px,
                 self.height_px,
                 0,
                 GL_RED,
                 GL_UNSIGNED_BYTE,
                 fragments);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    free(fragments);

    Vector_push_GlyphAtlasPage(&gfx->glyph_atlas.pages, self);
    GlyphAtlasPage* page = Vector_last_GlyphAtlasPage(&gfx->glyph_atlas.pages);
    float           t    = gfx->pen_begin_pixels_y;

#define L_TC_U(_u) (((float)(_u)) * scale_tex_u)
#define L_TC_V(_v) (((float)(_v)) * scale_tex_v)
#define L_ENT_PROPS                                                                                \
    .page_id = page->page_id, .texture_id = self.texture_id, .height = gfx->line_height_pixels,    \
    .width = gfx->glyph_width_pixels, .top = t, .left = 0

#define L_ENT_PROPS_S(_top, _left, _w, _h)                                                         \
    .page_id = page->page_id, .texture_id = self.texture_id, .height = _h, .width = _w,            \
    .top = t - _top, .left = _left

    { /* LIGHT SHADE */
        Rune rune = (Rune){
            .code    = 0x2591,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.5f),
                                                     L_TC_V(0.5f),
                                                     L_TC_U(0.5f),
                                                     L_TC_V(0.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* MEDIUM SHADE */
        Rune rune = (Rune){
            .code    = 0x2592,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(1.5f),
                                                     L_TC_V(0.5f),
                                                     L_TC_U(1.5f),
                                                     L_TC_V(0.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* DARK SHADE */
        Rune rune = (Rune){
            .code    = 0x2593,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(2.5f),
                                                     L_TC_V(0.5f),
                                                     L_TC_U(2.5f),
                                                     L_TC_V(0.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* FULL BLOCK */
        Rune rune = (Rune){
            .code    = 0x2588,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.5f),
                                                     L_TC_V(1.5f),
                                                     L_TC_U(0.5f),
                                                     L_TC_V(1.5f),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x2580,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + (gfx->line_height_pixels / 2) * 3),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + (gfx->line_height_pixels / 2)),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x2584,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + (gfx->line_height_pixels / 2)),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + (gfx->line_height_pixels * 3) / 2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x2582,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4)),
                                                     L_TC_U(gfx->glyph_width_pixels * 2),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4) +
                                                            gfx->line_height_pixels),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB82,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4) +
                                                            gfx->line_height_pixels),
                                                     L_TC_U(gfx->glyph_width_pixels * 2),
                                                     L_TC_V(1.0f + (gfx->line_height_pixels / 4)),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2586,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 4.0f * 3.0f))),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + (int)((float)gfx->line_height_pixels / 4.0f * 3.0f) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB85,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + (int)((float)gfx->line_height_pixels / 4.0f * 3.0f) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 4.0f * 3.0f))),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER ONE EIGTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x2581,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8)),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER ONE EIGTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x2594,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + MAX(1, gfx->line_height_pixels / 8)),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER THREE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2583,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f))),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f)) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER THREE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1fb83,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f)) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 3.0f))),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER FIVE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2585,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f))),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f)) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER FIVE EIGTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB84,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f)) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((int)((float)gfx->line_height_pixels / 8.0f * 5.0f))),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* UPPER SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB86,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8) +
                                      gfx->line_height_pixels),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8)),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LOWER SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2587,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8)),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0f + ((gfx->line_height_pixels * 7) / 8) +
                                      gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x2589,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT SEVEN EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB8B,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 1.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x258A,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT THREE QUARTERS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB8A,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT FIVE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x258B,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT FIVE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB89,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x258C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels / 2.0),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels / 2.0),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT HALF BLOCK */
        Rune rune = (Rune){
            .code    = 0x2590,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels / 2.0f),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels / 2.0f),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT THREE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x258D,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT THREE EIGHTHS BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB88,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 5.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x258E,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT ONE QUARTER BLOCK */
        Rune rune = (Rune){
            .code    = 0x1FB87,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 4.0f * 3.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* LEFT ONE EIGHTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x258E,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U((int)(gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0),
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* RIGHT ONE EIGHTH BLOCK */
        Rune rune = (Rune){
            .code    = 0x2595,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels +
                                      (int)((float)gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0),
                               L_TC_U((int)((float)gfx->glyph_width_pixels / 8.0f * 7.0f)),
                               L_TC_V(1.0f + gfx->line_height_pixels),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* left semielipse */
        Rune rune = (Rune){
            .code    = 0xE0B6,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2),
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* right semielipse */
        Rune rune = (Rune){
            .code    = 0xE0B4,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* left filled triangle */
        Rune rune = (Rune){
            .code    = 0xE0B0,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* right filled triangle */
        Rune rune = (Rune){
            .code    = 0xE0B2,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 2.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* left slant */
        Rune rune = (Rune){
            .code    = 0xE0B8,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* right slant */
        Rune rune = (Rune){
            .code    = 0xE0BA,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 3.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    /* SECTION 4 */

    { /* box light cross ┼ */
        Rune rune = (Rune){
            .code    = 0x253C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light horizontal ─ */
        Rune rune = (Rune){
            .code    = 0x2500,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(1),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light vertical │ */
        Rune rune = (Rune){
            .code    = 0x2502,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + 1.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light vert to right ├ */
        Rune rune = (Rune){
            .code    = 0x251C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t l2 = gfx->glyph_width_pixels / 2;

        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                                                 l2,
                                                                 gfx->glyph_width_pixels - l2,
                                                                 gfx->line_height_pixels),
                                                   .tex_coords = {
                                                     L_TC_U(l2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light vert to left ┤ */
        Rune rune = (Rune){
            .code    = 0x2524,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 1;

        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                                                 0.0,
                                                                 gfx->glyph_width_pixels - r2,
                                                                 gfx->line_height_pixels),
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(gfx->glyph_width_pixels - r2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light top left conrner ┌ */
        Rune rune = (Rune){
            .code    = 0x250C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t l2 = gfx->glyph_width_pixels / 2;
        int32_t t2 = gfx->line_height_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(l2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light top right conrner ┐ */
        Rune rune = (Rune){
            .code    = 0x2510,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t t2 = gfx->line_height_pixels / 2;
        int32_t r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 1;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels - r2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light bottom right conrner ┘ */
        Rune rune = (Rune){
            .code    = 0x2518,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 1;
        int32_t r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 1;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels - r2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light bottom left conrner └ */
        Rune rune = (Rune){
            .code    = 0x2514,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 1;
        int32_t l2 = gfx->glyph_width_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(l2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light T-block ┬ */
        Rune rune = (Rune){
            .code    = 0x252C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t t2 = gfx->line_height_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box light inverted T-block ┴ */
        Rune rune = (Rune){
            .code    = 0x2534,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 1;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    /* section 4 fat */

    { /* box heavy cross ┼ */
        Rune rune = (Rune){
            .code    = 0x254b,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy horizontal ─ */
        Rune rune = (Rune){
            .code    = 0x2501,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0 + gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                                                     L_TC_U(1 + gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy vertical │ */
        Rune rune = (Rune){
            .code    = 0x2503,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + 1.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy vert right ├ */
        Rune rune = (Rune){
            .code    = 0x2523,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t l2 = gfx->glyph_width_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels),
                             .tex_coords = {
                               L_TC_U(l2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy vert left ┤ */
        Rune rune = (Rune){
            .code    = 0x252B,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels),
                             .tex_coords = {
                               L_TC_U(0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels - r2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy top left conrner ┌ */
        Rune rune = (Rune){
            .code    = 0x250f,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t l2 = gfx->glyph_width_pixels / 2;
        int32_t t2 = gfx->line_height_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(l2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy top right conrner ┐ */
        Rune rune = (Rune){
            .code    = 0x2513,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t t2 = gfx->line_height_pixels / 2;
        int32_t r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(0.0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels - r2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy bottom right conrner ┘ */
        Rune rune = (Rune){
            .code    = 0x251b,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 2;
        int32_t r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(0.0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels - r2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy bottom left conrner └ */
        Rune rune = (Rune){
            .code    = 0x2517,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 2;
        int32_t l2 = gfx->glyph_width_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(l2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy T-block ┬ */
        Rune rune = (Rune){
            .code    = 0x2533,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t t2 = gfx->line_height_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(0.0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box heavy inverted T-block ┴ */
        Rune rune = (Rune){
            .code    = 0x253b,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        int32_t b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(0.0 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 4.0),
                               L_TC_U(gfx->glyph_width_pixels + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double cross ┼ */
        Rune rune = (Rune){
            .code    = 0x256C,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 6.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double vertical │ */
        Rune rune = (Rune){
            .code    = 0x2551,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS,
                             .tex_coords = {
                               L_TC_U(0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 5.0 + 1.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double horizontal ─ */
        Rune rune = (Rune){
            .code    = 0x2550,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                     L_TC_U(1),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 6.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double cross ┼ */
        Rune rune = (Rune){
            .code    = 0x2566,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 5.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 6.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double |- */
        Rune rune = (Rune){
            .code    = 0x2560,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 6.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 7.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double -| */
        Rune rune = (Rune){
            .code    = 0x2563,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 6.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 7.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* box double inverted T */
        Rune rune = (Rune){
            .code    = 0x2569,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 7.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 8.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* top left corner */
        Rune rune = (Rune){
            .code    = 0x2554,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 8.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 9.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* top right corner */
        Rune rune = (Rune){
            .code    = 0x2557,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 8.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 9.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* bottom left corner */
        Rune rune = (Rune){
            .code    = 0x255a,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 9.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* bottom right corner */
        Rune rune = (Rune){
            .code    = 0x255d,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 9.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + vertical double ╫ */
        Rune rune = (Rune){
            .code    = 0x256b,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(0.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }
    { /* + vertical double T */
        Rune rune = (Rune){
            .code    = 0x2565,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        uint32_t t2 = gfx->line_height_pixels / 2;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }
    { /* + vertical double inverted T */
        Rune rune = (Rune){
            .code    = 0x2568,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        uint32_t b2 = gfx->line_height_pixels - (gfx->line_height_pixels / 2) - 1;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + -horizontal double ╪ */
        Rune rune = (Rune){
            .code    = 0x256a,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS,
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double -| */
        Rune rune = (Rune){
            .code    = 0x2561,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        int32_t         r2    = gfx->glyph_width_pixels - (gfx->glyph_width_pixels / 2) - 1;
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                                                 0.0,
                                                                 gfx->glyph_width_pixels - r2,
                                                                 gfx->line_height_pixels),
                                                   .tex_coords = {
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0 - r2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double |- */
        Rune rune = (Rune){
            .code    = 0x255e,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        int32_t         l2    = gfx->glyph_width_pixels / 2;
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                                                 l2,
                                                                 gfx->glyph_width_pixels - l2,
                                                                 gfx->line_height_pixels),
                                                   .tex_coords = {
                                                     L_TC_U(l2 + gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                                                     L_TC_U(gfx->glyph_width_pixels * 2.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double top left corner */
        Rune rune = (Rune){
            .code    = 0x2552,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        t2 = gfx->line_height_pixels / 2 - dl_spread;
        int32_t         l2 = gfx->glyph_width_pixels / 2;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(l2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels * 2.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double bottom left corner */
        Rune rune = (Rune){
            .code    = 0x2558,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - dl_spread - 1;
        int32_t         l2 = gfx->glyph_width_pixels / 2;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(l2 + gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                               L_TC_U(gfx->glyph_width_pixels * 2.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double top right corner */
        Rune rune = (Rune){
            .code    = 0x2555,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        t2 = gfx->line_height_pixels / 2 - dl_spread;
        int32_t         r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 1;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels * 2.0 - r2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double bottom right corner */
        Rune rune = (Rune){
            .code    = 0x255b,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - dl_spread - 1;
        int32_t         r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - 1;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                               L_TC_U(gfx->glyph_width_pixels * 2.0 - r2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    // second box
    { /* + vert double top left corner */
        Rune rune = (Rune){
            .code    = 0x2553,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        t2 = gfx->line_height_pixels / 2;
        int32_t         l2 = gfx->glyph_width_pixels / 2 - dl_spread;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(l2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + vert double bottom left corner */
        Rune rune = (Rune){
            .code    = 0x2559,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 1;
        int32_t         l2 = gfx->glyph_width_pixels / 2 - dl_spread;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           l2,
                                           gfx->glyph_width_pixels - l2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(l2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double top right corner */
        Rune rune = (Rune){
            .code    = 0x2556,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        t2 = gfx->line_height_pixels / 2;
        int32_t         r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - dl_spread - 1;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels - r2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double bottom right corner */
        Rune rune = (Rune){
            .code    = 0x255c,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        uint32_t        b2 = gfx->line_height_pixels - gfx->line_height_pixels / 2 - 1;
        int32_t         r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - dl_spread - 1;
        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels - r2,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(0.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 10.0),
                               L_TC_U(gfx->glyph_width_pixels - r2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double T */
        Rune rune = (Rune){
            .code    = 0x2564,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        uint32_t t2 = gfx->line_height_pixels / 2 - dl_spread;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(t2,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - t2),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0 + t2),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 12.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + horizontal double inverted T */
        Rune rune = (Rune){
            .code    = 0x2567,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        uint32_t b2 = gfx->line_height_pixels - (gfx->line_height_pixels / 2) - 1 - dl_spread;

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels,
                                           gfx->line_height_pixels - b2),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                               L_TC_U(gfx->glyph_width_pixels * 2),
                               L_TC_V(1.0 + gfx->line_height_pixels * 12.0 - b2),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + vert double |- */
        Rune rune = (Rune){
            .code    = 0x255f,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        int32_t         l2    = gfx->glyph_width_pixels / 2 - dl_spread;
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                                                 l2,
                                                                 gfx->glyph_width_pixels - l2,
                                                                 gfx->line_height_pixels),
                                                   .tex_coords = {
                                                     L_TC_U(l2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                                                     L_TC_U(gfx->glyph_width_pixels),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 12.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    { /* + vert double -| */
        Rune rune = (Rune){
            .code    = 0x2562,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };
        int32_t         r2 = gfx->glyph_width_pixels - gfx->glyph_width_pixels / 2 - dl_spread - 1;
        GlyphAtlasEntry entry = (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                                                 0.0,
                                                                 gfx->glyph_width_pixels - r2,
                                                                 gfx->line_height_pixels),
                                                   .tex_coords = {
                                                     L_TC_U(0.0),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 11.0),
                                                     L_TC_U(gfx->glyph_width_pixels - r2),
                                                     L_TC_V(1.0 + gfx->line_height_pixels * 12.0),
                                                   } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

    // rounded corners
    {
        Rune rune = (Rune){
            .code    = 0x256d,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(rc_t,
                                           rc_l,
                                           gfx->glyph_width_pixels - rc_l,
                                           gfx->line_height_pixels - rc_t),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels + rc_l),
                               L_TC_V(1.0 + gfx->line_height_pixels * 7.0 + rc_t),
                               L_TC_U(gfx->glyph_width_pixels * 2.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 8.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }
    {
        Rune rune = (Rune){
            .code    = 0x256e,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(rc_t,
                                           0.0,
                                           gfx->glyph_width_pixels - rc_r,
                                           gfx->line_height_pixels - rc_t),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 7.0 + rc_t),
                               L_TC_U(gfx->glyph_width_pixels * 2.0 - rc_r),
                               L_TC_V(1.0 + gfx->line_height_pixels * 8.0),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }
    {
        Rune rune = (Rune){
            .code    = 0x256f,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           0.0,
                                           gfx->glyph_width_pixels - rc_r,
                                           gfx->line_height_pixels - rc_b),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels),
                               L_TC_V(1.0 + gfx->line_height_pixels * 7.0),
                               L_TC_U(gfx->glyph_width_pixels * 2.0 - rc_r),
                               L_TC_V(1.0 + gfx->line_height_pixels * 8.0 - rc_b),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }
    {
        Rune rune = (Rune){
            .code    = 0x2570,
            .combine = { 0 },
            .style   = VT_RUNE_UNSTYLED,
        };

        GlyphAtlasEntry entry =
          (GlyphAtlasEntry){ L_ENT_PROPS_S(0.0,
                                           rc_l,
                                           gfx->glyph_width_pixels - rc_l,
                                           gfx->line_height_pixels - rc_b),
                             .tex_coords = {
                               L_TC_U(gfx->glyph_width_pixels + rc_l),
                               L_TC_V(1.0 + gfx->line_height_pixels * 7.0),
                               L_TC_U(gfx->glyph_width_pixels * 2.0),
                               L_TC_V(1.0 + gfx->line_height_pixels * 8.0 - rc_b),
                             } };

        Map_insert_Rune_GlyphAtlasEntry(&gfx->glyph_atlas.entry_map, rune, entry);
    }

#undef L_TC_U
#undef L_TC_V
#undef L_ENT_PROPS
}
