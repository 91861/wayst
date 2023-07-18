/* See LICENSE for license information. */

/**
 * Ui elements
 */

#pragma once

#include "util.h"
#include "vector.h"
#define _GNU_SOURCE

#include "settings.h"
#include "vt.h"

#define UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX 10
#define UI_CSD_TITLEBAR_HEIGHT_PX             37
#define UI_CSD_TITLEBAR_RADIUS_PX             10
#define UI_CSD_TITLEBAR_BUTTON_RADIUS_PX      12
#define UI_CSD_TITLEBAR_BUTTON_MARGIN_PX      8

typedef enum /* current window state */
{
    UI_CSD_MODE_NONE = 0,
    UI_CSD_MODE_FLOATING,
    UI_CSD_MODE_TILED,
} ui_csd_mode_e;

typedef enum /* set in user prefs */
{
    UI_CSD_STYLE_FULL = 0,
    UI_CSD_STYLE_MINIMAL, /* try mimicking minimal decorations like mwm hints */
} ui_csd_style_e;

typedef enum
{
    UI_CSD_TITLEBAR_BUTTON_CLOSE,
    UI_CSD_TITLEBAR_BUTTON_MAXIMIZE,
    UI_CSD_TITLEBAR_BUTTON_MINIMIZE,
    UI_CSD_TITLEBAR_BUTTON_SHADE,
    UI_CSD_TITLEBAR_BUTTON_STICKY,
} ui_csd_titlebar_button_type_e;

typedef struct
{
    ui_csd_titlebar_button_type_e type;
    float                         highlight_fraction;
    Pair_uint32_t                 position;
} ui_csd_titlebar_button_info_t;

DEF_VECTOR(ui_csd_titlebar_button_info_t, NULL)

struct VtCursor;

typedef struct
{
    bool    visible, dragging;
    uint8_t width;
    float   top, length, opacity;
} Scrollbar;

typedef struct
{
    bool     active;
    size_t   start_line_idx, end_line_idx;
    uint16_t start_cell_idx, end_cell_idx;
} hovered_link_t;

typedef struct
{
    uint8_t pixel_offset_x, pixel_offset_y;

    Scrollbar        scrollbar;
    struct VtCursor* cursor;
    double           cursor_cell_fraction;
    double           cursor_cell_anim_start_point_cell_fraction;
    uint16_t         last_cursor_cell_position;
    uint16_t         last_cursor_row_position;

    hovered_link_t hovered_link;
    bool           draw_out_of_focus_tint;
    bool           window_in_focus;
    double         flash_fraction;
    double         cursor_fade_fraction;

    bool draw_cursor_blinking;
    bool draw_text_blinking;

    VtLineProxy      cursor_proxy;
    vt_line_damage_t cursor_damage;

    /* client-size window decoratons on wayland */
    struct ui_csd_data_t
    {
        uint16_t                             titlebar_height_px;
        ui_csd_mode_e                        mode;
        ui_csd_style_e                       style;
        bool                                 damage;
        bool                                 requires_attention;
        VtLine*                              titlebar_caption;
        Vector_ui_csd_titlebar_button_info_t buttons;
    } csd;
} Ui;

static ui_csd_titlebar_button_info_t* Ui_csd_get_hovered_button(Ui* self, uint32_t x, uint32_t y)
{
    for (ui_csd_titlebar_button_info_t* i = NULL;
         (i = Vector_iter_ui_csd_titlebar_button_info_t(&self->csd.buttons, i));) {

        int32_t xdiff = (int32_t)i->position.first - x;
        int32_t ydiff = (int32_t)i->position.second - y;
        int32_t mag   = xdiff * xdiff + ydiff * ydiff;

        if (mag <= POW2(UI_CSD_TITLEBAR_BUTTON_RADIUS_PX)) {
            return i;
        }
    }

    return NULL;
}

static void Ui_CSD_unhover_all_buttons(Ui* self)
{
    for (ui_csd_titlebar_button_info_t* i = NULL;
         (i = Vector_iter_ui_csd_titlebar_button_info_t(&self->csd.buttons, i));) {
        i->highlight_fraction = 0.0f;
    }
}

static void Ui_update_CSD_button_layout(Ui* self, Pair_uint32_t window_size_with_frame)
{
    uint32_t xoffset_px = window_size_with_frame.first - 17;
    uint32_t yoffset_px = UI_CSD_TITLEBAR_HEIGHT_PX / 2;

    for (ui_csd_titlebar_button_info_t* i = NULL;
         (i = Vector_iter_ui_csd_titlebar_button_info_t(&self->csd.buttons, i));) {
        i->position.first  = xoffset_px;
        i->position.second = yoffset_px;
        xoffset_px -= 37;
    }
}

static bool Ui_csd_titlebar_visible(Ui* self)
{
    return self->csd.style == UI_CSD_STYLE_FULL &&
           (self->csd.mode == UI_CSD_MODE_FLOATING || self->csd.mode == UI_CSD_MODE_TILED);
}

static void Ui_destroy(Ui* self)
{
    Vector_destroy_ui_csd_titlebar_button_info_t(&self->csd.buttons);
}

static bool Ui_any_overlay_element_visible(Ui* ui)
{
    return ui->scrollbar.visible || (ui->draw_out_of_focus_tint && settings.dim_tint.a != 0.0) ||
           ui->flash_fraction != 0.0 || ui->hovered_link.active;
}
