#pragma once

#include "colors.h"
#include "util.h"

#include "vt.h"
#include "vt_private.h"

DEF_VECTOR(Vector_uint8_t, Vector_destroy_uint8_t);

__attribute__((cold)) const char* control_char_get_pretty_string(const char c);

static const uint8_t SIXEL_DATA_CHANNEL_CNT = 4;

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

        while (v->size <= *carriage_col * SIXEL_DATA_CHANNEL_CNT) {
            static const uint8_t empty[4] = { 0, 0, 0, 0 };
            Vector_pushv_uint8_t(v, empty, ARRAY_SIZE(empty));
        }

        if (set) {
            if (zero_overwrites_color) {
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 0] = active_color->r;
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 1] = active_color->g;
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 2] = active_color->b;
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 3] = UINT8_MAX;
            } else {
                // interpret black as transparency
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 0] = active_color->r;
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 1] = active_color->g;
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 2] = active_color->b;
                bool black = active_color->r + active_color->g + active_color->b == 0;
                v->buf[*carriage_col * SIXEL_DATA_CHANNEL_CNT + 3] = black ? 0 : UINT8_MAX;
            }
        }
    }

    ++(*carriage_col);
    if (*carriage_col >= self->width) {
        self->width = *carriage_col;
    }
}

static VtSixelSurface VtSixelSurface_new_from_data(uint8_t                    pixel_aspect,
                                                   bool                       zero_overwrites_color,
                                                   uint8_t*                   data,
                                                   graphic_color_registers_t* color_registers)
{
    VtSixelSurface self = {
        .width              = 0,
        .height             = 0,
        .fragments          = Vector_new_uint8_t(),
        .pixel_aspect_ratio = pixel_aspect,
        .proxy              = { { 0 } },
    };

    int32_t  active_pixel_width = 1, active_pixel_height = pixel_aspect;
    ColorRGB active_color          = { 0, 0, 0 };
    uint32_t sixel_cursor_position = 0;

    ColorRGB palette[256];
    memset(palette, 0, sizeof(palette));

    Vector_Vector_uint8_t sixel_band = Vector_new_Vector_uint8_t();

    for (int i = 0; i < 6; ++i) {
        Vector_push_Vector_uint8_t(
          &sixel_band,
          Vector_new_with_capacity_uint8_t(self.width * SIXEL_DATA_CHANNEL_CNT));
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
                    WRN("invalid character \'%s" TERMCOLOR_RESET ""
                        "\' (%d) in sixel repeat sequence\n",
                        control_char_get_pretty_string(what),
                        what);
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

                active_pixel_height = values[0];
                active_pixel_width  = values[1];
                self.width          = MAX(values[2], self.width);
                self.height         = MAX(values[3], self.height);

                if (active_pixel_width > active_pixel_height) {
                    WRN("unsupported sixel pixel ratio %d:%d\n",
                        active_pixel_width,
                        active_pixel_height);
                }

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
                    Vector_push_Vector_uint8_t(
                      &sixel_band,
                      Vector_new_with_capacity_uint8_t(self.width * SIXEL_DATA_CHANNEL_CNT));
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

            default: {
                const char  raw_char[2] = { data[i], 0 };
                const char* preety_char = control_char_get_pretty_string(data[i]);
                WRN("ignoring unexpected sixel data character: '\%s" TERMCOLOR_RESET "\' (%d)\n",
                    preety_char ? preety_char : raw_char,
                    data[i]);
                ++i;
            } break;
        }
    }

    /* fill remaining data with zeros */
    for (Vector_uint8_t* i = NULL; (i = Vector_iter_Vector_uint8_t(&sixel_band, i));) {
        while (i->size < self.width * SIXEL_DATA_CHANNEL_CNT) {
            static uint8_t empty[4] = { 0, 0, 0, 0 };
            ARRAY_LAST(empty)       = zero_overwrites_color ? UINT8_MAX : 0;
            Vector_pushv_uint8_t(i, empty, ARRAY_SIZE(empty));
        }
    }

    /* convert data bands into pixels */
    for (Vector_uint8_t* i = NULL; (i = Vector_iter_Vector_uint8_t(&sixel_band, i));) {
        for (int j = 0; j < active_pixel_height / active_pixel_width; ++j) {
            Vector_pushv_uint8_t(&self.fragments, i->buf, i->size);
        }
    }

    self.height = MAX(self.height, sixel_band.size * (active_pixel_height / active_pixel_width));
    Vector_destroy_Vector_uint8_t(&sixel_band);

    /* not associated with a VtLine so no cell mask */
    self.cell_mask.buf  = 0;
    self.cell_mask.size = 0;
    self.cell_mask.cap  = 0;

    LOG("vt::sixel::surface_new{ P1_aspect_ratio %d:1, aspect_ratio %d:%d, "
        "zero_overwrites_color: " BOOL_FMT ", width: %u, height: %u }\n",
        pixel_aspect,
        active_pixel_height,
        active_pixel_width,
        BOOL_AP(zero_overwrites_color),
        self.width,
        self.height);

    return self;
}

static void Vt_clear_line_sixel_proxies(Vt* self, VtLine* ln)
{
    if (!ln || !ln->graphic_attachments || !ln->graphic_attachments->sixels) {
        return;
    }

    for (uint32_t i = 0; i < ln->graphic_attachments->sixels->size; ++i) {
        VtSixelSurface* sixel = RcPtr_get_VtSixelSurface(&ln->graphic_attachments->sixels->buf[i]);
        CALL(self->callbacks.destroy_sixel_proxy, self->callbacks.user_data, &sixel->proxy);
    }
}

static void Vt_sixel_clear_line(Vt* vt, size_t row)
{
    if (row >=  vt->lines.size) {
        return;
    }

    VtLine* ln = Vt_line_at(vt, row) ;
    if (!ln || !ln->graphic_attachments || !ln->graphic_attachments->sixels) {
        return;
    }

    Vector_destroy_RcPtr_VtSixelSurface(ln->graphic_attachments->sixels);
    free(ln->graphic_attachments->sixels);
    ln->graphic_attachments->sixels = NULL;
    if (!ln->graphic_attachments->images) {
        free(ln->graphic_attachments);
        ln->graphic_attachments = NULL;
    }
}

static void Vt_sixel_overwrite_cell_range(Vt* vt, size_t row, uint16_t col_begin, uint16_t col_end)
{
    VtLine* ln = Vt_cursor_line(vt);

    if (!ln || !ln->graphic_attachments || !ln->graphic_attachments->sixels) {
        return;
    }

    ln->damage.type = VT_LINE_DAMAGE_FULL;

    for (uint32_t i = 0; i < ln->graphic_attachments->sixels->size; ++i) {
        VtSixelSurface* sixel = RcPtr_get_VtSixelSurface(&ln->graphic_attachments->sixels->buf[i]);
        if (sixel->anchor_cell_idx + sixel->cell_mask.size > col_begin &&
            sixel->anchor_cell_idx <= col_begin) {
            for (uint16_t col = col_begin; col < col_end; ++col) {
                if (sixel->anchor_cell_idx + sixel->cell_mask.size > col) {
                    sixel->cell_mask.buf[col - sixel->anchor_cell_idx] = false;
                }
            }
        }

        bool all_clear = true;
        for (bool* msk = NULL; (msk = Vector_iter_bool(&sixel->cell_mask, msk));) {
            if (*msk) {
                all_clear = false;
                break;
            }
        }

        if (all_clear) {
            Vector_remove_at_RcPtr_VtSixelSurface(ln->graphic_attachments->sixels, i, 1);
        }
    }

    if (!ln->graphic_attachments->sixels->size) {
        free(ln->graphic_attachments->sixels);
        ln->graphic_attachments->sixels = NULL;

        if (!ln->graphic_attachments->images) {
            free(ln->graphic_attachments);
            ln->graphic_attachments = NULL;
        }
    }
}

static inline void Vt_sixel_overwrite_cell(Vt* vt, size_t row, uint16_t col)
{
    Vt_sixel_overwrite_cell_range(vt, row, col, col + 1);
}

/* Splits a sixel into parts coresponding to a line of text. all properties other than fragment data
 * and height are inherited. */
static Vector_RcPtr_VtSixelSurface VtSixelSurface_split_into_lines(VtSixelSurface* surface, Vt* vt)
{
    size_t count = CEIL_DIV(surface->height, surface->line_height_created_px);

    Vector_RcPtr_VtSixelSurface slices = Vector_new_with_capacity_RcPtr_VtSixelSurface(count);

    uint32_t remaining_y_pixels = surface->height;
    uint32_t y_pixel_offset     = 0;

    while (true) {
        size_t slice_height = surface->line_height_created_px;

        if (remaining_y_pixels < slice_height) {
            slice_height = remaining_y_pixels;
        }

        size_t slice_data_size   = slice_height * surface->width * SIXEL_DATA_CHANNEL_CNT;
        size_t slice_data_offset = y_pixel_offset * surface->width * SIXEL_DATA_CHANNEL_CNT;

        VtSixelSurface copy = *surface;
        copy.height         = slice_height;
        copy.fragments      = Vector_new_with_capacity_uint8_t(slice_data_size);
        Vector_pushv_uint8_t(&copy.fragments,
                             surface->fragments.buf + slice_data_offset,
                             slice_data_size);

        size_t cell_span_count = CEIL_DIV(surface->width, surface->cell_width_created_px);

        copy.cell_mask = Vector_new_with_capacity_bool(cell_span_count);

        for (uint32_t i = 0; i < cell_span_count; ++i) {
            Vector_push_bool(&copy.cell_mask, true);
        }

        RcPtr_VtSixelSurface rc        = RcPtr_new_VtSixelSurface(vt);
        *RcPtr_get_VtSixelSurface(&rc) = copy;

        Vector_push_RcPtr_VtSixelSurface(&slices, rc);

        y_pixel_offset += slice_height;
        remaining_y_pixels -= slice_height;

        if (!remaining_y_pixels) {
            break;
        }
    }

    return slices;
}
