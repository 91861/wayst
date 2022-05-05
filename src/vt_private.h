/* See LICENSE for license information. */

#pragma once

#include "vt.h"

static void                        Vt_insert_new_line(Vt* self);
static inline void                 Vt_move_cursor(Vt* self, uint16_t column, uint16_t rows);
__attribute__((hot, flatten)) void Vt_handle_literal(Vt* self, char c);
__attribute__((cold)) char*        pty_string_prettyfy(const char* str, int32_t max);

void Vt_output(Vt* self, const char* buf, size_t len);

#define Vt_output_formated(vt, fmt, ...)                                                           \
    {                                                                                              \
        char _tmp[256];                                                                            \
        int  _len = snprintf(_tmp, sizeof(_tmp), fmt, __VA_ARGS__);                                \
        Vt_output((vt), _tmp, _len);                                                               \
    }

static inline void Vt_mark_line_proxy_fully_damaged(Vt* self, VtLine* line)
{
    self->defered_events.action_performed = true;
    self->defered_events.repaint          = true;
    line->damage.type                     = VT_LINE_DAMAGE_FULL;
}

static inline void Vt_mark_proxy_fully_damaged(Vt* self, size_t idx)
{
    Vt_mark_line_proxy_fully_damaged(self, &self->lines.buf[idx]);
}

static inline void Vt_mark_proxy_damaged_cell(Vt* self, size_t line, size_t rune)
{
    self->defered_events.action_performed = true;
    switch (self->lines.buf[line].damage.type) {
        case VT_LINE_DAMAGE_NONE:
            self->lines.buf[line].damage.type  = VT_LINE_DAMAGE_RANGE;
            self->lines.buf[line].damage.front = rune;
            self->lines.buf[line].damage.end   = rune;
            break;

        case VT_LINE_DAMAGE_RANGE: {
            size_t lo = MIN(self->lines.buf[line].damage.front, rune);
            size_t hi = MAX(self->lines.buf[line].damage.end, rune);

            self->lines.buf[line].damage.front = lo;
            self->lines.buf[line].damage.end   = hi;
        } break;

        case VT_LINE_DAMAGE_SHIFT: {

        } break;

        default:
            return;
    }
}

static inline void Vt_mark_proxies_damaged_in_region(Vt* self, size_t begin, size_t end)
{
    size_t lo = MIN(begin, end);
    size_t hi = MAX(begin, end);
    for (size_t i = lo; i <= hi; ++i) {
        Vt_mark_proxy_fully_damaged(self, i);
    }
}

static inline void Vt_clear_line_proxy(Vt* self, VtLine* line)
{
    Vt_mark_line_proxy_fully_damaged(self, line);
    CALL(self->callbacks.destroy_proxy, self->callbacks.user_data, &line->proxy);
}

static inline void Vt_clear_proxy(Vt* self, size_t idx)
{
    Vt_clear_line_proxy(self, &self->lines.buf[idx]);
}

static inline void Vt_clear_proxies_in_region(Vt* self, size_t begin, size_t end)
{
    size_t lo = MIN(begin, end);
    size_t hi = MAX(begin, end);
    for (size_t i = lo; i <= hi; ++i) {
        Vt_clear_proxy(self, i);
    }
}

static inline void Vt_mark_proxies_damaged_in_selected_region(Vt* self)
{
    Vt_mark_proxies_damaged_in_region(self, self->selection.begin_line, self->selection.end_line);
}

static inline void Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(Vt* self)
{
    if (self->selection.mode) {
        size_t selection_lo = MIN(self->selection.begin_line, self->selection.end_line);
        size_t selection_hi = MAX(self->selection.begin_line, self->selection.end_line);

        size_t start = MAX(selection_lo, self->scroll_region_top);
        size_t end   = MIN(MIN(selection_hi, self->scroll_region_bottom - 1), self->lines.size - 1);
        Vt_mark_proxies_damaged_in_region(self, start ? (start - 1) : 0, end + 1);
    }
}
