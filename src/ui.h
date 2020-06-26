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
    Scrollbar scrollbar;
    struct Cursor* cursor;
} Ui;
