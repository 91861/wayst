/* See LICENSE for license information. */

/**
 * Ui elements
 */

#pragma once

#include "settings.h"
#include "timing.h"
#include "util.h"
#include "vt.h"

struct Cursor;

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

    Scrollbar      scrollbar;
    struct Cursor* cursor;
    double         cursor_cell_fraction;
    double         cursor_cell_anim_start_point_cell_fraction;
    uint16_t       last_cursor_cell_position;
    uint16_t       last_cursor_row_position;

    hovered_link_t hovered_link;
    bool           draw_out_of_focus_tint;
    bool           window_in_focus;
    double         flash_fraction;

    bool draw_cursor_blinking;
    bool draw_text_blinking;

    VtLineProxy      cursor_proxy;
    vt_line_damage_t cursor_damage;
} Ui;

static bool Ui_any_overlay_element_visible(Ui* ui)
{
    return ui->scrollbar.visible || (ui->draw_out_of_focus_tint && settings.dim_tint.a != 0.0) ||
           ui->flash_fraction != 0.0;
}
