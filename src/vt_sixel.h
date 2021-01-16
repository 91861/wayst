#pragma once

#include "colors.h"
#include "vt.h"

DEF_VECTOR(Vector_uint8_t, Vector_destroy_uint8_t);

__attribute__((always_inline)) static inline void _sixel_surface_new_from_data_push_char(
  bool                  zero_overwrites_color,
  VtSixelSurface*       self,
  uint32_t*             carriage_col,
  Vector_Vector_uint8_t sixel_band,
  ColorRGB*             active_color,
  uint8_t               data)
{
    data -= 0x3f;
    for (int j = 0; j < 6; ++j) {
        bool            set = data & (1 << j);
        Vector_uint8_t* v   = Vector_last_Vector_uint8_t(&sixel_band) + j - 5;

        while (v->size <= *carriage_col * 3) {
            static const uint8_t empty[3] = { 0, 0, 0 };
            Vector_pushv_uint8_t(v, empty, 3);
        }
        if (set) {
            v->buf[*carriage_col * 3 + 0] = active_color->r;
            v->buf[*carriage_col * 3 + 1] = active_color->g;
            v->buf[*carriage_col * 3 + 2] = active_color->b;
        } else if (unlikely(zero_overwrites_color)) {
            // TODO: ;
        }
    }

    ++(*carriage_col);
    if (*carriage_col >= self->width) {
        self->width = *carriage_col;
    }
}

static inline bool VtSixelSurface_is_visible(const Vt* vt, const VtSixelSurface* self)
{
    return self->anchor_global_index <= Vt_bottom_line(vt) &&
           self->anchor_global_index + Vt_row(vt) >= Vt_top_line(vt);
}

static inline bool VtSixelSurface_is_visual_visible(const Vt* vt, const VtSixelSurface* self)
{
    return self->anchor_global_index <= Vt_visual_bottom_line(vt) &&
           self->anchor_global_index + Vt_row(vt) >= Vt_visual_top_line(vt);
}

static VtSixelSurface VtSixelSurface_new_from_data(uint8_t                    pixel_aspect,
                                                   bool                       zero_overwrites_color,
                                                   uint8_t*                   data,
                                                   graphic_color_registers_t* color_registers)
{
    VtSixelSurface self = {
        .width     = 0,
        .height    = 0,
        .fragments = Vector_new_uint8_t(),
        .proxy     = { { 0 } },
    };

    // int32_t  active_pixel_width = 1, active_pixel_height = pixel_aspect;
    ColorRGB active_color = { 0, 0, 0 };

    uint32_t sixel_cursor_position = 0;

    ColorRGB palette[256];
    memset(palette, 0, sizeof(palette));

    Vector_Vector_uint8_t sixel_band = Vector_new_with_capacity_Vector_uint8_t(pixel_aspect);

    for (int i = 0; i < 6; ++i) {
        Vector_push_Vector_uint8_t(&sixel_band, Vector_new_with_capacity_uint8_t(self.width * 3));
    }

    static char buf[32];
    for (uint32_t i = 0; data[i];) {
        switch (data[i]) {
            case '!': {
                ++i;
                uint32_t     n;
                uint_fast8_t bof = 0;
                memset(buf, 0, sizeof(buf));
                while (isdigit(data[i])) {
                    buf[bof++] = data[i++];
                }
                uint8_t what = data[i++];
                n            = atoi(buf);
                if (likely(what >= 0x3f && what <= 0x7e)) {
                    for (uint32_t j = 0; j < n; ++j) {
                        _sixel_surface_new_from_data_push_char(zero_overwrites_color,
                                                               &self,
                                                               &sixel_cursor_position,
                                                               sixel_band,
                                                               &active_color,
                                                               what);
                    }
                } else {
                    WRN("invalid character \'%c\' (%d) in sixel repeat sequence\n", what, what);
                }
            } break;

            case '"': {
                uint32_t     values[4];
                uint_fast8_t bof = 0;
                for (uint_fast8_t j = 0; j < ARRAY_SIZE(values); ++j) {
                    ++i;
                    memset(buf, 0, sizeof(buf));
                    while (isdigit(data[i])) {
                        buf[bof++] = data[i++];
                    }
                    values[j] = atoi(buf);
                    bof       = 0;
                }

                /* active_pixel_height = values[0]; */
                /* active_pixel_width  = values[1]; */
                self.width  = MAX(values[2], self.width);
                self.height = MAX(values[3], self.height);

            } break;

            case '#': {
                uint_fast8_t bof = 0;
                int32_t      val[5];
                memset(val, 0, sizeof(val));
                ++i;
                memset(buf, 0, sizeof(buf));
                while (isdigit(data[i])) {
                    buf[bof++] = data[i++];
                }
                val[0] = atoi(buf);
                bof    = 0;
                if (data[i] == ';') {
                    for (uint_fast8_t j = 1; j < ARRAY_SIZE(val); ++j) {
                        ++i;
                        memset(buf, 0, sizeof(buf));
                        while (isdigit(data[i])) {
                            buf[bof++] = data[i++];
                        }
                        val[j] = atoi(buf);
                        bof    = 0;
                    }

                    if (val[1] == 2) /* RGB */ {
                        for (uint_fast8_t j = 2; j < ARRAY_SIZE(val); ++j) {
                            val[j] = CLAMP(val[j], 0, 100);
                            val[j] = val[j] * 255 / 100;
                        }

                        palette[val[0]] = (ColorRGB){
                            .r = val[2],
                            .g = val[3],
                            .b = val[4],
                        };
                    } else if (val[1] == 1) /* HLS (not HSL!) */ {
                        palette[val[0]] = ColorRGB_new_from_hsl((double)val[2] / 100,
                                                                (double)val[4] / 100,
                                                                (double)val[3] / 100);
                    } else {
                        WRN("invalid coordinate system in sixel color selection sequence\n");
                    }
                } else {
                    active_color = palette[val[0]];
                }
            } break;

            case '-':
                for (int j = 0; j < 6; ++j) {
                    Vector_push_Vector_uint8_t(&sixel_band,
                                               Vector_new_with_capacity_uint8_t(self.width * 3));
                }
                /* fallthrough */
            case '$':
                sixel_cursor_position = 0;
                ++i;
                break;

            case 0x3f ... 0x7e: {
                _sixel_surface_new_from_data_push_char(zero_overwrites_color,
                                                       &self,
                                                       &sixel_cursor_position,
                                                       sixel_band,
                                                       &active_color,
                                                       data[i]);
                ++i;
            } break;

            default:
                WRN("Invalid sixel character: %c (%d)\n", data[i], data[i]);
                ++i;
                break;
        }
    }

    for (Vector_uint8_t* i = NULL; (i = Vector_iter_Vector_uint8_t(&sixel_band, i));) {
        while (i->size < self.width * 3) {
            static const uint8_t empty[3] = { 0, 0, 0 };
            Vector_pushv_uint8_t(i, empty, 3);
        }
    }

    for (Vector_uint8_t* i = NULL; (i = Vector_iter_Vector_uint8_t(&sixel_band, i));) {
        Vector_pushv_uint8_t(&self.fragments, i->buf, i->size);
    }

    self.height = MAX(self.height, sixel_band.size);
    Vector_destroy_Vector_uint8_t(&sixel_band);

    LOG("vt::sixel::surface_new{ initial_aspect_ratio %d:1, zero_overwrites_color: " BOOL_FMT
        ", width: %u, height: %u }\n",
        pixel_aspect,
        BOOL_AP(zero_overwrites_color),
        self.width,
        self.height);

    return self;
};
