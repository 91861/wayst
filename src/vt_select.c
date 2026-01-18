#define _GNU_SOURCE

#include "vt.h"
#include "vt_private.h"

/**
 * initialize selection region to cell by clicked pixel */
void Vt_select_init(Vt* self, enum SelectMode mode, int32_t x, int32_t y)
{
    self->selection.next_mode = mode;

    x              = CLAMP(x, 0, self->ws.ws_xpixel);
    y              = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_x = (double)x / self->pixels_per_cell_x;
    size_t click_y = (double)y / self->pixels_per_cell_y;

    self->selection.click_begin_char_idx = click_x;
    self->selection.click_begin_line     = Vt_visual_top_line(self) + click_y;
}

/**
 * initialize selection region to cell by cell screen coordinates */
void Vt_select_init_cell(Vt* self, enum SelectMode mode, int32_t x, int32_t y)
{
    self->selection.next_mode = mode;

    x = CLAMP(x, 0, Vt_col(self));
    y = CLAMP(y, 0, Vt_row(self));

    self->selection.click_begin_char_idx = x;
    self->selection.click_begin_line     = Vt_visual_top_line(self) + y;
}

/**
 * initialize selection region to clicked word */
void Vt_select_init_word(Vt* self, int32_t x, int32_t y)
{
    self->selection.mode = SELECT_MODE_NORMAL;

    x = CLAMP(x, 0, self->ws.ws_xpixel);
    y = CLAMP(y, 0, self->ws.ws_ypixel);

    size_t click_x = (double)x / self->pixels_per_cell_x;
    size_t click_y = (double)y / self->pixels_per_cell_y;

    Vector_VtRune* ln = &self->lines.buf[Vt_visual_top_line(self) + click_y].data;

    size_t cmax  = ln->size;
    size_t begin = click_x;
    size_t end   = click_x;
    while (begin - 1 < cmax && begin > 0 && !isspace(ln->buf[begin - 1].rune.code)) {
        --begin;
    }
    while (end + 1 < cmax && end > 0 && !isspace(ln->buf[end + 1].rune.code)) {
        ++end;
    }
    self->selection.begin_char_idx = begin;
    self->selection.end_char_idx   = end;
    self->selection.begin_line = self->selection.end_line = Vt_visual_top_line(self) + click_y;
    Vt_mark_proxy_fully_damaged(self, self->selection.begin_line);
}

/**
 * initialize selection region to clicked line */
void Vt_select_init_line(Vt* self, int32_t y)
{
    self->selection.mode = SELECT_MODE_NORMAL;

    y = CLAMP(y, 0, self->ws.ws_ypixel);

    size_t click_y = (double)y / self->pixels_per_cell_y;

    self->selection.begin_char_idx = 0;
    self->selection.end_char_idx   = Vt_col(self);
    self->selection.begin_line = self->selection.end_line = Vt_visual_top_line(self) + click_y;
    Vt_mark_proxy_fully_damaged(self, self->selection.begin_line);
}

void Vt_select_commit(Vt* self)
{
    if (self->selection.next_mode != SELECT_MODE_NONE) {
        self->selection.mode           = self->selection.next_mode;
        self->selection.next_mode      = SELECT_MODE_NONE;
        self->selection.begin_line     = self->selection.click_begin_line;
        self->selection.end_line       = self->selection.click_begin_line;
        self->selection.begin_char_idx = self->selection.click_begin_char_idx;
        self->selection.end_char_idx   = self->selection.click_begin_char_idx;
        Vt_mark_proxies_damaged_in_selected_region(self);
    }
}

void Vt_select_set_end(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        x = CLAMP(x, 0, self->ws.ws_xpixel);
        y = CLAMP(y, 0, self->ws.ws_ypixel);

        size_t click_x = (double)x / self->pixels_per_cell_x;
        size_t click_y = (double)y / self->pixels_per_cell_y;
        Vt_select_set_end_cell(self, click_x, click_y);
    }
}

void Vt_select_set_end_cell(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        size_t old_end = self->selection.end_line;

        x = CLAMP(x, 0, Vt_col(self));
        y = CLAMP(y, 0, Vt_row(self));

        self->selection.end_line     = Vt_visual_top_line(self) + y;
        self->selection.end_char_idx = x;

        size_t lo = MIN(MIN(old_end, self->selection.end_line), self->selection.begin_line);
        size_t hi = MAX(MAX(old_end, self->selection.end_line), self->selection.begin_line);
        Vt_mark_proxies_damaged_in_region(self, hi, lo);
    }
}

void Vt_select_set_front(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        x = CLAMP(x, 0, self->ws.ws_xpixel);
        y = CLAMP(y, 0, self->ws.ws_ypixel);

        size_t click_x = (double)x / self->pixels_per_cell_x;
        size_t click_y = (double)y / self->pixels_per_cell_y;
        Vt_select_set_front_cell(self, click_x, click_y);
    }
}

void Vt_select_set_front_cell(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        size_t old_front = self->selection.begin_line;

        x = CLAMP(x, 0, Vt_col(self));
        y = CLAMP(y, 0, Vt_row(self));

        self->selection.begin_line     = Vt_visual_top_line(self) + y;
        self->selection.begin_char_idx = x;

        size_t lo = MIN(MIN(old_front, self->selection.end_line), self->selection.begin_line);
        size_t hi = MAX(MAX(old_front, self->selection.end_line), self->selection.begin_line);
        Vt_mark_proxies_damaged_in_region(self, hi, lo);
        self->defered_events.repaint = true;
    }
}

void Vt_select_clamp_to_buffer(Vt* self)
{
    self->selection.begin_line = MIN(self->selection.begin_line, self->lines.size - 1);
    self->selection.end_line   = MIN(self->selection.end_line, self->lines.size - 1);
}

void Vt_select_end(Vt* self)
{
    if (self->selection.mode) {
        Vt_mark_proxies_damaged_in_selected_region(self);
    }
    self->selection.mode = SELECT_MODE_NONE;
    CALL(self->callbacks.on_select_end, self->callbacks.user_data);
    self->defered_events.repaint = true;
}

/**
 * Get string from selected region */
Vector_char Vt_select_region_to_string(Vt* self)
{
    Vector_char ret, tmp;
    size_t      begin_char_idx, end_char_idx;
    size_t      begin_line = MIN(self->selection.begin_line, self->selection.end_line);
    size_t      end_line   = MAX(self->selection.begin_line, self->selection.end_line);

    if (begin_line == end_line && self->selection.mode != SELECT_MODE_NONE) {
        begin_char_idx = MIN(self->selection.begin_char_idx, self->selection.end_char_idx);
        end_char_idx   = MAX(self->selection.begin_char_idx, self->selection.end_char_idx);
        return Vt_line_to_string(self, begin_line, begin_char_idx, end_char_idx + 1, "");
    } else if (self->selection.begin_line < self->selection.end_line) {
        begin_char_idx = self->selection.begin_char_idx;
        end_char_idx   = self->selection.end_char_idx;
    } else {
        begin_char_idx = self->selection.end_char_idx;
        end_char_idx   = self->selection.begin_char_idx;
    }

    if (self->selection.mode == SELECT_MODE_NORMAL) {
        char* term = self->lines.buf[begin_line + 1].rejoinable ? "" : "\n";
        ret        = Vt_line_to_string(self, begin_line, begin_char_idx, Vt_col(self), term);
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i < end_line; ++i) {
            char* term_mid = self->lines.buf[i + 1].rejoinable ? "" : "\n";
            tmp            = Vt_line_to_string(self, i, 0, Vt_col(self), term_mid);
            Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
            Vector_destroy_char(&tmp);
        }
        tmp = Vt_line_to_string(self, end_line, 0, end_char_idx + 1, "");
        Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
        Vector_destroy_char(&tmp);
    } else if (self->selection.mode == SELECT_MODE_BOX) {
        ret = Vt_line_to_string(self, begin_line, begin_char_idx, end_char_idx + 1, "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i <= end_line; ++i) {
            char* term = i == end_line ? "" : "\n";
            tmp        = Vt_line_to_string(self, i, begin_char_idx, end_char_idx + 1, term);
            Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
            Vector_destroy_char(&tmp);
        }
    } else {
        ret = Vector_new_char();
    }
    Vector_push_char(&ret, '\0');
    return ret;
}
