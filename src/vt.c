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
#endif

#include <xkbcommon/xkbcommon.h>

#include "wcwidth/wcwidth.h"

VtRune blank_space;

static inline size_t Vt_top_line(const Vt* const self);
void                 Vt_visual_scroll_to(Vt* self, size_t line);
void                 Vt_visual_scroll_up(Vt* self);
void                 Vt_visual_scroll_down(Vt* self);
void                 Vt_visual_scroll_reset(Vt* self);
static inline size_t Vt_get_scroll_region_top(Vt* self);
static inline size_t Vt_get_scroll_region_bottom(Vt* self);
static inline bool   Vt_scroll_region_not_default(Vt* self);
static void          Vt_alt_buffer_on(Vt* self, bool save_mouse);
static void          Vt_alt_buffer_off(Vt* self, bool save_mouse);
static void          Vt_handle_multi_argument_SGR(Vt* self, Vector_char seq);
static void          Vt_reset_text_attribs(Vt* self);
static void          Vt_carriage_return(Vt* self);
static void          Vt_clear_right(Vt* self);
static void          Vt_clear_left(Vt* self);
static inline void   Vt_scroll_out_all_content(Vt* self);
static void          Vt_empty_line_fill_bg(Vt* self, size_t idx);
static void          Vt_cursor_down(Vt* self);
static void          Vt_cursor_up(Vt* self);
static void          Vt_cursor_left(Vt* self);
static void          Vt_cursor_right(Vt* self);
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
static void          Vt_move_cursor(Vt* self, uint32_t c, uint32_t r);
static void          Vt_move_cursor_to_column(Vt* self, uint32_t c);
static void          Vt_set_title(Vt* self, const char* title);
static void          Vt_push_title(Vt* self);
static void          Vt_pop_title(Vt* self);
static inline void   Vt_insert_char_at_cursor(Vt* self, VtRune c, bool apply_color_modifications);
static inline void   Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c);
static Vector_char line_to_string(Vector_VtRune* line, size_t begin, size_t end, const char* tail);
static inline void Vt_mark_proxy_fully_damaged(Vt* self, size_t idx);
static void        Vt_mark_proxy_damaged_cell(Vt* self, size_t line, size_t rune);

static inline VtLine VtLine_new()
{
    return (VtLine){ .damage =
                       (struct VtLineDamage){
                         .type  = VT_LINE_DAMAGE_FULL,
                         .front = 0,
                         .end   = 0,
                         .shift = 0,
                       },
                     .reflowable = true,
                     .rejoinable = false,
                     .data       = Vector_new_VtRune(),
                     .proxy      = { { 0 } } };
}

static void Vt_output(Vt* self, const char* buf, size_t len)
{
    Vector_pushv_char(&self->output, buf, len);
}

#define Vt_output_formated(vt, fmt, ...)                                                           \
    char _tmp[64];                                                                                 \
    int  _len = snprintf(_tmp, sizeof(_tmp), fmt, __VA_ARGS__);                                    \
    Vt_output((vt), _tmp, _len);

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

        return line_to_string(&self->lines.buf[begin_line].data,
                              begin_char_idx,
                              end_char_idx + 1,
                              "");
    } else if (self->selection.begin_line < self->selection.end_line) {
        begin_char_idx = self->selection.begin_char_idx;
        end_char_idx   = self->selection.end_char_idx;
    } else {
        begin_char_idx = self->selection.end_char_idx;
        end_char_idx   = self->selection.begin_char_idx;
    }

    if (self->selection.mode == SELECT_MODE_NORMAL) {
        ret = line_to_string(&self->lines.buf[begin_line].data,
                             begin_char_idx,
                             0,
                             self->lines.buf[begin_line + 1].rejoinable ? "" : "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i < end_line; ++i) {
            tmp = line_to_string(&self->lines.buf[i].data,
                                 0,
                                 0,
                                 self->lines.buf[i + 1].rejoinable ? "" : "\n");
            Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
            Vector_destroy_char(&tmp);
        }
        tmp = line_to_string(&self->lines.buf[end_line].data, 0, end_char_idx + 1, "");
        Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
        Vector_destroy_char(&tmp);
    } else if (self->selection.mode == SELECT_MODE_BOX) {
        ret =
          line_to_string(&self->lines.buf[begin_line].data, begin_char_idx, end_char_idx + 1, "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i <= end_line; ++i) {
            tmp = line_to_string(&self->lines.buf[i].data,
                                 begin_char_idx,
                                 end_char_idx + 1,
                                 i == end_line ? "" : "\n");
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
    self->selection.next_mode            = mode;
    x                                    = CLAMP(x, 0, self->ws.ws_xpixel);
    y                                    = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_x                       = (double)x / self->pixels_per_cell_x;
    size_t click_y                       = (double)y / self->pixels_per_cell_y;
    self->selection.click_begin_char_idx = click_x;
    self->selection.click_begin_line     = Vt_visual_top_line(self) + click_y;
}

/**
 * initialize selection region to cell by cell screen coordinates */
void Vt_select_init_cell(Vt* self, enum SelectMode mode, int32_t x, int32_t y)
{
    self->selection.next_mode            = mode;
    x                                    = CLAMP(x, 0, self->ws.ws_col);
    y                                    = CLAMP(y, 0, self->ws.ws_row);
    self->selection.click_begin_char_idx = x;
    self->selection.click_begin_line     = Vt_visual_top_line(self) + y;
}

/**
 * initialize selection region to clicked word */
void Vt_select_init_word(Vt* self, int32_t x, int32_t y)
{
    self->selection.mode   = SELECT_MODE_NORMAL;
    x                      = CLAMP(x, 0, self->ws.ws_xpixel);
    y                      = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t         click_x = (double)x / self->pixels_per_cell_x;
    size_t         click_y = (double)y / self->pixels_per_cell_y;
    Vector_VtRune* ln      = &self->lines.buf[Vt_visual_top_line(self) + click_y].data;
    size_t         cmax    = ln->size;
    size_t         begin   = click_x;
    size_t         end     = click_x;
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
    self->selection.mode           = SELECT_MODE_NORMAL;
    y                              = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_y                 = (double)y / self->pixels_per_cell_y;
    self->selection.begin_char_idx = 0;
    self->selection.end_char_idx   = self->ws.ws_col;
    self->selection.begin_line = self->selection.end_line = Vt_visual_top_line(self) + click_y;
    Vt_mark_proxy_fully_damaged(self, self->selection.begin_line);
}

static inline void Vt_mark_proxy_fully_damaged(Vt* self, size_t idx)
{
    CALL_FP(self->callbacks.on_action_performed, self->callbacks.user_data);
    self->lines.buf[idx].damage.type = VT_LINE_DAMAGE_FULL;
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
            size_t lo                          = MIN(self->lines.buf[line].damage.front, rune);
            size_t hi                          = MAX(self->lines.buf[line].damage.end, rune);
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

static inline void Vt_clear_proxy(Vt* self, size_t idx)
{
    Vt_mark_proxy_fully_damaged(self, idx);
    CALL_FP(self->callbacks.destroy_proxy, self->callbacks.user_data, &self->lines.buf[idx].proxy);
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
    if (self->alt_lines.buf) {
        for (size_t i = 0; i < self->alt_lines.size - 1; ++i) {
            Vt_clear_proxy(self, i);
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
        size_t start        = MAX(selection_lo, self->scroll_region_top);
        size_t end = MIN(MIN(selection_hi, self->scroll_region_bottom - 1), self->lines.size - 1);
        Vt_mark_proxies_damaged_in_region(self, start ? (start - 1) : 0, end + 1);
    }
}

void Vt_select_commit(Vt* self)
{
    if (self->selection.next_mode != SELECT_MODE_NONE) {
        self->selection.mode       = self->selection.next_mode;
        self->selection.next_mode  = SELECT_MODE_NONE;
        self->selection.begin_line = self->selection.end_line = self->selection.click_begin_line;
        self->selection.begin_char_idx                        = self->selection.end_char_idx =
          self->selection.click_begin_char_idx;

        Vt_mark_proxies_damaged_in_selected_region(self);
        CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
    }
}

void Vt_select_set_end(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        x              = CLAMP(x, 0, self->ws.ws_xpixel);
        y              = CLAMP(y, 0, self->ws.ws_ypixel);
        size_t click_x = (double)x / self->pixels_per_cell_x;
        size_t click_y = (double)y / self->pixels_per_cell_y;
        Vt_select_set_end_cell(self, click_x, click_y);
    }
}

void Vt_select_set_end_cell(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        size_t old_end               = self->selection.end_line;
        x                            = CLAMP(x, 0, self->ws.ws_col);
        y                            = CLAMP(y, 0, self->ws.ws_row);
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
        x              = CLAMP(x, 0, self->ws.ws_xpixel);
        y              = CLAMP(y, 0, self->ws.ws_ypixel);
        size_t click_x = (double)x / self->pixels_per_cell_x;
        size_t click_y = (double)y / self->pixels_per_cell_y;
        Vt_select_set_front_cell(self, click_x, click_y);
    }
}

void Vt_select_set_front_cell(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        size_t old_front               = self->selection.begin_line;
        x                              = CLAMP(x, 0, self->ws.ws_col);
        y                              = CLAMP(y, 0, self->ws.ws_row);
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
    } else
        return original;
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
        if (ctr)
            Vector_pushv_char(&fmt, ctr, strlen(ctr));
        else {
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
static Vector_char line_to_string(Vector_VtRune* line, size_t begin, size_t end, const char* tail)
{
    Vector_char res;
    end   = MIN(end ? end : line->size, line->size);
    begin = MIN(begin, line->size - 1);

    if (begin >= end) {
        res = Vector_new_with_capacity_char(2);
        if (tail)
            Vector_pushv_char(&res, tail, strlen(tail) + 1);
        return res;
    }
    res = Vector_new_with_capacity_char(end - begin);
    char utfbuf[4];

    for (uint32_t i = begin; i < end; ++i) {
        Rune* rune = &line->buf[i].rune;

        if (rune->code == VT_RUNE_CODE_WIDE_TAIL) {
            continue;
        }

        if (rune->code > CHAR_MAX) {
            static mbstate_t mbstate;
            size_t           bytes = c32rtomb(utfbuf, rune->code, &mbstate);
            if (bytes > 0) {
                Vector_pushv_char(&res, utfbuf, bytes);
            }
        } else {
            Vector_push_char(&res, rune->code);
        }

        for (int j = 0; j < VT_RUNE_MAX_COMBINE && rune->combine[j]; ++j) {
            Vector_push_char(&res, rune->combine[j]);
        }
    }
    if (tail) {
        Vector_pushv_char(&res, tail, strlen(tail) + 1);
    }

    return res;
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

__attribute__((hot)) static inline bool is_csi_sequence_terminated(const char*  seq,
                                                                   const size_t size)
{
    if (!size)
        return false;

    return isalpha(seq[size - 1]) || seq[size - 1] == '@' || seq[size - 1] == '{' ||
           seq[size - 1] == '}' || seq[size - 1] == '~' || seq[size - 1] == '|';
}

static inline bool is_string_sequence_terminated(const char* seq, const size_t size)
{
    if (!size)
        return false;

    return seq[size - 1] == '\a' || (size > 1 && seq[size - 2] == '\e' && seq[size - 1] == '\\');
}

void Vt_init(Vt* self, uint32_t cols, uint32_t rows)
{
    memset(self, 0, sizeof(Vt));
    self->ws = (struct winsize){ .ws_col = cols, .ws_row = rows };

    self->scroll_region_bottom = rows - 1;
    self->parser.state         = PARSER_STATE_LITERAL;
    self->parser.in_mb_seq     = false;

    self->parser.char_state = blank_space = (VtRune){
        .rune          = ((Rune){ .code = ' ', .combine = { 0 }, .style = VT_RUNE_NORMAL }),
        .bg            = settings.bg,
        .fg            = settings.fg,
        .dim           = false,
        .hidden        = false,
        .blinkng       = false,
        .underlined    = false,
        .strikethrough = false,
    };

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

    self->title       = NULL;
    self->title_stack = Vector_new_DynStr();

    self->unicode_input.buffer = Vector_new_char();
}

static inline size_t Vt_top_line_alt(const Vt* const self)
{
    return self->alt_lines.size <= self->ws.ws_row ? 0 : self->alt_lines.size - self->ws.ws_row;
}

static inline size_t Vt_bottom_line(const Vt* self)
{
    return Vt_top_line(self) + self->ws.ws_row - 1;
}

static inline size_t Vt_bottom_line_alt(Vt* self)
{
    return Vt_top_line_alt(self) + self->ws.ws_row - 1;
}

static inline size_t Vt_get_cursor_row_screen(Vt* self)
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

void Vt_visual_scroll_up(Vt* self)
{
    if (self->scrolling_visual) {
        if (self->visual_scroll_top)
            --self->visual_scroll_top;
    } else if (Vt_top_line(self)) {
        self->scrolling_visual  = true;
        self->visual_scroll_top = Vt_top_line(self) - 1;
    }
}

void Vt_visual_scroll_down(Vt* self)
{
    if (self->scrolling_visual && Vt_top_line(self) > self->visual_scroll_top) {
        ++self->visual_scroll_top;
        if (self->visual_scroll_top == Vt_top_line(self))
            self->scrolling_visual = false;
    }
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
    printf("  foreground color:   " COLOR_RGB_FMT "\n", COLOR_RGB_AP(self->parser.char_state.fg));
    printf("  background color:   " COLOR_RGBA_FMT "\n", COLOR_RGBA_AP(self->parser.char_state.bg));
    printf("  line color uses fg: " BOOL_FMT "\n",
           BOOL_AP(!self->parser.char_state.linecolornotdefault));
    printf("  line color:         " COLOR_RGB_FMT "\n", COLOR_RGB_AP(self->parser.char_state.line));
    printf("  dim:                " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.dim));
    printf("  hidden:             " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.hidden));
    printf("  blinking:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.blinkng));
    printf("  underlined:         " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.underlined));
    printf("  strikethrough:      " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.strikethrough));
    printf("  double underline:   " BOOL_FMT "\n",
           BOOL_AP(self->parser.char_state.doubleunderline));
    printf("  curly underline:    " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.curlyunderline));
    printf("  overline:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.overline));
    printf("  inverted:           " BOOL_FMT "\n", BOOL_AP(self->parser.color_inverted));
    printf("Modes:\n");
    printf("  application keypad:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.application_keypad));
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
    printf("  no auto wrap:                     " BOOL_FMT "\n", BOOL_AP(self->modes.no_auto_wrap));
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
    printf("W L V | Active line:  real: %zu (visible: %zu)\n",
           self->cursor.row,
           Vt_get_cursor_row_screen(self));
    printf("P   I | Cursor position: %zu type: %d blink: %d hidden: %d\n",
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
        Vector_char str = line_to_string(&self->lines.buf[i].data, 0, 0, "");
        printf(
          "%c %c %c %4zu%c s:%3zu dmg:%d proxy{%3d,%3d,%3d,%3d} reflow{%d,%d,%d} data{%.50s%s}\n",
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
          (str.size > 50 ? "…" : ""));
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

        if (self->lines.buf[i].data.size < x && self->lines.buf[i].reflowable) {
            int32_t chars_to_move = x - self->lines.buf[i].data.size;

            if (i + 1 < bottom_bound && self->lines.buf[i + 1].rejoinable) {
                chars_to_move = MIN(chars_to_move, (int32_t)self->lines.buf[i + 1].data.size);

                Vector_pushv_VtRune(&self->lines.buf[i].data,
                                    self->lines.buf[i + 1].data.buf,
                                    chars_to_move);

                Vector_remove_at_VtRune(&self->lines.buf[i + 1].data, 0, chars_to_move);

                if (self->selection.mode == SELECT_MODE_NORMAL) {
                    if (self->selection.begin_line == i + 1) {
                        if (self->selection.begin_char_idx <= chars_to_move) {
                            --self->selection.begin_line;
                            self->selection.begin_char_idx =
                              self->selection.begin_char_idx + self->lines.buf[i].data.size - 1;
                        } else {
                            self->selection.begin_char_idx -= chars_to_move;
                        }
                    }
                    if (self->selection.end_line == i + 1) {
                        if (self->selection.end_char_idx < chars_to_move) {
                            --self->selection.end_line;
                            self->selection.end_char_idx =
                              self->selection.end_char_idx + self->lines.buf[i].data.size - 1;
                        } else {
                            self->selection.end_char_idx -= chars_to_move;
                        }
                    }
                }

                Vt_mark_proxy_fully_damaged(self, i);
                Vt_mark_proxy_fully_damaged(self, i + 1);

                if (!self->lines.buf[i + 1].data.size) {
                    self->lines.buf[i].was_reflown = false;
                    size_t remove_index            = i + 1;
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
        }
    }

    int underflow = -((int64_t)self->lines.size - self->ws.ws_row);

    if (underflow > 0) {
        for (int i = 0; i < (int)MIN(underflow, removals); ++i)
            Vector_push_VtLine(&self->lines, VtLine_new());
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
        if (self->lines.buf[i].data.size > x && self->lines.buf[i].reflowable) {
            size_t chars_to_move = self->lines.buf[i].data.size - x;

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
            if (i + 1 < bottom_bound && self->lines.buf[i + 1].rejoinable) {
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

                    Vector_insert_VtRune(
                      &self->lines.buf[i + 1].data,
                      self->lines.buf[i + 1].data.buf,
                      *(self->lines.buf[i].data.buf + x + chars_to_move - ii - 1));
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
                Vector_pushv_VtRune(&self->lines.buf[i + 1].data,
                                    self->lines.buf[i].data.buf + x,
                                    chars_to_move);
                self->lines.buf[i].was_reflown    = true;
                self->lines.buf[i + 1].rejoinable = true;
            }
        }
    }

    if (self->lines.size - 1 != self->cursor.row) {
        size_t overflow =
          self->lines.size > self->ws.ws_row ? self->lines.size - self->ws.ws_row : 0;
        size_t whitespace_below = self->lines.size - 1 - self->cursor.row;
        size_t to_pop           = MIN(overflow, MIN(whitespace_below, insertions_made));
        Vector_pop_n_VtLine(&self->lines, to_pop);
    }
}

/**
 * Remove extra columns from all lines */
static void Vt_trim_columns(Vt* self)
{
    for (size_t i = 0; i < self->lines.size; ++i) {
        if (self->lines.buf[i].data.size > (size_t)self->ws.ws_col) {
            Vt_mark_proxy_fully_damaged(self, i);

            CALL_FP(self->callbacks.destroy_proxy,
                    self->callbacks.user_data,
                    &self->lines.buf[i].proxy);

            size_t blanks = 0;

            size_t s = self->lines.buf[i].data.size;
            Vector_pop_n_VtRune(&self->lines.buf[i].data, s - self->ws.ws_col);

            if (self->lines.buf[i].was_reflown)
                continue;

            s = self->lines.buf[i].data.size;

            for (blanks = 0; blanks < s; ++blanks) {
                if (!(self->lines.buf[i].data.buf[s - 1 - blanks].rune.code == ' ' &&
                      ColorRGBA_eq(settings.bg, self->lines.buf[i].data.buf[s - 1 - blanks].bg))) {
                    break;
                }
            }

            Vector_pop_n_VtRune(&self->lines.buf[i].data, blanks);
        }
    }
}

void Vt_resize(Vt* self, uint32_t x, uint32_t y)
{
    if (!x || !y) {
        return;
    }
    if (!self->alt_lines.buf) {
        Vt_trim_columns(self);
    }
    self->saved_cursor_pos  = MIN(self->saved_cursor_pos, x);
    self->saved_active_line = MIN(self->saved_active_line, self->lines.size);
    static uint32_t ox = 0, oy = 0;
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
        if (self->ws.ws_row > y) {
            size_t to_pop = self->ws.ws_row - y;
            if (self->cursor.row + to_pop > Vt_bottom_line(self)) {
                to_pop -= self->cursor.row + to_pop - Vt_bottom_line(self);
            }
            Vector_pop_n_VtLine(&self->lines, to_pop);
            if (self->alt_lines.buf) {
                size_t to_pop_alt = self->ws.ws_row - y;
                if (self->alt_active_line + to_pop_alt > Vt_bottom_line_alt(self)) {
                    to_pop_alt -= self->alt_active_line + to_pop_alt - Vt_bottom_line_alt(self);
                }
                Vector_pop_n_VtLine(&self->alt_lines, to_pop_alt);
            }
        } else {
            for (size_t i = 0; i < y - self->ws.ws_row; ++i) {
                Vector_push_VtLine(&self->lines, VtLine_new());
            }
            if (self->alt_lines.buf) {
                for (size_t i = 0; i < y - self->ws.ws_row; ++i) {
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

    self->pixels_per_cell_x = (double)self->ws.ws_xpixel / self->ws.ws_col;
    self->pixels_per_cell_y = (double)self->ws.ws_ypixel / self->ws.ws_row;

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
    self->scroll_region_bottom = self->ws.ws_row - 1;
}

__attribute__((always_inline, flatten)) static inline int32_t short_sequence_get_int_argument(
  const char* seq)
{
    return *seq == 0 || seq[1] == 0 ? 1 : strtol(seq, NULL, 10);
}

static inline void Vt_handle_dec_mode(Vt* self, int code, bool on)
{
    switch (code) {

        /* Application Cursor Keys (DECCKM) */
        // FIXME: that should be some other separate mode
        case 1:
            self->modes.application_keypad = on;
            break;

        /* Column mode 132/80 (DECCOLM) */
        case 3:
            break;

        /* Reverse video (DECSCNM) */
        case 5:
            // TODO:
            break;

        /* DSR—Extended Cursor Position Report (DECXCPR) */
        case 6:
            // TODO:
            WRN("DECXCPR not implemented\n");
            break;

        /* DECAWM */
        case 7:
            self->modes.no_auto_wrap = !on;
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
            self->cursor.hidden = !on;
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

        /* bell sets urgent WM hint */
        case 1042:
            WRN("Urgency hints not implemented\n");
            break;

        /* bell raises window */
        case 1043:
            WRN("xterm popOnBell not implemented\n");
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
        char  second_last_char =
          self->parser.active_sequence.size < 3
            ? '\0'
            : self->parser.active_sequence.buf[self->parser.active_sequence.size - 3];

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
                        WRN("DECSTR not implemented\n");
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
                                    errno     = 0;
                                    long code = strtol(token->buf + 1, NULL, 10);
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
                                /* 6 - report cursor position */
                                if (arg == 6) {
                                    Vt_output_formated(self,
                                                       "\e[%zu;%zuR",
                                                       Vt_get_cursor_row_screen(self) + 1,
                                                       self->cursor.col + 1);
                                } else {
                                    WRN("Unimplemented DSR(DEC) code: %d\n", arg);
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
                    /* <ESC>[> Pp m / <ESC>[> Pp ; Pv m - Set/reset key modifier options (XTMODKEYS)
                     * Pp = 0 - modifyKeyboard.
                     * Pp = 1 - modifyCursorKeys.
                     * Pp = 2 - modifyFunctionKeys.
                     * Pp = 4 - modifyOtherKeys.
                     */
                    case 'm':
                        // TODO:
                        // break;

                    /* <ESC>[> Ps n - Disable key modifier options, xterm
                     * Pp = 0 - modifyKeyboard.
                     * Pp = 1 - modifyCursorKeys.
                     * Pp = 2 - modifyFunctionKeys.
                     * Pp = 4 - modifyOtherKeys.
                     */
                    case 'n':
                        // TODO:
                        WRN("XTMODKEYS not implemented\n");
                        break;

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

                            /* <ESC>[ Ps $p - Request ANSI mode (DECRQM). VT300 and up */
                            case 'p': {
                                // TODO:
                                /* Not recognized */
                                Vt_output(self, "\e[0$p", 5);
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
                                Vt_handle_multi_argument_SGR(self, self->parser.active_sequence);
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

                                    case 2:
                                        Vt_clear_right(self);
                                        /* fallthrough */
                                    case 1:
                                        Vt_clear_left(self);
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
                                    Vt_insert_char_at_cursor_with_shift(self, blank_space);
                                }
                            } break;

                            /* <ESC>[ Ps a - move cursor right (forward) Ps lines (HPR) */
                            case 'a':
                            /* <ESC>[ Ps C - move cursor right (forward) Ps lines (CUF) */
                            case 'C': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_cursor_right(self);
                            } break;

                            /* <ESC>[ Ps L - Insert line at cursor shift rest down (IL) */
                            case 'L': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_insert_line(self);
                            } break;

                            /* <ESC>[ Ps D - move cursor left (back) Ps lines (CUB) */
                            case 'D': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_cursor_left(self);
                            } break;

                            /* <ESC>[ Ps A - move cursor up Ps lines (CUU) */
                            case 'A': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_cursor_up(self);
                            } break;

                            /* <ESC>[ Ps e - move cursor down Ps lines (VPR) */
                            case 'e':
                            /* <ESC>[ Ps B - move cursor down Ps lines (CUD) */
                            case 'B': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_cursor_down(self);
                            } break;

                            /* <ESC>[ Ps ` - move cursor to column Ps (CBT)*/
                            case '`':
                            /* <ESC>[ Ps G - move cursor to column Ps (CHA)*/
                            case 'G': {
                                MULTI_ARG_IS_ERROR
                                Vt_move_cursor_to_column(self,
                                                         short_sequence_get_int_argument(seq) - 1);
                            } break;

                            /* <ESC>[ Ps J - Erase display (ED) - clear... */
                            case 'J': {
                                MULTI_ARG_IS_ERROR
                                if (*seq == 'J') /* ...from cursor to end of screen */
                                    Vt_erase_to_end(self);
                                else {
                                    int arg = short_sequence_get_int_argument(seq);
                                    switch (arg) {
                                        case 1: /* ...from start to cursor */
                                            if (Vt_scroll_region_not_default(self)) {
                                                Vt_clear_above(self);
                                            } else {
                                                Vt_scroll_out_above(self);
                                            }
                                            break;

                                        case 3: /* ...whole display + scrollback buffer */
                                            /* if (settings.allow_scrollback_clear) { */
                                            /*     Vt_clear_display_and_scrollback(self); */
                                            /* } */
                                            break;

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
                                Vt_move_cursor(self,
                                               self->cursor.col,
                                               short_sequence_get_int_argument(seq) - 1);
                            } break;

                            /* <ESC>[ Ps ; Ps r - Set scroll region (top;bottom) (DECSTBM)
                             * default: full window */
                            case 'r': {
                                uint32_t top, bottom;

                                if (*seq != 'r') {
                                    if (sscanf(seq, "%u;%u", &top, &bottom) == EOF) {
                                        WRN("invalid CSI(DECSTBM) sequence %s\n", seq);
                                        break;
                                    }
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

                            /* <ESC>[ Pn I - cursor forward ps tabulations (CHT) */
                            case 'I': {
                                MULTI_ARG_IS_ERROR
                                // TODO:
                            } break;

                            /* <ESC>[ Pn Z - cursor backward ps tabulations (CBT) */
                            case 'Z': {
                                MULTI_ARG_IS_ERROR
                                // TODO:
                            } break;

                            /* <ESC>[ Pn g - tabulation clear (TBC) */
                            case 'g': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);

                                switch (arg) {
                                    case 0:
                                        // TODO: clear currnet tabstop
                                        break;
                                    case 3:
                                        // TODO: clear all tabstops
                                        break;
                                    default:;
                                }

                            } break;

                            /* no args: 1:1, one arg: x:1 */
                            /* <ESC>[ Py ; Px f - move cursor to Px-Py (HVP) (deprecated) */
                            case 'f':
                            /* <ESC>[ Py ; Px H - move cursor to Px-Py (CUP) */
                            case 'H': {
                                uint32_t x = 1, y = 1;
                                if (*seq != 'H' && sscanf(seq, "%u;%u", &y, &x) == EOF) {
                                    WRN("invalid CSI(CUP) sequence %s\n", seq);
                                    break;
                                }
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
                                                       "\e[%zu;%zuR",
                                                       Vt_get_cursor_row_screen(self) + 1,
                                                       self->cursor.col + 1);
                                } else {
                                    WRN("Unimplemented DSR code: %d\n", arg);
                                }
                            } break;

                            /* <ESC>[ Ps M - Delete lines (default = 1) (DL) */
                            case 'M': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_delete_line(self);
                            } break;

                            /* <ESC>[ Ps S - Scroll up (default = 1) (SU) */
                            case 'S': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_scroll_up(self);
                            } break;

                            /* <ESC>[ Ps T - Scroll down (default = 1) (SD) */
                            case 'T': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i)
                                    Vt_scroll_down(self);
                            } break;

                            /* <ESC>[ Ps X - Erase Ps Character(s) (default = 1) (ECH) */
                            case 'X':
                                MULTI_ARG_IS_ERROR
                                Vt_erase_chars(self, short_sequence_get_int_argument(seq));
                                break;

                            /* <ESC>[ Ps P - Delete Ps Character(s) (default = 1) (DCH) */
                            case 'P':
                                MULTI_ARG_IS_ERROR
                                Vt_delete_chars(self, short_sequence_get_int_argument(seq));
                                break;

                            /* <ESC>[ Ps b -  Repeat the preceding graphic character Ps times (REP)
                             * in xterm any cursor movement or SGR sequences after inserting the
                             * character cause this to have no effect */
                            case 'b': {
                                MULTI_ARG_IS_ERROR
                                if (likely(self->last_interted)) {
                                    int arg = short_sequence_get_int_argument(seq);
                                    for (int i = 0; i < arg; ++i) {
                                        Vt_insert_char_at_cursor(self, *self->last_interted, false);
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
                                    *(args + nargs) = *seq == ';' ? -1 : strtol(seq, NULL, 10);
                                    seq             = strstr(seq, ";");
                                    if (seq)
                                        ++seq;
                                }

                                if (!nargs)
                                    break;

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
                                        if (nargs >= 2) {
                                            int32_t target_w = args[1];
                                            int32_t target_h = nargs >= 3 ? args[2] : -1;

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
                                        CALL_FP(self->callbacks.on_action_performed,
                                                self->callbacks.user_data);
                                        CALL_FP(self->callbacks.on_repaint_required,
                                                self->callbacks.user_data);
                                        break;

                                    /* Resize in cells */
                                    case 8: {
                                        if (nargs >= 2) {
                                            int32_t target_rows = args[1];
                                            int32_t target_cols = nargs >= 3 ? args[2] : -1;

                                            Pair_uint32_t target_text_area_dims = CALL_FP(
                                              self->callbacks.on_window_size_from_cells_requested,
                                              self->callbacks.user_data,
                                              target_rows > 0 ? target_rows : 1,
                                              target_cols > 0 ? target_cols : 1);

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
                                    case 10:
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
                                        break;

                                    /* Report iconification state */
                                    case 11: {
                                        bool is_minimized =
                                          CALL_FP(self->callbacks.on_minimized_state_requested,
                                                  self->callbacks.user_data);
                                        Vt_output_formated(self, "\e[%d", is_minimized ? 1 : 2);

                                    } break;

                                    /* Report window position */
                                    case 13: {
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
                                        Vt_output_formated(self,
                                                           "\e[4;%d;%d;t",
                                                           self->ws.ws_xpixel,
                                                           self->ws.ws_ypixel);
                                    } break;

                                    /* Report text area size in chars */
                                    case 18: {
                                        Vt_output_formated(self,
                                                           "\e[8;%d;%d;t",
                                                           self->ws.ws_col,
                                                           self->ws.ws_row);

                                    } break;

                                    /* Report window size in chars */
                                    case 19: {
                                        Vt_output_formated(self,
                                                           "\e[9;%d;%d;t",
                                                           self->ws.ws_col,
                                                           self->ws.ws_row);

                                    } break;

                                    /* Report icon name */
                                    case 20:
                                        /* Report window title */
                                    case 21: {
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

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
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
    for (size_t i = 0; i < self->ws.ws_row; ++i) {
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
    Vt_select_end(self);
    self->last_interted = NULL;
    if (self->alt_lines.buf) {
        Vector_destroy_VtLine(&self->lines);
        self->lines          = self->alt_lines;
        self->alt_lines.buf  = NULL;
        self->alt_lines.size = 0;
        if (save_mouse) {
            self->cursor.col = self->alt_cursor_pos;
            self->cursor.row = self->alt_active_line;
        }
        self->scroll_region_top    = 0;
        self->scroll_region_bottom = self->ws.ws_row - 1;
        Vt_visual_scroll_reset(self);
    }
}

/**
 * Interpret a single argument SGR command */
__attribute__((hot)) static void Vt_handle_single_argument_SGR(Vt* self, char* command)
{
    int cmd = *command ? strtol(command, NULL, 10) : 0;

#define MAYBE_DISABLE_ALL_UNDERLINES                                                               \
    if (!settings.allow_multiple_underlines) {                                                     \
        self->parser.char_state.underlined      = false;                                           \
        self->parser.char_state.doubleunderline = false;                                           \
        self->parser.char_state.curlyunderline  = false;                                           \
    }

    switch (cmd) {
        /**'Enable' character attribs */

        /* Normal (default) */
        case 0:
            Vt_reset_text_attribs(self);
            break;

        /* Bold, VT100 */
        case 1:
            if (self->parser.char_state.rune.style == VT_RUNE_ITALIC) {
                self->parser.char_state.rune.style = VT_RUNE_BOLD_ITALIC;
            } else {
                self->parser.char_state.rune.style = VT_RUNE_BOLD;
            }
            break;

        /* Faint/dim/decreased intensity, ECMA-48 2nd */
        case 2:
            self->parser.char_state.dim = true;
            break;

        /* Italicized, ECMA-48 2nd */
        case 3:
            if (self->parser.char_state.rune.style == VT_RUNE_BOLD) {
                self->parser.char_state.rune.style = VT_RUNE_BOLD_ITALIC;
            } else {
                self->parser.char_state.rune.style = VT_RUNE_ITALIC;
            }
            break;

        /* Underlined */
        case 4:
            MAYBE_DISABLE_ALL_UNDERLINES
            self->parser.char_state.underlined = true;
            break;

        /* Slow (less than 150 per minute) blink */
        case 5:
        /* Fast blink (MS-DOS) (most terminals that implement this use the same speed) */
        case 6:
            self->parser.char_state.blinkng = true;
            break;

        /* Inverse
         *
         * There is no clear definition of what this should actually do, but reversing all colors,
         * after bg and fg were determined, seems to be the widely accepted behavior. So swaping
         * parser->char_state::{bg, fg} right here will not work because all color change sequences
         * should be inverted if this is set. We can get away with not storing this for each VtRune
         * by changing the colors when a character is inserted.
         */
        case 7:
            self->parser.color_inverted = true;
            break;

        /* Invisible, i.e., hidden, ECMA-48 2nd, VT300 */
        case 8:
            self->parser.char_state.hidden = true;
            break;

        /* Crossed-out characters, ECMA-48 3rd */
        case 9:
            self->parser.char_state.strikethrough = true;
            break;

        /* Fraktur (not widely supported) */
        /* case 20: */
        /*     break; */

        /* Doubly-underlined, ECMA-48 3rd */
        case 21:
            MAYBE_DISABLE_ALL_UNDERLINES
            self->parser.char_state.doubleunderline = true;
            break;

        /* Framed (not widely supported) */
        /* case 51: */
        /*     break; */

        /* Encircled (not widely supported) */
        /* case 52: */
        /*     break; */

        /* Overlined (widely supported extension) */
        case 53:
            self->parser.char_state.overline = true;
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
            if (self->parser.char_state.rune.style == VT_RUNE_BOLD_ITALIC) {
                self->parser.char_state.rune.style = VT_RUNE_ITALIC;
            } else if (self->parser.char_state.rune.style == VT_RUNE_BOLD) {
                self->parser.char_state.rune.style = VT_RUNE_NORMAL;
            }

            self->parser.char_state.dim = false;
            break;

        /* Not italicized, ECMA-48 3rd */
        case 23:
            if (self->parser.char_state.rune.style == VT_RUNE_BOLD_ITALIC) {
                self->parser.char_state.rune.style = VT_RUNE_BOLD;
            } else if (self->parser.char_state.rune.style == VT_RUNE_ITALIC) {
                self->parser.char_state.rune.style = VT_RUNE_NORMAL;
            }
            break;

        /*  Not underlined, ECMA-48 3rd */
        case 24:
            self->parser.char_state.underlined = false;
            break;

        /* Steady (not blinking), ECMA-48 3rd */
        case 25:
            self->parser.char_state.blinkng = false;
            break;

        /* Positive (not inverse), ECMA-48 3rd */
        case 27:
            self->parser.color_inverted = false;
            break;

        /* Visible (not hidden), ECMA-48 3rd, VT300 */
        case 28:
            self->parser.char_state.hidden = false;
            break;

        /* Not crossed-out, ECMA-48 3rd */
        case 29:
            self->parser.char_state.strikethrough = false;
            break;

        /* Set foreground color to default, ECMA-48 3rd */
        case 39:
            self->parser.char_state.fg = settings.fg;
            break;

        /* Set background color to default, ECMA-48 3rd */
        case 49:
            self->parser.char_state.bg = settings.bg;
            break;

        /* Set underline color to default (widely supported extension) */
        case 59:
            self->parser.char_state.linecolornotdefault = false;
            break;

            /* Disable all ideogram attributes */
            /* case 65: */
            /*     break; */

        default:
            if (30 <= cmd && cmd <= 37) {
                self->parser.char_state.fg = settings.colorscheme.color[cmd - 30];
            } else if (40 <= cmd && cmd <= 47) {
                self->parser.char_state.bg =
                  ColorRGBA_from_RGB(settings.colorscheme.color[cmd - 40]);
            } else if (90 <= cmd && cmd <= 97) {
                self->parser.char_state.fg = settings.colorscheme.color[cmd - 82];
            } else if (100 <= cmd && cmd <= 107) {
                self->parser.char_state.bg =
                  ColorRGBA_from_RGB(settings.colorscheme.color[cmd - 92]);
            } else {
                WRN("Unknown SGR code: %d\n", cmd);
            }
    }
}

/**
 * Interpret an SGR sequence
 *
 * SGR codes are separated by one ';' or ':', some values require a set number of following
 * 'arguments'. 'Commands' may be combined into a single sequence. A ';' without any text should be
 * interpreted as a 0 (CSI ; 3 m == CSI 0 ; 3 m), but ':' should not
 * (CSI 58:2::130:110:255 m == CSI 58:2:130:110:255 m)" */
static void Vt_handle_multi_argument_SGR(Vt* self, Vector_char seq)
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
                    long idx = MIN(strtol(args[2]->buf + 1, NULL, 10), 255);

                    if (args[0]->buf[1] == '3') {
                        self->parser.char_state.fg = color_palette_256[idx];
                    } else if (args[0]->buf[1] == '4') {
                        self->parser.char_state.bg = ColorRGBA_from_RGB(color_palette_256[idx]);
                    } else if (args[0]->buf[1] == '5') {
                        self->parser.char_state.linecolornotdefault = true;
                        self->parser.char_state.line                = color_palette_256[idx];
                    }

                } else if (!strcmp(args[1]->buf + 1, "2")) {
                    /* sent as 24-bit rgb (three arguments) */
                    if ((args[3] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                        (args[4] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                        long c[3] = { MIN(strtol(args[2]->buf + 1, NULL, 10), 255),
                                      MIN(strtol(args[3]->buf + 1, NULL, 10), 255),
                                      MIN(strtol(args[4]->buf + 1, NULL, 10), 255) };

                        if (args[0]->buf[1] == '3') {
                            self->parser.char_state.fg =
                              (ColorRGB){ .r = c[0], .g = c[1], .b = c[2] };
                        } else if (args[0]->buf[1] == '4') {
                            self->parser.char_state.bg =
                              (ColorRGBA){ .r = c[0], .g = c[1], .b = c[2], .a = 255 };
                        } else if (args[0]->buf[1] == '5') {
                            self->parser.char_state.linecolornotdefault = true;
                            self->parser.char_state.line =
                              (ColorRGB){ .r = c[0], .g = c[1], .b = c[2] };
                        }
                    }
                }
            }
        } else if (!strcmp(token->buf + 1, "4")) {

            /* possible curly underline */
            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token)))) {

                /* enable this only on "4:3" not "4;3" */
                if (!strcmp(args[1]->buf, ":3")) {

                    if (!settings.allow_multiple_underlines) {
                        self->parser.char_state.underlined      = false;
                        self->parser.char_state.doubleunderline = false;
                    }

                    self->parser.char_state.curlyunderline = true;
                } else {
                    Vt_handle_single_argument_SGR(self, args[0]->buf + 1);
                    token = Vector_iter_back_Vector_char(&tokens, token);
                }
            } else {
                Vt_handle_single_argument_SGR(self, args[0]->buf + 1);
                break; // end of sequence
            }

        } else {
            Vt_handle_single_argument_SGR(self, token->buf + 1);
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
        char* seq  = self->parser.active_sequence.buf;
        int   arg  = 0;
        char* text = seq;

        if (isdigit(*seq)) {
            arg = strtol(seq, &text, 10);
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

            /* Modify regular color palette */
            case 4:
            /* Modify special color palette */
            case 5:
            /* enable/disable special color */
            case 6:
                // TODO:
                WRN("Dynamic colors not implemented\n");
                break;

            /* pwd info as URI */
            case 7: {
                free(self->work_dir);
                char* uri = seq + 2; // 7;
                if (streq_wildcard(uri, "file:*") && strlen(uri) >= 8) {
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
            case 8:
                // TODO:
                WRN("OSC 8 hyperlinks not implemented\n");
                break;

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

            /* Set dynamic colors for xterm colorOps */
            case 10 ... 19:
                WRN("Dynamic colors not implemented\n");
                break;

            /* Coresponding colorOps resets */
            case 110 ... 119:
                // TODO: reset things, when there are things to reset
                break;

            case 50:
                WRN("xterm fontOps not implemented\n");
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

static void Vt_reset_text_attribs(Vt* self)
{
    memset(&self->parser.char_state, 0, sizeof(self->parser.char_state));
    self->parser.char_state.rune.code = ' ';
    self->parser.char_state.bg        = settings.bg;
    self->parser.char_state.fg        = settings.fg;
    self->parser.color_inverted       = false;
}

/**
 * Move cursor to first column */
static void Vt_carriage_return(Vt* self)
{
    self->last_interted = NULL;
    Vt_move_cursor_to_column(self, 0);
}

/**
 * make a new empty line at cursor position, scroll down contents below */
static void Vt_insert_line(Vt* self)
{
    self->last_interted = NULL;
    Vector_insert_VtLine(&self->lines,
                         Vector_at_VtLine(&self->lines, self->cursor.row),
                         VtLine_new());

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
        Vector_insert_VtLine(&self->lines,
                             Vector_at_VtLine(&self->lines, self->cursor.row),
                             VtLine_new());
        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else {
        Vt_cursor_up(self);
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * delete active line, content below scrolls up */
static void Vt_delete_line(Vt* self)
{
    self->last_interted = NULL;

    Vector_remove_at_VtLine(&self->lines, self->cursor.row, 1);

    size_t  insert_idx   = MIN(Vt_get_scroll_region_bottom(self), Vt_bottom_line(self));
    VtLine* insert_point = Vector_at_VtLine(&self->lines, insert_idx);

    Vector_insert_VtLine(&self->lines, insert_point, VtLine_new());

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
    Vector_insert_VtLine(&self->lines,
                         Vector_at_VtLine(&self->lines, Vt_get_scroll_region_top(self)),
                         VtLine_new());
    Vector_remove_at_VtLine(&self->lines,
                            MAX(Vt_top_line(self), Vt_get_scroll_region_bottom(self)),
                            1);
    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

/**
 * Move cursor one cell down if possible */
static void Vt_cursor_down(Vt* self)
{
    self->last_interted = NULL;
    if (self->cursor.row < Vt_bottom_line(self))
        ++self->cursor.row;
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Move cursor one cell up if possible */
static void Vt_cursor_up(Vt* self)
{
    self->last_interted = NULL;
    if (self->cursor.row > Vt_top_line(self))
        --self->cursor.row;
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Move cursor one cell to the left if possible */
static inline void Vt_cursor_left(Vt* self)
{
    self->last_interted = NULL;
    if (self->cursor.col)
        --self->cursor.col;
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Move cursor one cell to the right if possible */
static inline void Vt_cursor_right(Vt* self)
{
    self->last_interted = NULL;
    if (self->cursor.col < self->ws.ws_col)
        ++self->cursor.col;
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
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
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
    Vt_cursor_left(self);
}

/**
 * Overwrite characters with colored space */
static inline void Vt_erase_chars(Vt* self, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        size_t idx = self->cursor.col + i;
        if (idx >= self->lines.buf[self->cursor.row].data.size) {
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, self->parser.char_state);
        } else {
            self->lines.buf[self->cursor.row].data.buf[idx] = self->parser.char_state;
        }
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * remove characters at cursor, remaining content scrolls left */
static void Vt_delete_chars(Vt* self, size_t n)
{
    /* Trim if line is longer than screen area */
    if (self->lines.buf[self->cursor.row].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->lines.buf[self->cursor.row].data.size - self->ws.ws_col);
    }

    Vector_remove_at_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->cursor.col,
                            MIN(self->lines.buf[self->cursor.row].data.size == self->cursor.col
                                  ? self->lines.buf[self->cursor.row].data.size - self->cursor.col
                                  : self->lines.buf[self->cursor.row].data.size,
                                n));

    /* Fill line to the cursor position with spaces with original propreties
     * before scolling so we get the expected result, when we... */
    VtRune tmp        = self->parser.char_state;
    bool   tmp_invert = self->parser.color_inverted;

    Vt_reset_text_attribs(self);
    self->parser.color_inverted = false;

    if (self->lines.buf[self->cursor.row].data.size >= 2) {
        self->parser.char_state.bg = self->lines.buf[self->cursor.row]
                                       .data.buf[self->lines.buf[self->cursor.row].data.size - 2]
                                       .bg;
    } else {
        self->parser.char_state.bg = settings.bg;
    }

    for (size_t i = self->lines.buf[self->cursor.row].data.size - 1; i < self->ws.ws_col; ++i) {
        Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, self->parser.char_state);
    }

    self->parser.char_state     = tmp;
    self->parser.color_inverted = tmp_invert;

    if (self->lines.buf[self->cursor.row].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->lines.buf[self->cursor.row].data.size - self->ws.ws_col);
    }

    /* ...add n spaces with currently set attributes to the end */
    for (size_t i = 0; i < n && self->cursor.col + i < self->ws.ws_col; ++i) {
        Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, self->parser.char_state);
    }

    /* Trim to screen size again */
    if (self->lines.buf[self->cursor.row].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->lines.buf[self->cursor.row].data.size - self->ws.ws_col);
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static inline void Vt_scroll_out_all_content(Vt* self)
{
    int64_t to_add = 0;
    for (size_t i = Vt_visual_bottom_line(self); i >= Vt_visual_top_line(self); --i) {
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

    self->cursor.row += to_add;
}

static inline void Vt_scroll_out_above(Vt* self)
{
    size_t to_add = Vt_get_cursor_row_screen(self);
    for (size_t i = 0; i < to_add; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }

    self->cursor.row += to_add;
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
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
    Vector_destroy_VtLine(&self->lines);
    self->lines = Vector_new_VtLine(self);
    for (size_t i = 0; i < self->ws.ws_row; ++i) {
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
    for (size_t i = 0; i <= self->cursor.col; ++i) {
        if (i < self->lines.buf[self->cursor.row].data.size)
            self->lines.buf[self->cursor.row].data.buf[i] = self->parser.char_state;
        else
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, self->parser.char_state);
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Clear active line right of cursor and fill it with whatever character
 * attributes are set */
static inline void Vt_clear_right(Vt* self)
{
    for (int32_t i = self->cursor.col; i <= (int32_t)self->ws.ws_col; ++i) {
        if (i + 1 <= (int32_t)self->lines.buf[self->cursor.row].data.size) {
            self->lines.buf[self->cursor.row].data.buf[i] = self->parser.char_state;
        } else {
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, self->parser.char_state);
        }
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Insert character literal at cursor position, deal with reaching column limit */
__attribute__((hot)) static inline void Vt_insert_char_at_cursor(Vt*    self,
                                                                 VtRune c,
                                                                 bool   apply_color_modifications)
{
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);

    if (unlikely(self->cursor.col >= (size_t)self->ws.ws_col)) {
        if (unlikely(self->modes.no_auto_wrap)) {
            --self->cursor.col;
        } else {
            self->cursor.col = 0;
            Vt_insert_new_line(self);
            self->lines.buf[self->cursor.row].rejoinable = true;
        }
    }

    while (self->lines.buf[self->cursor.row].data.size <= self->cursor.col) {
        Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, blank_space);
    }

    if (apply_color_modifications && unlikely(self->parser.color_inverted)) {
        ColorRGB tmp = c.fg;
        c.fg         = ColorRGB_from_RGBA(c.bg);
        c.bg         = ColorRGBA_from_RGB(tmp);
    }

    VtRune* insert_point = &self->lines.buf[self->cursor.row].data.buf[self->cursor.col];
    if (likely(memcmp(insert_point, &c, sizeof(VtRune)))) {
        Vt_mark_proxy_damaged_cell(self, self->cursor.row, self->cursor.col);
        self->lines.buf[self->cursor.row].data.buf[self->cursor.col] = c;
    }

    self->last_interted = &self->lines.buf[self->cursor.row].data.buf[self->cursor.col];
    ++self->cursor.col;

    int width;
    if (unlikely((width = wcwidth(c.rune.code)) > 1)) {
        VtRune tmp    = c;
        tmp.rune.code = VT_RUNE_CODE_WIDE_TAIL;

        for (int i = 0; i < (width - 1); ++i) {
            if (self->lines.buf[self->cursor.row].data.size <= self->cursor.col) {
                Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, tmp);
            } else {
                self->lines.buf[self->cursor.row].data.buf[self->cursor.col] = tmp;
            }
            ++self->cursor.col;
        }
    }
}

static inline void Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c)
{
    if (unlikely(self->cursor.col >= (size_t)self->ws.ws_col)) {
        if (unlikely(self->modes.no_auto_wrap)) {
            --self->cursor.col;
        } else {
            self->cursor.col = 0;
            Vt_insert_new_line(self);
            self->lines.buf[self->cursor.row].rejoinable = true;
        }
    }

    VtRune* insert_point =
      Vector_at_VtRune(&self->lines.buf[self->cursor.row].data, self->cursor.col);
    Vector_insert_VtRune(&self->lines.buf[self->cursor.row].data, insert_point, c);

    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static inline void Vt_empty_line_fill_bg(Vt* self, size_t idx)
{
    ASSERT(self->lines.buf[idx].data.size == 0, "line is empty");

    Vt_mark_proxy_fully_damaged(self, idx);
    if (!ColorRGBA_eq(self->parser.char_state.bg, settings.bg)) {
        for (size_t i = 0; i < self->ws.ws_col; ++i) {
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
        Vector_insert_VtLine(&self->lines,
                             Vector_at_VtLine(&self->lines, self->cursor.row),
                             VtLine_new());
        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else {
        if (Vt_bottom_line(self) == self->cursor.row) {
            Vector_push_VtLine(&self->lines, VtLine_new());
            Vt_empty_line_fill_bg(self, self->lines.size - 1);
        }
        Vt_cursor_down(self);
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Move cursor to given location (@param rows is relative to the screen!) */
static inline void Vt_move_cursor(Vt* self, uint32_t columns, uint32_t rows)
{
    self->last_interted = NULL;
    self->cursor.row    = MIN(rows, (uint32_t)self->ws.ws_row - 1) + Vt_top_line(self);
    self->cursor.col    = MIN(columns, (uint32_t)self->ws.ws_col);
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Move cursor to given column */
static inline void Vt_move_cursor_to_column(Vt* self, uint32_t columns)
{
    self->last_interted = NULL;
    self->cursor.col    = MIN(columns, (uint32_t)self->ws.ws_col);
    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Add a character as a combining character for that rune */
static void VtRune_push_combining(VtRune* self, char32_t codepoint)
{
    ASSERT(unicode_is_combining(codepoint), "must be a combining character");
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
    if (!self->linecolornotdefault) {
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
            if (old_len == strlen(res)) {
                VtRune_push_combining(self->last_interted, c);
            } else if (mbrtoc32(conv, res, ARRAY_SIZE(buff) - 1, &mbs) < 1) {
                /* conversion failed */
                WRN("Unicode normalization failed %s", strerror(errno));
            } else {
                self->last_interted->rune.code = conv[0];
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
    if (unlikely(self->parser.in_mb_seq)) {
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
            if (unlikely(unicode_is_combining(res))) {
                Vt_handle_combinable(self, res);
                return;
            }
            VtRune new_rune    = self->parser.char_state;
            new_rune.rune.code = res;
            Vt_insert_char_at_cursor(self, new_rune, true);
        }
    } else {
        switch (c) {
            case '\a':
                if (!settings.no_flash) {
                    CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
                }
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

            case '\e':
                self->parser.state = PARSER_STATE_ESCAPED;
                break;

            /* Invoke the G1 character set as GL */
            case 14 /* SO */:
                self->charset_gl = &self->charset_g1;
                break;

            /* Invoke the G0 character set (the default) as GL */
            case 15 /* SI */:
                self->charset_gl = &self->charset_g0;
                break;

            case '\t': {
                size_t cp = self->cursor.col;

                // TODO: do this properly
                for (size_t i = 0; i < self->tabstop - (cp % self->tabstop); ++i)
                    Vt_cursor_right(self);
            } break;

            default: {
                // TODO: ISO 8859-1 charset (not UTF-8 mode)
                if (c & (1 << 7)) {
                    mbrtoc32(NULL, &c, 1, &self->parser.input_mbstate);
                    self->parser.in_mb_seq = true;
                    break;
                }
                VtRune new_char    = self->parser.char_state;
                new_char.rune.code = c;

                if (unlikely(self->charset_single_shift && *self->charset_single_shift)) {
                    new_char.rune.code         = (*(self->charset_single_shift))(c);
                    self->charset_single_shift = NULL;
                } else if (unlikely(self->charset_gl && (*self->charset_gl))) {
                    new_char.rune.code = (*(self->charset_gl))(c);
                }
                Vt_insert_char_at_cursor(self, new_char, true);
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
            Vt_handle_CSI(self, c);
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
                    if (!settings.no_flash) {
                        CALL_FP(self->callbacks.on_visual_bell, self->callbacks.user_data);
                    }
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Application Keypad (DECPAM) */
                case '=':
                    self->modes.application_keypad = true;
                    self->parser.state             = PARSER_STATE_LITERAL;
                    return;

                /* Normal Keypad (DECPNM) */
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
                    Vt_select_end(self);
                    Vt_clear_display_and_scrollback(self);
                    Vt_move_cursor(self, 0, 0);
                    self->tabstop              = 8;
                    self->parser.state         = PARSER_STATE_LITERAL;
                    self->scroll_region_top    = 0;
                    self->charset_g0           = NULL;
                    self->charset_g1           = NULL;
                    self->charset_g2           = NULL;
                    self->charset_g3           = NULL;
                    self->charset_single_shift = NULL;
                    self->last_interted        = NULL;
                    self->scroll_region_bottom =
                      CALL_FP(self->callbacks.on_number_of_cells_requested,
                              self->callbacks.user_data)
                        .second -
                      1;
                    Vector_clear_DynStr(&self->title_stack);
                    free(self->title);
                    self->title = NULL;
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
    /* alt buffer is active */
    if (self->alt_lines.buf)
        return;

    int64_t ln_cnt = self->lines.size;
    if (unlikely(ln_cnt > MAX(settings.scrollback * 1.1, self->ws.ws_row))) {
        int64_t to_remove = ln_cnt - settings.scrollback - self->ws.ws_row;
        Vector_remove_at_VtLine(&self->lines, 0, to_remove);
        self->cursor.row -= to_remove;
    }
}

static inline void Vt_clear_proxies(Vt* self)
{
    if (self->scrolling_visual) {
        if (self->visual_scroll_top > self->ws.ws_row * 5) {
            Vt_clear_proxies_in_region(self,
                                       Vt_visual_bottom_line(self) + 4 * self->ws.ws_row,
                                       self->lines.size - 1);
        }
    } else if (self->lines.size > self->ws.ws_row) {
        Vt_clear_proxies_in_region(self,
                                   0,
                                   Vt_visual_top_line(self) ? Vt_visual_top_line(self) - 1 : 0);
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

static const char* normal_keypad_response(const uint32_t key)
{
    switch (key) {
        case XKB_KEY_Up:
            return "\e[A";
        case XKB_KEY_Down:
            return "\e[B";
        case XKB_KEY_Right:
            return "\e[C";
        case XKB_KEY_Left:
            return "\e[D";
        case XKB_KEY_End:
            return "\e[F";
        case XKB_KEY_Home:
            return "\e[H";
        case 127:
            return "\e[P";
        default:
            return NULL;
    }
}

static const char* application_keypad_response(const uint32_t key)
{
    switch (key) {
        case XKB_KEY_Up:
            return "\eOA";
        case XKB_KEY_Down:
            return "\eOB";
        case XKB_KEY_Right:
            return "\eOC";
        case XKB_KEY_Left:
            return "\eOD";
        case XKB_KEY_End:
            return "\eOF";
        case XKB_KEY_Home:
            return "\eOH";
        case XKB_KEY_KP_Enter:
            return "\eOM";
        case XKB_KEY_KP_Multiply:
            return "\eOj";
        case XKB_KEY_KP_Add:
            return "\eOk";
        case XKB_KEY_KP_Separator:
            return "\eOl";
        case XKB_KEY_KP_Subtract:
            return "\eOm";
        case XKB_KEY_KP_Divide:
            return "\eOo";
        case 127:
            return "\e[3~";
        default:
            return NULL;
    }
}

/**
 * Get response format string in normal keypad mode */
static const char* normal_mod_keypad_response(const uint32_t key)
{
    switch (key) {
        case XKB_KEY_Up:
            return "\e[1;%dA";
        case XKB_KEY_Down:
            return "\e[1;%dB";
        case XKB_KEY_Right:
            return "\e[1;%dC";
        case XKB_KEY_Left:
            return "\e[1;%dD";
        case XKB_KEY_End:
            return "\e[1;%dF";
        case XKB_KEY_Home:
            return "\e[1;%dH";
        default:
            return NULL;
    }
}

/**
 * Get response format string in application keypad mode */
static const char* application_mod_keypad_response(const uint32_t key)
{
    switch (key) {
        case XKB_KEY_Up:
            return "\e[1;%dA";
        case XKB_KEY_Down:
            return "\e[1;%dB";
        case XKB_KEY_Right:
            return "\e[1;%dC";
        case XKB_KEY_Left:
            return "\e[1;%dD";
        case XKB_KEY_End:
            return "\e[1;%dF";
        case XKB_KEY_Home:
            return "\e[1;%dH";
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
        resp = self->modes.application_keypad ? application_mod_keypad_response(key)
                                              : normal_mod_keypad_response(key);
        if (resp) {
            Vt_output_formated(self, resp, mods + 1);
            return true;
        }
    } else {
        bool appl_key = self->modes.application_keypad;
        resp          = appl_key ? application_keypad_response(key) : normal_keypad_response(key);
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
    if (key >= XKB_KEY_F1 && key <= XKB_KEY_F35) {
        int f_num = (key + 1) - XKB_KEY_F1;
        if (mods) {
            if (f_num < 5) {
                Vt_output_formated(self, "\e[[1;%u%c", mods + 1, f_num + 'O');
            } else if (f_num == 5) {
                Vt_output_formated(self, "\e[%d;%u~", f_num + 10, mods + 1);
            } else {
                Vt_output_formated(self, "\e[%d;%u~", f_num + 11, mods + 1);
            }
        } else {
            if (f_num < 5) {
                Vt_output_formated(self, "\eO%c", f_num + 'O');
            } else if (f_num == 5) {
                Vt_output_formated(self, "\e[%d~", f_num + 10);
            } else {
                Vt_output_formated(self, "\e[%d~", f_num + 11);
            }
        }
        return true;
    } else if (key == XKB_KEY_Insert) {
        Vt_output(self, "\e[2~", 4);
        return true;
    } else if (key == XKB_KEY_Page_Up) {
        Vt_output(self, "\e[5~", 4);
        return true;
    } else if (key == XKB_KEY_Page_Down) {
        Vt_output(self, "\e[6~", 4);
        return true;
    }

    return false;
}

/**
 *  Substitute keypad keys with normal ones */
static uint32_t numpad_key_convert(uint32_t key)
{
    switch (key) {
        case XKB_KEY_KP_Add:
            return '+';
        case XKB_KEY_KP_Subtract:
            return '-';
        case XKB_KEY_KP_Multiply:
            return '*';
        case XKB_KEY_KP_Divide:
            return '/';
        case XKB_KEY_KP_Equal:
            return '=';
        case XKB_KEY_KP_Decimal:
            return '.';
        case XKB_KEY_KP_Separator:
            return '.';
        case XKB_KEY_KP_Space:
            return ' ';
        case XKB_KEY_KP_Delete:
            return XKB_KEY_Delete;
        case XKB_KEY_KP_Home:
            return XKB_KEY_Home;
        case XKB_KEY_KP_End:
            return XKB_KEY_End;
        case XKB_KEY_KP_Tab:
            return XKB_KEY_Tab;
        case XKB_KEY_KP_0 ... XKB_KEY_KP_9:
            return '0' + key - XKB_KEY_KP_0;
        default:
            return key;
    }
}

/**
 * Respond to key event */
void Vt_handle_key(void* _self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    Vt* self = _self;

    if (!Vt_maybe_handle_unicode_input_key(self, key, rawkey, mods) &&
        !Vt_maybe_handle_keypad_key(self, key, mods) &&
        !Vt_maybe_handle_function_key(self, key, mods)) {
        key = numpad_key_convert(key);

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
                                   "\e[<%u;%lu;%lu%c",
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

    if (!text)
        return;

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
    if (self->alt_lines.buf) {
        Vector_destroy_VtLine(&self->alt_lines);
    }
    Vector_destroy_char(&self->parser.active_sequence);
    Vector_destroy_DynStr(&self->title_stack);
    Vector_destroy_char(&self->unicode_input.buffer);
    Vector_destroy_char(&self->output);
    free(self->title);
    free(self->work_dir);
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
