/* See LICENSE for license information. */

/**
 * Ui elements
 */

#pragma once

#include "timing.h"
#include "util.h"

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
    uint8_t        pixel_offset_x, pixel_offset_y;
    Scrollbar      scrollbar;
    struct Cursor* cursor;
    hovered_link_t hovered_link;
} Ui;
