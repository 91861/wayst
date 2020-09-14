/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "settings.h"
#include "util.h"

#include "vt.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <uchar.h>

#ifndef NOUTF8PROC
#include <utf8proc.h>
#else
#include "wcwidth/wcwidth.h"
#endif

#include "key.h"

static inline size_t Vt_top_line(const Vt* const self);
void                 Vt_visual_scroll_to(Vt* self, size_t line);
void                 Vt_visual_scroll_reset(Vt* self);
static inline size_t Vt_get_scroll_region_top(Vt* self);
static inline size_t Vt_get_scroll_region_bottom(Vt* self);
static inline bool   Vt_scroll_region_not_default(Vt* self);
static void          Vt_alt_buffer_on(Vt* self, bool save_mouse);
static void          Vt_alt_buffer_off(Vt* self, bool save_mouse);
static void          Vt_handle_multi_argument_SGR(Vt* self, Vector_char seq, VtRune* opt_target);
static void          Vt_reset_text_attribs(Vt* self, VtRune* opt_target);
static void          Vt_carriage_return(Vt* self);
static void          Vt_clear_right(Vt* self);
static void          Vt_clear_left(Vt* self);
static inline void   Vt_scroll_out_all_content(Vt* self);
static void          Vt_empty_line_fill_bg(Vt* self, size_t idx);
static void          Vt_insert_new_line(Vt* self);
static void          Vt_scroll_up(Vt* self);
static void          Vt_scroll_down(Vt* self);
static void          Vt_reverse_line_feed(Vt* self);
static void          Vt_delete_line(Vt* self);
static void          Vt_delete_chars(Vt* self, size_t n);
static void          Vt_erase_chars(Vt* self, size_t n);
static void          Vt_clear_above(Vt* self);
static inline void   Vt_scroll_out_above(Vt* self);
static void          Vt_insert_line(Vt* self);
static void          Vt_clear_display_and_scrollback(Vt* self);
static void          Vt_erase_to_end(Vt* self);
static void          Vt_move_cursor(Vt* self, uint16_t c, uint16_t r);
static void          Vt_set_title(Vt* self, const char* title);
static void          Vt_push_title(Vt* self);
static void          Vt_pop_title(Vt* self);
static void          Vt_insert_char_at_cursor(Vt* self, VtRune c);
static void          Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c);
static bool          Vt_alt_buffer_enabled(Vt* self);
static Vector_char   rune_vec_to_string(Vector_VtRune* line,
                                        size_t         begin,
                                        size_t         end,
                                        const char*    tail);
static Vector_char   VtLine_to_string(VtLine* line, size_t begin, size_t end, const char* tail);
static Vector_char   Vt_line_to_string(Vt*         self,
                                       size_t      idx,
                                       size_t      begin,
                                       size_t      end,
                                       const char* tail);
static inline void   Vt_mark_proxy_fully_damaged(Vt* self, size_t idx);
static void          Vt_mark_proxy_damaged_cell(Vt* self, size_t line, size_t rune);
static void          Vt_init_tab_ruler(Vt* self);
static void          Vt_reset_tab_ruler(Vt* self);

static inline VtLine VtLine_new()
{
    VtLine line;
    memset(&line, 0, sizeof(VtLine));

    line.damage.type = VT_LINE_DAMAGE_FULL;
    line.reflowable  = true;
    line.data        = Vector_new_VtRune();

    return line;
}

static void Vt_output(Vt* self, const char* buf, size_t len)
{
    Vector_pushv_char(&self->output, buf, len);
}

#define Vt_output_formated(vt, fmt, ...)                                                           \
    char _tmp[64];                                                                                 \
    int  _len = snprintf(_tmp, sizeof(_tmp), fmt, __VA_ARGS__);                                    \
    Vt_output((vt), _tmp, _len);

static void Vt_bell(Vt* self)
{
    if (!settings.no_flash) {
        CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
    }
    if (self->modes.pop_on_bell) {
        // TODO: CALL_FP(self->callbacks.on_raise, self->callbacks.user_data);
    }
    if (self->modes.urgency_on_bell) {
        // TODO: CALL_FP(self->callbacks.on_set_urgent, self->callbacks.user_data);
    }
}

static void Vt_grapheme_break(Vt* self)
{
#ifndef NOUTF8PROC
    self->utf8proc_state = 0;
#endif
    self->last_codepoint = 0;
    self->last_interted  = NULL;
}

static void Vt_set_fg_color_custom(Vt* self, ColorRGB color, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->fg_is_palette_entry = false;
    r->fg_data.rgb         = color;
}

static void Vt_set_fg_color_palette(Vt* self, int16_t index, VtRune* opt_target)
{
    ASSERT(index >= 0 && index <= 256, "in palette range");
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->fg_is_palette_entry = true;
    r->fg_data.index       = index;
}

static void Vt_set_fg_color_default(Vt* self, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->fg_is_palette_entry = true;
    r->fg_data.index       = VT_RUNE_PALETTE_INDEX_TERM_DEFAULT;
}

static void Vt_set_bg_color_custom(Vt* self, ColorRGBA color, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->bg_is_palette_entry = false;
    r->bg_data.rgba        = color;
}

static void Vt_set_bg_color_palette(Vt* self, int16_t index, VtRune* opt_target)
{
    ASSERT(index >= 0 && index <= 256, "in palette range");
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->bg_is_palette_entry = true;
    r->bg_data.index       = index;
}

static void Vt_set_bg_color_default(Vt* self, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->bg_is_palette_entry = true;
    r->bg_data.index       = VT_RUNE_PALETTE_INDEX_TERM_DEFAULT;
}

static void Vt_set_line_color_custom(Vt* self, ColorRGB color, VtRune* opt_target)
{
    VtRune* r                  = OR(opt_target, &self->parser.char_state);
    r->line_color_not_default  = true;
    r->ln_clr_is_palette_entry = false;
    r->ln_clr_data.rgb         = color;
}

static void Vt_set_line_color_palette(Vt* self, int16_t index, VtRune* opt_target)
{
    ASSERT(index >= 0 && index <= 256, "in palette range");
    VtRune* r                  = OR(opt_target, &self->parser.char_state);
    r->ln_clr_is_palette_entry = true;
    r->ln_clr_data.index       = index;
}

static void Vt_set_line_color_default(Vt* self, VtRune* opt_target)
{
    VtRune* r                 = OR(opt_target, &self->parser.char_state);
    r->line_color_not_default = false;
}

static ColorRGB Vt_active_fg_color(const Vt* self)
{
    return Vt_rune_fg(self, &self->parser.char_state);
}

static ColorRGBA Vt_active_bg_color(const Vt* self)
{
    return Vt_rune_bg(self, &self->parser.char_state);
}

static ColorRGB Vt_active_line_color(const Vt* self)
{
    return Vt_rune_ln_clr(self, &self->parser.char_state);
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
        ret        = Vt_line_to_string(self, begin_line, begin_char_idx, 0, term);
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i < end_line; ++i) {
            char* term_mid = self->lines.buf[i + 1].rejoinable ? "" : "\n";
            tmp            = Vt_line_to_string(self, i, 0, 0, term_mid);
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

static void Vt_mark_line_proxy_fully_damaged(Vt* self, VtLine* line)
{
    CALL_FP(self->callbacks.on_action_performed, self->callbacks.user_data);
    line->damage.type = VT_LINE_DAMAGE_FULL;
}

static void Vt_mark_proxy_fully_damaged(Vt* self, size_t idx)
{
    Vt_mark_line_proxy_fully_damaged(self, &self->lines.buf[idx]);
}

static void Vt_mark_proxy_damaged_cell(Vt* self, size_t line, size_t rune)
{
    CALL_FP(self->callbacks.on_action_performed, self->callbacks.user_data);
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
    CALL_FP(self->callbacks.destroy_proxy, self->callbacks.user_data, &line->proxy);
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

void Vt_clear_all_proxies(Vt* self)
{
    Vt_clear_proxies_in_region(self, 0, self->lines.size - 1);
    if (Vt_alt_buffer_enabled(self)) {
        for (size_t i = 0; i < self->alt_lines.size - 1; ++i) {
            Vt_clear_line_proxy(self, &self->alt_lines.buf[i]);
        }
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
        CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
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
        CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
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
        CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
    }
}

void Vt_select_end(Vt* self)
{
    self->selection.mode = SELECT_MODE_NONE;
    Vt_mark_proxies_damaged_in_selected_region(self);
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

static inline char32_t char_sub_uk(char original)
{
    return original == '#' ? 0xa3 /* £ */ : original;
}

static inline char32_t char_sub_gfx(char original)
{
    static const char32_t substitutes[] = {
        0x2592, // ▒
        0x2409, // ␉
        0x240c, // ␌
        0x240d, // ␍
        0x240a, // ␊
        0x00b0, // °
        0x00b1, // ±
        0x2424, // ␤
        0x240b, // ␋
        0x2518, // ┘
        0x2510, // ┐
        0x250c, // ┌
        0x2514, // └
        0x253c, // ┼
        0x23ba, // ⎺
        0x23bb, // ⎻
        0x2500, // ─
        0x23BC, // ⎼
        0x23BD, // ⎽
        0x251C, // ├
        0x2524, // ┤
        0x2534, // ┴
        0x252C, // ┬
        0x2502, // │
        0x2264, // ≤
        0x2265, // ≥
        0x03C0, // π
        0x00A3, // £
        0x2260, // ≠
        0x22C5, // ⋅
        0x2666, // ♦
    };

    if (unlikely(original >= 'a' && original <= '~')) {
        return substitutes[original - 'a'];
    } else {
        return original;
    }
}

/**
 * substitute invisible characters with a readable string */
__attribute__((cold)) static const char* control_char_get_pretty_string(const char c)
{
    static const char* const strings[] = {
        TERMCOLOR_BG_BLACK TERMCOLOR_RED "␀" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_YELLOW "␁" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_YELLOW_LIGHT "␂" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␃" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␄" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␅" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␆" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_YELLOW "␇" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_RED "␈" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_BLUE "␉" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␊" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_BLUE_LIGHT "␋" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_RED_LIGHT "␌" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␍" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␎" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA_LIGHT "␏" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␐" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA_LIGHT "␑" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␒" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␓" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␔" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␕" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␖" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN "␗" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_BLUE_LIGHT "␘" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN "␙" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN_LIGHT "␚" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN_LIGHT "␛" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_RED_LIGHT "␜" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␝" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␞" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␟" TERMCOLOR_DEFAULT,
    };

    if ((uint32_t)c < ARRAY_SIZE(strings)) {
        return strings[(int)c];
    } else if (c == 127) {
        return TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA_LIGHT "␡" TERMCOLOR_DEFAULT;
    } else {
        return NULL;
    }
}

/**
 * make pty messages more readable */
__attribute__((cold)) char* pty_string_prettyfy(const char* str, int32_t max)
{
    bool esc = false, seq = false, important = false;

    Vector_char fmt = Vector_new_char();
    for (const char* s = str; *s && ((s - str) < max); ++s) {
        if (seq) {
            if (!isdigit(*s) && *s != '?' && *s != ';' && *s != ':') {
                Vector_pushv_char(&fmt, TERMCOLOR_BG_DEFAULT, strlen(TERMCOLOR_BG_DEFAULT));
                seq       = false;
                important = true;
            }
        } else {
            if (*s == '\e') {
                esc = true;
                Vector_pushv_char(&fmt, TERMCOLOR_BG_GRAY_DARK, strlen(TERMCOLOR_BG_GRAY_DARK));
            } else if (*s == '[' && esc) {
                seq = true;
                esc = false;
            }
        }

        const char* ctr = control_char_get_pretty_string(*s);
        if (ctr) {
            Vector_pushv_char(&fmt, ctr, strlen(ctr));
        } else {
            if (important) {
                switch (*s) {
                    case 'H':
                    case 'G':
                    case 'f':
                    case '`':
                    case 'd':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_GREEN, strlen(TERMCOLOR_BG_GREEN));
                        break;

                    case 'm':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_BLUE, strlen(TERMCOLOR_BG_BLUE));
                        break;

                    case 'B':
                    case 'C':
                    case 'e':
                    case 'a':
                    case 'D':
                    case 'E':
                    case 'F':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_CYAN, strlen(TERMCOLOR_BG_CYAN));
                        break;

                    case 'M':
                    case 'T':
                    case 'X':
                    case 'S':
                    case '@':
                    case 'L':
                    case 'P':
                        Vector_pushv_char(&fmt,
                                          TERMCOLOR_BG_MAGENTA_LIGHT,
                                          strlen(TERMCOLOR_BG_MAGENTA_LIGHT));
                        break;

                    case 'I':
                    case 'Z':
                    case 'g':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_MAGENTA, strlen(TERMCOLOR_BG_MAGENTA));
                        break;

                    default:
                        Vector_pushv_char(&fmt,
                                          TERMCOLOR_BG_RED_LIGHT,
                                          strlen(TERMCOLOR_BG_RED_LIGHT));
                }
                Vector_push_char(&fmt, *s);
                Vector_pushv_char(&fmt, TERMCOLOR_RESET, strlen(TERMCOLOR_RESET));

            } else if (*s == ';' && seq) {
                Vector_pushv_char(&fmt, TERMCOLOR_RED_LIGHT, strlen(TERMCOLOR_RED_LIGHT));
                Vector_push_char(&fmt, *s);
                Vector_pushv_char(&fmt, TERMCOLOR_DEFAULT, strlen(TERMCOLOR_DEFAULT));
            } else if (isdigit(*s) && seq) {
                Vector_pushv_char(&fmt,
                                  TERMCOLOR_BG_WHITE TERMCOLOR_BLACK,
                                  strlen(TERMCOLOR_BG_WHITE TERMCOLOR_BLACK));
                Vector_push_char(&fmt, *s);
                Vector_pushv_char(&fmt,
                                  TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT,
                                  strlen(TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT));
            } else {
                Vector_push_char(&fmt, *s);
            }
        }
        important = false;
    }
    Vector_pushv_char(&fmt, TERMCOLOR_BG_DEFAULT, strlen(TERMCOLOR_BG_DEFAULT));
    Vector_push_char(&fmt, '\0');
    return fmt.buf;
}

/**
 * get utf-8 text from @param line in range from @param begin to @param end */
static Vector_char rune_vec_to_string(Vector_VtRune* line,
                                      size_t         begin,
                                      size_t         end,
                                      const char*    tail)
{
    Vector_char res;
    end   = MIN((end ? end : line->size), line->size);
    begin = MIN(begin, line->size - 1);

    if (begin >= end) {
        res = Vector_new_with_capacity_char(2);
        if (tail) {
            Vector_pushv_char(&res, tail, strlen(tail) + 1);
        }
        return res;
    }
    res = Vector_new_with_capacity_char(end - begin);
    char             utfbuf[4];
    static mbstate_t mbstate;

    for (uint32_t i = begin; i < end; ++i) {
        Rune* rune = &line->buf[i].rune;

        if (rune->code == VT_RUNE_CODE_WIDE_TAIL) {
            continue;
        }

        if (rune->code > CHAR_MAX) {
            size_t bytes = c32rtomb(utfbuf, rune->code, &mbstate);
            if (bytes > 0) {
                Vector_pushv_char(&res, utfbuf, bytes);
            }
        } else if (!rune->code) {
            Vector_push_char(&res, ' ');
        } else {
            Vector_push_char(&res, rune->code);
        }

        for (int j = 0; j < VT_RUNE_MAX_COMBINE && rune->combine[j]; ++j) {
            size_t bytes = c32rtomb(utfbuf, rune->combine[j], &mbstate);
            if (bytes > 0) {
                Vector_pushv_char(&res, utfbuf, bytes);
            }
        }
    }
    if (tail) {
        Vector_pushv_char(&res, tail, strlen(tail) + 1);
    }

    return res;
}

static Vector_char VtLine_to_string(VtLine* line, size_t begin, size_t end, const char* tail)
{
    return rune_vec_to_string(&line->data, begin, end, tail);
}

static Vector_char Vt_line_to_string(Vt*         self,
                                     size_t      idx,
                                     size_t      begin,
                                     size_t      end,
                                     const char* tail)
{
    return VtLine_to_string(&self->lines.buf[idx], begin, end, tail);
}

/**
 * Split string on any character in @param delimiters, filter out any character in @param filter.
 * first character of returned string is the immediately preceding delimiter, '\0' if none. Multiple
 * @param greedy_delimiters are treated as a single delimiter */
static Vector_Vector_char string_split_on(const char* str,
                                          const char* delimiters,
                                          const char* greedy_delimiters,
                                          const char* filter)
{
    Vector_Vector_char ret = Vector_new_with_capacity_Vector_char(8);
    Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
    Vector_push_char(&ret.buf[0], '\0');

    for (; *str; ++str) {
        /* skip filtered */
        for (const char* i = filter; filter && *i; ++i)
            if (*str == *i)
                goto continue_outer;

        bool is_non_greedy = false;
        char any_symbol    = 0;
        if (delimiters) {
            for (const char* i = delimiters; *i; ++i) {
                if (*i == *str) {
                    is_non_greedy = true;
                    any_symbol    = *i;
                    break;
                }
            }
        }
        if (greedy_delimiters) {
            for (const char* i = greedy_delimiters; *i; ++i) {
                if (*i == *str) {
                    any_symbol = *i;
                    break;
                }
            }
        }

        if (any_symbol) {
            Vector_push_char(&ret.buf[ret.size - 1], '\0');

            if (ret.buf[ret.size - 1].size == 2 && !is_non_greedy) {
                ret.buf[ret.size - 1].size = 0;
            } else {
                Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
            }
            Vector_push_char(&ret.buf[ret.size - 1], any_symbol);
        } else {
            /* record delimiter */
            Vector_push_char(&ret.buf[ret.size - 1], *str);
        }
    continue_outer:;
    }
    Vector_push_char(&ret.buf[ret.size - 1], '\0');

    return ret;
}

static inline bool is_csi_sequence_terminated(const char* seq, const size_t size)
{
    if (!size) {
        return false;
    }

    return isalpha(seq[size - 1]) || seq[size - 1] == '@' || seq[size - 1] == '{' ||
           seq[size - 1] == '}' || seq[size - 1] == '~' || seq[size - 1] == '|';
}

static inline bool is_string_sequence_terminated(const char* seq, const size_t size)
{
    if (!size) {
        return false;
    }

    return seq[size - 1] == '\a' || (size > 1 && seq[size - 2] == '\e' && seq[size - 1] == '\\');
}

static void generate_color_palette_entry(ColorRGB* color, int16_t idx)
{
    ASSERT(idx >= 0, "index not negative");
    ASSERT(color, "got color*");

    if (idx < 16) {
        /* Primary - from colorscheme */
        *color = settings.colorscheme.color[idx];
    } else if (idx < 232) {
        /* Extended */
        int16_t tmp = idx - 16;
        color->b    = (double)((tmp % 6) * 255) / 5.0;
        color->g    = (double)(((tmp /= 6) % 6) * 255) / 5.0;
        color->r    = (double)(((tmp / 6) % 6) * 255) / 5.0;
    } else {
        /* Grayscale */
        double tmp = (double)((idx - 232) * 10 + 8) / 256.0 * 255.0;

        *color = (ColorRGB){
            .r = tmp,
            .g = tmp,
            .b = tmp,
        };
    }
}

static void Vt_reset_color_palette_entry(Vt* self, int16_t idx)
{
    generate_color_palette_entry(&self->colors.palette_256[idx], idx);
}

static void Vt_init_color_palette(Vt* self)
{
    for (int16_t i = 0; i < 256; ++i) {
        Vt_reset_color_palette_entry(self, i);
    }
}

static const char* const color_palette_names[] = {
    "Grey0",
    "NavyBlue",
    "DarkBlue",
    "Blue3",
    "Blue3",
    "Blue1",
    "DarkGreen",
    "DeepSkyBlue4",
    "DeepSkyBlue4",
    "DeepSkyBlue4",
    "DodgerBlue3",
    "DodgerBlue2",
    "Green4",
    "SpringGreen4",
    "Turquoise4",
    "DeepSkyBlue3",
    "DeepSkyBlue3",
    "DodgerBlue1",
    "Green3",
    "SpringGreen3",
    "DarkCyan",
    "LightSeaGreen",
    "DeepSkyBlue2",
    "DeepSkyBlue1",
    "Green3",
    "SpringGreen3",
    "SpringGreen2",
    "Cyan3",
    "DarkTurquoise",
    "Turquoise2",
    "Green1",
    "SpringGreen2",
    "SpringGreen1",
    "MediumSpringGreen",
    "Cyan2",
    "Cyan1",
    "DarkRed",
    "DeepPink4",
    "Purple4",
    "Purple4",
    "Purple3",
    "BlueViolet",
    "Orange4",
    "Grey37",
    "MediumPurple4",
    "SlateBlue3",
    "SlateBlue3",
    "RoyalBlue1",
    "Chartreuse4",
    "DarkSeaGreen4",
    "PaleTurquoise4",
    "SteelBlue",
    "SteelBlue3",
    "CornflowerBlue",
    "Chartreuse3",
    "DarkSeaGreen4",
    "CadetBlue",
    "CadetBlue",
    "SkyBlue3",
    "SteelBlue1",
    "Chartreuse3",
    "PaleGreen3",
    "SeaGreen3",
    "Aquamarine3",
    "MediumTurquoise",
    "SteelBlue1",
    "Chartreuse2",
    "SeaGreen2",
    "SeaGreen1",
    "SeaGreen1",
    "Aquamarine1",
    "DarkSlateGray2",
    "DarkRed",
    "DeepPink4",
    "DarkMagenta",
    "DarkMagenta",
    "DarkViolet",
    "Purple",
    "Orange4",
    "LightPink4",
    "Plum4",
    "MediumPurple3",
    "MediumPurple3",
    "SlateBlue1",
    "Yellow4",
    "Wheat4",
    "Grey53",
    "LightSlateGrey",
    "MediumPurple",
    "LightSlateBlue",
    "Yellow4",
    "DarkOliveGreen3",
    "DarkSeaGreen",
    "LightSkyBlue3",
    "LightSkyBlue3",
    "SkyBlue2",
    "Chartreuse2",
    "DarkOliveGreen3",
    "PaleGreen3",
    "DarkSeaGreen3",
    "DarkSlateGray3",
    "SkyBlue1",
    "Chartreuse1",
    "LightGreen",
    "LightGreen",
    "PaleGreen1",
    "Aquamarine1",
    "DarkSlateGray1",
    "Red3",
    "DeepPink4",
    "MediumVioletRed",
    "Magenta3",
    "DarkViolet",
    "Purple",
    "DarkOrange3",
    "IndianRed",
    "HotPink3",
    "MediumOrchid3",
    "MediumOrchid",
    "MediumPurple2",
    "DarkGoldenrod",
    "LightSalmon3",
    "RosyBrown",
    "Grey63",
    "MediumPurple2",
    "MediumPurple1",
    "Gold3",
    "DarkKhaki",
    "NavajoWhite3",
    "Grey69",
    "LightSteelBlue3",
    "LightSteelBlue",
    "Yellow3",
    "DarkOliveGreen3",
    "DarkSeaGreen3",
    "DarkSeaGreen2",
    "LightCyan3",
    "LightSkyBlue1",
    "GreenYellow",
    "DarkOliveGreen2",
    "PaleGreen1",
    "DarkSeaGreen2",
    "DarkSeaGreen1",
    "PaleTurquoise1",
    "Red3",
    "DeepPink3",
    "DeepPink3",
    "Magenta3",
    "Magenta3",
    "Magenta2",
    "DarkOrange3",
    "IndianRed",
    "HotPink3",
    "HotPink2",
    "Orchid",
    "MediumOrchid1",
    "Orange3",
    "LightSalmon3",
    "LightPink3",
    "Pink3",
    "Plum3",
    "Violet",
    "Gold3",
    "LightGoldenrod3",
    "Tan",
    "MistyRose3",
    "Thistle3",
    "Plum2",
    "Yellow3",
    "Khaki3",
    "LightGoldenrod2",
    "LightYellow3",
    "Grey84",
    "LightSteelBlue1",
    "Yellow2",
    "DarkOliveGreen1",
    "DarkOliveGreen1",
    "DarkSeaGreen1",
    "Honeydew2",
    "LightCyan1",
    "Red1",
    "DeepPink2",
    "DeepPink1",
    "DeepPink1",
    "Magenta2",
    "Magenta1",
    "OrangeRed1",
    "IndianRed1",
    "IndianRed1",
    "HotPink",
    "HotPink",
    "MediumOrchid1",
    "DarkOrange",
    "Salmon1",
    "LightCoral",
    "PaleVioletRed1",
    "Orchid2",
    "Orchid1",
    "Orange1",
    "SandyBrown",
    "LightSalmon1",
    "LightPink1",
    "Pink1",
    "Plum1",
    "Gold1",
    "LightGoldenrod2",
    "LightGoldenrod2",
    "NavajoWhite1",
    "MistyRose1",
    "Thistle1",
    "Yellow1",
    "LightGoldenrod1",
    "Khaki1",
    "Wheat1",
    "Cornsilk1",
    "Grey100",
    "Grey3",
    "Grey7",
    "Grey11",
    "Grey15",
    "Grey19",
    "Grey23",
    "Grey27",
    "Grey30",
    "Grey35",
    "Grey39",
    "Grey42",
    "Grey46",
    "Grey50",
    "Grey54",
    "Grey58",
    "Grey62",
    "Grey66",
    "Grey70",
    "Grey74",
    "Grey78",
    "Grey82",
    "Grey85",
    "Grey89",
    "Grey93",
};

static int palette_color_index_from_xterm_name(const char* name)
{
    for (uint16_t i = 0; i < ARRAY_SIZE(color_palette_names); ++i) {
        if (strcasecmp(color_palette_names[i], name)) {
            return i + 16;
        }
    }
    return 0;
}

static ColorRGB color_from_xterm_name(const char* name, bool* fail)
{
    for (uint16_t i = 0; i < ARRAY_SIZE(color_palette_names); ++i) {
        if (strcasecmp(color_palette_names[i], name)) {
            ColorRGB color;
            generate_color_palette_entry(&color, i + 16);
            return color;
        }
    }
    if (fail) {
        *fail = true;
    }
    return (ColorRGB){ 0 };
}

const char* name_from_color_palette_index(uint16_t index)
{
    if (index < 16 || index > 255) {
        return NULL;
    } else {
        return color_palette_names[index - 16];
    }
}

/**
 * Parse a color form xterm name or as XParseColor() */
static void set_rgb_color_from_xterm_string(ColorRGB* color, const char* string)
{
    bool     failed = false;
    ColorRGB c;
    if (strstr(string, "rgbi:")) {
        c = ColorRGB_from_xorg_rgb_intensity_specification(string, &failed);
    } else if (strstr(string, "rgb:")) {
        c = ColorRGB_from_xorg_rgb_specification(string, &failed);
    } else if (*string == '#') {
        c = ColorRGB_from_xorg_old_rgb_specification(string, &failed);
    } else {
        c = color_from_xterm_name(string, &failed);
    }

    if (!failed) {
        *color = c;
    } else {
        WRN("Failed to parse \'%s\' as color\n", string);
    }
}

static void set_rgba_color_from_xterm_string(ColorRGBA* color, const char* string)
{
    ColorRGB c;
    set_rgb_color_from_xterm_string(&c, string);
    *color = ColorRGBA_from_RGB(c);
}

static void Vt_hard_reset(Vt* self)
{
    memset(&self->modes, 0, sizeof(self->modes));
    Vt_alt_buffer_off(self, false);
    Vt_select_end(self);
    Vt_clear_display_and_scrollback(self);
    Vt_move_cursor(self, 0, 0);

    self->parser.state = PARSER_STATE_LITERAL;

    self->charset_g0           = NULL;
    self->charset_g1           = NULL;
    self->charset_g2           = NULL;
    self->charset_g3           = NULL;
    self->charset_single_shift = NULL;
    self->last_interted        = NULL;

    self->scroll_region_top = 0;
    self->scroll_region_bottom =
      CALL_FP(self->callbacks.on_number_of_cells_requested, self->callbacks.user_data).second - 1;

    Vector_clear_DynStr(&self->title_stack);
    free(self->title);
    self->title = NULL;

    self->colors.bg = settings.bg;
    self->colors.fg = settings.fg;

    self->colors.highlight.bg = settings.bghl;
    self->colors.highlight.fg = settings.fghl;

    Vt_init_color_palette(self);

    self->tabstop = 8;
    Vt_reset_tab_ruler(self);

    // TODO: Clear DECUDK
}

static void Vt_soft_reset(Vt* self)
{
    Vt_alt_buffer_off(self, false);
    Vt_move_cursor(self, 0, 0);
    self->tabstop              = 8;
    self->parser.state         = PARSER_STATE_LITERAL;
    self->charset_g0           = NULL;
    self->charset_g1           = NULL;
    self->charset_g2           = NULL;
    self->charset_g3           = NULL;
    self->charset_single_shift = NULL;
    self->last_interted        = NULL;
    self->scroll_region_top    = 0;
    self->scroll_region_bottom =
      CALL_FP(self->callbacks.on_number_of_cells_requested, self->callbacks.user_data).second - 1;
    Vector_clear_DynStr(&self->title_stack);
}

void Vt_init(Vt* self, uint32_t cols, uint32_t rows)
{
    memset(self, 0, sizeof(Vt));
    self->ws = (struct winsize){ .ws_col = cols, .ws_row = rows };

    self->scroll_region_bottom = rows - 1;
    self->parser.state         = PARSER_STATE_LITERAL;
    self->parser.in_mb_seq     = false;

    self->colors.bg = settings.bg;
    self->colors.fg = settings.fg;

    self->colors.highlight.bg = settings.bghl;
    self->colors.highlight.fg = settings.fghl;

    Vt_reset_text_attribs(self, NULL);

    memcpy(&self->blank_space, &self->parser.char_state, sizeof(VtRune));

    self->parser.active_sequence = Vector_new_char();
    self->output                 = Vector_new_char();
    self->lines                  = Vector_new_VtLine(self);

    for (size_t i = 0; i < self->ws.ws_row; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
    }

    self->cursor.type     = CURSOR_BLOCK;
    self->cursor.blinking = true;
    self->cursor.col      = 0;

    self->tabstop = 8;
    Vt_init_tab_ruler(self);

    self->title       = NULL;
    self->title_stack = Vector_new_DynStr();

    self->unicode_input.buffer = Vector_new_char();

    self->xterm_modify_keyboard      = VT_XT_MODIFY_KEYBOARD_DFT;
    self->xterm_modify_cursor_keys   = VT_XT_MODIFY_CURSOR_KEYS_DFT;
    self->xterm_modify_function_keys = VT_XT_MODIFY_FUNCTION_KEYS_DFT;
    self->xterm_modify_other_keys    = VT_XT_MODIFY_OTHER_KEYS_DFT;

    Vt_init_color_palette(self);
}

static void Vt_init_tab_ruler(Vt* self)
{
    free(self->tab_ruler);
    self->tab_ruler = calloc(1, Vt_col(self) + 1);
    Vt_reset_tab_ruler(self);
}

static void Vt_reset_tab_ruler(Vt* self)
{
    for (uint16_t i = 0; i < Vt_col(self); ++i) {
        self->tab_ruler[i] = i % self->tabstop == 0;
    }
}

static void Vt_clear_all_tabstops(Vt* self)
{
    memset(self->tab_ruler, false, Vt_col(self));
}

static inline size_t Vt_top_line_alt(const Vt* const self)
{
    return self->alt_lines.size <= Vt_row(self) ? 0 : self->alt_lines.size - Vt_row(self);
}

static inline size_t Vt_bottom_line_alt(Vt* self)
{
    return Vt_top_line_alt(self) + Vt_row(self) - 1;
}

static inline uint16_t Vt_cursor_row(Vt* self)
{
    return self->cursor.row - Vt_top_line(self);
}

static inline size_t Vt_get_scroll_region_top(Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_top;
}

static inline size_t Vt_get_scroll_region_bottom(Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_bottom;
}

static inline bool Vt_scroll_region_not_default(Vt* self)
{
    return Vt_get_scroll_region_top(self) != Vt_top_line(self) ||
           Vt_get_scroll_region_bottom(self) != Vt_bottom_line(self);
}

bool Vt_visual_scroll_up(Vt* self)
{
    if (self->scrolling_visual) {
        if (self->visual_scroll_top) {
            --self->visual_scroll_top;
        } else {
            return true;
        }
    } else if (Vt_top_line(self)) {
        self->scrolling_visual  = true;
        self->visual_scroll_top = Vt_top_line(self) - 1;
    }
    return false;
}

bool Vt_visual_scroll_down(Vt* self)
{
    if (self->scrolling_visual && Vt_top_line(self) > self->visual_scroll_top) {
        ++self->visual_scroll_top;
        if (self->visual_scroll_top == Vt_top_line(self)) {
            self->scrolling_visual = false;
            return true;
        }
    }
    return false;
}

void Vt_visual_scroll_to(Vt* self, size_t line)
{
    line                    = MIN(line, Vt_top_line(self));
    self->visual_scroll_top = line;
    self->scrolling_visual  = line != Vt_top_line(self);
}

void Vt_visual_scroll_reset(Vt* self)
{
    self->scrolling_visual = false;
}

__attribute__((cold)) void Vt_dump_info(Vt* self)
{
    static int dump_index = 0;
    printf("\n====================[ STATE DUMP %2d ]====================\n", dump_index++);
    printf("Active character attributes:\n");
    printf("  foreground color:   " COLOR_RGB_FMT "\n",
           COLOR_RGB_AP((Vt_rune_fg(self, &self->parser.char_state))));
    printf("  background color:   " COLOR_RGBA_FMT "\n",
           COLOR_RGBA_AP((Vt_rune_bg(self, &self->parser.char_state))));
    printf("  line color uses fg: " BOOL_FMT "\n",
           BOOL_AP(!self->parser.char_state.line_color_not_default));
    printf("  line color:         " COLOR_RGB_FMT "\n",
           COLOR_RGB_AP((Vt_rune_ln_clr(self, &self->parser.char_state))));
    printf("  dim:                " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.dim));
    printf("  hidden:             " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.hidden));
    printf("  blinking:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.blinkng));
    printf("  underlined:         " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.underlined));
    printf("  strikethrough:      " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.strikethrough));
    printf("  double underline:   " BOOL_FMT "\n",
           BOOL_AP(self->parser.char_state.doubleunderline));
    printf("  curly underline:    " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.curlyunderline));
    printf("  overline:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.overline));
    printf("  inverted:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.invert));
    printf("Tab ruler:\n");
    printf("  tabstop: %d\n  ", self->tabstop);
    for (int i = 0; i < Vt_col(self); ++i) {
        printf("%c", self->tab_ruler[i] ? '|' : '_');
    }
    printf("\nModes:\n");
    printf("  application keypad:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.application_keypad));
    printf("  application keypad cursor:        " BOOL_FMT "\n",
           BOOL_AP(self->modes.application_keypad_cursor));
    printf("  auto repeat:                      " BOOL_FMT "\n", BOOL_AP(self->modes.auto_repeat));
    printf("  bracketed paste:                  " BOOL_FMT "\n",
           BOOL_AP(self->modes.bracketed_paste));
    printf("  send DEL on delete:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.del_sends_del));
    printf("  don't send esc on alt:            " BOOL_FMT "\n",
           BOOL_AP(self->modes.no_alt_sends_esc));
    printf("  extended reporting:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.extended_report));
    printf("  window focus events reporting:    " BOOL_FMT "\n",
           BOOL_AP(self->modes.window_focus_events_report));
    printf("  mouse button reporting:           " BOOL_FMT "\n",
           BOOL_AP(self->modes.mouse_btn_report));
    printf("  motion on mouse button reporting: " BOOL_FMT "\n",
           BOOL_AP(self->modes.mouse_motion_on_btn_report));
    printf("  mouse motion reporting:           " BOOL_FMT "\n",
           BOOL_AP(self->modes.mouse_motion_report));
    printf("  x10 compat mouse reporting:       " BOOL_FMT "\n",
           BOOL_AP(self->modes.x10_mouse_compat));
    printf("  no auto wrap:                     " BOOL_FMT "\n",
           BOOL_AP(self->modes.no_wraparound));
    printf("  reverse auto wrap:                " BOOL_FMT "\n",
           BOOL_AP(self->modes.reverse_wraparound));
    printf("  reverse video:                    " BOOL_FMT "\n",
           BOOL_AP(self->modes.video_reverse));

    printf("\n");
    printf("  S S | Number of lines %zu (last index: %zu)\n",
           self->lines.size,
           Vt_bottom_line(self));
    printf("  C C | Terminal size %hu x %hu\n", self->ws.ws_col, self->ws.ws_row);
    printf("V R R | \n");
    printf("I O . | Visible region: %zu - %zu\n",
           Vt_visual_top_line(self),
           Vt_visual_bottom_line(self));
    printf("E L   | \n");
    printf("W L V | Active line:  real: %zu (visible: %u)\n",
           self->cursor.row,
           Vt_cursor_row(self));
    printf("P   I | Cursor position: %u type: %d blink: %d hidden: %d\n",
           self->cursor.col,
           self->cursor.type,
           self->cursor.blinking,
           self->cursor.hidden);
    printf("O R E | Scroll region: %zu - %zu\n",
           Vt_get_scroll_region_top(self),
           Vt_get_scroll_region_bottom(self));
    printf("R E W | \n");
    printf("T G . +----------------------------------------------------\n");
    printf("| | |  BUFFER: %s\n", (self->alt_lines.buf ? "ALTERNATE" : "MAIN"));
    printf("V V V  \n");

    for (size_t i = 0; i < self->lines.size; ++i) {
        Vector_char str = rune_vec_to_string(&self->lines.buf[i].data, 0, 0, "");
        printf(
          "%c %c %c %4zu%c s:%3zu dmg:%d proxy{%3d,%3d,%3d,%3d} reflow{%d,%d,%d} data{%.90s%s}\n",
          i == Vt_top_line(self) ? 'v' : i == Vt_bottom_line(self) ? '^' : ' ',
          i == Vt_get_scroll_region_top(self) || i == Vt_get_scroll_region_bottom(self) ? '-' : ' ',
          i == Vt_visual_top_line(self) || i == Vt_visual_bottom_line(self) ? '*' : ' ',
          i,
          i == self->cursor.row ? '<' : ' ',
          self->lines.buf[i].data.size,
          self->lines.buf[i].damage.type != VT_LINE_DAMAGE_NONE,
          self->lines.buf[i].proxy.data[0],
          self->lines.buf[i].proxy.data[1],
          self->lines.buf[i].proxy.data[2],
          self->lines.buf[i].proxy.data[3],
          self->lines.buf[i].reflowable,
          self->lines.buf[i].rejoinable,
          self->lines.buf[i].was_reflown,
          str.buf,
          (str.size > 90 ? "…" : ""));

        if (self->lines.buf[i].links) {
            for (uint16_t j = 0; j < self->lines.buf[i].links->size; ++j) {
                printf("              URI[%u]: %s\n",
                       j,
                       self->lines.buf[i].links->buf[j].uri_string);
            }
        }

        Vector_destroy_char(&str);
    }
}

static void Vt_reflow_expand(Vt* self, uint32_t x)
{
    size_t bottom_bound = self->cursor.row;

    int removals = 0;

    while (bottom_bound > 0 && self->lines.buf[bottom_bound].rejoinable) {
        --bottom_bound;
    }

    for (size_t i = 0; i < bottom_bound; ++i) {
        VtLine* srcline = &self->lines.buf[i + 1];
        VtLine* tgtline = &self->lines.buf[i];

        if (tgtline->data.size < x && tgtline->reflowable) {
            int32_t chars_to_move = x - tgtline->data.size;
            if (i + 1 < bottom_bound && srcline->rejoinable) {
                chars_to_move = MIN(chars_to_move, (int32_t)srcline->data.size);

                /* Copy uri strings to target line and convert uri idx-es to the new line */
                if (srcline->links) {
                    for (int32_t j = 0; j < chars_to_move; ++j) {
                        VtRune*  r      = &srcline->data.buf[j];
                        uint16_t srcidx = r->hyperlink_idx;
                        if (srcidx && srcidx <= srcline->links->size) {
                            const char* uri =
                              Vector_at_VtUri(srcline->links, srcidx - 1)->uri_string;
                            int16_t newidx   = VtLine_add_link(tgtline, uri) + 1;
                            r->hyperlink_idx = newidx;
                        }
                    }
                }

                /* Move the actual data */
                Vector_pushv_VtRune(&tgtline->data, srcline->data.buf, chars_to_move);
                Vector_remove_at_VtRune(&srcline->data, 0, chars_to_move);

                if (self->selection.mode == SELECT_MODE_NORMAL) {
                    if (self->selection.begin_line == i + 1) {
                        if (self->selection.begin_char_idx <= chars_to_move) {
                            --self->selection.begin_line;
                            self->selection.begin_char_idx =
                              self->selection.begin_char_idx + tgtline->data.size - 1;
                        } else {
                            self->selection.begin_char_idx -= chars_to_move;
                        }
                    }
                    if (self->selection.end_line == i + 1) {
                        if (self->selection.end_char_idx < chars_to_move) {
                            --self->selection.end_line;
                            self->selection.end_char_idx =
                              self->selection.end_char_idx + tgtline->data.size - 1;
                        } else {
                            self->selection.end_char_idx -= chars_to_move;
                        }
                    }
                }

                Vt_mark_proxy_fully_damaged(self, i);
                Vt_mark_proxy_fully_damaged(self, i + 1);

                if (!srcline->data.size) {
                    tgtline->was_reflown = false;
                    size_t remove_index  = i + 1;
                    Vector_remove_at_VtLine(&self->lines, remove_index, 1);
                    --self->cursor.row;
                    --bottom_bound;
                    ++removals;

                    /* correct scroll region */
                    if (self->scrolling_visual && remove_index < Vt_visual_top_line(self)) {
                        Vt_visual_scroll_up(self);
                    }

                    /* correct selection */
                    if (self->selection.mode == SELECT_MODE_NORMAL) {
                        if (self->selection.begin_line >= remove_index) {
                            --self->selection.begin_line;
                        }
                        if (self->selection.end_line > remove_index) {
                            --self->selection.end_line;
                        }
                    }
                }
            }
        } // end should reflow
    }

    int underflow = -((int64_t)self->lines.size - Vt_row(self));

    if (underflow > 0) {
        for (int i = 0; i < (int)MIN(underflow, removals); ++i) {
            Vector_push_VtLine(&self->lines, VtLine_new());
        }
    }

    /* do not scroll past end of screen (self->ws was not updated yet, so Vt_scroll_down does not
     * prevent this) */
    if (Vt_visual_top_line(self) > Vt_top_line(self)) {
        Vt_visual_scroll_reset(self);
    }
}

static void Vt_reflow_shrink(Vt* self, uint32_t x)
{
    size_t insertions_made = 0;
    size_t bottom_bound    = self->cursor.row;

    while (bottom_bound > 0 && self->lines.buf[bottom_bound].rejoinable) {
        --bottom_bound;
    }

    for (size_t i = 0; i < bottom_bound; ++i) {
        VtLine* srcline = &self->lines.buf[i];
        VtLine* tgtline = &self->lines.buf[i + 1];

        if (srcline->data.size > x && srcline->reflowable) {
            size_t chars_to_move = srcline->data.size - x;

            /* move select to next line */
            bool end_just_moved = false;
            if (self->selection.mode == SELECT_MODE_NORMAL) {
                if (self->selection.begin_char_idx > (int32_t)x && self->selection.begin_line) {
                    ++self->selection.begin_line;
                    self->selection.begin_char_idx = self->selection.begin_char_idx - x - 1;
                }
                if (self->selection.end_char_idx >= (int32_t)x && self->selection.end_line) {
                    ++self->selection.end_line;
                    self->selection.end_char_idx = self->selection.end_char_idx - x - 1;
                    end_just_moved               = true;
                }
            }

            /* line below is a reflow already */
            if (i + 1 < bottom_bound && tgtline->rejoinable) {
                for (size_t ii = 0; ii < chars_to_move; ++ii) {

                    /* shift selection points right */
                    if (self->selection.mode == SELECT_MODE_NORMAL) {
                        if (self->selection.begin_line == i + 1) {
                            ++self->selection.begin_char_idx;
                        }
                        if (self->selection.end_line == i + 1 && !end_just_moved) {
                            ++self->selection.end_char_idx;
                        }
                    }

                    VtRune* r = srcline->data.buf + x + chars_to_move - ii - 1;

                    /* update link idx-es */
                    if (r->hyperlink_idx && srcline->links &&
                        r->hyperlink_idx >= (uint16_t)srcline->links->size) {
                        const char* uri = srcline->links->buf[r->hyperlink_idx - 1].uri_string;
                        int16_t     new_uri_idx = VtLine_add_link(tgtline, uri) + 1;
                        r->hyperlink_idx        = new_uri_idx;
                    }

                    Vector_insert_VtRune(&tgtline->data, tgtline->data.buf, *r);
                }
                Vt_mark_proxy_fully_damaged(self, i + 1);
            } else if (i < bottom_bound) {
                ++insertions_made;
                size_t insert_index = i + 1;
                Vector_insert_VtLine(&self->lines, self->lines.buf + insert_index, VtLine_new());

                /* correct visual scroll region */
                if (self->scrolling_visual && Vt_visual_top_line(self) >= insert_index &&
                    Vt_visual_bottom_line(self) < self->lines.size - 1) {
                    Vt_visual_scroll_down(self);
                }

                /* correct selection region */
                if (self->selection.mode == SELECT_MODE_NORMAL) {
                    if (self->selection.begin_line >= insert_index) {
                        ++self->selection.begin_line;
                    }
                    if (self->selection.end_line >= insert_index) {
                        ++self->selection.end_line;
                    }
                }

                ++self->cursor.row;
                ++bottom_bound;

                /* update link idx-es */
                for (size_t j = 0; j < chars_to_move; ++j) {
                    VtRune* r = srcline->data.buf + x + j;
                    if (r->hyperlink_idx && srcline->links &&
                        r->hyperlink_idx >= (uint16_t)srcline->links->size) {
                        const char* uri = srcline->links->buf[r->hyperlink_idx - 1].uri_string;
                        int16_t     new_uri_idx = VtLine_add_link(tgtline, uri) + 1;
                        r->hyperlink_idx        = new_uri_idx;
                    }
                }

                Vector_pushv_VtRune(&tgtline->data, srcline->data.buf + x, chars_to_move);

                srcline->was_reflown = true;
                tgtline->rejoinable  = true;
            }
        }
    }

    if (self->lines.size - 1 != self->cursor.row) {
        size_t overflow = self->lines.size > Vt_row(self) ? self->lines.size - Vt_row(self) : 0;
        size_t whitespace_below = self->lines.size - 1 - self->cursor.row;
        size_t to_pop           = MIN(overflow, MIN(whitespace_below, insertions_made));
        Vector_pop_n_VtLine(&self->lines, to_pop);
    }
}

/**
 * Remove extra columns from all lines */
static void Vt_trim_columns(Vt* self)
{
    for (uint16_t i = 0; i < self->lines.size; ++i) {
        if (self->lines.buf[i].data.size > Vt_col(self)) {
            Vt_mark_proxy_fully_damaged(self, i);

            CALL_FP(self->callbacks.destroy_proxy,
                    self->callbacks.user_data,
                    &self->lines.buf[i].proxy);

            size_t blanks = 0;

            size_t s = self->lines.buf[i].data.size;
            Vector_pop_n_VtRune(&self->lines.buf[i].data, s - Vt_col(self));

            if (self->lines.buf[i].was_reflown) {
                continue;
            }

            s = self->lines.buf[i].data.size;

            for (blanks = 0; blanks < s; ++blanks) {
                if (!(self->lines.buf[i].data.buf[s - 1 - blanks].rune.code == ' ' &&
                      ColorRGBA_eq(
                        self->colors.bg,
                        Vt_rune_bg(self, &self->lines.buf[i].data.buf[s - 1 - blanks])))) {
                    break;
                }
            }
            Vector_pop_n_VtRune(&self->lines.buf[i].data, blanks);
        }
    }
}

void Vt_resize(Vt* self, uint32_t x, uint32_t y)
{
    if (x < 2 || y < 2) {
        return;
    }

    if (!self->alt_lines.buf) {
        Vt_trim_columns(self);
    }

    self->saved_cursor_pos  = MIN(self->saved_cursor_pos, x);
    self->saved_active_line = MIN(self->saved_active_line, self->lines.size);

    static uint16_t ox = 0, oy = 0;
    if (x != ox || y != oy) {
        if (!self->alt_lines.buf && !Vt_scroll_region_not_default(self)) {
            if (self->selection.mode == SELECT_MODE_BOX) {
                Vt_select_end(self);
            }
            if (x < ox) {
                Vt_reflow_shrink(self, x);
            } else if (x > ox) {
                Vt_reflow_expand(self, x);
            }
        } else {
            Vt_select_end(self);
        }
        if (Vt_row(self) > y) {
            uint16_t to_pop = Vt_row(self) - y;
            if (self->cursor.row + to_pop > Vt_bottom_line(self)) {
                to_pop -= self->cursor.row + to_pop - Vt_bottom_line(self);
            }
            Vector_pop_n_VtLine(&self->lines, to_pop);
            if (self->alt_lines.buf) {
                uint16_t to_pop_alt = Vt_row(self) - y;
                if (self->alt_active_line + to_pop_alt > Vt_bottom_line_alt(self)) {
                    to_pop_alt -= self->alt_active_line + to_pop_alt - Vt_bottom_line_alt(self);
                }
                Vector_pop_n_VtLine(&self->alt_lines, to_pop_alt);
            }
        } else {
            for (uint16_t i = 0; i < y - Vt_row(self); ++i) {
                Vector_push_VtLine(&self->lines, VtLine_new());
            }
            if (self->alt_lines.buf) {
                for (uint16_t i = 0; i < y - Vt_row(self); ++i) {
                    Vector_push_VtLine(&self->alt_lines, VtLine_new());
                }
            }
        }
        ox = x;
        oy = y;
    }

    Pair_uint32_t px =
      CALL_FP(self->callbacks.on_window_size_from_cells_requested, self->callbacks.user_data, x, y);

    self->ws =
      (struct winsize){ .ws_col = x, .ws_row = y, .ws_xpixel = px.first, .ws_ypixel = px.second };

    LOG("resized to: %d %d [%d %d]\n",
        self->ws.ws_col,
        self->ws.ws_row,
        self->ws.ws_xpixel,
        self->ws.ws_ypixel);

    self->pixels_per_cell_x = (double)self->ws.ws_xpixel / Vt_col(self);
    self->pixels_per_cell_y = (double)self->ws.ws_ypixel / Vt_row(self);

    if (self->master_fd > 1) {
        if (ioctl(self->master_fd, TIOCSWINSZ, &self->ws) < 0) {
            WRN("ioctl(%d, TIOCSWINSZ, winsize { %d, %d, %d, %d }) failed:  %s\n",
                self->master_fd,
                self->ws.ws_col,
                self->ws.ws_row,
                self->ws.ws_xpixel,
                self->ws.ws_ypixel,
                strerror(errno));
        }
    }

    self->scroll_region_top    = 0;
    self->scroll_region_bottom = Vt_row(self) - 1;

    Vt_init_tab_ruler(self);
}

static inline int32_t short_sequence_get_int_argument(const char* seq)
{
    return *seq == 0 || seq[1] == 0 ? 1 : atoi(seq);
}

/*
 * value:
 * 0 => not recognized
 * 1 => enabled
 * 2 => disabled
 * 3 => permanently enabled
 * 4 => permanently disabled
 */
static inline void Vt_report_dec_mode(Vt* self, int code)
{
    bool value;
    switch (code) {
        case 1:
            value = self->modes.application_keypad_cursor;
            break;
        case 7:
            value = self->modes.no_wraparound;
            break;
        case 8:
            value = self->modes.auto_repeat;
            break;
        case 25:
            value = self->cursor.hidden;
            break;
        case 1000:
            value = self->modes.mouse_btn_report;
            break;
        case 1002:
            value = self->modes.mouse_motion_on_btn_report;
            break;
        case 1003:
            value = self->modes.mouse_motion_report;
            break;
        case 1004:
            value = self->modes.window_focus_events_report;
            break;
        case 1006:
            value = self->modes.extended_report;
            break;
        case 1037:
            value = self->modes.del_sends_del;
            break;
        case 1039:
            value = self->modes.no_alt_sends_esc;
            break;
        case 1042:
            value = self->modes.urgency_on_bell;
            break;
        case 1043:
            value = self->modes.pop_on_bell;
            break;
        case 47:
        case 1047:
        case 1049:
            value = Vt_alt_buffer_enabled(self);
            break;
        default: {
            Vt_output_formated(self, "\e[%d;0$y", code);
            return;
        }
    }
    Vt_output_formated(self, "\e[%d;%c$y", code, value ? '1' : '2');
}

static inline void Vt_handle_dec_mode(Vt* self, int code, bool on)
{
    switch (code) {

        /* Cursor Keys Mode (DEC Private) (DECCKM)
         *
         * This is a private parameter applicable to set mode (SM) and reset mode (RM) control
         * sequences. This mode is only effective when the terminal is in keypad application mode
         * (see DECKPAM) and the ANSI/VT52 mode (DECANM) is set (see DECANM). Under these
         * conditions, if the cursor key mode is reset, the four cursor function keys will send ANSI
         * cursor control commands. If cursor key mode is set, the four cursor function keys will
         * send application functions.
         */
        case 1:
            self->modes.application_keypad_cursor = on;
            break;

            /* Column mode 132/80 (DECCOLM)
             *
             * The reset state causes a maximum of 80 columns on the screen. The set state causes a
             * maximum of 132 columns on the screen.
             */
        case 3: {
            if (self->modes.allow_column_size_switching && settings.windowops_manip) {
                Pair_uint32_t target_text_area_dims;
                if (on) {
                    target_text_area_dims =
                      CALL_FP(self->callbacks.on_window_size_from_cells_requested,
                              self->callbacks.user_data,
                              132,
                              26);

                } else {
                    target_text_area_dims =
                      CALL_FP(self->callbacks.on_window_size_from_cells_requested,
                              self->callbacks.user_data,
                              80,
                              24);
                }
                CALL_FP(self->callbacks.on_text_area_dimensions_set,
                        self->callbacks.user_data,
                        target_text_area_dims.first,
                        target_text_area_dims.second);
            }
            Vt_move_cursor(self, 0, 0);
            Vt_clear_display_and_scrollback(self);
        } break;

        /* Smooth (Slow) Scroll (DECSCLM), VT100. */
        case 4:
            WRN("DECSCLM not implemented\n");
            break;

        /* Reverse video (DECSCNM) */
        case 5:
            // TODO:
            break;

        /* Origin mode (DECCOM)
         * makes cursor movement relative to the scroll region */
        case 6:
            self->modes.origin = on;
            Vt_move_cursor(self, 0, 0);
            break;

        /* DECAWM */
        case 7:
            self->modes.no_wraparound = !on;
            break;

        /* DECARM */
        case 8:
            self->modes.auto_repeat = on;
            break;

        /* Show toolbar (rxvt) */
        case 10:
            break;

        /* Start Blinking Cursor (AT&T 610) */
        case 12:
            break;

        /* Very visible cursor (CVVIS) */
        /* case 12: */
        /* break; */

        /* Printer status request (DSR) */
        case 15:
            /* Printer not connected. */
            Vt_output(self, "\e[?13n", 6);
            break;

        /* hide/show cursor (DECTCEM) */
        case 25:
            if (likely(!settings.debug_slow)) {
                self->cursor.hidden = !on;
            }
            break;

        /* Allow 80 => 132 Mode, xterm */
        case 40:
            self->modes.allow_column_size_switching = on;
            break;

        /* Reverse-wraparound Mode, xterm. */
        case 45:
            self->modes.reverse_wraparound = on;
            break;

        /* Page cursor-coupling mode (DECPCCM) */
        case 64:
        /* Vertical cursor-coupling mode (DECVCCM) */
        case 61:
            WRN("Page cursor-coupling not implemented\n");
            break;

        /* Numeric keypad (DECNKM) */
        case 66:
            WRN("DECNKM not implemented\n");
            break;

        /* Set Backarrow key to backspace/delete (DECBKM) */
        case 67:
            // TODO:
            WRN("DECBKM not implemented\n");
            break;

        /* Keyboard usage (DECKBUM) */
        case 68:
            WRN("DECKBUM not implemented\n");
            break;

        /* X11 xterm mouse protocol. */
        case 1000:
            self->modes.mouse_btn_report = on;
            break;

        /* Highlight mouse tracking, xterm */
        case 1001:
            WRN("Highlight mouse tracking not implemented\n");
            break;

        /* xterm cell motion mouse tracking */
        case 1002:
            self->modes.mouse_motion_on_btn_report = on;
            break;

        /* xterm all motion tracking */
        case 1003:
            self->modes.mouse_motion_report = on;
            break;

        case 1004:
            self->modes.window_focus_events_report = on;
            break;

        /* utf8 Mouse Mode */
        case 1005:
            WRN("utf8 mouse mode not implemented\n");
            break;

        /* SGR mouse mode */
        case 1006:
            self->modes.extended_report = on;
            break;

        /* urxvt mouse mode */
        case 1015:
            WRN("urxvt mouse mode not implemented\n");
            break;

        case 1034:
            WRN("xterm eightBitInput not implemented\n");
            break;

        case 1035:
            WRN("xterm numLock not implemented\n");
            break;

        case 1036:
            WRN("xterm metaSendsEscape not implemented\n");
            break;

        case 1037:
            self->modes.del_sends_del = on;
            break;

        case 1039:
            self->modes.no_alt_sends_esc = on;
            break;

        /* Bell sets urgent WM hint */
        case 1042:
            self->modes.urgency_on_bell = on;
            break;

        /* Bell raises window */
        case 1043:
            self->modes.pop_on_bell = on;
            break;

        /* Use alternate screen buffer, xterm */
        case 47:
        /* Also use alternate screen buffer, xterm */
        case 1047:
        /* After saving the cursor, switch to the Alternate Screen Buffer,
         * clearing it first. */
        case 1049:
            if (on) {
                Vt_alt_buffer_on(self, code == 1049);
            } else {
                Vt_alt_buffer_off(self, code == 1049);
            }
            break;

        case 2004:
            self->modes.bracketed_paste = on;
            break;

        case 1051: /* Sun function-key mode, xterm. */
        case 1052: /* HP function-key mode, xterm. */
        case 1053: /* SCO function-key mode, xterm. */
        case 1060: /* legacy keyboard emulation, i.e, X11R6, */
        case 1061: /* VT220 keyboard emulation, xterm. */
            WRN("Unimplemented keyboard option\n");
            break;

        default:
            WRN("Unknown DECSET/DECRST code: %d\n", code);
    }
}

__attribute__((hot)) static inline void Vt_handle_CSI(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_csi_sequence_terminated(self->parser.active_sequence.buf,
                                   self->parser.active_sequence.size)) {
        CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
        Vector_push_char(&self->parser.active_sequence, '\0');
        char* seq        = self->parser.active_sequence.buf;
        char  first_char = *seq;
        char  last_char  = self->parser.active_sequence.buf[self->parser.active_sequence.size - 2];
        char  second_last_char;
        if (self->parser.active_sequence.size < 3) {
            second_last_char = '\0';
        } else {
            second_last_char =
              self->parser.active_sequence.buf[self->parser.active_sequence.size - 3];
        }
        bool is_single_arg = !strchr(seq, ';') && !strchr(seq, ':');

#define MULTI_ARG_IS_ERROR                                                                         \
    if (!is_single_arg) {                                                                          \
        WRN("Unexpected additional arguments for CSI sequence \'%s\'\n", seq);                     \
        break;                                                                                     \
    }

        switch (first_char) {

            /* <ESC>[! ... */
            case '!': {
                switch (last_char) {
                    /* <ESC>[!p - Soft terminal reset (DECSTR), VT220 and up. */
                    case 'p': {
                        Vt_soft_reset(self);
                    } break;

                    default:
                        WRN("Unknown CSI sequence: %s\n", seq);
                }
            } break;

            /* <ESC>[? ... */
            case '?': {
                switch (second_last_char) {

                    /* <ESC>[? ... $ ... */
                    case '$': {
                        switch (last_char) {

                            /* <ESC>[? Ps $p - Request DEC private mode (DECRQM). VT300 and up
                             *
                             * Ps - mode id as per DECSET/DECSET
                             *
                             * reply:
                             *   CSI? <mode id>;<value> $y
                             */
                            case 'p': {
                                /* Not recognized */
                                WRN("DEC mode state reports not implemented\n");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                    } break;

                    default:
                        switch (last_char) {
                            /* <ESC>[? Pm h - DEC Private Mode Set (DECSET) */
                            case 'h':
                            /* <ESC>[? Pm l - DEC Private Mode Reset (DECRST) */
                            case 'l': {
                                bool               is_enable = last_char == 'h';
                                Vector_Vector_char tokens =
                                  string_split_on(seq + 1, ";:", NULL, NULL);
                                for (Vector_char* token = NULL;
                                     (token = Vector_iter_Vector_char(&tokens, token));) {
                                    errno         = 0;
                                    uint32_t code = atoi(token->buf + 1);
                                    if (code && !errno) {
                                        Vt_handle_dec_mode(self, code, is_enable);
                                    } else {
                                        WRN("Invalid %s argument: \'%s\'\n",
                                            is_enable ? "DECSET" : "DECRST",
                                            token->buf + 1);
                                    }
                                }
                                Vector_destroy_Vector_char(&tokens);
                            } break;

                            /* <ESC>[? Ps i -  Media Copy (MC), DEC-specific */
                            case 'i':
                                break;

                                /* <ESC>[? Ps n Device Status Report (DSR, DEC-specific) */
                            case 'n': {
                                int arg = short_sequence_get_int_argument(seq);
                                switch (arg) {
                                    case 6: { /* report cursor position */
                                        Vt_output_formated(self,
                                                           "\e[?%u;%uR",
                                                           Vt_cursor_row(self) + 1,
                                                           self->cursor.col + 1);
                                    } break;

                                    case 15: /* Report Printer status */
                                             /* not ready */
                                        Vt_output(self, "\e[?11n", 6);
                                        break;

                                    case 26: /* Report keyboard status */
                                             /* always report US */
                                        Vt_output(self, "\e[?27;1;0;0n", 12);
                                        break;

                                    case 53: /* Report locator status */
                                             /* No locator (xterm not compiled-in) */
                                        Vt_output(self, "\e[?50n", 6);
                                        break;

                                    case 56: /* Report locator type */
                                             /* Cannot identify (xterm not compiled-in) */
                                        Vt_output(self, "\e[?57;0n", 8);
                                        break;

                                    case 85: /* Report multi-session configuration */
                                        /* Device not configured for multi-session operation */
                                        Vt_output(self, "\e[?83n", 6);
                                        break;

                                    default:
                                        WRN("Unimplemented DSR sequence: %d\n", arg);
                                }
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                }
            } break;

            /* <ESC>[> ... */
            case '>': {
                switch (last_char) {
                        /* <ESC>[> Pp m / <ESC>[> Pp ; Pv m - Set/reset key modifier options
                         * (XTMODKEYS)
                         *
                         * 0 => modifyKeyboard
                         * 1 => modifyCursorKeys
                         * 2 => modifyFunctionKeys
                         * 4 => modifyOtherKeys */
                    case 'm': {
                        int resource, value;
                        int nargs = sscanf(seq, "%d;%d", &resource, &value);
                        if (nargs == EOF) {
                            goto invalid;
                        }
                        if (!nargs) {
                            self->xterm_modify_keyboard      = VT_XT_MODIFY_KEYBOARD_DFT;
                            self->xterm_modify_cursor_keys   = VT_XT_MODIFY_CURSOR_KEYS_DFT;
                            self->xterm_modify_function_keys = VT_XT_MODIFY_FUNCTION_KEYS_DFT;
                            self->xterm_modify_other_keys    = VT_XT_MODIFY_OTHER_KEYS_DFT;
                            break;
                        }

                        switch (resource) {
                            case 0:
                                self->xterm_modify_keyboard =
                                  nargs == 2 ? value : VT_XT_MODIFY_KEYBOARD_DFT;
                                break;
                            case 1:
                                self->xterm_modify_cursor_keys =
                                  nargs == 2 ? value : VT_XT_MODIFY_CURSOR_KEYS_DFT;
                                break;
                            case 2:
                                self->xterm_modify_function_keys =
                                  nargs == 2 ? value : VT_XT_MODIFY_FUNCTION_KEYS_DFT;
                                break;
                            case 4:
                                self->xterm_modify_other_keys =
                                  nargs == 2 ? value : VT_XT_MODIFY_OTHER_KEYS_DFT;
                                break;
                            default:
                                goto invalid;
                        }

                        break;
                    invalid:
                        WRN("Invalid XTMODKEYS command \'%s\'\n", seq);
                    } break;

                        /* <ESC>[> Ps n - Disable key modifier options, xterm
                         *
                         * This control sequence corresponds to a resource value of "-1", which
                         * cannot be set with the other sequence
                         *
                         * If the parameter is omitted, modifyFunctionKeys is disabled
                         *
                         * 0 => modifyKeyboard
                         * 1 => modifyCursorKeys
                         * 2 => modifyFunctionKeys
                         * 4 => modifyOtherKeys */
                    case 'n': {
                        MULTI_ARG_IS_ERROR
                        int arg = seq[1] == 'n' ? 2 : short_sequence_get_int_argument(seq);
                        switch (arg) {
                            case 0:
                                self->xterm_modify_keyboard = -1;
                                break;
                            case 1:
                                self->xterm_modify_cursor_keys = -1;
                                break;
                            case 2:
                                self->xterm_modify_function_keys = -1;
                                break;
                            case 4:
                                self->xterm_modify_other_keys = -1;
                                break;
                            default:
                                WRN("Invalid XTMODKEYS command \'%s\'\n", seq);
                        }
                    } break;

                    /* <ESC>[ > Ps c - Send Device Attributes (Secondary DA) */
                    case 'c': {
                        MULTI_ARG_IS_ERROR
                        int arg = short_sequence_get_int_argument(seq);
                        if (arg == 0) {
                            /* report VT100, firmware ver. 0, ROM number 0 */
                            Vt_output(self, "\e[>0;0;0c", 9);
                        }
                    } break;

                    default:
                        WRN("Unknown CSI sequence: %s\n", seq);
                }
            } break;

            /* <ESC>[= ... */
            case '=': {
                switch (last_char) {
                    /* <ESC>[ = Ps c - Send Device Attributes (Tertiary DA). */
                    case 'c': {
                        MULTI_ARG_IS_ERROR
                        int arg = short_sequence_get_int_argument(seq);
                        if (arg == 0) {
                            Vt_output(self, "\e[?6c", 5);
                        }
                    } break;

                    default:
                        WRN("Unknown CSI sequence: %s\n", seq);
                }
            } break;

            /* <ESC>[... */
            default: {
                switch (second_last_char) {
                    /* <ESC>[ .. ; .. SP ?  */
                    case ' ':
                        switch (last_char) {
                            /* <ECS>[ Ps SP @ - Shift left Ps columns(s) (default = 1) (SL), ECMA-48
                             */
                            case '@': {
                                WRN("SL not implemented\n");
                            } break;

                            /* <ESC>[ Ps SP A - Shift right Ps columns(s) (default = 1) (SR),
                             * ECMA-48 */
                            case 'A': {
                                WRN("SP not implemented\n");
                            } break;

                            /* <ESC>[ Ps SP q - Set cursor style (DECSCUSR) */
                            case 'q': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                switch (arg) {
                                    case 0:
                                    case 1:
                                        self->cursor.type     = CURSOR_BLOCK;
                                        self->cursor.blinking = false;
                                        break;
                                    case 2:
                                        self->cursor.type     = CURSOR_BLOCK;
                                        self->cursor.blinking = true;
                                        break;
                                    case 3:
                                        self->cursor.type     = CURSOR_UNDERLINE;
                                        self->cursor.blinking = true;
                                        break;
                                    case 4:
                                        self->cursor.type     = CURSOR_UNDERLINE;
                                        self->cursor.blinking = false;
                                        break;
                                    case 5:
                                        self->cursor.type     = CURSOR_BEAM;
                                        self->cursor.blinking = true;
                                        break;
                                    case 6:
                                        self->cursor.type     = CURSOR_BEAM;
                                        self->cursor.blinking = false;
                                        break;

                                    default:
                                        WRN("Unknown DECSCUR code: %d\n", arg);
                                }
                            } break;
                        }
                        break;

                    /* <ESC>[ .. ; .. "?  */
                    case '\"':
                        switch (last_char) {
                            /* <ESC>[ Ps "q - Select character protection attribute (DECSCA), VT220.
                             *
                             * 0 => DECSED and DECSEL can erase (default).
                             * 1 => DECSED and DECSEL cannot erase.
                             * 2 => DECSED and DECSEL can erase.
                             */
                            case 'q': {
                                WRN("Character protection not implemented\n");
                            } break;

                            /* <ESC>[ Pl ; Pc "p - Set conformance level (DECSCL), VT220 and up.
                             * (Pl)
                             *   61 => level 1, e.g., VT100.
                             *   62 => level 2, e.g., VT200.
                             *   63 => level 3, e.g., VT300.
                             *   64 => level 4, e.g., VT400.
                             *   65 => level 5, e.g., VT500.
                             * (Pc)
                             *   0 => 8-bit controls.
                             *   1 => 7-bit controls (DEC factory default).
                             *   2 => 8-bit controls.
                             */
                            case 'p': {
                                WRN("DEC conformance levels not implemented\n");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. #?  */
                    case '#':
                        switch (last_char) {
                            /* <ESC>[ Pm #{ - Push video attributes onto stack (XTPUSHSGR), xterm.
                             *
                             * The optional parameters correspond to the SGR encoding for video
                             * attributes, except for colors (which do not have a unique SGR code):
                             * 1  => Bold
                             * 2  => Faint
                             * 3  => Italicized
                             * 4  => Underlined
                             * 5  => Blink
                             * 7  => Inverse
                             * 8  => Invisible
                             * 9  => Crossed-out characters
                             * 21 => Doubly-underlined
                             * 30 => Foreground color
                             * 31 => Background color
                             */
                            case '{': {
                                MULTI_ARG_IS_ERROR
                                WRN("XTPUSHSGR not implemented\n");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr #|
                             *
                             * Report selected graphic rendition (XTREPORTSGR), xterm. The
                             * response is an SGR sequence which contains the attributes which
                             * are common to all cells in a rectangle. Pt ; Pl ; Pb ; Pr denotes
                             * the rectangle.
                             */
                            case '|': {
                                MULTI_ARG_IS_ERROR
                                WRN("XTREPORTSGR not implemented\n");
                            } break;

                            /* <ESC>[#} - Pop video attributes from stack (XTPOPSGR), xterm.
                             *
                             * Popping restores the video-attributes which were saved using
                             * XTPUSHSGR to their previous state.
                             */
                            case '}':

                            /* <ESC>[#q - Alias for <ESC>[#} */
                            case 'q': {
                                MULTI_ARG_IS_ERROR
                                WRN("XTPOPSGR not implemented\n");
                            } break;

                            /* <ESC>[ Pm #P - Push current dynamic and ANSI-palette colors onto
                             * stack (XTPUSHCOLORS), xterm.
                             *
                             * Parameters (integers in the range 1 through 10, since the default 0
                             * will push) may be used to store the palette into the stack without
                             * pushing.
                             */
                            case 'P': {
                                MULTI_ARG_IS_ERROR
                                WRN("XTPUSHCOLORS not implemented\n");
                            } break;

                            /* <ESC>[ Pm #Q -  Pop stack to set dynamic- and ANSI-palette
                             * colors (XTPOPCOLORS), xterm.
                             *
                             * Parameters (integers in the range 1 through 10, since the default
                             * 0 will pop) may be used to restore the palette from the stack
                             * without popping.
                             */
                            case 'Q': {
                                MULTI_ARG_IS_ERROR
                                WRN("XTPOPCOLORS not implemented\n");
                            } break;

                            /* <ESC> #R
                             * Report the current entry on the palette stack, and the number of
                             * palettes stored on the stack, using the same form as XTPOPCOLOR
                             * (default = 0) (XTREPORTCOLORS), xterm.
                             */
                            case 'R': {
                                MULTI_ARG_IS_ERROR
                                WRN("XTREPORTCOLORS not implemented\n");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. '?  */
                    case '\'':
                        switch (last_char) {

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr 'w - Enable Filter Rectangle (DECEFR),
                             * VT420 and up
                             *
                             * Parameters are [top;left;bottom;right]. Defines the coordinates
                             * of a filter rectangle and activates it. Anytime the locator is
                             * detected outside of the filter rectangle, an outside rectangle
                             * event is generated and the rectangle is disabled. Filter
                             * rectangles are always treated as "one-shot" events. Any
                             * parameters that are omitted default to the current locator
                             * position. If all parameters are omitted, any locator motion
                             * will be reported. DECELR always cancels any previous rectangle
                             * definition.
                             */
                            case 'w': {
                                WRN("Filter rectangle locator events not implemented\n");
                            } break;

                            /* <ESC>[ Ps ; Pu 'z - Enable Locator Reporting (DECELR)
                             * (Ps)
                             *   0 => Locator disabled (default)
                             *   1 => Locator enabled
                             *   2 => Locator enabled for one report
                             * (Pu) <coordinate unit>
                             *   0, 2 => Cells (default)
                             *   1    => Pixels
                             */
                            case 'z': {
                                WRN("Locator reporting not implemented\n");
                            } break;

                            /* <ESC>[ Pm '{ - Select Locator Events (DECSLE)
                             *
                             * 0 => Explicit host request only (DECRQLP) (default)
                             * 1 => on button down ON
                             * 2 => on button down OFF
                             * 3 => on button up ON
                             * 4 => on button up OFF
                             */
                            case '{': {
                                WRN("Locator events not implemented\n");
                            } break;

                            /* <ESC>[ Ps '| - Request Locator Position (DECRQLP)
                             *
                             * Valid values for the parameter are 0, 1 or omitted => transmit a
                             * single DECLRP locator report.
                             *
                             * If Locator Reporting has been enabled by a DECELR, xterm will respond
                             * with a DECLRP Locator Report.  This report is also generated on
                             * button up and down events if they have been enabled with a DECSLE, or
                             * when the locator is detected outside of a filter rectangle, if filter
                             * rectangles have been enabled with a DECEFR.
                             *
                             * CSI Pe ; Pb ; Pr ; Pc ; Pp &w
                             * Parameters are [event;button;row;column;page].
                             * Valid values for the event:
                             * (Pe)
                             *   0  =>  locator unavailable - no other parameters sent.
                             *   1  =>  request - xterm received a DECRQLP.
                             *   2  =>  left button down.
                             *   3  =>  left button up.
                             *   4  =>  middle button down.
                             *   5  =>  middle button up.
                             *   6  =>  right button down.
                             *   7  =>  right button up.
                             *   8  =>  M4 button down.
                             *   9  =>  M4 button up.
                             *   10 =>  locator outside filter rectangle.
                             *
                             * The "button" parameter is a bitmask indicating which buttons are
                             * pressed: Pb = 0  =>  no buttons down. Pb & 1  =>  right button down.
                             * Pb & 2  =>  middle button down.
                             * Pb & 4  =>  left button down.
                             * Pb & 8  =>  M4 button down.
                             *
                             * The "row" and "column" parameters are the coordinates of the locator
                             * position in the xterm window, encoded as ASCII decimal. The "page"
                             * parameter is not used by xterm.
                             */
                            case '|': {
                                /* locator unavailable */
                                Vt_output(self, "\e[0&w", 5);
                            } break;

                            /* <ESC>['} - Insert Ps Column(s) (default = 1) (DECIC), VT420 and up */
                            case '}': {
                                WRN("DECIC not implemented\n");
                            } break;

                            /* <ESC>['~ - Delete Ps Column(s) (default = 1) (DECDC), VT420 and up */
                            case '~': {
                                WRN("DECDC not implemented\n");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. * ..  */
                    case '*':
                        switch (last_char) {
                            /* <ESC>[ Ps *x - Select Attribute Change Extent (DECSACE), VT420 and up
                             * (Ps)
                             *   0, 1 => from start to end position, wrapped
                             *   2    => rectangle (exact)
                             */
                            case 'x': {
                                MULTI_ARG_IS_ERROR
                                WRN("DECSACE not implemented\n");
                            } break;

                            /* <ESC>[ Pi ; Pg ; Pt ; Pl ; Pb ; Pr *y
                             * Request Checksum of Rectangular Area (DECRQCRA), VT420 and up
                             *
                             * Response is DCS Pi ! ~ x x x x ST Pi is the request id. Pg is the
                             * page number. Pt ; Pl ; Pb ; Pr denotes the rectangle. The x's are
                             * hexadecimal digits 0-9 and A-F.
                             */
                            case 'y': {
                                WRN("DECRQCRA not implemented\n");
                            } break;

                            /* <ESC>[*| - Select number of lines per screen (DECSNLS), VT420 and up
                             */
                            case '|': {
                                WRN("DECSNLS not implemented\n");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. $ ..  */
                    case '$':
                        switch (last_char) {

                            /* <ESC>[ Ps $p - Request ANSI mode (DECRQM). VT300 and up
                             *
                             * reply: CSI ? <DECSET/DECRT code>; <value> $ y
                             *
                             * value:
                             * 0 => not recognized
                             * 1 => enabled
                             * 2 => disabled
                             * 3 => permanently enabled
                             * 4 => permanently disabled
                             */
                            case 'p': {
                                int arg = short_sequence_get_int_argument(seq);
                                Vt_report_dec_mode(self, arg);
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr ; Ps $r - Change Attributes in Rectangular
                             * Area (DECCARA), VT400 and up
                             *
                             * Pt ; Pl ; Pb ; Pr denotes the rectangle.
                             * Ps denotes the SGR attributes to change: 0, 1, 4, 5, 7
                             */
                            case 'r': {
                                WRN("DECCARA not implemented\n");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr ; Ps $t - Reverse Attributes in Rectangular
                             * Area (DECRARA), VT400 and up
                             *
                             * Pt ; Pl ; Pb ; Pr denotes the rectangle. Ps denotes the attributes to
                             * reverse, i.e.,  1, 4, 5, 7.
                             */
                            case 't': {
                                WRN("DECRARA not implemented\n");
                            } break;

                            /* <ESC>[ Ps $w - Request presentation state report (DECRQPSR), VT320
                             * and up
                             *
                             * (Ps)
                             *   0 => error
                             *   1 => cursor information report (DECCIR)
                             *     Response is DCS 1 $ u Pt ST Refer to the VT420 programming
                             *     manual, which requires six pages to document the data string Pt,
                             *   2 => tab stop report (DECTABSR). Response is DCS 2 $ u Pt ST The
                             *     data string Pt is a list of the tab-stops, separated by "/"
                             *     characters.
                             */
                            case 'w': {
                                WRN("DECRQPSR not implemented\n");
                            } break;

                            /* <ESC>[ Pc ; Pt ; Pl ; Pb ; Pr $x - Fill Rectangular Area (DECFRA),
                             * VT420 and up
                             *
                             * Pc is the character to use
                             * Pt ; Pl ; Pb ; Pr denotes the rectangle
                             */
                            case 'x': {
                                WRN("DECFRA not implemented\n");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr $z
                             * Erase Rectangular Area (DECERA), VT400 and up
                             */
                            case 'z': {
                                WRN("DECERA not implemented\n");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr ${
                             * Selective Erase Rectangular Area (DECSERA), VT400 and up
                             */
                            case '{': {
                                WRN("DECSERA not implemented\n");
                            } break;

                            /* <ESC>[ Ps $| - Select columns per page (DECSCPP), VT340 */
                            case '|': {
                                WRN("DECSCPP not implemented\n");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                        /* <ESC>[ .. ; .. ?  */
                    default: {
                        switch (last_char) {
                            /* <ESC>[ Ps ; ... m - change one or more text attributes (SGR) */
                            case 'm': {
                                Vector_pop_n_char(&self->parser.active_sequence, 2); // 'm', '\0'
                                Vector_push_char(&self->parser.active_sequence, '\0');
                                Vt_handle_multi_argument_SGR(self,
                                                             self->parser.active_sequence,
                                                             NULL);
                            } break;

                            /* <ESC>[ Ps K - clear(erase) line right of cursor (EL)
                             * none/0 - right 1 - left 2 - all */
                            case 'K': {
                                MULTI_ARG_IS_ERROR
                                int arg = *seq == 'K' ? 0 : short_sequence_get_int_argument(seq);
                                switch (arg) {
                                    case 0:
                                        Vt_clear_right(self);
                                        break;
                                    case 1:
                                        Vt_clear_left(self);
                                        break;
                                    case 2:
                                        Vt_clear_left(self);
                                        Vt_clear_right(self);
                                        break;

                                    default:
                                        WRN("Unknown CSI(EL) sequence: %s\n", seq);
                                }
                            } break;

                            /* <ECS>[ Ps @ - Insert Ps Chars (ICH) */
                            case '@': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i) {
                                    Vt_insert_char_at_cursor_with_shift(self, self->blank_space);
                                }
                            } break;

                            /* <ESC>[ Ps a - move cursor right (forward) Ps lines (HPR) */
                            case 'a':
                            /* <ESC>[ Ps C - move cursor right (forward) Ps lines (CUF) */
                            case 'C': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, self->cursor.col + arg, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps L - Insert line at cursor shift rest down (IL) */
                            case 'L': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_insert_line(self);
                            } break;

                            /* <ESC>[ Ps D - move cursor left (back) Ps lines (CUB) */
                            case 'D': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                uint16_t new_col =
                                  arg >= self->cursor.col ? 0 : (self->cursor.col - arg);
                                Vt_move_cursor(self, new_col, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps A - move cursor up Ps lines (CUU) */
                            case 'A': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                int32_t new_row = Vt_cursor_row(self) <= (uint16_t)arg
                                                    ? 0
                                                    : (Vt_cursor_row(self) - arg);
                                Vt_move_cursor(self, self->cursor.col, new_row);
                            } break;

                            /* <ESC>[ Ps e - move cursor down Ps lines (VPR) */
                            case 'e':
                            /* <ESC>[ Ps B - move cursor down Ps lines (CUD) */
                            case 'B': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, self->cursor.col, Vt_cursor_row(self) + arg);
                            } break;

                            /* <ESC>[ Ps ` - move cursor to column Ps (CBT)*/
                            case '`':
                            /* <ESC>[ Ps G - move cursor to column Ps (CHA)*/
                            case 'G': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, arg - 1, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps J - Erase display (ED) - clear... */
                            case 'J': {
                                MULTI_ARG_IS_ERROR
                                if (*seq == 'J') /* ...from cursor to end of screen */
                                    Vt_erase_to_end(self);
                                else {
                                    int arg = short_sequence_get_int_argument(seq);
                                    switch (arg) {
                                        case 0:
                                            Vt_erase_to_end(self);
                                            break;

                                        case 1: /* ...from start to cursor */
                                            if (Vt_scroll_region_not_default(self) ||
                                                Vt_alt_buffer_enabled(self)) {
                                                Vt_clear_above(self);
                                            } else {
                                                Vt_clear_above(self);
                                                // Vt_scroll_out_above(self);
                                            }
                                            break;

                                        case 3: /* ...whole display + scrollback buffer */
                                            /* if (settings.allow_scrollback_clear) { */
                                            /*     Vt_clear_display_and_scrollback(self); */
                                            /* } */
                                            /* break; */

                                        case 2: /* ...whole display. Contents should not
                                                 * actually be removed, but saved to scroll
                                                 * history if no scroll region is set */
                                            if (self->alt_lines.buf) {
                                                Vt_clear_display_and_scrollback(self);
                                            } else {
                                                if (Vt_scroll_region_not_default(self)) {
                                                    Vt_clear_above(self);
                                                    Vt_erase_to_end(self);
                                                } else {
                                                    Vt_scroll_out_all_content(self);
                                                }
                                            }
                                            break;
                                    }
                                }
                            } break;

                            /* <ESC>[ Ps d - move cursor to row Ps (VPA) */
                            case 'd': {
                                MULTI_ARG_IS_ERROR
                                /* origin is 1:1 */
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, self->cursor.col, arg - 1);
                            } break;

                            /* <ESC>[ Ps ; Ps r - Set scroll region (top;bottom) (DECSTBM)
                             * default: full window */
                            case 'r': {
                                int32_t top, bottom;

                                if (*seq != 'r') {
                                    if (sscanf(seq, "%d;%d", &top, &bottom) == EOF) {
                                        WRN("invalid CSI(DECSTBM) sequence %s\n", seq);
                                        break;
                                    }
                                    if (top <= 0)
                                        top = 1;
                                    if (bottom <= 0)
                                        bottom = 1;
                                    --top;
                                    --bottom;
                                } else {
                                    top    = 0;
                                    bottom = CALL_FP(self->callbacks.on_number_of_cells_requested,
                                                     self->callbacks.user_data)
                                               .second -
                                             1;
                                }

                                self->scroll_region_top    = top;
                                self->scroll_region_bottom = bottom;
                            } break;

                            /* <ESC>[ Ps I - cursor forward ps tabulations (CHT) */
                            case 'I': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                uint16_t rt;
                                for (rt = 0; self->cursor.col + rt < Vt_col(self) && arg; ++rt) {
                                    if (self->tab_ruler[self->cursor.col + rt])
                                        --arg;
                                }
                                Vt_move_cursor(self, self->cursor.col + rt, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps Z - cursor backward ps tabulations (CBT) */
                            case 'Z': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                uint16_t lt;
                                for (lt = 0; self->cursor.col - lt && arg; ++lt) {
                                    if (self->tab_ruler[self->cursor.col - lt])
                                        --arg;
                                }
                                Vt_move_cursor(self, self->cursor.col - lt, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Pn g - tabulation clear (TBC) */
                            case 'g': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);

                                switch (arg) {
                                    case 0:
                                        self->tab_ruler[self->cursor.col] = false;
                                        break;
                                    case 3:
                                        Vt_clear_all_tabstops(self);
                                        break;
                                    default:;
                                }

                            } break;

                            /* no args: 1:1, one arg: x:1 */
                            /* <ESC>[ Py ; Px f - move cursor to Px-Py (HVP) (deprecated) */
                            case 'f':
                            /* <ESC>[ Py ; Px H - move cursor to Px-Py (CUP) */
                            case 'H': {
                                int32_t x = 1, y = 1;
                                if (*seq != 'H' && sscanf(seq, "%d;%d", &y, &x) == EOF) {
                                    WRN("invalid CUP/HVP sequence %s\n", seq);
                                    break;
                                }
                                if (x <= 0)
                                    x = 1;
                                if (y <= 0)
                                    y = 1;
                                --x;
                                --y;
                                Vt_move_cursor(self, x, y);
                            } break;

                            /* <ESC>[...c - Send device attributes (Primary DA) */
                            case 'c': {
                                /* report VT 102 */
                                Vt_output(self, "\e[?6c", 5);
                            } break;

                            /* <ESC>[...n - Device status report (DSR) */
                            case 'n': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg == 5) {
                                    /* 5 - is terminal ok
                                     *  ok - 0, not ok - 3 */
                                    Vt_output(self, "\e[0n", 4);
                                } else if (arg == 6) {
                                    /* 6 - report cursor position */
                                    Vt_output_formated(self,
                                                       "\e[%u;%uR",
                                                       Vt_cursor_row(self) + 1,
                                                       self->cursor.col + 1);
                                } else {
                                    WRN("Unimplemented DSR code: %d\n", arg);
                                }
                            } break;

                            /* <ESC>[ Ps M - Delete lines (default = 1) (DL) */
                            case 'M': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_delete_line(self);
                            } break;

                            /* <ESC>[ Ps S - Scroll up (default = 1) (SU) */
                            case 'S': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_scroll_up(self);
                            } break;

                            /* <ESC>[ Ps T - Scroll down (default = 1) (SD) */
                            case 'T': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_scroll_down(self);
                            } break;

                                /* <ESC>[ Ps X - Erase Ps Character(s) (default = 1) (ECH) */
                            case 'X': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_erase_chars(self, arg);
                            } break;

                                /* <ESC>[ Ps P - Delete Ps Character(s) (default = 1) (DCH) */
                            case 'P': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_delete_chars(self, arg);
                            } break;

                            /* <ESC>[ Ps b -  Repeat the preceding graphic character Ps times (REP)
                             * in xterm any cursor movement or SGR sequences after inserting the
                             * character cause this to have no effect */
                            case 'b': {
                                MULTI_ARG_IS_ERROR
                                if (likely(self->last_interted)) {
                                    int arg = short_sequence_get_int_argument(seq);
                                    if (arg <= 0)
                                        arg = 1;
                                    for (int i = 0; i < arg; ++i) {
                                        Vt_insert_char_at_cursor(self, *self->last_interted);
                                    }
                                }
                            } break;

                            /* <ESC>[ Ps i -  Media Copy (MC) Local printing related commands */
                            case 'i':
                                break;

                            /* <ESC>[  Pm... l - Reset Mode (RM) */
                            case 'l': {
                                MULTI_ARG_IS_ERROR
                                switch (short_sequence_get_int_argument(seq)) {
                                    case 4:
                                        // TODO: turn off IRM
                                        break;
                                }
                            } break;

                            /* <ESC>[u - Restore cursor (SCORC, also ANSI.SYS) */
                            /* <ESC>[Ps SP u - Set margin-bell volume (DECSMBV), VT520 */
                            case 'u':
                                if (*seq == 'u') {
                                    // TODO: cursor restore
                                } else {
                                    WRN("DECSMBV not implemented\n");
                                }
                                break;

                            /* <ESC>[s - Save cursor (SCOSC, also ANSI.SYS) available only when
                             * DECLRMM is disabled */
                            case 's': {
                                // TODO: save cursor
                            } break;

                            /* <ESC>[ Ps q - Manipulate keyboard LEDs (DECLL), VT100 */
                            case 'q':
                                break;

                            /* <ESC>[ Ps ; Ps ; Ps t - xterm windowOps (XTWINOPS)*/
                            case 't': {
                                int32_t nargs;
                                int32_t args[4];

                                /* Set omitted args to -1 */
                                for (nargs = 0; seq && nargs < 4 && *seq != 't'; ++nargs) {
                                    *(args + nargs) = *seq == ';' ? -1 : atoi(seq);
                                    seq             = strstr(seq, ";");
                                    if (seq)
                                        ++seq;
                                }
                                if (!nargs) {
                                    break;
                                }

                                switch (args[0]) {

                                    /* de-iconyfy */
                                    case 1:
                                        // TODO:
                                        break;

                                    /* iconyfy */
                                    case 2:
                                        // TODO:
                                        break;

                                    /* move window to args[1]:args[2] */
                                    case 3:
                                        // TODO:
                                        break;

                                    /* Resize window in pixels
                                     *
                                     * Omitted parameters reuse the current height or width. Zero
                                     * parameters use the display's height or width.
                                     *
                                     * FIXME: This should accounts for window decorations.
                                     */
                                    case 4:
                                        if (!settings.windowops_manip) {
                                            break;
                                        }

                                        if (nargs >= 2) {
                                            int32_t target_h = args[1];
                                            int32_t target_w = nargs >= 3 ? args[2] : -1;

                                            if (target_w == -1 || target_h == -1) {
                                                Pair_uint32_t current_dims =
                                                  CALL_FP(self->callbacks.on_window_size_requested,
                                                          self->callbacks.user_data);

                                                if (target_w == -1) {
                                                    target_w = current_dims.first;
                                                }
                                                if (target_h == -1) {
                                                    target_h = current_dims.second;
                                                }
                                            }
                                            if (target_w == 0 || target_h == 0) {
                                                // TODO: get display size
                                                WRN("Display size in XTWINOPS not implemented\n");
                                                break;
                                            }

                                            CALL_FP(self->callbacks.on_window_dimensions_set,
                                                    self->callbacks.user_data,
                                                    target_w,
                                                    target_h);
                                        } else {
                                            WRN("Invalid XTWINOPS sequence: %s\n", seq);
                                        }
                                        break;

                                    /* Raise window */
                                    case 5:
                                        // TODO:
                                        break;

                                    /* lower window */
                                    case 6:
                                        // TODO:
                                        break;

                                    /* Refresh window */
                                    case 7:
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        CALL_FP(self->callbacks.on_action_performed,
                                                self->callbacks.user_data);
                                        CALL_FP(self->callbacks.on_repaint_required,
                                                self->callbacks.user_data);
                                        break;

                                    /* Resize in cells */
                                    case 8: {
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        if (nargs >= 2) {
                                            int32_t target_rows = args[1];
                                            int32_t target_cols = nargs >= 3 ? args[2] : -1;

                                            Pair_uint32_t target_text_area_dims = CALL_FP(
                                              self->callbacks.on_window_size_from_cells_requested,
                                              self->callbacks.user_data,
                                              target_cols > 0 ? target_cols : 1,
                                              target_rows > 0 ? target_rows : 1);

                                            Pair_uint32_t currnet_text_area_dims =
                                              CALL_FP(self->callbacks.on_text_area_size_requested,
                                                      self->callbacks.user_data);

                                            if (target_cols == -1) {
                                                target_text_area_dims.first =
                                                  currnet_text_area_dims.first;
                                            }
                                            if (target_rows == -1) {
                                                target_text_area_dims.second =
                                                  currnet_text_area_dims.second;
                                            }
                                            if (target_cols == 0 || target_rows == 0) {
                                                WRN("Display size in XTWINOPS not implemented\n");
                                                break;
                                            }

                                            CALL_FP(self->callbacks.on_text_area_dimensions_set,
                                                    self->callbacks.user_data,
                                                    target_text_area_dims.first,
                                                    target_text_area_dims.second);
                                        } else {
                                            WRN("Invalid XTWINOPS sequence: %s\n", seq);
                                        }
                                    } break;

                                    /* Maximize */
                                    case 9: {
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        if (nargs >= 2) {
                                            switch (args[1]) {
                                                /* Unmaximize */
                                                case 0:
                                                    CALL_FP(
                                                      self->callbacks.on_window_maximize_state_set,
                                                      self->callbacks.user_data,
                                                      false);
                                                    break;

                                                /* invisible-island.net:
                                                 * `xterm uses Extended Window Manager Hints (EWMH)
                                                 * to maximize the window.  Some window managers
                                                 * have incomplete support for EWMH.  For instance,
                                                 * fvwm, flwm and quartz-wm advertise support for
                                                 * maximizing windows horizontally or vertically,
                                                 * but in fact equate those to the maximize
                                                 * operation.`
                                                 *
                                                 * Waylands xdg_shell/wl_shell have no concept of
                                                 * 'vertical/horizontal window maximization' so we
                                                 * should also treat that as regular maximization.
                                                 */
                                                /* Maximize */
                                                case 1:
                                                /* Maximize vertically */
                                                case 2:
                                                /* Maximize horizontally */
                                                case 3:
                                                    CALL_FP(
                                                      self->callbacks.on_window_maximize_state_set,
                                                      self->callbacks.user_data,
                                                      true);
                                                    break;

                                                default:
                                                    WRN("Invalid XTWINOPS: %s\n", seq);
                                            }
                                        } else {
                                            WRN("Invalid XTWINOPS: %s\n", seq);
                                        }
                                    } break;

                                        /* Fullscreen */
                                    case 10: {
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        if (nargs >= 2) {
                                            switch (args[1]) {
                                                /* Disable */
                                                case 0:
                                                    CALL_FP(self->callbacks
                                                              .on_window_fullscreen_state_set,
                                                            self->callbacks.user_data,
                                                            false);
                                                    break;

                                                /* Enable */
                                                case 1:
                                                    CALL_FP(self->callbacks
                                                              .on_window_fullscreen_state_set,
                                                            self->callbacks.user_data,
                                                            true);
                                                    break;

                                                    /* Toggle */
                                                case 2: {
                                                    bool current_state = CALL_FP(
                                                      self->callbacks.on_fullscreen_state_requested,
                                                      self->callbacks.user_data);
                                                    CALL_FP(self->callbacks
                                                              .on_window_fullscreen_state_set,
                                                            self->callbacks.user_data,
                                                            !current_state);
                                                } break;

                                                default:
                                                    WRN("Invalid XTWINOPS: %s\n", seq);
                                                    break;
                                            }
                                        } else {
                                            WRN("Invalid XTWINOPS: %s\n", seq);
                                        }
                                    } break;

                                    /* Report iconification state */
                                    case 11: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        bool is_minimized =
                                          CALL_FP(self->callbacks.on_minimized_state_requested,
                                                  self->callbacks.user_data);
                                        Vt_output_formated(self, "\e[%d", is_minimized ? 1 : 2);

                                    } break;

                                    /* Report window position */
                                    case 13: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Pair_uint32_t pos =
                                          CALL_FP(self->callbacks.on_window_position_requested,
                                                  self->callbacks.user_data);
                                        Vt_output_formated(self,
                                                           "\e[3;%d;%d;t",
                                                           pos.first,
                                                           pos.second);
                                    } break;

                                    /* Report window size in pixels */
                                    case 14: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self,
                                                           "\e[4;%d;%d;t",
                                                           self->ws.ws_xpixel,
                                                           self->ws.ws_ypixel);
                                    } break;

                                    /* Report text area size in chars */
                                    case 18: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self,
                                                           "\e[8;%d;%d;t",
                                                           Vt_col(self),
                                                           Vt_row(self));

                                    } break;

                                    /* Report window size in chars */
                                    case 19: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self,
                                                           "\e[9;%d;%d;t",
                                                           Vt_col(self),
                                                           Vt_row(self));

                                    } break;

                                    /* Report icon name */
                                    case 20:
                                        /* Report window title */
                                    case 21: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self, "\e]L%s\e\\", self->title);
                                    } break;

                                    /* push title to stack */
                                    case 22:
                                        Vt_push_title(self);
                                        LOG("Title stack push\n");
                                        break;

                                    /* pop title from stack */
                                    case 23:
                                        Vt_pop_title(self);
                                        LOG("Title stack pop\n");
                                        break;

                                    /* Resize window to args[1] lines (DECSLPP) */
                                    default: {
                                        // int arg = short_sequence_get_int_argument(seq);
                                        // uint32_t ypixels = gfx_pixels(arg, 0).first;
                                    }
                                }

                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);

                        } // end switch (last_char)
                    }
                } // end switch (second_last_char)
            }
        } // end switch (first_char)

        Vector_clear_char(&self->parser.active_sequence);
        self->parser.state = PARSER_STATE_LITERAL;
    }
}

static inline void Vt_alt_buffer_on(Vt* self, bool save_mouse)
{
    if (self->alt_lines.buf) {
        return;
    }
    Vt_clear_all_proxies(self);
    Vt_visual_scroll_reset(self);
    Vt_select_end(self);
    self->last_interted = NULL;
    self->alt_lines     = self->lines;
    self->lines         = Vector_new_VtLine(self);
    for (uint16_t i = 0; i < Vt_row(self); ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
    }
    if (save_mouse) {
        self->alt_cursor_pos  = self->cursor.col;
        self->alt_active_line = self->cursor.row;
    }
    self->cursor.row = 0;
}

static inline void Vt_alt_buffer_off(Vt* self, bool save_mouse)
{
    if (self->alt_lines.buf) {
        self->last_interted = NULL;
        Vt_select_end(self);
        Vector_destroy_VtLine(&self->lines);
        self->lines          = self->alt_lines;
        self->alt_lines.buf  = NULL;
        self->alt_lines.size = 0;
        if (save_mouse) {
            self->cursor.col = self->alt_cursor_pos;
            self->cursor.row = self->alt_active_line;
        }
        self->scroll_region_top    = 0;
        self->scroll_region_bottom = Vt_row(self) - 1;
        Vt_visual_scroll_reset(self);
    }
}

static bool Vt_alt_buffer_enabled(Vt* self)
{
    return self->alt_lines.buf;
}

/**
 * Interpret a single argument SGR command */
__attribute__((hot)) static void Vt_handle_single_argument_SGR(Vt*     self,
                                                               char*   command,
                                                               VtRune* opt_target)
{
    int cmd = *command ? atoi(command) : 0;

    VtRune* r = OR(opt_target, &self->parser.char_state);

#define MAYBE_DISABLE_ALL_UNDERLINES                                                               \
    if (!settings.allow_multiple_underlines) {                                                     \
        r->underlined      = false;                                                                \
        r->doubleunderline = false;                                                                \
        r->curlyunderline  = false;                                                                \
    }

    switch (cmd) {
        /**'Enable' character attribs */

        /* Normal (default) */
        case 0:
            Vt_reset_text_attribs(self, opt_target);
            break;

        /* Bold, VT100 */
        case 1:
            if (r->rune.style == VT_RUNE_ITALIC) {
                r->rune.style = VT_RUNE_BOLD_ITALIC;
            } else {
                r->rune.style = VT_RUNE_BOLD;
            }
            break;

        /* Faint/dim/decreased intensity, ECMA-48 2nd */
        case 2:
            r->dim = true;
            break;

        /* Italicized, ECMA-48 2nd */
        case 3:
            if (r->rune.style == VT_RUNE_BOLD) {
                r->rune.style = VT_RUNE_BOLD_ITALIC;
            } else {
                r->rune.style = VT_RUNE_ITALIC;
            }
            break;

        /* Underlined */
        case 4:
            MAYBE_DISABLE_ALL_UNDERLINES
            r->underlined = true;
            break;

        /* Slow (less than 150 per minute) blink */
        case 5:
        /* Fast blink (MS-DOS) (most terminals that implement this use the same speed) */
        case 6:
            r->blinkng = true;
            break;

        /* Inverse
         * There is no clear definition of what this should actually do, but reversing all colors,
         * after bg and fg were determined, seems to be the widely accepted behavior. */
        case 7:
            r->invert = true;
            break;

        /* Invisible, i.e., hidden, ECMA-48 2nd, VT300 */
        case 8:
            r->hidden = true;
            break;

        /* Crossed-out characters, ECMA-48 3rd */
        case 9:
            r->strikethrough = true;
            break;

        /* Fraktur (not widely supported) */
        /* case 20: */
        /*     break; */

        /* Doubly-underlined, ECMA-48 3rd */
        case 21:
            MAYBE_DISABLE_ALL_UNDERLINES
            r->doubleunderline = true;
            break;

        /* Framed (not widely supported) */
        /* case 51: */
        /*     break; */

        /* Encircled (not widely supported) */
        /* case 52: */
        /*     break; */

        /* Overlined (widely supported extension) */
        case 53:
            r->overline = true;
            break;

            /* Superscript (non-standard extension) */
            /* case 73: */
            /*     break; */

            /* Subscript (non-standard extension) */
            /* case 74: */
            /*     break; */

            /* Ideogram */
            /* case 60 ... 64: */
            /*     break; */

            /** 'Disable' character attribs */

        /* Normal (neither bold nor faint), ECMA-48 3rd
         *
         * It works this way because 'Bold' used to be 'bright'/'high intensity', the oposite of
         * 'faint'. SGR 22 was supposed to reset the color intensity to default. At some point
         * terminals started representing 'bright' characters as bold instead of changing the color.
         */
        case 22:
            if (r->rune.style == VT_RUNE_BOLD_ITALIC) {
                r->rune.style = VT_RUNE_ITALIC;
            } else if (r->rune.style == VT_RUNE_BOLD) {
                r->rune.style = VT_RUNE_NORMAL;
            }
            r->dim = false;
            break;

        /* Not italicized, ECMA-48 3rd */
        case 23:
            if (r->rune.style == VT_RUNE_BOLD_ITALIC) {
                r->rune.style = VT_RUNE_BOLD;
            } else if (r->rune.style == VT_RUNE_ITALIC) {
                r->rune.style = VT_RUNE_NORMAL;
            }
            break;

        /*  Not underlined, ECMA-48 3rd */
        case 24:
            r->underlined = false;
            break;

        /* Steady (not blinking), ECMA-48 3rd */
        case 25:
            r->blinkng = false;
            break;

        /* Positive (not inverse), ECMA-48 3rd */
        case 27:
            r->invert = false;
            break;

        /* Visible (not hidden), ECMA-48 3rd, VT300 */
        case 28:
            r->hidden = false;
            break;

        /* Not crossed-out, ECMA-48 3rd */
        case 29:
            r->strikethrough = false;
            break;

        /* Set foreground color to default, ECMA-48 3rd */
        case 39:
            Vt_set_fg_color_default(self, opt_target);
            break;

        /* Set background color to default, ECMA-48 3rd */
        case 49:
            Vt_set_bg_color_default(self, opt_target);
            break;

        /* Set underline color to default (widely supported extension) */
        case 59:
            r->line_color_not_default = false;
            break;

            /* Disable all ideogram attributes */
            /* case 65: */
            /*     break; */

        case 30 ... 37:
            Vt_set_fg_color_palette(self, cmd - 30, opt_target);
            break;

        case 40 ... 47:
            Vt_set_bg_color_palette(self, cmd - 40, opt_target);
            break;

        case 90 ... 97:
            Vt_set_fg_color_palette(self, cmd - 82, opt_target);
            break;

        case 100 ... 107:
            Vt_set_bg_color_palette(self, cmd - 92, opt_target);
            break;

        default:
            WRN("Unknown SGR code: %d\n", cmd);
    }
}

/**
 * Interpret an SGR sequence
 *
 * SGR codes are separated by one ';' or ':', some values require a set number of following
 * 'arguments'. 'Commands' may be combined into a single sequence. A ';' without any text should be
 * interpreted as a 0 (CSI ; 3 m == CSI 0 ; 3 m), but ':' should not
 * (CSI 58:2::130:110:255 m == CSI 58:2:130:110:255 m)" */
static void Vt_handle_multi_argument_SGR(Vt* self, Vector_char seq, VtRune* opt_target)
{
    Vector_Vector_char tokens = string_split_on(seq.buf, ";", ":", NULL);
    for (Vector_char* token = NULL; (token = Vector_iter_Vector_char(&tokens, token));) {
        Vector_char* args[] = { token, NULL, NULL, NULL, NULL };

        /* color change 'commands' */
        if (!strcmp(token->buf + 1, "38") /* foreground */ ||
            !strcmp(token->buf + 1, "48") /* background */ ||
            !strcmp(token->buf + 1, "58") /* underline  */) {
            /* next argument determines how the color will be set and final number of args */

            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                (args[2] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                if (!strcmp(args[1]->buf + 1, "5")) {
                    /* from 256 palette (one argument) */
                    uint32_t idx = MIN(atoi(args[2]->buf + 1), 255);
                    if (args[0]->buf[1] == '3') {
                        Vt_set_fg_color_palette(self, idx, opt_target);
                    } else if (args[0]->buf[1] == '4') {
                        Vt_set_bg_color_palette(self, idx, opt_target);
                    } else if (args[0]->buf[1] == '5') {
                        Vt_set_line_color_palette(self, idx, opt_target);
                    }
                } else if (!strcmp(args[1]->buf + 1, "2")) {
                    /* sent as 24-bit rgb (three arguments) */
                    if ((args[3] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                        (args[4] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                        uint32_t c[3] = { MIN(atoi(args[2]->buf + 1), 255),
                                          MIN(atoi(args[3]->buf + 1), 255),
                                          MIN(atoi(args[4]->buf + 1), 255) };

                        if (args[0]->buf[1] == '3') {
                            ColorRGB clr = { .r = c[0], .g = c[1], .b = c[2] };
                            Vt_set_fg_color_custom(self, clr, opt_target);
                        } else if (args[0]->buf[1] == '4') {
                            ColorRGBA clr = { .r = c[0], .g = c[1], .b = c[2], .a = 255 };
                            Vt_set_bg_color_custom(self, clr, opt_target);
                        } else if (args[0]->buf[1] == '5') {
                            ColorRGB clr = { .r = c[0], .g = c[1], .b = c[2] };
                            Vt_set_line_color_custom(self, clr, opt_target);
                        }
                    }
                }
            }
        } else if (!strcmp(token->buf + 1, "4")) {
            /* possible curly underline */
            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                /* enable this only on "4:3" not "4;3" */
                if (!strcmp(args[1]->buf, ":3")) {
                    VtRune* r = OR(opt_target, &self->parser.char_state);
                    if (!settings.allow_multiple_underlines) {
                        r->underlined      = false;
                        r->doubleunderline = false;
                    }
                    r->curlyunderline = true;
                } else {
                    Vt_handle_single_argument_SGR(self, args[0]->buf + 1, opt_target);
                    token = Vector_iter_back_Vector_char(&tokens, token);
                }
            } else {
                Vt_handle_single_argument_SGR(self, args[0]->buf + 1, opt_target);
                break; // end of sequence
            }
        } else {
            Vt_handle_single_argument_SGR(self, token->buf + 1, opt_target);
        }
    }
    Vector_destroy_Vector_char(&tokens);
}

static void Vt_handle_APC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);
    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {
        Vector_push_char(&self->parser.active_sequence, '\0');
        const char* seq = self->parser.active_sequence.buf;
        char*       str = pty_string_prettyfy(seq, strlen(seq));
        WRN("Unknown APC: %s\n", str);
        free(str);
        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_DCS(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {
        Vector_push_char(&self->parser.active_sequence, '\0');

        const char* seq = self->parser.active_sequence.buf;
        switch (*seq) {
            /* Terminal image protocol */
            case 'G':
                break;

            /* sixel or ReGIS */
            case '0':
                break;

            /* Synchronized update */
            case '=':
                if ((seq[1] == '1' || seq[1] == '2') && seq[2] == 's') {
                    if (seq[1] == '1') {
                        /* Begin synchronized update (BSU) (iTerm2)
                         *
                         * Meant to reduce flicker when redrawing large portions of the screen.
                         * The terminal should create a snapshot of the screen content and display
                         * that until ESU is sent (or some kind of timeout in case the program
                         * is killed/crashes).
                         */

                        // TODO: some way to deal with proxy object updates
                    } else {
                        /* End synchronized update (ESU) (iTerm2) */
                    }
                }
                return;
            default:;
        }

        char* str =
          pty_string_prettyfy(self->parser.active_sequence.buf, self->parser.active_sequence.size);
        WRN("Unknown DCS: %s\n", str);
        free(str);

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_PM(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);
    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {
        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_OSC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {

        if (*Vector_last_char(&self->parser.active_sequence) == '\\') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        if (*Vector_last_char(&self->parser.active_sequence) == '\e') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        if (*Vector_last_char(&self->parser.active_sequence) == '\a') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        Vector_push_char(&self->parser.active_sequence, '\0');
        char*    seq  = self->parser.active_sequence.buf;
        uint32_t arg  = 0;
        char*    text = seq;

        if (isdigit(*seq)) {
            arg = strtoul(seq, &text, 10);
            if (text && !(*text == ';' || *text == ':')) {
                text = seq;
            } else if (text && *text) {
                ++text;
            }
        } else {
            WRN("no numerical argument in OSC \'%s\'\n", seq);
        }

        switch (arg) {
            /* Change Icon Name and Window Title */
            case 0:
            /* Change Icon Name */
            case 1:
            /* Change Window Title */
            case 2:
                Vt_set_title(self, text);
                break;

            /* Set X property on top-level window (prop=val) */
            case 3:
                // TODO:
                WRN("OSC 3 not implemented\n");
                break;

            /* Modify regular color palette
             *
             * Any number of c/spec pairs may be given
             *
             * If a "?" is given rather than a name or RGB specification, xterm replies with a
             * control sequence of the same form which can be used to set the corresponding color.
             */
            case 4: {
                seq += 2;
                char *arg_idx, *arg_clr;
                while ((arg_idx = strsep(&seq, ";")) && (arg_clr = strsep(&seq, ";"))) {
                    uint32_t index = atoi(arg_idx);
                    if (index >= ARRAY_SIZE(self->colors.palette_256)) {
                        continue;
                    }
                    if (*arg_clr == '?') {
                        ColorRGB color = self->colors.palette_256[index];
                        Vt_output_formated(self,
                                           "\e]4;%u;rgb:%x/%x/%x\a",
                                           index,
                                           color.r,
                                           color.g,
                                           color.b);
                    } else {
                        set_rgb_color_from_xterm_string(&self->colors.palette_256[index], arg_clr);
                    }
                }
                Vt_clear_all_proxies(self);
                CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
            } break;

            /* 104 ; c Reset Color Number
             * It is reset to the color specified by the corresponding X resource. Any number of
             * c parameters may be given.  These parameters correspond to the ANSI colors 0-7,
             * their bright versions 8-15, and if supported, the remainder of the 88-color or
             * 256-color table. If no parameters are given, the entire table will be reset.
             */
            case 104: {
                seq += 3;
                if (!*seq) {
                    Vt_init_color_palette(self);
                } else {
                    ++seq;
                    for (char* index; (index = strsep(&seq, ";"));) {
                        uint32_t idx = atoi(index);
                        if (idx >= ARRAY_SIZE(self->colors.palette_256)) {
                            continue;
                        }
                        Vt_reset_color_palette_entry(self, idx);
                    }
                }
                Vt_clear_all_proxies(self);
                CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
            } break;

            /* Modify special color palette */
            case 5:
            /* Enable/disable special color */
            case 6:

            /* 105 ; c Reset Special Color Number c.
             * It is reset to the color specified by the corresponding X resource. Any number
             * of c parameters may be given.  These parameters corre- spond to the special
             * colors which can be set using an OSC 5 control (or by adding the maximum number
             * of colors using an OSC 4  control).
             */
            case 105:

            /* 1 0 6 ; c ; f  Enable/disable Special Color Number c.
             * The second parameter tells xterm to enable the corresponding color mode if
             * nonzero, disable it if zero
             */
            case 106:
                // TODO:
                WRN("Dynamic colors not implemented \'%s\'\n", seq);
                break;

            /* pwd info as URI */
            case 7: {
                free(self->work_dir);
                char* uri = seq + 2; // 7;
                if (streq_wildcard(uri, "file:*") && strnlen(uri, 8) == 8) {
                    uri += 7; // skip 'file://'

                    /* skip hostname */
                    while (*uri && *uri != '/') {
                        ++uri;
                    }
                    self->work_dir = strdup(uri);
                    LOG("Program changed work dir to \'%s\'\n", self->work_dir);
                } else {
                    self->work_dir = NULL;
                    WRN("Bad URI \'%s\', scheme is not \'file\'\n", uri);
                }
            } break;

                /* mark text as hyperlink with URL */
            case 8: {
                strsep(&seq, ";");
                strsep(&seq, ";");
                char* link = strsep(&seq, ";");

                if (strnlen(link, 1)) {
                    free(self->active_hyperlink);
                    self->active_hyperlink = strdup(link);
                } else {
                    free(self->active_hyperlink);
                    self->active_hyperlink = NULL;
                }
            } break;

            /* Send Growl(some kind of notification daemon for OSX) notification (iTerm2) */
            case 9:
                CALL_FP(self->callbacks.on_desktop_notification_sent,
                        self->callbacks.user_data,
                        NULL,
                        seq + 2 /* 9; */);
                break;

            /* Set title for tab (konsole extension) */
            case 30:
                break;

            /* Set dynamic colors for xterm colorOps
             *
             * If a "?" is given rather than a name or RGB specification, xterm replies with a
             * control sequence of the same form which can be used to set the corresponding
             * dynamic color.
             */
            case 10 ... 19: {
                bool query = *(seq + 3) == '?';
                if (query) {
                    switch (arg) {
                        /* VT100 text foreground color */
                        case 10: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%3u/%3u/%3u\a",
                                               arg,
                                               self->colors.fg.r,
                                               self->colors.fg.g,
                                               self->colors.fg.b);
                        } break;

                        /* VT100 text background color */
                        case 11: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%3u/%3u/%3u\a",
                                               arg,
                                               self->colors.bg.r,
                                               self->colors.bg.g,
                                               self->colors.bg.b);
                        } break;

                        /* highlight background color */
                        case 17: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%3u/%3u/%3u\a",
                                               arg,
                                               self->colors.highlight.bg.r,
                                               self->colors.highlight.bg.g,
                                               self->colors.highlight.bg.b);
                        } break;

                        /* highlight foreground color */
                        case 19: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%3u/%3u/%3u\a",
                                               arg,
                                               self->colors.highlight.fg.r,
                                               self->colors.highlight.fg.g,
                                               self->colors.highlight.fg.b);
                        } break;

                        /* Tektronix background color */
                        case 16:
                        /* pointer foreground color */
                        case 13:
                        /* pointer background color */
                        case 14:
                        /* text cursor color */
                        case 12:
                        /* Tektronix foreground color */
                        case 15:
                        /* Tektronix cursor color */
                        case 18:
                            WRN("Unimplemented color \'%d\'\n", arg);
                            break;

                        default:
                            ASSERT_UNREACHABLE;
                            break;
                    }
                } else {
                    /*
                     * At least one parameter is expected. Each successive parameter changes the
                     * next color in the list.
                     */

                    char* sequence_arg = seq + 3;
                    while (sequence_arg && *sequence_arg) {
                        switch (arg) {
                            /* VT100 text foreground color */
                            case 10: {
                                set_rgb_color_from_xterm_string(&self->colors.fg, sequence_arg);
                            } break;

                            /* VT100 text background color */
                            case 11: {
                                set_rgba_color_from_xterm_string(&self->colors.bg, sequence_arg);
                            } break;

                            /* highlight background color */
                            case 17: {
                                set_rgba_color_from_xterm_string(&self->colors.highlight.bg,
                                                                 sequence_arg);
                            } break;

                            /* highlight foreground color */
                            case 19: {
                                set_rgb_color_from_xterm_string(&self->colors.highlight.fg,
                                                                sequence_arg);
                            } break;

                            /* Tektronix background color */
                            case 16:
                            /* text cursor color */
                            case 12:
                            /* Tektronix foreground color */
                            case 15:
                            /* Tektronix cursor color */
                            case 18:
                            /* pointer foreground color */
                            case 13:
                            /* pointer background color */
                            case 14:
                            default:
                                break;
                        }
                        sequence_arg = strstr(sequence_arg, ";");
                        if (!sequence_arg || !*sequence_arg) {
                            break;
                        }
                        ++sequence_arg;
                        ++arg;
                    }
                    Vt_clear_all_proxies(self);
                    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
                }
            } break;

            /* Coresponding resets */
            case 110 ... 119: {
                switch (arg - 100) {
                    /* VT100 text foreground color */
                    case 10:
                    /* text cursor color */
                    case 12:
                    /* Tektronix foreground color */
                    case 15:
                    /* Tektronix cursor color */
                    case 18: {
                        self->colors.fg = settings.fg;
                    } break;

                    /* VT100 text background color */
                    case 11:
                    /* Tektronix background color */
                    case 16: {
                        self->colors.bg = settings.bg;
                    } break;

                    /* pointer foreground color */
                    case 13:
                        break;

                    /* pointer background color */
                    case 14:
                        break;

                    /* highlight background color */
                    case 17: {
                        self->colors.highlight.bg = settings.bghl;
                    } break;

                    /* highlight foreground color */
                    case 19: {
                        self->colors.highlight.fg = settings.fghl;
                    } break;

                    default:
                        ASSERT_UNREACHABLE;
                        break;
                }
            } break;

            case 50:
                WRN("xterm fontOps not implemented\n");
                break;

            /* Manipulate selection data */
            case 52:
                WRN("Selection manipulation not implemented\n");
                break;

            /* Send desktop notification (rxvt extension)
             * OSC 777;notify;title;body ST */
            case 777: {
                Vector_Vector_char tokens = string_split_on(seq + 4 /* 777; */, ";", NULL, NULL);
                if (tokens.size >= 2) {
                    if (!strcmp(tokens.buf[0].buf + 1, "notify")) {
                        if (tokens.size == 2) {
                            CALL_FP(self->callbacks.on_desktop_notification_sent,
                                    self->callbacks.user_data,
                                    NULL,
                                    tokens.buf[1].buf + 1);
                        } else if (tokens.size == 3) {
                            CALL_FP(self->callbacks.on_desktop_notification_sent,
                                    self->callbacks.user_data,
                                    tokens.buf[1].buf + 1,
                                    tokens.buf[2].buf + 1);
                        } else {
                            WRN("Unexpected argument in OSC 777 \'%s\'\n", seq);
                        }
                    } else {
                        WRN("Second argument to OSC 777 \'%s\' is not \'notify\'\n", seq);
                    }
                } else {
                    WRN("OSC 777 \'%s\' not enough arguments\n", seq);
                }

                Vector_destroy_Vector_char(&tokens);
            } break;

            default:
                WRN("Unknown OSC: %s\n", self->parser.active_sequence.buf);
        }

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_push_title(Vt* self)
{
    if (self->title) {
        Vector_push_DynStr(&self->title_stack, (DynStr){ .s = strdup(self->title) });
    }
}

static void Vt_pop_title(Vt* self)
{
    if (self->title_stack.size) {
        Vt_set_title(self, Vector_last_DynStr(&self->title_stack)->s);
        Vector_pop_DynStr(&self->title_stack);
    } else {
        free(self->title);
        self->title = NULL;
    }
}

static void Vt_reset_text_attribs(Vt* self, VtRune* opt_target)
{
    VtRune* r       = OR(opt_target, &self->parser.char_state);
    Rune    oldrune = r->rune;
    memset(r, 0, sizeof(self->parser.char_state));
    r->rune       = oldrune;
    r->rune.style = VT_RUNE_NORMAL;
    Vt_set_bg_color_default(self, opt_target);
    Vt_set_fg_color_default(self, opt_target);
    Vt_set_line_color_default(self, opt_target);
}

/**
 * Move cursor to first column */
static void Vt_carriage_return(Vt* self)
{
    self->last_interted = NULL;
    Vt_move_cursor(self, 0, Vt_cursor_row(self));
}

/**
 * make a new empty line at cursor position, scroll down contents below */
static void Vt_insert_line(Vt* self)
{
    self->last_interted = NULL;
    Vector_insert_at_VtLine(&self->lines, self->cursor.row, VtLine_new());
    Vt_empty_line_fill_bg(self, self->cursor.row);

    size_t rem_idx = MIN(Vt_get_scroll_region_bottom(self), Vt_bottom_line(self));
    Vector_remove_at_VtLine(&self->lines, rem_idx, 1);

    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

/**
 * the same as insert line, but adds before cursor line */
static void Vt_reverse_line_feed(Vt* self)
{
    self->last_interted = NULL;
    if (self->cursor.row == Vt_get_scroll_region_top(self)) {
        Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_bottom(self), 1);
        Vector_insert_at_VtLine(&self->lines, self->cursor.row, VtLine_new());
        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else if (Vt_cursor_row(self)) {
        Vt_move_cursor(self, self->cursor.col, Vt_cursor_row(self) - 1);
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * delete active line, content below scrolls up */
static void Vt_delete_line(Vt* self)
{
    self->last_interted = NULL;

    Vector_remove_at_VtLine(&self->lines, self->cursor.row, 1);

    size_t insert_idx = MIN(Vt_get_scroll_region_bottom(self), Vt_bottom_line(self));
    Vector_insert_at_VtLine(&self->lines, insert_idx, VtLine_new());
    Vt_empty_line_fill_bg(self, MIN(Vt_get_scroll_region_bottom(self), Vt_bottom_line(self)));

    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

static void Vt_scroll_up(Vt* self)
{
    self->last_interted  = NULL;
    size_t  insert_idx   = MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self)) + 1;
    VtLine* insert_point = Vector_at_VtLine(&self->lines, insert_idx);
    Vector_insert_VtLine(&self->lines, insert_point, VtLine_new());

    size_t new_line_idx = MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self));
    Vt_empty_line_fill_bg(self, new_line_idx);

    Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_top(self) - 1, 1);
    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

static void Vt_scroll_down(Vt* self)
{
    self->last_interted = NULL;
    Vector_insert_at_VtLine(&self->lines, Vt_get_scroll_region_top(self), VtLine_new());
    size_t rm_idx = MAX(Vt_top_line(self), Vt_get_scroll_region_bottom(self));
    Vector_remove_at_VtLine(&self->lines, rm_idx, 1);
    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

static inline void Vt_erase_to_end(Vt* self)
{
    for (size_t i = self->cursor.row + 1; i <= Vt_bottom_line(self); ++i) {
        Vector_clear_VtRune(&self->lines.buf[i].data);
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_right(self);
}

static inline void Vt_handle_backspace(Vt* self)
{
    if (self->cursor.col)
        Vt_move_cursor(self, self->cursor.col - 1, Vt_cursor_row(self));
    else if (self->modes.reverse_wraparound) {
        uint16_t r = Vt_cursor_row(self);
        Vt_move_cursor(self, Vt_col(self) - 1, r ? (r - 1) : 0);
    }
}

/**
 * Overwrite characters with colored space */
static inline void Vt_erase_chars(Vt* self, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        size_t idx = self->cursor.col + i;
        if (idx >= Vt_cursor_line(self)->data.size) {
            Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
        } else {
            Vt_cursor_line(self)->data.buf[idx] = self->parser.char_state;
        }
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * remove characters at cursor, remaining content scrolls left */
static void Vt_delete_chars(Vt* self, size_t n)
{
    /* Trim if line is longer than screen area */
    if (Vt_cursor_line(self)->data.size > Vt_col(self)) {
        Vector_pop_n_VtRune(&Vt_cursor_line(self)->data,
                            Vt_cursor_line(self)->data.size - Vt_col(self));
    }

    size_t rm_size = Vt_cursor_line(self)->data.size == self->cursor.col
                       ? Vt_cursor_line(self)->data.size - self->cursor.col
                       : Vt_cursor_line(self)->data.size;
    Vector_remove_at_VtRune(&Vt_cursor_line(self)->data, self->cursor.col, MIN(rm_size, n));

    /* Fill line to the cursor position with spaces with original propreties
     * before scolling so we get the expected result, when we... */
    VtRune tmp = self->parser.char_state;

    Vt_reset_text_attribs(self, NULL);

    if (Vt_cursor_line(self)->data.size >= 2) {
        self->parser.char_state.bg_data =
          Vt_cursor_line(self)->data.buf[Vt_cursor_line(self)->data.size - 2].bg_data;

        self->parser.char_state.bg_is_palette_entry =
          Vt_cursor_line(self)->data.buf[Vt_cursor_line(self)->data.size - 2].bg_is_palette_entry;
    } else {
        Vt_set_bg_color_default(self, NULL);
    }

    for (uint16_t i = Vt_cursor_line(self)->data.size - 1; i < Vt_col(self); ++i) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
    }

    self->parser.char_state = tmp;

    if (Vt_cursor_line(self)->data.size > Vt_col(self)) {
        Vector_pop_n_VtRune(&Vt_cursor_line(self)->data,
                            Vt_cursor_line(self)->data.size - Vt_col(self));
    }

    /* ...add n spaces with currently set attributes to the end */
    for (size_t i = 0; i < n && self->cursor.col + i < Vt_col(self); ++i) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
    }

    /* Trim to screen size again */
    if (self->lines.buf[self->cursor.row].data.size > Vt_col(self)) {
        Vector_pop_n_VtRune(&Vt_cursor_line(self)->data,
                            Vt_cursor_line(self)->data.size - Vt_col(self));
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static inline void Vt_scroll_out_all_content(Vt* self)
{
    int64_t to_add = 0;
    for (size_t i = Vt_visual_bottom_line(self); i > Vt_visual_top_line(self); --i) {
        if (self->lines.buf[i].data.size) {
            to_add += i;
            break;
        }
    }
    to_add -= Vt_visual_top_line(self);
    to_add += 1;

    for (int64_t i = 0; i < to_add; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }
    if (to_add > 0) {
        self->cursor.row += to_add;
    }
}

static inline void Vt_scroll_out_above(Vt* self)
{
    size_t to_add = Vt_cursor_row(self);
    for (size_t i = 0; i <= to_add; ++i) {
        size_t insert_point = self->cursor.row;
        Vector_insert_at_VtLine(&self->lines, insert_point, VtLine_new());
        Vt_empty_line_fill_bg(self, insert_point);
        ++self->cursor.row;
    }
}

static inline void Vt_clear_above(Vt* self)
{
    for (size_t i = Vt_visual_top_line(self); i < self->cursor.row; ++i) {
        self->lines.buf[i].data.size = 0;
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_left(self);
}

static inline void Vt_clear_display_and_scrollback(Vt* self)
{
    Vt_visual_scroll_reset(self);
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
    Vector_destroy_VtLine(&self->lines);
    self->lines = Vector_new_VtLine(self);

    for (uint16_t i = 0; i < Vt_row(self); ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }
    self->cursor.row = 0;
}

/**
 * Clear active line left of cursor and fill it with whatever character
 * attributes are set */
static inline void Vt_clear_left(Vt* self)
{
    size_t to_add = 0;
    if (self->cursor.col >= Vt_cursor_line(self)->data.size) {
        to_add = self->cursor.col - Vt_cursor_line(self)->data.size;
    }
    for (size_t i = 0; i < to_add; ++i) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
    }
    for (size_t i = 0; i <= self->cursor.col; ++i) {
        Vt_cursor_line(self)->data.buf[i] = self->parser.char_state;
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Clear active line right of cursor and fill it with whatever character
 * attributes are set */
static inline void Vt_clear_right(Vt* self)
{
    for (int32_t i = self->cursor.col; i <= (int32_t)Vt_col(self); ++i) {
        if (i + 1 <= (int32_t)Vt_cursor_line(self)->data.size) {
            Vt_cursor_line(self)->data.buf[i] = self->parser.char_state;
        } else {
            Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
        }
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static void Vt_overwrite_char_at(Vt* self, size_t column, size_t row, VtRune c)
{
    VtLine* line = Vt_line_at(self, row);
    while (line->data.size <= column) {
        Vector_push_VtRune(&line->data, self->blank_space);
    }
    line->data.buf[column] = c;
}

/**
 * Insert character literal at cursor position, deal with reaching column limit */
__attribute__((hot)) static void Vt_insert_char_at_cursor(Vt* self, VtRune c)
{
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);

    if (self->wrap_next && !self->modes.no_wraparound) {
        self->cursor.col = 0;
        Vt_insert_new_line(self);
        Vt_cursor_line(self)->rejoinable = true;
    }

    while (Vt_cursor_line(self)->data.size <= self->cursor.col) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->blank_space);
    }

    VtRune* insert_point = &self->lines.buf[self->cursor.row].data.buf[self->cursor.col];
    if (likely(memcmp(insert_point, &c, sizeof(VtRune)))) {
        Vt_mark_proxy_damaged_cell(self, self->cursor.row, self->cursor.col);
        *Vt_cursor_cell(self) = c;
    }

    self->last_interted = Vt_cursor_cell(self);
    ++self->cursor.col;

    int width;
#ifndef NOUTF8PROC
    width = utf8proc_charwidth(c.rune.code);
#else
    width = wcwidth(c.rune.code);
#endif
    if (unlikely(width > 1)) {
        VtRune tmp    = c;
        tmp.rune.code = VT_RUNE_CODE_WIDE_TAIL;
        for (int i = 0; i < (width - 1); ++i) {
            if (Vt_cursor_line(self)->data.size <= self->cursor.col)
                Vector_push_VtRune(&Vt_cursor_line(self)->data, tmp);
            else
                *Vt_cursor_cell(self) = tmp;
            ++self->cursor.col;
        }
    }

    self->wrap_next  = self->cursor.col >= (size_t)Vt_col(self);
    self->cursor.col = MIN(self->cursor.col, (Vt_col(self) - 1));
}

static void Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c)
{
    if (unlikely(self->cursor.col >= (size_t)Vt_col(self))) {
        if (unlikely(self->modes.no_wraparound)) {
            --self->cursor.col;
        } else {
            self->cursor.col = 0;
            Vt_insert_new_line(self);
            Vt_cursor_line(self)->rejoinable = true;
        }
    }
    Vector_insert_at_VtRune(&Vt_cursor_line(self)->data, self->cursor.col, c);
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static inline void Vt_empty_line_fill_bg(Vt* self, size_t idx)
{
    ASSERT(self->lines.buf[idx].data.size == 0, "line is empty");

    Vt_mark_proxy_fully_damaged(self, idx);
    if (!ColorRGBA_eq(Vt_active_bg_color(self), self->colors.bg)) {
        for (uint16_t i = 0; i < Vt_col(self); ++i) {
            Vector_push_VtRune(&self->lines.buf[idx].data, self->parser.char_state);
        }
    }
}

/**
 * Move one line down or insert a new one, scrolls if region is set */
static inline void Vt_insert_new_line(Vt* self)
{
    if (self->cursor.row == Vt_get_scroll_region_bottom(self) &&
        Vt_scroll_region_not_default(self)) {
        Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_top(self), 1);
        Vector_insert_at_VtLine(&self->lines, self->cursor.row, VtLine_new());
        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else if (Vt_bottom_line(self) == self->cursor.row) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }
    Vt_move_cursor(self, self->cursor.col, Vt_cursor_row(self) + 1);
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Move cursor to given location (@param rows is relative to the screen!) */
static inline void Vt_move_cursor(Vt* self, uint16_t column, uint16_t rows)
{
    self->wrap_next = false;
    size_t max_row, min_row;
    if (self->modes.origin) {
        rows += (Vt_get_scroll_region_top(self) - Vt_top_line(self));
        min_row = Vt_get_scroll_region_top(self);
        max_row = Vt_get_scroll_region_bottom(self);
    } else {
        max_row = Vt_bottom_line(self);
        min_row = Vt_top_line(self);
    }
    self->last_interted = NULL;
    self->cursor.row    = CLAMP(rows + Vt_top_line(self), min_row, max_row);
    self->cursor.col    = MIN(column, (uint32_t)Vt_col(self) - 1);
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Add a character as a combining character for that rune */
static void VtRune_push_combining(VtRune* self, char32_t codepoint)
{
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(self->rune.combine); ++i) {
        if (!self->rune.combine[i]) {
            self->rune.combine[i] = codepoint;
            return;
        }
    }
    WRN("Combining character limit (%zu) exceeded\n", ARRAY_SIZE(self->rune.combine));
}

/**
 * Try to interpret a combining character as an SGR property
 * @return was succesfull  */
static bool VtRune_try_normalize_as_property(VtRune* self, char32_t codepoint)
{
    if (!self->line_color_not_default) {
        switch (codepoint) {
            case 0x00001AB6: /* COMBINING WIGGLY LINE BELOW */
                self->curlyunderline = true;
                return true;
            case 0x00000332: /* COMBINING UNDERLINE */
                self->underlined = true;
                return true;
            case 0x00000333: /* COMBINING DOUBLE LOW LINE */
                self->doubleunderline = true;
                return true;
            case 0x00000305: /* COMBINING OVERLINE */
                self->overline = true;
                return true;
            case 0x00000336: /* COMBINING LONG STROKE OVERLAY */
                self->strikethrough = true;
                return true;
        }
    }
    return false;
}

/**
 * Try to do something about combinable characters */
static void Vt_handle_combinable(Vt* self, char32_t c)
{
    if (self->last_interted) {
        if (VtRune_try_normalize_as_property(self->last_interted, c)) {
            return;
        }
#ifndef NOUTF8PROC
        if (self->last_interted->rune.combine[0]) {
#endif
            /* Already contains a combining char that failed to normalize */
            VtRune_push_combining(self->last_interted, c);
#ifndef NOUTF8PROC
        } else {
            mbstate_t mbs;
            memset(&mbs, 0, sizeof(mbs));
            char   buff[MB_CUR_MAX * 2 + 1];
            size_t oft = c32rtomb(buff, self->last_interted->rune.code, &mbs);
            buff[c32rtomb(buff + oft, c, &mbs) + oft] = 0;
            size_t   old_len                          = strlen(buff);
            char*    res     = (char*)utf8proc_NFC((const utf8proc_uint8_t*)buff);
            char32_t conv[2] = { 0, 0 };

            if (old_len == strnlen(res, old_len + 1)) {
                VtRune_push_combining(self->last_interted, c);
            } else if (mbrtoc32(conv, res, ARRAY_SIZE(buff) - 1, &mbs) < 1) {
                /* conversion failed */
                WRN("Unicode normalization failed %s\n", strerror(errno));
                Vt_grapheme_break(self);
            } else {
                LOG("Vt::unicode{ u+%x + u+%x -> u+%x }\n",
                    self->last_interted->rune.code,
                    c,
                    conv[0]);

                self->last_interted->rune.code = conv[0];
                self->last_codepoint           = conv[0];
            }
            free(res);
        }
#endif
    } else {
        WRN("Got combining character, but no previous character is recorded\n");
    }
}

__attribute__((always_inline, hot, flatten)) static inline void Vt_handle_literal(Vt* self, char c)
{
    if (self->parser.in_mb_seq) {
        char32_t res;
        size_t   rd = mbrtoc32(&res, &c, 1, &self->parser.input_mbstate);

        if (unlikely(rd == (size_t)-1)) {
            /* encoding error */
            WRN("%s\n", strerror(errno));
            errno = 0;

            /* zero-initialized mbstate_t always represents the initial conversion state */
            memset(&self->parser.input_mbstate, 0, sizeof(mbstate_t));
        } else if (rd != (size_t)-2) {
            /* sequence is complete */
            self->parser.in_mb_seq = false;

            bool is_combining = false;
#ifndef NOUTF8PROC
            if (self->last_codepoint) {
                is_combining = !utf8proc_grapheme_break_stateful(self->last_codepoint,
                                                                 res,
                                                                 &self->utf8proc_state) &&
                               utf8proc_charwidth(res) == 0;
            } else {
                is_combining = unicode_is_combining(res);
            }
#else
            is_combining = unicode_is_combining(res);
#endif

            if (unlikely(is_combining)) {
                Vt_handle_combinable(self, res);
                self->last_codepoint = res;
            } else {
                VtRune new_rune      = self->parser.char_state;
                self->last_codepoint = res;
                new_rune.rune.code   = res;
                if (self->active_hyperlink) {
                    new_rune.hyperlink_idx =
                      VtLine_add_link(Vt_cursor_line(self), self->active_hyperlink) + 1;
                }
                Vt_insert_char_at_cursor(self, new_rune);
            }
        }
    } else {
        switch (c) {
            case '\a':
                Vt_grapheme_break(self);
                Vt_bell(self);
                break;

            case '\b':
                Vt_grapheme_break(self);
                Vt_handle_backspace(self);
                break;

            case '\r':
                Vt_grapheme_break(self);
                Vt_carriage_return(self);
                break;

            case '\f':
            case '\v':
            case '\n':
                Vt_grapheme_break(self);
                Vt_insert_new_line(self);
                break;

            case '\e':
                Vt_grapheme_break(self);
                self->parser.state = PARSER_STATE_ESCAPED;
                break;

            /* Invoke the G1 character set as GL */
            case 14 /* SO */:
                Vt_grapheme_break(self);
                self->charset_gl = &self->charset_g1;
                break;

            /* Invoke the G0 character set (the default) as GL */
            case 15 /* SI */:
                Vt_grapheme_break(self);
                self->charset_gl = &self->charset_g0;
                break;

            case '\t': {
                Vt_grapheme_break(self);
                uint16_t rt;
                for (rt = 0; self->cursor.col + rt + 1 < Vt_col(self);) {
                    if (self->tab_ruler[self->cursor.col + ++rt])
                        break;
                }
                Vt_move_cursor(self, self->cursor.col + rt, Vt_cursor_row(self));
            } break;

            default: {
                // TODO: ISO 8859-1 charset (not UTF-8 mode)
                if (c & (1 << 7)) {
                    mbrtoc32(NULL, &c, 1, &self->parser.input_mbstate);
                    self->parser.in_mb_seq = true;
                    break;
                }

                VtRune new_rune    = self->parser.char_state;
                new_rune.rune.code = c;

                if (self->active_hyperlink) {
                    new_rune.hyperlink_idx =
                      VtLine_add_link(Vt_cursor_line(self), self->active_hyperlink) + 1;
                }

                if (unlikely(self->charset_single_shift && *self->charset_single_shift)) {
                    new_rune.rune.code         = (*(self->charset_single_shift))(c);
                    self->charset_single_shift = NULL;
                } else if (unlikely(self->charset_gl && (*self->charset_gl))) {
                    new_rune.rune.code = (*(self->charset_gl))(c);
                }

                self->last_codepoint = new_rune.rune.code;
                Vt_insert_char_at_cursor(self, new_rune);
            }
        }
    }
}

__attribute__((always_inline, hot)) static inline void Vt_handle_char(Vt* self, char c)
{
    switch (self->parser.state) {

        case PARSER_STATE_LITERAL:
            Vt_handle_literal(self, c);
            break;

        case PARSER_STATE_CSI:
            switch (c) {
                case '\a':
                    Vt_bell(self);
                    break;
                case '\b':
                    Vt_handle_backspace(self);
                    break;
                case '\r':
                    Vt_carriage_return(self);
                    break;
                case '\f':
                case '\v':
                case '\n':
                    Vt_insert_new_line(self);
                    break;
                default:
                    Vt_handle_CSI(self, c);
            }
            break;

        case PARSER_STATE_ESCAPED:
            switch (expect(c, '[')) {

                /* Control sequence introduce (CSI) */
                case '[':
                    self->parser.state = PARSER_STATE_CSI;
                    return;

                /* Operating system command (OSC) */
                case ']':
                    self->parser.state = PARSER_STATE_OSC;
                    return;

                /* Device control */
                case 'P':
                    self->parser.state = PARSER_STATE_DCS;
                    return;

                /* Application Programming Command (APC) */
                case '_':
                    self->parser.state = PARSER_STATE_APC;
                    return;

                /* Reverse line feed (RI) */
                case 'M':
                    Vt_reverse_line_feed(self);
                    self->parser.state = PARSER_STATE_LITERAL;
                    return;

                /* New line (NEL) */
                case 'E':
                    Vt_carriage_return(self);
                    /* fallthrough */

                /* Line feed (IND) */
                case 'D':
                    Vt_insert_new_line(self);
                    self->parser.state = PARSER_STATE_LITERAL;
                    return;

                case '#':
                    self->parser.state = PARSER_STATE_DEC_SPECIAL;
                    return;

                /* Set tab stop at current column (HTS) */
                case 'H':
                    self->tab_ruler[self->cursor.col] = true;
                    self->parser.state                = PARSER_STATE_LITERAL;
                    return;

                /* set primary charset G0 */
                case '(':
                    self->parser.state = PARSER_STATE_CHARSET_G0;
                    return;

                /* set secondary charset G1 */
                case ')':
                    self->parser.state = PARSER_STATE_CHARSET_G1;
                    return;

                /* set tertiary charset G2 */
                case '*':
                    self->parser.state = PARSER_STATE_CHARSET_G2;
                    return;

                /* set quaternary charset G3 */
                case '+':
                    self->parser.state = PARSER_STATE_CHARSET_G3;
                    return;

                /* Select default character set(<ESC>%@) or Select UTF-8 character set(<ESC>%G)
                 */
                case '%':
                    self->parser.state = PARSER_STATE_CHARSET;
                    return;

                case 'g':
                    Vt_bell(self);
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Application Keypad (DECKPAM) */
                case '=':
                    self->modes.application_keypad = true;
                    self->parser.state             = PARSER_STATE_LITERAL;
                    return;

                /* Normal Keypad (DECKPNM) */
                case '>':
                    self->modes.application_keypad = false;
                    self->parser.state             = PARSER_STATE_LITERAL;
                    return;

                /* Disable Manual Input (DMI) */
                case '`':
                /* Enable Manual Input (EMI) */
                case 'b':
                    WRN("Manual input not implemented\n");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Reset initial state (RIS) */
                case 'c':
                    Vt_hard_reset(self);
                    return;

                /* Save cursor (DECSC) */
                case '7':
                    self->saved_active_line = self->cursor.row;
                    self->saved_cursor_pos  = self->cursor.col;
                    self->parser.state      = PARSER_STATE_LITERAL;
                    return;

                /* Restore cursor (DECRC) */
                case '8':
                    self->cursor.row   = self->saved_active_line;
                    self->cursor.col   = self->saved_cursor_pos;
                    self->parser.state = PARSER_STATE_LITERAL;
                    return;

                /* Back Index (DECBI) (VT400)
                 *
                 * This control function moves the cursor backward one column. If the cursor is at
                 * the left margin, all screen data within the margins moves onecolumn to the right.
                 * The column shifted past the right margin is lost
                 */
                case '6':

                /* Forward Index (DECFI) (VT400)
                 *
                 * This control function moves the cursor forward one column. If the cursor is at
                 * the right margin, all screen data within the margins moves onecolumn to the left.
                 * The column shifted past the left margin is lost.
                 **/
                case '9':
                    WRN("VT400 shifting cursor movement not implemented\n");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Coding Method Delimiter (CMD) */
                case 'd':
                    WRN("CMD not implemented\n");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G2 Character Set into GL (VT200 mode only) (LS2) */
                case 'n':
                    self->charset_gl   = &self->charset_g2;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G3 Character Set into GL (VT200 mode only) (LS3) */
                case 'o':
                    self->charset_gl   = &self->charset_g3;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /*  Invoke the G3 Character Set into GR (VT200 mode only) (LS3R) */
                case '|':
                    self->charset_gr   = &self->charset_g3;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G2 Character Set into GR (VT200 mode only) (LS2R) */
                case '}':
                    self->charset_gr   = &self->charset_g2;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G1 Character Set into GR (VT200 mode only) (LS1R) */
                case '~':
                    self->charset_gr   = &self->charset_g1;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Single Shift Select of G2 Character Set (SS2), VT220 */
                case 'N':
                    self->charset_single_shift = &self->charset_g2;
                    self->parser.state         = PARSER_STATE_LITERAL;
                    break;

                /* Single Shift Select of G3 Character Set (SS3), VT220 */
                case 'O':
                    self->charset_single_shift = &self->charset_g3;
                    self->parser.state         = PARSER_STATE_LITERAL;
                    break;

                /* Old (tab)title set sequence */
                case 'k':
                    self->parser.state = PARSER_STATE_TITLE;
                    break;

                /* Start of string */
                case 'X':
                    WRN("SOS not implemented\n");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Start of guarded area */
                case 'V':
                /* End of guarded area */
                case 'W':
                    WRN("Guarded areas not implemented\n");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                case '\e':
                    return;

                default: {
                    const char* cs    = control_char_get_pretty_string(c);
                    char        cb[2] = { c, 0 };
                    WRN("Unknown escape sequence: %s (%d)\n", cs ? cs : cb, c);

                    self->parser.state = PARSER_STATE_LITERAL;
                    return;
                }
            }
            break;

        case PARSER_STATE_CHARSET_G0:
            self->parser.state = PARSER_STATE_LITERAL;
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            switch (c) {
                case '0':
                    self->charset_g0 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g0 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g0 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G1:
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            self->parser.state = PARSER_STATE_LITERAL;
            switch (c) {
                case '0':
                    self->charset_g1 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g1 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g1 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G2:
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            self->parser.state = PARSER_STATE_LITERAL;
            switch (c) {
                case '0':
                    self->charset_g2 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g2 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g2 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G3:
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            self->parser.state = PARSER_STATE_LITERAL;
            switch (c) {
                case '0':
                    self->charset_g3 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g3 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g3 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET:
            WRN("Unimplemented character set select command\n");
            self->parser.state = PARSER_STATE_LITERAL;
            break;

        case PARSER_STATE_OSC:
            Vt_handle_OSC(self, c);
            break;

        case PARSER_STATE_PM:
            Vt_handle_PM(self, c);
            break;

        case PARSER_STATE_DCS:
            Vt_handle_DCS(self, c);
            break;

        case PARSER_STATE_APC:
            Vt_handle_APC(self, c);
            break;

        case PARSER_STATE_DEC_SPECIAL:
            switch (c) {
                /* DEC Screen Alignment Test (DECALN), VT100. */
                case '8': {
                    VtRune blank_E    = self->blank_space;
                    blank_E.rune.code = 'E';
                    for (size_t cl = 0; cl < Vt_col(self); ++cl) {
                        for (size_t r = Vt_top_line(self); r <= Vt_bottom_line(self); ++r) {
                            Vt_overwrite_char_at(self, cl, r, blank_E);
                        }
                    }
                } break;

                default:
                    WRN("Unknown DEC escape\n");
            }
            self->parser.state = PARSER_STATE_LITERAL;
            break;

        case PARSER_STATE_TITLE: {
            Vector_push_char(&self->parser.active_sequence, c);
            size_t len = self->parser.active_sequence.size;
            if ((len >= 2 && self->parser.active_sequence.buf[len - 2] == '\e' &&
                 self->parser.active_sequence.buf[len - 1] == '\\') ||
                c == '\a') {
                Vector_pop_n_char(&self->parser.active_sequence, 2);
                Vector_push_char(&self->parser.active_sequence, '\0');
                Vt_set_title(self, self->parser.active_sequence.buf);
                self->parser.state = PARSER_STATE_LITERAL;
                Vector_clear_char(&self->parser.active_sequence);
            }
        } break;

        default:
            ASSERT_UNREACHABLE;
    }
}

static void Vt_shrink_scrollback(Vt* self)
{
    if (Vt_alt_buffer_enabled(self)) {
        return;
    }
    int64_t ln_cnt = self->lines.size;
    if (unlikely(ln_cnt > MAX(settings.scrollback * 1.1, Vt_row(self)))) {
        int64_t to_remove = ln_cnt - settings.scrollback - Vt_row(self);
        Vector_remove_at_VtLine(&self->lines, 0, to_remove);
        self->cursor.row -= to_remove;
        self->visual_scroll_top -= to_remove;
    }
}

static inline void Vt_clear_proxies(Vt* self)
{
    if (self->scrolling_visual) {
        if (self->visual_scroll_top > Vt_row(self) * 5) {
            size_t begin = Vt_visual_bottom_line(self) + 4 * Vt_row(self);
            Vt_clear_proxies_in_region(self, begin, self->lines.size - 1);
        }
    } else if (self->lines.size > Vt_row(self)) {
        size_t end = Vt_visual_top_line(self) ? Vt_visual_top_line(self) - 1 : 0;
        Vt_clear_proxies_in_region(self, 0, end);
    }
}

inline void Vt_interpret(Vt* self, char* buf, size_t bytes)
{
    if (unlikely(settings.debug_pty)) {
        char* str = pty_string_prettyfy(buf, bytes);
        fprintf(stderr, "pty.read (%3zu) ~> { %s }\n\n", bytes, str);
        free(str);
    }
    for (size_t i = 0; i < bytes; ++i) {
        Vt_handle_char(self, buf[i]);
    }
    Vt_shrink_scrollback(self);
}

void Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end)
{
    if (out_begin) {
        *out_begin = self->lines.buf + Vt_visual_top_line(self);
    }

    if (out_end) {
        *out_end = self->lines.buf + Vt_visual_bottom_line(self) + 1;
    }
}

static const char* application_cursor_key_response(const uint32_t key)
{
    switch (key) {
        case KEY(Up):
            return "\eOA";
        case KEY(Down):
            return "\eOB";
        case KEY(Right):
            return "\eOC";
        case KEY(Left):
            return "\eOD";
        case KEY(End):
            return "\eOF";
        case KEY(Home):
            return "\eOH";
        case 127:
            return "\e[3~";
        default:
            return NULL;
    }
}

static const char* Vt_get_normal_cursor_key_response(Vt* self, const uint32_t key)
{
    if (self->modes.application_keypad_cursor) {
        return application_cursor_key_response(key);
    }

    switch (key) {
        case KEY(Up):
            return "\e[A";
        case KEY(Down):
            return "\e[B";
        case KEY(Right):
            return "\e[C";
        case KEY(Left):
            return "\e[D";
        case KEY(End):
            return "\e[F";
        case KEY(Home):
            return "\e[H";
        case 127:
            return "\e[3~";
        default:
            return NULL;
    }
}

/**
 * Get response format string in normal keypad mode */
static const char* mod_cursor_key_response(const uint32_t key)
{
    switch (key) {
        case KEY(Up):
            return "\e[1;%dA";
        case KEY(Down):
            return "\e[1;%dB";
        case KEY(Right):
            return "\e[1;%dC";
        case KEY(Left):
            return "\e[1;%dD";
        case KEY(End):
            return "\e[1;%dF";
        case KEY(Home):
            return "\e[1;%dH";
        case 127:
            return "\e[3;%d~";
        default:
            return NULL;
    }
}

/**
 * Start entering unicode codepoint as hex */
void Vt_start_unicode_input(Vt* self)
{
    self->unicode_input.active = true;
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Respond to key event for unicode input
 * @return keypress was consumed */
static bool Vt_maybe_handle_unicode_input_key(Vt*      self,
                                              uint32_t key,
                                              uint32_t rawkey,
                                              uint32_t mods)
{
    if (self->unicode_input.active) {
        if (key == 13) {
            // Enter
            Vector_push_char(&self->unicode_input.buffer, 0);
            self->unicode_input.active = false;
            char32_t result            = strtol(self->unicode_input.buffer.buf, NULL, 16);
            Vector_clear_char(&self->unicode_input.buffer);
            if (result) {
                LOG("unicode input \'%s\' -> %d\n", self->unicode_input.buffer.buf, result);

                char             tmp[32];
                static mbstate_t mbstate;
                int              mb_len = c32rtomb(tmp, result, &mbstate);
                if (mb_len) {
                    Vt_output(self, tmp, mb_len);
                }
            } else {
                WRN("Failed to parse \'%s\'\n", self->unicode_input.buffer.buf);
            }
        } else if (key == 27) {
            // Escape
            self->unicode_input.buffer.size = 0;
            self->unicode_input.active      = false;
            CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
        } else if (key == 8) {
            // Backspace
            if (self->unicode_input.buffer.size) {
                Vector_pop_char(&self->unicode_input.buffer);
            } else {
                self->unicode_input.buffer.size = 0;
                self->unicode_input.active      = false;
            }
            CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
        } else if (isxdigit(key)) {
            if (self->unicode_input.buffer.size > 8) {
                CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
            } else {
                Vector_push_char(&self->unicode_input.buffer, key);
                CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
            }
        } else {
            CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
        }
        return true;
    }
    return false;
}

/**
 * Respond to key event if it is a keypad key
 * @return keypress was consumed */
static bool Vt_maybe_handle_keypad_key(Vt* self, uint32_t key, uint32_t mods)
{
    const char* resp = NULL;
    if (mods) {
        resp = mod_cursor_key_response(key);
        if (resp) {
            Vt_output_formated(self, resp, mods + 1);
            return true;
        }
    } else {
        resp = Vt_get_normal_cursor_key_response(self, key);
        if (resp) {
            Vt_output(self, resp, strlen(resp));
            return true;
        }
    }

    return false;
}

/**
 * Respond to key event if it is a function key
 * @return keypress was consumed */
static bool Vt_maybe_handle_function_key(Vt* self, uint32_t key, uint32_t mods)
{
    if (key >= KEY(F1) && key <= KEY(F35)) {
        int f_num = (key + 1) - KEY(F1);
        if (mods) {
            if (f_num < 5) {
                Vt_output_formated(self, "\e[1;%u%c", mods + 1, f_num + 'O');
            } else if (f_num == 5) {
                Vt_output_formated(self, "\e[%d;%u~", f_num + 10, mods + 1);
            } else if (f_num < 11) {
                Vt_output_formated(self, "\e[%d;%u~", f_num + 11, mods + 1);
            } else {
                Vt_output_formated(self, "\e[%d;%u~", f_num + 12, mods + 1);
            }
        } else {
            if (f_num < 5) {
                Vt_output_formated(self, "\eO%c", f_num + 'O');
            } else if (f_num == 5) {
                Vt_output_formated(self, "\e[%d~", f_num + 10);
            } else if (f_num < 11) {
                Vt_output_formated(self, "\e[%d~", f_num + 11);
            } else {
                Vt_output_formated(self, "\e[%d~", f_num + 12);
            }
        }
        return true;
    } else /* not f-key */ {
        if (mods) {
            if (key == KEY(Insert)) {
                Vt_output_formated(self, "\e[2;%u~", mods + 1);
                return true;
            } else if (key == KEY(Delete)) {
                Vt_output_formated(self, "\e[3;%u~", mods + 1);
                return true;
            } else if (key == KEY(Home)) {
                Vt_output_formated(self, "\e[1;%u~", mods + 1);
                return true;
            } else if (key == KEY(End)) {
                Vt_output_formated(self, "\e[4;%u~", mods + 1);
                return true;
            } else if (key == KEY(Page_Up)) {
                Vt_output_formated(self, "\e[5;%u~", mods + 1);
                return true;
            } else if (key == KEY(Page_Down)) {
                Vt_output_formated(self, "\e[6;%u~", mods + 1);
                return true;
            }

        } else /* no mods */ {
            if (key == KEY(Insert)) {
                Vt_output(self, "\e[2~", 4);
                return true;
            } else if (key == KEY(Delete)) {
                Vt_output(self, "\e[3~", 4);
                return true;
            } else if (key == KEY(Page_Up)) {
                Vt_output(self, "\e[5~", 4);
                return true;
            } else if (key == KEY(Page_Down)) {
                Vt_output(self, "\e[6~", 4);
                return true;
            }
        }
    }

    return false;
}

/**
 *  Substitute keypad keys with normal ones */
static uint32_t numpad_key_convert(uint32_t key)
{
    switch (key) {
        case KEY(KP_Add):
            return '+';
        case KEY(KP_Subtract):
            return '-';
        case KEY(KP_Multiply):
            return '*';
        case KEY(KP_Divide):
            return '/';
        case KEY(KP_Equal):
            return '=';
        case KEY(KP_Decimal):
            return '.';
        case KEY(KP_Separator):
            return '.';
        case KEY(KP_Space):
            return ' ';

        case KEY(KP_Up):
            return KEY(Up);
        case KEY(KP_Down):
            return KEY(Down);
        case KEY(KP_Left):
            return KEY(Left);
        case KEY(KP_Right):
            return KEY(Right);

        case KEY(KP_Page_Up):
            return KEY(Page_Up);
        case KEY(KP_Page_Down):
            return KEY(Page_Down);

        case KEY(KP_Insert):
            return KEY(Insert);
        case KEY(KP_Delete):
            return KEY(Delete);
        case KEY(KP_Home):
            return KEY(Home);
        case KEY(KP_End):
            return KEY(End);
        case KEY(KP_Begin):
            return KEY(Begin);
        case KEY(KP_Tab):
            return KEY(Tab);
        case KEY(KP_Enter):
            return KEY(Return);

        case KEY(KP_F1):
            return KEY(F1);
        case KEY(KP_F2):
            return KEY(F2);
        case KEY(KP_F3):
            return KEY(F3);
        case KEY(KP_F4):
            return KEY(F4);

        case KEY(KP_0)... KEY(KP_9):
            return '0' + key - KEY(KP_0);
        default:
            return key;
    }
}

/**
 * Respond to key event */
void Vt_handle_key(void* _self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    Vt* self = _self;

    key = numpad_key_convert(key);

    if (!Vt_maybe_handle_unicode_input_key(self, key, rawkey, mods) &&
        !Vt_maybe_handle_keypad_key(self, key, mods) &&
        !Vt_maybe_handle_function_key(self, key, mods)) {

        if (FLAG_IS_SET(mods, MODIFIER_ALT) && !self->modes.no_alt_sends_esc) {
            Vector_push_char(&self->output, '\e');
        }
        if (unlikely(FLAG_IS_SET(mods, MODIFIER_CONTROL))) {
            if (unlikely(key == ' ')) {
                key = 0;
            } else if (isalpha(key)) {
                key = tolower(key) - 'a';
            }
        }
        if (unlikely(key == '\b') && ((mods & MODIFIER_ALT) == mods || !mods) &&
            settings.bsp_sends_del) {
            key = 127;
        }
        char             tmp[32];
        static mbstate_t mbstate;
        size_t           mb_len = c32rtomb(tmp, key, &mbstate);
        if (mb_len) {
            Vt_output(self, tmp, mb_len);
        }
    }

    if (settings.scroll_on_key) {
        Vt_visual_scroll_reset(self);
    }
}

void Vt_handle_button(void*    _self,
                      uint32_t button,
                      bool     state,
                      int32_t  x,
                      int32_t  y,
                      int32_t  ammount,
                      uint32_t mods)
{
    Vt*  self        = _self;
    bool in_window   = x >= 0 && x <= self->ws.ws_xpixel && y >= 0 && y <= self->ws.ws_ypixel;
    bool btn_reports = Vt_reports_mouse(self);

    if (btn_reports && in_window) {
        if (!self->scrolling_visual) {
            self->last_click_x = (double)x / self->pixels_per_cell_x;
            self->last_click_y = (double)y / self->pixels_per_cell_y;

            if (self->modes.x10_mouse_compat) {
                button += (FLAG_IS_SET(mods, MODIFIER_SHIFT) ? 4 : 0) +
                          (FLAG_IS_SET(mods, MODIFIER_ALT) ? 8 : 0) +
                          (FLAG_IS_SET(mods, MODIFIER_CONTROL) ? 16 : 0);
            }
            if (self->modes.extended_report) {
                Vt_output_formated(self,
                                   "\e[<%u;%d;%d%c",
                                   button - 1,
                                   self->last_click_x + 1,
                                   self->last_click_y + 1,
                                   state ? 'M' : 'm');
            } else if (self->modes.mouse_btn_report) {
                Vt_output_formated(self,
                                   "\e[M%c%c%c",
                                   32 + button - 1 + !state * 3,
                                   (char)(32 + self->last_click_x + 1),
                                   (char)(32 + self->last_click_y + 1));
            }
        }
    }
}

void Vt_handle_motion(void* _self, uint32_t button, int32_t x, int32_t y)
{
    Vt* self = _self;

    if (self->modes.extended_report) {
        if (!self->scrolling_visual) {
            x              = CLAMP(x, 0, self->ws.ws_xpixel);
            y              = CLAMP(y, 0, self->ws.ws_ypixel);
            size_t click_x = (double)x / self->pixels_per_cell_x;
            size_t click_y = (double)y / self->pixels_per_cell_y;
            if (click_x != self->last_click_x || click_y != self->last_click_y) {
                self->last_click_x = click_x;
                self->last_click_y = click_y;
                Vt_output_formated(self,
                                   "\e[<%d;%zu;%zuM",
                                   (int)button - 1 + 32,
                                   click_x + 1,
                                   click_y + 1);
            }
        }
    }
}

void Vt_handle_clipboard(void* self, const char* text)
{
    Vt* vt = self;

    if (!text) {
        return;
    }

    size_t len = strlen(text);

    if (vt->modes.bracketed_paste) {
        Vt_output(vt, "\e[200~", 6);
    }

    char last = '\0';
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\n') {
            if (last != '\r') {
                Vector_push_char(&vt->output, '\r');
            }
        } else {
            Vector_push_char(&vt->output, c);
        }
        last = c;
    }

    if (vt->modes.bracketed_paste) {
        Vt_output(vt, "\e[201~", 6);
    }
}

static void Vt_set_title(Vt* self, const char* title)
{
    free(self->title);
    self->title = strdup(title);
    CALL_FP(self->callbacks.on_title_changed, self->callbacks.user_data, self->title);
}

void Vt_destroy(Vt* self)
{
    Vector_destroy_VtLine(&self->lines);
    if (Vt_alt_buffer_enabled(self)) {
        Vector_destroy_VtLine(&self->alt_lines);
    }
    Vector_destroy_char(&self->parser.active_sequence);
    Vector_destroy_DynStr(&self->title_stack);
    Vector_destroy_char(&self->unicode_input.buffer);
    Vector_destroy_char(&self->output);
    free(self->title);
    free(self->active_hyperlink);
    free(self->work_dir);
    free(self->tab_ruler);
}

void Vt_get_output(Vt* self, char** out_buf, size_t* out_bytes)
{
    ASSERT(out_buf && out_bytes, "has outputs");

    if (unlikely(settings.debug_pty) && self->output.size) {
        char* str = pty_string_prettyfy(self->output.buf, self->output.size);
        fprintf(stderr, "pty.write(%3zu) <~ { %s }\n\n", self->output.size, str);
        free(str);
    }

    *out_buf   = self->output.buf;
    *out_bytes = self->output.size;
    Vector_clear_char(&self->output);
}
