/* See LICENSE for license information. */


#define _GNU_SOURCE

#include "vt.h"

#include <fcntl.h>
#include <pty.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <utmp.h>

#include "gfx.h"
#include "gui.h"
#include "wcwidth/wcwidth.h"

#define SCROLLBAR_HIDE_DELAY_MS 1500
#define DOUBLE_CLICK_DELAY_MS 300
#define AUTOSCROLL_DELAY_MS 50

VtRune space;

void
(*Vt_destroy_line_proxy)(int32_t proxy[static 4]) = NULL;


static inline size_t
Vt_top_line(const Vt* const self);

static void
Vt_visual_scroll_to(Vt* self, size_t line);

static void
Vt_visual_scroll_up(Vt* self);

static void
Vt_visual_scroll_down(Vt* self);

static void
Vt_visual_scroll_reset(Vt* self);

static inline size_t
Vt_get_scroll_region_top(Vt* self);

static inline size_t
Vt_get_scroll_region_bottom(Vt* self);

static inline bool
Vt_scroll_region_not_default(Vt* self);

static void
Vt_alt_buffer_on(Vt* self, bool save_mouse);

static void
Vt_alt_buffer_off(Vt* self, bool save_mouse);

static void
Vt_handle_prop_seq(Vt* self, Vector_char seq);

static void
Vt_reset_text_attribs(Vt* self);

static void
Vt_carriage_return(Vt* self);

static void
Vt_clear_right(Vt* self);

static void
Vt_clear_left(Vt* self);

static inline void
Vt_scroll_out_all_conten(Vt* self);

static void
Vt_empty_line_fill_bg(Vt* self, size_t idx);

static void
Vt_cursor_down(Vt* self);

static void
Vt_cursor_up(Vt* self);

static void
Vt_cursor_left(Vt* self);

static void
Vt_cursor_right(Vt* self);

static void
Vt_insert_new_line(Vt* self);

static void
Vt_scroll_up(Vt* self);

static void
Vt_scroll_down(Vt* self);

static void
Vt_reverse_line_feed(Vt* self);

static void
Vt_delete_line(Vt* self);

static void
Vt_delete_chars(Vt* self, size_t n);

static void
Vt_erase_chars(Vt* self, size_t n);

static void
Vt_clear_above(Vt* self);

static inline void
Vt_scroll_out_above(Vt* self);

static void
Vt_insert_line(Vt* self);

static void
Vt_clear_display_and_scrollback(Vt* self);

static void
Vt_erase_to_end(Vt* self);

static void
Vt_move_cursor(Vt* self, uint32_t c, uint32_t r);

static void
Vt_push_title(Vt* self);

static void
Vt_pop_title(Vt* self);

static Vector_char
line_to_string(Vector_VtRune* line, size_t begin, size_t end, const char* tail);


#define UTF8_CHAR_INCOMPLETE (-1)
#define UTF8_CHAR_INVALID (-2)
#define UTF8_CHAR_INVALID_INPUT (-3)

static inline int64_t
utf8_decode_validated(char* buf, uint8_t size)
{
    switch (size) {
    case 1:
        if ((*buf & 0b10000000))
            return UTF8_CHAR_INCOMPLETE;
        else
            return *buf;
        break;
    case 2:
        if ( (*buf & 0b11000000) &&
            !(*buf & 0b00100000) ) {
            return (buf[1] & 0b00111111)
                 | (((uint64_t) (buf[0] & 0b00011111)) << 6);
        } else
            return (*buf & 0b11100000) ? UTF8_CHAR_INCOMPLETE : UTF8_CHAR_INVALID;
        break;
    case 3:
        if ( (*buf & 0b11100000) &&
            !(*buf & 0b00010000) ) {
            return (buf[2] & 0b00111111)
                | (((uint64_t) (buf[1] & 0b00111111)) << 6)
                | (((uint64_t) (buf[0] & 0b00001111)) << 12);
        } else
            return (*buf & 0b11110000) ? UTF8_CHAR_INCOMPLETE : UTF8_CHAR_INVALID;
        break;
    case 4:
        if ( (*buf & 0b11110000) &&
            !(*buf & 0b00001000) ) {
            return (buf[3] & 0b00111111)
                | (((uint64_t) (buf[2] & 0b00111111)) << 6)
                | (((uint64_t) (buf[1] & 0b00111111)) << 12)
                | (((uint64_t) (buf[0] & 0b00000111)) << 18);
        } else
            return UTF8_CHAR_INVALID; 
        break;
    default:
        return UTF8_CHAR_INVALID_INPUT;
    }
    return UTF8_CHAR_INVALID_INPUT;
}


static inline uint8_t
utf8_encode2(uint32_t codepoint, char* buf)
{
    if (codepoint > 1114111)
        return 0;
    else if (codepoint > 65536) {
        buf[0] = 0b11110000 | ( (codepoint >> 18) & 0b00000111 );
        buf[1] = 0b10000000 | ( (codepoint >> 12) & 0b00111111 );
        buf[2] = 0b10000000 | ( (codepoint >> 6) & 0b00111111 );
        buf[3] = 0b10000000 | ( (codepoint) & 0b00111111 );
        return 4;
    } else if (codepoint > 2047) {
        buf[0] = 0b11100000 | ( (codepoint >> 12) & 0b00001111 );
        buf[1] = 0b10000000 | ( (codepoint >> 6) & 0b00111111 );
        buf[2] = 0b10000000 | ( (codepoint) & 0b00111111 );
        return 3;
    } else if (codepoint > 127) {
        buf[0] = 0b11000000 | ( (codepoint >> 6) & 0b00011111 );
        buf[1] = 0b10000000 | ( (codepoint) & 0b00111111 );
        return 2;
    } else {
        *buf = (char) codepoint;
        return 1;
    }
}


/**
 * Update gui scrollbar dimensions */
static void
Vt_update_scrollbar_dims(Vt* self)
{
    self->scrollbar.length = 2.0 / self->lines.size * self->ws.ws_row;
    self->scrollbar.top = 2.0 * (double) Vt_visual_top_line(self)
        / (self->lines.size -1);
}


/**
 * Update gui scrollbar visibility */
static void
Vt_update_scrollbar_vis(Vt* self)
{
    static bool last_scrolling = false;
    if (!self->scrolling) {
        if (last_scrolling) {
            self->scrollbar.hide_time =
                TimePoint_ms_from_now(SCROLLBAR_HIDE_DELAY_MS);
        } else if (self->scrollbar.dragging) {
            self->scrollbar.hide_time =
                TimePoint_ms_from_now(SCROLLBAR_HIDE_DELAY_MS);
        } else if (TimePoint_passed(self->scrollbar.hide_time)) {
            if (self->scrollbar.visible) {
                self->scrollbar.visible = false;
                self->repaint_required_notify(self->window_data);
            }
        }
    }
    last_scrolling = self->scrolling;
}


/**
 * @return click event was consumed by gui scrollbar */
static bool
Vt_scrollbar_consume_click(Vt* self,
                           uint32_t button,
                           uint32_t state,
                           int32_t x,
                           int32_t y)
{
    self->scrollbar.autoscroll = AUTOSCROLL_NONE;
    
    if (!self->scrollbar.visible || button > 3)
        return false;

    if (self->scrollbar.dragging && !state) {
        self->scrollbar.dragging = false;
        self->repaint_required_notify(self->window_data);
        return false;
    }

    float dp = 2.0f * ((float) y / (float) self->ws.ws_ypixel);

    if (x > self->ws.ws_xpixel - self->scrollbar.width) {
        // inside region
        if (self->scrollbar.top < dp &&
            self->scrollbar.top + self->scrollbar.length > dp)
        {
            // inside scrollbar
            if (state && (button == MOUSE_BTN_LEFT  ||
                          button == MOUSE_BTN_RIGHT ||
                          button == MOUSE_BTN_MIDDLE))
            {
                self->scrollbar.dragging = true;
                self->scrollbar.drag_position = dp - self->scrollbar.top;
            }
        } else {
            // autside of scrollbar
            if (state && button == MOUSE_BTN_LEFT) {
                /* jump to that position and start dragging in the middle */
                self->scrollbar.dragging = true;
                self->scrollbar.drag_position = self->scrollbar.length /2;
                dp = 2.0f * ((float) y / (float) self->ws.ws_ypixel) -
                    self->scrollbar.drag_position;
                float range = 2.0f -self->scrollbar.length;
                size_t target_line =
                    Vt_top_line(self) * CLAMP(dp, 0.0, range) / range;
                if (target_line != Vt_visual_top_line(self)) {
                    Vt_visual_scroll_to(self, target_line);
                }
            } else if (state && button == MOUSE_BTN_RIGHT) {
                self->scrollbar.autoscroll_next_step =
                    TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);

                if (dp > self->scrollbar.top + self->scrollbar.length /2) {
                    self->scrollbar.autoscroll = AUTOSCROLL_DN;
                } else {
                    self->scrollbar.autoscroll = AUTOSCROLL_UP;
                }

            } else if (state && button == MOUSE_BTN_MIDDLE) {
                /* jump one screen in that direction */
                if (dp > self->scrollbar.top + self->scrollbar.length /2) {
                    Vt_visual_scroll_to(self,
                        self->visual_scroll_top + self->ws.ws_row);
                } else {
                    size_t to = self->visual_scroll_top > self->ws.ws_row ?
                        self->visual_scroll_top - self->ws.ws_row : 0;
                    Vt_visual_scroll_to(self, to);
                }
            }
        }
    } else {
        return false;
    }

    Vt_update_scrollbar_dims(self);
    self->repaint_required_notify(self->window_data);
    
    return true;
}


static bool
Vt_scrollbar_consume_drag(Vt* self, uint32_t button, int32_t x, int32_t y)
{
    if (!self->scrollbar.dragging)
        return false;

    y = CLAMP(y, 0, self->ws.ws_ypixel);
    float dp = 2.0f * ((float) y / (float) self->ws.ws_ypixel) -
        self->scrollbar.drag_position;
    float range = 2.0f - self->scrollbar.length;
    size_t target_line = Vt_top_line(self) * CLAMP(dp, 0.0, range) / range;

    if (target_line != Vt_visual_top_line(self)) {
        Vt_visual_scroll_to(self, target_line);
        Vt_update_scrollbar_dims(self);
        self->repaint_required_notify(self->window_data);
    }

    return true;
}


/**
 * Get string from selected region */
static Vector_char
Vt_select_region_to_string(Vt* self)
{
    Vector_char ret, tmp;
    size_t begin_char_idx, end_char_idx;
    size_t begin_line = MIN(self->selection.begin_line,
                            self->selection.end_line);
    size_t end_line = MAX(self->selection.begin_line, self->selection.end_line);

    if (begin_line == end_line && self->selection.mode != SELECT_MODE_NONE) {
        begin_char_idx = MIN(self->selection.begin_char_idx,
                             self->selection.end_char_idx);
        end_char_idx = MAX(self->selection.begin_char_idx,
                           self->selection.end_char_idx);

        return line_to_string(&self->lines.buf[begin_line].data,
                              begin_char_idx,
                              end_char_idx +1,
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
                             "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line +1; i < end_line; ++i) {
            tmp = line_to_string(&self->lines.buf[i].data, 0, 0, "\n");
            Vector_pushv_char(&ret, tmp.buf, tmp.size -1);
            Vector_destroy_char(&tmp);
        }
        tmp = line_to_string(&self->lines.buf[end_line].data,
                             0,
                             end_char_idx +1,
                             "");
        Vector_pushv_char(&ret, tmp.buf, tmp.size -1);
        Vector_destroy_char(&tmp);
    } else if (self->selection.mode == SELECT_MODE_BOX) {
        ret = line_to_string(&self->lines.buf[begin_line].data,
                             begin_char_idx,
                             end_char_idx +1,
                             "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line +1; i <= end_line; ++i) {
            tmp = line_to_string(&self->lines.buf[i].data,
                                 begin_char_idx,
                                 end_char_idx +1,
                                 i == end_line ? "" : "\n");
            Vector_pushv_char(&ret, tmp.buf, tmp.size -1);
            Vector_destroy_char(&tmp);
        }
    } else {
        ret = Vector_new_char();
    }
    Vector_push_char(&ret, '\0');
    return ret;
}


/**
 * initialize selection region */
static void
Vt_select_init(Vt* self, enum SelectMode mode, int32_t x, int32_t y)
{
    self->selection.next_mode = mode;
    x = CLAMP(x, 0, self->ws.ws_xpixel);
    y = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_x = (double) x / self->pixels_per_cell_x;
    size_t click_y = (double) y / self->pixels_per_cell_y;
    self->selection.click_begin_char_idx = click_x;
    self->selection.click_begin_line =
        Vt_visual_top_line(self) + click_y;
}



/**
 * initialize selection region to clicked word */
static void
Vt_select_init_word(Vt* self, int32_t x, int32_t y)
{
    self->selection.mode = SELECT_MODE_NORMAL;
    x = CLAMP(x, 0, self->ws.ws_xpixel);
    y = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_x = (double) x / self->pixels_per_cell_x;
    size_t click_y = (double) y / self->pixels_per_cell_y;

    Vector_VtRune* ln = &self->lines
        .buf[Vt_visual_top_line(self) +click_y].data;
    size_t cmax = ln->size, begin = click_x, end = click_x;

    while (begin -1 < cmax && begin > 0 && !isspace(ln->buf[begin -1].code))
        --begin;

    while (end +1 < cmax && end > 0 && !isspace(ln->buf[end +1].code))
        ++end;

    self->selection.begin_char_idx = begin;
    self->selection.end_char_idx = end;
    self->selection.begin_line = self->selection.end_line =
        Vt_visual_top_line(self) +click_y;
}


/**
 * initialize selection region to clicked line */
static void
Vt_select_init_line(Vt* self, int32_t y)
{
    self->selection.mode = SELECT_MODE_NORMAL;
    y = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_y = (double) y / self->pixels_per_cell_y;
    self->selection.begin_char_idx = 0;
    self->selection.end_char_idx = self->ws.ws_col;
    self->selection.begin_line = self->selection.end_line =
        Vt_visual_top_line(self) + click_y;
}


static inline void
Vt_destroy_proxies_in_select_region(Vt* self)
{
    for (size_t i = self->selection.begin_line;
         i <= self->selection.end_line;
         ++i)
    {
        if (!self->lines.buf[i].damaged) {
            self->lines.buf[i].damaged = true;
            Vt_destroy_line_proxy(self->lines.buf[i].proxy.data);
        }
    }
}


/**
 * start selection */
static void
Vt_select_commit(Vt* self)
{
    if (self->selection.next_mode != SELECT_MODE_NONE) {
        self->selection.mode = self->selection.next_mode;
        self->selection.next_mode = SELECT_MODE_NONE;
        self->selection.begin_line = self->selection.end_line =
            self->selection.click_begin_line;
        self->selection.begin_char_idx = self->selection.end_char_idx =
            self->selection.click_begin_char_idx;

        Vt_destroy_proxies_in_select_region(self);
    }
}


/**
 * set end glyph for selection
 */
static void
Vt_select_set_end(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        size_t old_end = self->selection.end_line;
        x = CLAMP(x, 0, self->ws.ws_xpixel);
        y = CLAMP(y, 0, self->ws.ws_ypixel);
        size_t click_x = (double) x / self->pixels_per_cell_x;
        size_t click_y = (double) y / self->pixels_per_cell_y;
        self->selection.end_line = Vt_visual_top_line(self) + click_y;
        self->selection.end_char_idx = click_x;
        self->repaint_required_notify(self->window_data);

        for (size_t i = MIN(old_end, self->selection.end_line);
             i <= MAX(old_end, self->selection.end_line);
             ++i)
        {
            if (!self->lines.buf[i].damaged) {
                self->lines.buf[i].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i].proxy.data);
            }
        }
    }
}


static void
Vt_select_end(Vt* self)
{
    self->selection.mode = SELECT_MODE_NONE;
    Vt_destroy_proxies_in_select_region(self);
}


static bool
Vt_consume_drag(Vt* self, uint32_t button, int32_t x, int32_t y)
{
    self->selection.click_count = 0;

    if (button != 1 || !self->selection.dragging)
        return false;

    if (self->selection.next_mode)
        Vt_select_commit(self);

    Vt_select_set_end(self, x, y);
    return true;
}

/**
 * @return does text field consume click event */
static bool
Vt_select_consume_click(Vt* self,
                        uint32_t button,
                        uint32_t state,
                        int32_t x,
                        int32_t y,
                        uint32_t mods)
{
    if (!state)
        self->selection.dragging = false;

    if (self->modes.x10_mouse_compat)
        return false;

    if (button == MOUSE_BTN_LEFT &&
        (!(self->modes.extended_report             ||
           self->modes.mouse_btn_report            ||
           self->modes.mouse_motion_on_btn_report) ||
         FLAG_IS_SET(mods, MODIFIER_SHIFT)))
    {
        if (!state && self->selection.mode == SELECT_MODE_NONE)
            return false;

        if (state) {
            if (!TimePoint_passed(self->selection.next_click_limit)) {
                ++self->selection.click_count;
            } else {
                self->selection.click_count = 0;
            }
            self->selection.next_click_limit =
                TimePoint_ms_from_now(DOUBLE_CLICK_DELAY_MS);
            if (self->selection.click_count == 0) {
                Vt_select_end(self);
                Vt_select_init(self,
                               FLAG_IS_SET(mods, MODIFIER_CONTROL) ?
                                 SELECT_MODE_BOX : SELECT_MODE_NORMAL,
                               x,
                               y);
                self->selection.dragging = true;
            } else if (self->selection.click_count == 1) {
                Vt_select_end(self);
                Vt_select_init_word(self, x, y);
                self->repaint_required_notify(self->window_data);
            } else if (self->selection.click_count == 2) {
                Vt_select_end(self);
                Vt_select_init_line(self, y);
                self->repaint_required_notify(self->window_data);
            }

        }
        return true;

    /* paste from primary selection */
    } else if (button == MOUSE_BTN_MIDDLE &&
               state &&
               (!(self->modes.mouse_btn_report ||
                  self->modes.mouse_motion_on_btn_report) ||
                (FLAG_IS_SET(mods, MODIFIER_CONTROL) ||
                 FLAG_IS_SET(mods, MODIFIER_SHIFT))))
    {
        if (self->selection.mode != SELECT_MODE_NONE) {
            Vector_char text = Vt_select_region_to_string(self);
            Vt_handle_clipboard(self, text.buf);
            Vector_destroy_char(&text);
        } else {
            ;// TODO: we don't own primary, get it from the window system
        }
    } else if (self->selection.mode != SELECT_MODE_NONE) {
        Vt_select_end(self);
        return true;
    }

    return false;
}


static Rune
char_sub_uk(char original)
{
    return original == '#' ? 0xa3/* £ */ : original;
}


static Rune
char_sub_gfx(char original)
{
    switch (original) {
    case 'a': return 0x2592; // ▒
    case 'b': return 0x2409; // ␉
    case 'c': return 0x240c; // ␌
    case 'd': return 0x240d; // ␍
    case 'e': return 0x240a; // ␊
    case 'f': return 0x00b0; // °
    case 'g': return 0x00b1; // ±
    case 'h': return 0x2424; // ␤
    case 'i': return 0x240b; // ␋
    case 'j': return 0x2518; // ┘
    case 'k': return 0x2510; // ┐
    case 'l': return 0x250c; // ┌
    case 'm': return 0x2514; // └
    case 'n': return 0x253c; // ┼
    case 'o': return 0x23ba; // ⎺
    case 'p': return 0x23bb; // ⎻
    case 'q': return 0x2500; // ─
    case 'r': return 0x23BC; // ⎼
    case 's': return 0x23BD; // ⎽
    case 't': return 0x251C; // ├
    case 'u': return 0x2524; // ┤
    case 'v': return 0x2534; // ┴
    case 'w': return 0x252C; // ┬
    case 'x': return 0x2502; // │
    case 'y': return 0x2264; // ≤
    case 'z': return 0x2265; // ≥
    case '{': return 0x03C0; // π
    case '}': return 0x00A3; // £
    case '|': return 0x2260; // ≠
    case '~': return 0x22C5; // ⋅
    case '`': return 0x2666; // ♦
    default : return original;
    }
}


/**
 * substitute invisible characters with readable string
 */
static char*
control_char_get_pretty_string(const char c)
{
    switch (c) {
    case '\f': return TERMCOLOR_RED_LIGHT"<FF>"TERMCOLOR_DEFAULT;
    case '\n': return TERMCOLOR_CYAN"<LF>"TERMCOLOR_DEFAULT;
    case '\a': return TERMCOLOR_YELLOW"<BELL>"TERMCOLOR_DEFAULT;
    case '\r': return TERMCOLOR_MAGENTA"<CR>"TERMCOLOR_DEFAULT;
    case '\t': return TERMCOLOR_BLUE"<TAB>"TERMCOLOR_DEFAULT;
    case '\v': return TERMCOLOR_BLUE_LIGHT"<V-TAB>"TERMCOLOR_DEFAULT;
    case '\b': return TERMCOLOR_RED"<BS>"TERMCOLOR_DEFAULT;
    case '\e': return TERMCOLOR_GREEN_LIGHT"<ESC>"TERMCOLOR_DEFAULT;
    case  0xE: return TERMCOLOR_CYAN_LIGHT"<SO>"TERMCOLOR_DEFAULT;
    case  0xF: return TERMCOLOR_MAGENTA_LIGHT"<SI>"TERMCOLOR_DEFAULT;
    case  127: return TERMCOLOR_MAGENTA_LIGHT"<DEL>"TERMCOLOR_DEFAULT;
    default  : return NULL;
    }
}


/**
 * make pty messages more readable
 */
static char*
pty_string_prettyfy(const char* str)
{
    bool esc = false, seq = false, important = false;

    Vector_char fmt = Vector_new_char();
    for (; *str; ++str) {
        if (seq) {
            if (isalpha(*str)) {
                Vector_pushv_char(&fmt,
                                  TERMCOLOR_BG_DEFAULT,
                                  strlen(TERMCOLOR_BG_DEFAULT));
                seq = false;
                important = true;
            }
        } else {
            if (*str == '\e') {
                esc = true;
                Vector_pushv_char(&fmt,
                                  TERMCOLOR_BG_GRAY_DARK,
                                  strlen(TERMCOLOR_BG_GRAY_DARK));
            } else if (*str == '[' && esc) {
                seq = true;
                esc = false;
            }
        }

        char* ctr = control_char_get_pretty_string(*str);
        if (ctr)
            Vector_pushv_char(&fmt, ctr, strlen(ctr));
        else {
            if (important) {
                switch (*str) {
                case 'H':
                    Vector_pushv_char(&fmt,
                                      TERMCOLOR_BG_GREEN,
                                      strlen(TERMCOLOR_BG_GREEN));
                    break;
                case 'm':
                    Vector_pushv_char(&fmt,
                                      TERMCOLOR_BG_BLUE,
                                      strlen(TERMCOLOR_BG_BLUE));
                    break;
                default:
                    Vector_pushv_char(&fmt,
                                      TERMCOLOR_BG_RED_LIGHT,
                                      strlen(TERMCOLOR_BG_RED_LIGHT));
                }
                Vector_push_char(&fmt, *str);
                Vector_pushv_char(&fmt, TERMCOLOR_RESET,
                                  strlen(TERMCOLOR_RESET));

            } else if (*str == ';' && seq) {
                Vector_pushv_char(&fmt, TERMCOLOR_RED_LIGHT,
                                  strlen(TERMCOLOR_RED_LIGHT));
                Vector_push_char(&fmt, *str);
                Vector_pushv_char(&fmt, TERMCOLOR_DEFAULT,
                                  strlen(TERMCOLOR_DEFAULT));
            } else if (isdigit(*str) && seq) {
                Vector_pushv_char(
                    &fmt,
                    TERMCOLOR_BG_WHITE TERMCOLOR_BLACK,
                    strlen(TERMCOLOR_BG_WHITE TERMCOLOR_BLACK));
                Vector_push_char(&fmt, *str);
                Vector_pushv_char(
                    &fmt,
                    TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT,
                    strlen(TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT));
            } else {
                Vector_push_char(&fmt, *str);
            }
        }
        important = false;
    }
    Vector_pushv_char(&fmt, TERMCOLOR_BG_DEFAULT, strlen(TERMCOLOR_BG_DEFAULT));
    Vector_push_char(&fmt, '\0');
    return fmt.buf;
}


/**
 * get utf-8 text from @param line in range from @param begin to @param end 
 */
static Vector_char
line_to_string(Vector_VtRune* line, size_t begin, size_t end, const char* tail)
{
    Vector_char res;

    end = MIN(end ? end : line->size, line->size);
    begin = MIN(begin, line->size -1);

    if (begin >= end) {
        res = Vector_new_with_capacity_char(2);
        if (tail)
            Vector_pushv_char(&res, tail, strlen(tail) +1);
        return res;
    }
    res = Vector_new_with_capacity_char(end - begin);
    char utfbuf[4];
    for (uint32_t i = begin; i < end; ++i) {
        if (line->buf[i].code > CHAR_MAX) {
            int bytes = utf8_encode2(line->buf[i].code, utfbuf);
            Vector_pushv_char(&res, utfbuf, bytes);
        } else
            Vector_push_char(&res, line->buf[i].code);
    }

    if (tail)
        Vector_pushv_char(&res, tail, strlen(tail) +1);
    return res;
}


/**
 * Split string on any character in @param symbols, filter out any character in
 * @param filter first character of returned string is the immediately preceding
 * delimiter, '\0' if none.
 */
static inline Vector_Vector_char
string_split_on(const char* str, const char* symbols, const char* filter)
{
    Vector_Vector_char ret = Vector_new_with_capacity_Vector_char(8);
    Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
    Vector_push_char(&ret.buf[0], '\0');

    for (;*str; ++str) {
        for (const char* i = filter; filter && *i; ++i)
            if (*str == *i)
                goto continue_outer;

        char any_symbol = 0;
        for (const char* i = symbols; *i; ++i) {
            if (*i == *str) {
                any_symbol = *i;
                break;
            }
        }

        if (any_symbol) {
            Vector_push_char(&ret.buf[ret.size -1], '\0');
            if (ret.buf[ret.size -1].size == 2) {
                ret.buf[ret.size -1].size = 0;
            } else {
                Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
            }
            Vector_push_char(&ret.buf[ret.size -1], any_symbol);
        } else
            Vector_push_char(&ret.buf[ret.size -1], *str);
        continue_outer:;
    }
    Vector_push_char(&ret.buf[ret.size - 1], '\0');

    return ret;
}


__attribute__((always_inline))
static inline bool
is_csi_sequence_terminated(const char* seq, const size_t size)
{
    if (!size)
        return false;

    return isalpha(seq[size - 1]);
}


__attribute__((always_inline))
static inline bool
is_osc_sequence_terminated(const char* seq, const size_t size)
{
    if (!size)
        return false;

    if (seq[size - 1] == '\a' ||
        (size > 1 && seq[size -2] == '\e'))
    {
        return true;
    }

    return false;
}


Vt
Vt_new(uint32_t cols, uint32_t rows)
{
    Vt self;
    memset(&self, 0, sizeof(Vt));

    self.ws = (struct winsize) { .ws_col = cols, .ws_row = rows };
    self.scroll_region_bottom = rows;

    openpty(&self.master, &self.slave,
            #ifdef DEBUG
            self.dev_name,
            #else
            NULL,
            #endif
            NULL, &self.ws);

    self.pid = fork();

    if (self.pid == 0) {

        close(self.master);

        /* does all required magic and creates a new session id for this
         * process */
        login_tty(self.slave);

        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");

        setenv("COLORTERM", "truecolor", 1);

        setenv("TERM", settings.term, 1);

        if (execvp(settings.shell, (char* const*) settings.shell_argv)) {
            /* stdout from here will be displayed in terminal window. */
            printf(TERMCOLOR_RED"Failed to execute command: \"%s\".\n%s"
                   "\n\narguments: ",
                   settings.shell, strerror(errno));
            for (int i = 0; i < settings.shell_argc; ++i)
                printf("%s%s", settings.shell_argv[i],
                       i == settings.shell_argc -1 ? "" : ", ");
            puts("\nPress Ctrl-c to exit");

            for (;;)
                pause();
        }

    } else if (self.pid < 0) {
        int errnocpy = errno;
        ERR("Failed to fork process %s", strerror(errnocpy));
    }

    self.is_done = false;
    self.parser.state = PARSER_STATE_LITERAL;
    self.parser.utf8_cur_seq_len = 1;
    self.parser.utf8_in_seq = false;

    self.parser.char_state = space = (VtRune) {
        .code = ' ',
        .bg = settings.bg,
        .fg = settings.fg,
        .state = VT_RUNE_NORMAL,
        .dim           = false,
        .hidden        = false,
        .blinkng       = false,
        .underlined    = false,
        .strikethrough = false,
    };

    self.parser.active_sequence = Vector_new_char();

    close(self.slave);

    self.lines = Vector_new_VtLine();

    for (size_t i = 0; i < self.ws.ws_row; ++i) {
        Vector_push_VtLine(&self.lines, VtLine_new());
    }

    self.cursor.type = CURSOR_BLOCK;
    self.cursor.blinking = true;
    self.cursor_pos = 0;

    self.tabstop = 8;

    self.title = NULL;
    self.title_stack = Vector_new_size_t();

    self.scrollbar.width = 10;

    return self;
}


void
Vt_kill_program(Vt* self)
{
    if (self->pid > 1)
        kill(self->pid, SIGKILL);
    self->pid = 0;
}


static inline size_t
Vt_top_line_alt(const Vt* const self)
{
    return self->alt_lines.size <= self->ws.ws_row ? 0 :
        self->alt_lines.size - self->ws.ws_row;
}


static inline size_t
Vt_bottom_line(Vt* self)
{
    return Vt_top_line(self) + self->ws.ws_row -1;
}


static inline size_t
Vt_bottom_line_alt(Vt* self)
{
    return Vt_top_line_alt(self) + self->ws.ws_row -1;
}


static inline size_t
Vt_active_screen_index(Vt* self)
{
    return self->active_line - Vt_top_line(self);
}


static inline size_t
Vt_get_scroll_region_top(Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_top;
}


static inline size_t
Vt_get_scroll_region_bottom(Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_bottom -1;
}


static inline bool
Vt_scroll_region_not_default(Vt* self)
{
    return Vt_get_scroll_region_top(self) != Vt_visual_top_line(self) ||
        Vt_get_scroll_region_bottom(self) +1 != Vt_visual_bottom_line(self);
}


static void
Vt_visual_scroll_up(Vt* self)
{
    if (self->scrolling) {
        if (self->visual_scroll_top) {
            --self->visual_scroll_top;
        }
    } else if (Vt_top_line(self)) {
        self->scrolling = true;
        self->scrollbar.visible = true;
        self->visual_scroll_top = Vt_top_line(self) -1;
    }
}


static void
Vt_visual_scroll_down(Vt* self)
{
    if (self->scrolling && Vt_top_line(self) > self->visual_scroll_top) {
        ++self->visual_scroll_top;
        if (self->visual_scroll_top == Vt_top_line(self))
            self->scrolling = false;
    }
}


static void
Vt_visual_scroll_to(Vt* self, size_t line)
{
    line = MIN(line, Vt_top_line(self));

    self->visual_scroll_top = line;
    self->scrolling = line != Vt_top_line(self);
}


static void
Vt_visual_scroll_reset(Vt* self)
{
    self->scrolling = false;
    Vt_update_scrollbar_dims(self);
}


static void
Vt_dump_info(Vt* self)
{
    static int dump_index = 0;
    printf("\n====================[ STATE DUMP %2d ]====================\n", dump_index++);


    printf("Modes:\n");
    printf("  application keypad:               %d\n", self->modes.application_keypad);
    printf("  auto repeat:                      %d\n", self->modes.auto_repeat);
    printf("  bracketed paste:                  %d\n", self->modes.bracket_paste);
    printf("  send DEL on delete:               %d\n", self->modes.del_sends_del);
    printf("  don't send esc on alt:            %d\n", self->modes.no_alt_sends_esc);
    printf("  extended reporting:               %d\n", self->modes.extended_report);
    printf("  window focus events reporting:    %d\n", self->modes.window_focus_events_report);
    printf("  mouse button reporting:           %d\n", self->modes.mouse_btn_report);
    printf("  motion on mouse button reporting: %d\n", self->modes.mouse_motion_on_btn_report);
    printf("  mouse motion reporting:           %d\n", self->modes.mouse_motion_report);
    printf("  x11 compat mouse reporting:       %d\n", self->modes.x10_mouse_compat);
    printf("  no auto wrap:                     %d\n", self->modes.no_auto_wrap);
    printf("  reverse video:                    %d\n", self->modes.video_reverse);


    printf("\n");
    
    printf("  S | Number of lines %zu (last index: %zu)\n",
           self->lines.size, Vt_bottom_line(self));
    printf("  C | Terminal size %hu x %hu\n",
           self->ws.ws_col, self->ws.ws_row);
    printf("V R | \n");
    printf("I O | Visible region: %zu - %zu\n",
           Vt_visual_top_line(self), Vt_visual_bottom_line(self));
    printf("E L | \n");
    printf("W L | Active line:  real: %zu (visible: %zu)\n",
           self->active_line, Vt_active_screen_index(self));
    printf("P   | Cursor position: %zu type: %d blink: %d hidden: %d\n",
           self->cursor_pos, self->cursor.type, self->cursor.blinking, self->cursor.hidden);
    printf("O R | Scroll region: %zu - %zu\n",
           Vt_get_scroll_region_top(self), Vt_get_scroll_region_bottom(self));
    printf("R E | \n");
    printf("T G +----------------------------------------------------\n");
    printf("| |  BUFFER: %s\n", (self->alt_lines.buf? "ALTERNATIVE" : "MAIN"));
    printf("V V  \n");
    for (size_t i = 0; i < self->lines.size; ++i) {
        Vector_char str = line_to_string(&self->lines.buf[i].data, 0, 0, "");
        printf("%c %c %4zu%c sz:%4zu dmg:%d proxy{%3d,%3d,%3d,%3d} reflow{%d,%d} data: %.30s\n",
               i == Vt_top_line(self) ? 'v' :
               i == Vt_bottom_line(self) ? '^' : ' ',
               i == Vt_get_scroll_region_top(self) ||
               i == Vt_get_scroll_region_bottom(self) ? '-' : ' ',
               i,
               i == self->active_line ? '*' : ' ',
               self->lines.buf[i].data.size,
               self->lines.buf[i].damaged,
               self->lines.buf[i].proxy.data[0],
               self->lines.buf[i].proxy.data[1],
               self->lines.buf[i].proxy.data[2],
               self->lines.buf[i].proxy.data[3],

               self->lines.buf[i].reflowable,
               self->lines.buf[i].rejoinable,
               
               str.buf);
        Vector_destroy_char(&str);
    }
}


static void
Vt_reflow_expand(Vt* self, uint32_t x)
{
    size_t bottom_bound = self->active_line;

    int removals = 0;

    while (bottom_bound > 0 && self->lines.buf[bottom_bound].rejoinable) {
        --bottom_bound;
    }

    for (size_t i = 0; i < bottom_bound; ++i) {

        if (self->lines.buf[i].data.size < x && self->lines.buf[i].reflowable) {
            size_t chars_to_move = x - self->lines.buf[i].data.size;

            if (i +1 < bottom_bound && self->lines.buf[i +1].rejoinable) {
                chars_to_move = MIN(chars_to_move,
                                    self->lines.buf[i +1].data.size);

                Vector_pushv_VtRune(&self->lines.buf[i].data,
                                    self->lines.buf[i +1].data.buf,
                                    chars_to_move);

                Vector_remove_at_VtRune(&self->lines.buf[i +1].data,
                                        0, chars_to_move);
                
                self->lines.buf[i].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i].proxy.data);

                self->lines.buf[i +1].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i +1].proxy.data);

                if (!self->lines.buf[i +1].data.size) {
                    self->lines.buf[i].was_reflown = false;
                    Vector_remove_at_VtLine(&self->lines, i +1, 1);
                    --self->active_line;
                    --bottom_bound;
                    ++removals;
                }
            }
        }
    }


    int underflow =  -((int64_t) self->lines.size - self->ws.ws_row);


    if (underflow > 0) {
        for (int i = 0; i < (int) MIN(underflow, removals); ++i)
            Vector_push_VtLine(&self->lines, VtLine_new());
    }
    
}


static void
Vt_reflow_shrink(Vt* self, uint32_t x)
{
    size_t insertions_made = 0;

    size_t bottom_bound = self->active_line;

    while (bottom_bound > 0 && self->lines.buf[bottom_bound].rejoinable) {
        --bottom_bound;
    }

    for (size_t i = 0; i < bottom_bound; ++i) {
        if (self->lines.buf[i].data.size > x && self->lines.buf[i].reflowable) {
            size_t chars_to_move = self->lines.buf[i].data.size - x;

            // line below is a reflow already
            if (i +1 < bottom_bound && self->lines.buf[i +1].rejoinable) {
                for (size_t ii = 0; ii < chars_to_move; ++ii) {
                    Vector_insert_VtRune(&self->lines.buf[i +1].data,
                                        self->lines.buf[i +1].data.buf,
                                        *(self->lines.buf[i].data.buf + x + ii));
                }

                self->lines.buf[i +1].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i +1].proxy.data);
                
            } else if (i < bottom_bound) {
                ++insertions_made;
                
                Vector_insert_VtLine(&self->lines,
                                     (self->lines.buf) + (i +1),
                                     VtLine_new());
                ++self->active_line;
                ++bottom_bound;

                Vector_pushv_VtRune(&self->lines.buf[i +1].data,
                                    self->lines.buf[i].data.buf + x,
                                    chars_to_move);

                self->lines.buf[i].was_reflown = true;
                self->lines.buf[i +1].rejoinable = true;
            }
        }
    }


    if (self->lines.size -1 != self->active_line) {

        size_t overflow = self->lines.size > self->ws.ws_row ? self->lines.size - self->ws.ws_row : 0;

        size_t whitespace_below = self->lines.size -1 - self->active_line;
        
        Vector_pop_n_VtLine(&self->lines, MIN(overflow,
                                              MIN(whitespace_below,
                                                  insertions_made)));
    }
}

/**
 * Remove extra columns from all lines
 */
static void
Vt_trim_columns(Vt* self)
{
    for (size_t i = 0; i < self->lines.size; ++i) {
        if (self->lines.buf[i].data.size > (size_t) self->ws.ws_col)
        {
            self->lines.buf[i].damaged = true;
            Vt_destroy_line_proxy(self->lines.buf[i].proxy.data);

            size_t blanks = 0;

            size_t s = self->lines.buf[i].data.size;
            Vector_pop_n_VtRune(&self->lines.buf[i].data, s - self->ws.ws_col);


            if (self->lines.buf[i].was_reflown)
                continue;
            
            s = self->lines.buf[i].data.size;
            
            for (blanks = 0; blanks < s; ++blanks) {
                if (!(self->lines.buf[i].data.buf[s -1 - blanks].code == ' ' &&
                    ColorRGBA_eq(settings.bg,
                                 self->lines.buf[i].data.buf[s -1 - blanks].bg)))
                {
                    break;
                }
            }

            Vector_pop_n_VtRune(&self->lines.buf[i].data, blanks);
        }
    }
}


void
Vt_resize(Vt* self, uint32_t x, uint32_t y)
{
    if (!x || !y)
        return;

    if (!self->alt_lines.buf)
        Vt_trim_columns(self);

    static uint32_t ox = 0, oy = 0;
    if (x != ox || y != oy) {

        if (!self->alt_lines.buf && !Vt_scroll_region_not_default(self)) {
            if (x < ox) {
                Vt_reflow_shrink(self, x);
            } else if (x > ox) {
                Vt_reflow_expand(self, x);
            }
        }

        if (self->ws.ws_row > y) {
            size_t to_pop = self->ws.ws_row - y;

            if (self->active_line + to_pop > Vt_bottom_line(self)) {
                to_pop -= self->active_line + to_pop -
                    Vt_bottom_line(self);
            }

            Vector_pop_n_VtLine(&self->lines, to_pop);

            if (self->alt_lines.buf) {
                size_t to_pop_alt = self->ws.ws_row - y;

                if (self->alt_active_line + to_pop_alt >
                    Vt_bottom_line_alt(self))
                {
                    to_pop_alt -= self->alt_active_line + to_pop_alt -
                        Vt_bottom_line_alt(self);
                }
                
                Vector_pop_n_VtLine(&self->alt_lines, to_pop_alt);
            }
        } else {
            for (size_t i = 0; i < y - self->ws.ws_row; ++i)
                Vector_push_VtLine(&self->lines, VtLine_new());

            if (self->alt_lines.buf) {
                for (size_t i = 0; i < y - self->ws.ws_row; ++i)
                    Vector_push_VtLine(&self->alt_lines, VtLine_new());
            }
        }

        ox = x;
        oy = y;
        Pair_uint32_t px = gl_pixels(x, y);
        self->ws = (struct winsize) { .ws_col = x,
                                      .ws_row = y,
                                      .ws_xpixel = px.first,
                                      .ws_ypixel = px.second
        };

        self->pixels_per_cell_x = (double) self->ws.ws_xpixel / self->ws.ws_col;
        self->pixels_per_cell_y = (double) self->ws.ws_ypixel / self->ws.ws_row;

        if (ioctl(self->master, TIOCSWINSZ, &self->ws) < 0)
            WRN("IO operation failed %s\n", strerror(errno));

        self->scroll_region_top = 0;
        self->scroll_region_bottom = gl_get_char_size().second;

        Vt_update_scrollbar_dims(self);
    }
}


bool
Vt_wait(Vt* self)
{
    /* FD_ZERO(&self->rfdset); */
    /* FD_ZERO(&self->wfdset); */

    // needs to be reset every time
    FD_SET(self->master, &self->rfdset);
    FD_SET(self->master, &self->wfdset);

    if (0 > pselect(MAX(self->master, self->io) + 1,
                    &self->rfdset,
                    &self->wfdset,
                    NULL, NULL, NULL))
    {
        if (errno == EINTR || errno == EAGAIN) {
            errno = 0;
            return true;
        } else {
            ERR("IO operation failed %s", strerror(errno));
        }
    }

    return false;
}


__attribute__((always_inline, flatten))
static inline int32_t
short_sequence_get_int_argument(const char* seq)
{
    return *seq == 0 || seq[1] == 0 ? 1 : strtol(seq, NULL, 10);
}


__attribute__((always_inline))
static inline void
Vt_handle_mode_set(Vt* self, int code, bool on)
{
    switch (code) {
    /* keypad mode (DECCKM) */
    case 1:
        self->modes.application_keypad = on;
        break;

    case 5: /* DECSCNM -- Reverse video */
        // TODO:
        break;


    /* DECAWM */
    case 7:
        self->modes.no_auto_wrap = !on;
        break;

    /* DECARM */
    case 8:
        self->modes.auto_repeat = on;
        break;

        /* hide/show cursor (DECTCEM) */
    case 25:
        self->cursor.hidden = on;
        break;

    /* Very visible cursor (CVVIS) */
        //case 12:
        //break;

    case 1000:
        self->modes.mouse_btn_report = !on;
        break;

    case 1002:
        self->modes.mouse_motion_on_btn_report = !on;
        break;

    case 1003:
        self->modes.mouse_motion_report = on;
        break;

    case 1004:
        self->modes.window_focus_events_report = !on;
        break;

    case 1006:
        self->modes.extended_report = !on;
        break;

    case 1037:
        self->modes.del_sends_del = on;
        break;

    case 1039:
        self->modes.no_alt_sends_esc = !on;
        break;

    case 47:
    case 1047:
    case 1049:
        if (on)
            Vt_alt_buffer_off(self, code == 1049);
        else
            Vt_alt_buffer_on(self, code == 1049);
        break;

    case 2004:
        self->modes.bracket_paste = on;
        break;

    case 1001:
    case 1005:
    case 1015:
        WRN("Requested unimplemeted mouse mode %d\n", code);
        break;

    default:
        WRN("Unknown DECSET/DECRET code: "TERMCOLOR_DEFAULT"%d\n", code);
    }
}


__attribute__((always_inline))
static inline void
Vt_handle_cs(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_csi_sequence_terminated(self->parser.active_sequence.buf,
                                   self->parser.active_sequence.size))
    {

        Vector_push_char(&self->parser.active_sequence, '\0');
        char* seq = self->parser.active_sequence.buf;
        char last_char = self->parser.active_sequence.buf
            [self->parser.active_sequence.size - 2];

        if (*seq != '?') {

            /* sequence without question mark */
            switch (last_char) {

            /* <ESC>[ Ps ; ... m - change one or more text attributes (SGR) */
            case 'm': {
                Vector_pop_n_char(&self->parser.active_sequence, 2);
                Vector_push_char(&self->parser.active_sequence, '\0');
                Vt_handle_prop_seq(self, self->parser.active_sequence);
            }
            break;


            /* <ESC>[ Ps K - clear(erase) line right of cursor (EL)
             * none/0 - right 1 - left 2 - all */
            case 'K': {
                switch (*seq == 'K' ? 0 : short_sequence_get_int_argument(seq)) {
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
                    WRN("Unknown control sequence: "TERMCOLOR_DEFAULT"%s\n", seq);
                }
            }
            break;


            /* <ESC>[ Ps C - move cursor right (forward) Ps lines (CUF) */
            case 'C': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg;++i)
                    Vt_cursor_right(self);
            }
            break;


            /* <ESC>[ Ps L - Insert line at cursor shift rest down (IL) */
            case 'L': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg;++i)
                    Vt_insert_line(self);
            }
            break;


            /* <ESC>[ Ps D - move cursor left (back) Ps lines (CUB) */
            case 'D': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg; ++i)
                    Vt_cursor_left(self);
            }
            break;


            /* <ESC>[ Ps A - move cursor up Ps lines (CUU) */
            case 'A': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg; ++i)
                    Vt_cursor_up(self);
            }
            break;


            /* <ESC>[ Ps B - move cursor down Ps lines (CUD) */
            case 'B': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg; ++i)
                    Vt_cursor_down(self);
            }
            break;


            /* <ESC>[ Ps ` - move cursor to column Ps (CBT)*/
            case '`':
            /* <ESC>[ Ps G - move cursor to column Ps (CHA)*/
            case 'G': {
                self->cursor_pos = MIN(short_sequence_get_int_argument(seq) -1,
                                       self->ws.ws_col);
            }
            break;


            /* <ESC>[ Ps J - Erase display (ED) - clear... */
            case 'J': {
                if (*seq == 'J')  /* ...from cursor to end of screen */
                    Vt_erase_to_end(self);
                else {
                    int arg = short_sequence_get_int_argument(seq);
                    switch (arg) {
                    case 1:  /* ...from start to cursor */
                        if (Vt_scroll_region_not_default(self)) {
                            Vt_clear_above(self);
                        } else {
                            Vt_scroll_out_above(self);
                        }
                        break;

                    case 3:  /* ...whole display + scrollback buffer */
                        /* if (settings.allow_scrollback_clear) { */
                        /*     Vt_clear_display_and_scrollback(self); */
                        /* } */
                        break;

                    case 2:  /* ...whole display. Contents should not actually be
                              * removed, but saved to scroll history if no scroll
                              * region is set */
                        if (self->alt_lines.buf) {
                            Vt_clear_display_and_scrollback(self);
                        } else {
                            if (Vt_scroll_region_not_default(self)) {
                                Vt_clear_above(self);
                                Vt_erase_to_end(self);
                            } else {
                                Vt_scroll_out_all_conten(self);
                            }
                        }
                        break;
                    }
                }
            }
            break;


            /* <ESC>[ Ps d - move cursor to row Ps (VPA) */
            case 'd': {
                /* origin is 1:1 */
                Vt_move_cursor(self,
                                self->cursor_pos,
                                short_sequence_get_int_argument(seq) -1);
            }
            break;


            /* <ESC>[ Ps ; Ps r - Set scroll region (top;bottom) (DECSTBM)
             * default: full window */
            case 'r': {
                uint32_t top = 0, bottom = gl_get_char_size().second;
                if (*seq != 'r') {
                    sscanf(seq, "%u;%u", &top, &bottom);
                    --top;
                    --bottom;
                }
                self->scroll_region_top = top;
                self->scroll_region_bottom = bottom;
            }
            break;


            /* <ESC>[ Py ; Px H - move cursor to Px-Py (CUP)
             * no args: 1:1 */
            case 'f':
            case 'H': {
                uint32_t x = 0, y = 0;
                if (*seq != 'H') {
                    sscanf(seq, "%u;%u", &y, &x);
                    --x;
                    --y;
                }
                Vt_move_cursor(self, x, y);
            }
            break;


            /* <ESC>[...c - Send device attributes (Primary DA) */
            case 'c': {
                /* VT 102 */
                memcpy(self->out_buf, "\e[?6c", 6);
                Vt_write(self);
            }
            break;

            
            /* <ESC>[...n - Device status report (DSR) */
            case 'n': {
                int arg = short_sequence_get_int_argument(seq);
                if (arg == 5) {
                    /* 5 - is terminal ok
                     *  ok - 0, not ok - 3 */
                    memcpy(self->out_buf, "\e[0n", 5);
                    Vt_write(self);
                } else if (arg == 6) {
                    /* 6 - report cursor position */
                    sprintf(self->out_buf,
                            "\e[%zu;%zuR",
                            Vt_active_screen_index(self) + 1,
                            self->cursor_pos + 1);
                }
            }
            break;


            /* <ESC>[ Ps M - Delete lines (default = 1) (DL) */
            case 'M': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg; ++i)
                    Vt_delete_line(self);
            }
            break;

            /* <ESC>[ Ps S - Scroll up (default = 1) (SU) */
            case 'S': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg; ++i)
                    Vt_scroll_up(self);
            }
            break;


            /* <ESC>[ Ps T - Scroll down (default = 1) (SD) */
            case 'T': {
                int arg = short_sequence_get_int_argument(seq);
                for (int i = 0; i < arg; ++i)
                    Vt_scroll_down(self);
            }
            break;


            /* <ESC>[ Ps X - Erase Ps Character(s) (default = 1) (ECH) */
            case 'X':
                Vt_erase_chars(self, short_sequence_get_int_argument(seq));
            break;


            /* <ESC>[ Ps P - Delete Ps Character(s) (default = 1) (DCH) */
            case 'P':
                Vt_delete_chars(self, short_sequence_get_int_argument(seq));
            break;


            /* <ESC>[ Ps i - Local printing related commands */
            case 'i':
            break;


            /* <ESC>[ Ps q - Set cursor style (DECSCUSR) */
            case 'q': {
                int arg = short_sequence_get_int_argument(seq);
                switch (arg) {
                case 0:
                case 1:
                    self->cursor.type = CURSOR_BLOCK;
                    self->cursor.blinking = false;
                    break;
                case 2:
                    self->cursor.type = CURSOR_BLOCK;
                    self->cursor.blinking = true;
                    break;
                case 3:
                    self->cursor.type = CURSOR_UNDERLINE;
                    self->cursor.blinking = true;
                    break;
                case 4:
                    self->cursor.type = CURSOR_UNDERLINE;
                    self->cursor.blinking = false;
                    break;
                case 5:
                    self->cursor.type = CURSOR_BEAM;
                    self->cursor.blinking = true;
                    break;
                case 6:
                    self->cursor.type = CURSOR_BEAM;
                    self->cursor.blinking = false;
                    break;

                default:
                    WRN("Unknown DECSCUR code:"TERMCOLOR_DEFAULT" %d\n", arg);
                }
            }
            break;


            /* <ESC>[ Ps ; ... t - WindowOps */
            case 't': {
                int nargs;
                int args[4];

                for (nargs = 0; seq && nargs < 4 && *seq != 't'; ++nargs) {
                    *(args + nargs) = strtol(seq, NULL, 10);
                    seq = strstr(seq, ";");
                    if (seq) ++seq;
                }

                if (!nargs)
                    break;

                switch (args[0]) {

                /* de-iconyfy */
                case 1:
                    break;

                /* iconyfy */
                case 2:
                    break;

                /* move window to args[1]:args[2] */
                case 3:
                    break;

                /* resize in pixels */
                case 4:
                    break;

                /* raise window */
                case 5:
                    break;

                /* lower window */
                case 6:
                    break;

                /* refresh(?!) window */
                case 7:
                    break;

                /* resize in characters */
                case 8:
                    break;

                case 9: {
                    /* unmaximize window */
                    if (args[1] == 0 && nargs >= 2) {
                    }

                    /* maximize window */
                    else if (args[1] == 1 && nargs >= 2) {
                    } else {
                        WRN("Invalid control sequence:"TERMCOLOR_DEFAULT" %s\n",
                            seq);
                    }
                }
                break;

                /* Report iconification state */
                case 11:
                    /* if (iconified) */
                    /*     write(CSI 1 t) */
                    /* else */
                    /*     write(CSI 2 t) */
                    break;

                /* Report window position */
                case 13: {
                    Pair_uint32_t pos = self->get_position(self->window_data);
                    snprintf(self->out_buf, sizeof self->out_buf, "\e[3;%d;%d;t",
                             pos.first, pos.second);
                    Vt_write(self);
                }
                break;


                /* Report window size in pixels */
                case 14: {
                    snprintf(self->out_buf, sizeof self->out_buf, "\e[4;%d;%d;t",
                             self->ws.ws_xpixel, self->ws.ws_ypixel);
                    Vt_write(self);
                }
                break;


                /* Report text area size in chars */
                case 18:
                    snprintf(self->out_buf, sizeof self->out_buf, "\e[8;%d;%d;t",
                             self->ws.ws_col, self->ws.ws_row);
                    Vt_write(self);
                    break;

                /* Report window size in chars */
                case 19:
                    snprintf(self->out_buf, sizeof self->out_buf, "\e[9;%d;%d;t",
                             self->ws.ws_col, self->ws.ws_row);
                    Vt_write(self);
                    break;


                /* Report icon name */
                case 20:
                /* Report window title */
                case 21:
                    snprintf(self->out_buf, sizeof self->out_buf, "\e]L%s\e\\",
                             self->title);
                    Vt_write(self);
                    break;
                    
                /* push title to stack */
                case 22:
                    break;

                /* pop title from stack */
                case 23:
                    break;

                /* Resize window to args[1] lines (DECSLPP) */
                default: {
                    int arg = short_sequence_get_int_argument(seq);
                    uint32_t ypixels = gl_pixels(arg, 0).first;
                }

                }

            }
            break;

            default:
                ;
                WRN("Unknown control sequence: "TERMCOLOR_DEFAULT"%s\n", seq);

            }//end switch

        } else {
            /* sequence starts with question mark */
            switch (last_char) {

            /* DEC Private Mode Reset (DECRST) */
            case 'l': {
                int arg = short_sequence_get_int_argument(seq + 1);
                Vt_handle_mode_set(self, arg, true);
            }
                break;

            /* DEC Private Mode Set (DECSET) */
            case 'h': {
                int arg = short_sequence_get_int_argument(seq + 1);
                Vt_handle_mode_set(self, arg, false);
            }
                break;
            }
        }

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state = PARSER_STATE_LITERAL;
    }
}


__attribute__((always_inline))
static inline void
Vt_handle_simple_prop_cmd(Vt* self, char* command)
{
    int cmd = *command ? strtol(command, NULL, 10) : 0;

    #define MAYBE_DISABLE_ALL_UNDERLINES                     \
        if (!settings.allow_multiple_underlines) {           \
            self->parser.char_state.underlined = false;      \
            self->parser.char_state.doubleunderline = false; \
            self->parser.char_state.curlyunderline = false;  \
        }

    switch (cmd) {

        /* Set property */
        case 0:
            Vt_reset_text_attribs(self);
            break;

        case 1:
            self->parser.char_state.state = VT_RUNE_BOLD;
            break;

        case 2:
            self->parser.char_state.dim = true;
            break;

        case 3:
            self->parser.char_state.state = VT_RUNE_ITALIC;
            break;

        case 4:
            MAYBE_DISABLE_ALL_UNDERLINES
            self->parser.char_state.underlined = true;
            break;

            /* slow blink */
        case 5:
            self->parser.char_state.blinkng = true;
            break;

            /* fast blink */
        case 6:
            self->parser.char_state.blinkng = true;
            break;

        case 7:
            self->parser.color_inverted = true;
            break;

        case 8:
            self->parser.char_state.hidden = true;
            break;

        case 9:
            self->parser.char_state.strikethrough = true;
            break;

        case 53:
            self->parser.char_state.overline = true;
            break;

        case 21:
            MAYBE_DISABLE_ALL_UNDERLINES
            self->parser.char_state.doubleunderline = true;
            break;

        /* Unset property */
        case 22:
            self->parser.char_state.dim = false;
            break;

        case 23:
            self->parser.char_state.state = VT_RUNE_NORMAL;
            break;

        case 24:
            self->parser.char_state.underlined = false;
            break;

        case 25:
            self->parser.char_state.blinkng = false;
            break;

        case 27:
            self->parser.color_inverted = false;
            break;

        case 28:
            self->parser.char_state.hidden = false;
            break;

        case 29:
            self->parser.char_state.strikethrough = false;
            break;

        case 19:
            self->parser.char_state.strikethrough = false;
            break;

        case 39:
            self->parser.char_state.fg = settings.fg;
            break;

        case 49:
            self->parser.char_state.bg = settings.bg;
            break;

        default:
            if (30 <= cmd && cmd <= 37) {
                self->parser.char_state.fg =
                    settings.colorscheme.color[cmd - 30];
            } else if (40 <= cmd && cmd <= 47) {
            self->parser.char_state.bg =
                ColorRGBA_from_RGB(settings.colorscheme.color[cmd - 40]);
            } else if (90 <= cmd && cmd <= 97) {
                self->parser.char_state.fg =
                    settings.colorscheme.color[cmd - 82];
            } else if (100 <= cmd && cmd <= 107) {
                self->parser.char_state.bg =
                    ColorRGBA_from_RGB(settings.colorscheme.color[cmd - 92]);
            } else
                WRN("Unknown SGR code: %d\n", cmd);
    }
}


__attribute__((always_inline))
static inline void
Vt_alt_buffer_on(Vt* self, bool save_mouse)
{
    Vt_visual_scroll_reset(self);
    self->alt_lines = self->lines;
    self->lines = Vector_new_VtLine();
    for (size_t i = 0; i < self->ws.ws_row; ++i)
        Vector_push_VtLine(&self->lines, VtLine_new());
    if (save_mouse) {
        self->alt_cursor_pos = self->cursor_pos;
        self->alt_active_line = self->active_line;
    }
    self->active_line = 0;
}


__attribute__((always_inline))
static inline void
Vt_alt_buffer_off(Vt* self, bool save_mouse)
{
    if (self->alt_lines.buf) {
        Vector_destroy_VtLine(&self->lines);
        self->lines = self->alt_lines;
        self->alt_lines.buf = NULL;
        self->alt_lines.size = 0;
        if (save_mouse) {
            self->cursor_pos = self->alt_cursor_pos;
            self->active_line = self->alt_active_line;
        }
        self->scroll_region_top = 0;
        self->scroll_region_bottom = self->ws.ws_row;
        Vt_visual_scroll_reset(self);
    }
}


/**
 * SGR codes are separated by one or multiple ';' or ':',
 * some values require a set number of following 'arguments'.
 * 'Commands' may be combined into a single sequence. 
 */
__attribute__((always_inline, flatten))
static inline void
Vt_handle_prop_seq(Vt* self, Vector_char seq)
{
    Vector_Vector_char tokens = string_split_on(seq.buf, ";:", NULL);

    for (Vector_char* token = NULL;
         (token = Vector_iter_Vector_char(&tokens, token));)
    {
        Vector_char* args[] = { token, NULL, NULL, NULL, NULL };
        
        /* color change 'commands' */
        if (!strcmp(token->buf +1, "38") /* foreground */||
            !strcmp(token->buf +1, "48") /* background */||
            !strcmp(token->buf +1, "58") /* underline  */)
        {
            /* next argument determines how the color will be set and final
             * number of args */
            
            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                (args[2] = (token = Vector_iter_Vector_char(&tokens, token))))
            {
                if (!strcmp(args[1]->buf +1, "5")) {
                    /* from 256 palette (one argument) */
                    long idx = MIN(strtol(args[2]->buf +1, NULL, 10), 255);

                    if (args[0]->buf[1] == '3')
                            self->parser.char_state.fg = color_palette_256[idx];

                    else if (args[0]->buf[1] == '4')
                            self->parser.char_state.bg =
                                ColorRGBA_from_RGB(color_palette_256[idx]);

                    else if (args[0]->buf[1] == '5') {
                        self->parser.char_state.linecolornotdefault = true;
                        self->parser.char_state.line = color_palette_256[idx];
                    }

                } else if (!strcmp(args[1]->buf +1, "2")) {
                    /* sent as 24-bit rgb (three arguments) */
                    if ((args[3] =
                         (token = Vector_iter_Vector_char(&tokens, token))) &&
                        (args[4] =
                         (token = Vector_iter_Vector_char(&tokens, token))))
                    {
                        long c[3] = {
                            MIN(strtol(args[2]->buf +1, NULL, 10), 255),
                            MIN(strtol(args[3]->buf +1, NULL, 10), 255),
                            MIN(strtol(args[4]->buf +1, NULL, 10), 255)
                        };

                        if (args[0]->buf[1] == '3') {
                            self->parser.char_state.fg = (ColorRGB) {
                                .r = c[0],
                                .g = c[1],
                                .b = c[2]
                            };
                        } else if (args[0]->buf[1] == '4') {
                            self->parser.char_state.bg = (ColorRGBA) {
                                .r = c[0],
                                .g = c[1],
                                .b = c[2],
                                .a = 255
                            };
                        } else if (args[0]->buf[1] == '5') {
                            self->parser.char_state.linecolornotdefault = true;
                            self->parser.char_state.line = (ColorRGB) {
                                .r = c[0],
                                .g = c[1],
                                .b = c[2]
                            };
                        }
                    }
                }
            }
        } else if (!strcmp(token->buf +1, "4")) {

            // possible curly underline
            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token)))) {

                // enable this only on "4:3" not "4;3"
                if (!strcmp(args[1]->buf, ":3")) {

                    if (!settings.allow_multiple_underlines) {
                        self->parser.char_state.underlined = false;
                        self->parser.char_state.doubleunderline = false;
                    }
                    
                    self->parser.char_state.curlyunderline = true;
                } else {
                    Vt_handle_simple_prop_cmd(self, args[0]->buf +1);
                    Vt_handle_simple_prop_cmd(self, args[1]->buf +1);
                }
            } else {
                Vt_handle_simple_prop_cmd(self, args[0]->buf +1);
                break; // end of sequence
            }

        } else {
            Vt_handle_simple_prop_cmd(self, token->buf +1);
        }
    } // end for

    Vector_destroy_Vector_char(&tokens);
}


__attribute__((always_inline))
static inline
void Pty_handle_OSC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_osc_sequence_terminated(self->parser.active_sequence.buf,
                                   self->parser.active_sequence.size))
    {
        Vector_push_char(&self->parser.active_sequence, '\0');

        Vector_Vector_char tokens =
            string_split_on(self->parser.active_sequence.buf,
                            ";:",
                            "\a\b\n\t\v");

        int arg = strtol(tokens.buf[0].buf +1, NULL, 10);

        switch (arg) {
        case 0:
        case 1:
        case 2:
            /* Set title */
            if (tokens.size >= 2) {
                if (self->title)
                    free(self->title);
                self->title = strdup(tokens.buf[1].buf +1);
                self->on_title_update(self->window_data, tokens.buf[1].buf +1);
            }
            break;

        /* Notification */
        case 777:
            break;

        default:
            WRN("Unknown operating system command:"TERMCOLOR_DEFAULT" %s\n",
                self->parser.active_sequence.buf);
        }

        Vector_destroy_Vector_char(&tokens);
        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state = PARSER_STATE_LITERAL;
    }
}


//TODO: figure out how this should work
__attribute__((always_inline))
static inline void
Vt_push_title(Vt* self)
{
    Vector_push_size_t(&self->title_stack, (size_t)self->title);
    self->title = NULL;
}

__attribute__((always_inline))
static inline void
Vt_pop_title(Vt* self)
{
    if (self->title)
        free(self->title);

    if (self->title_stack.size) {
        self->title = (char*) self->title_stack.buf[self->title_stack.size -1];
        Vector_pop_size_t(&self->title_stack);
    } else {
        self->title = NULL;
    }
}


static inline void
Vt_reset_text_attribs(Vt* self)
{
    memset(&self->parser.char_state, 0, sizeof(self->parser.char_state));
    self->parser.char_state.code = ' ';
    self->parser.char_state.bg = settings.bg;
    self->parser.char_state.fg = settings.fg;
    self->parser.color_inverted = false;
}


/**
 * Move cursor to first column */
__attribute__((always_inline))
static inline void
Vt_carriage_return(Vt* self)
{
    self->cursor_pos = 0;
}


/**
 * make a new empty line at cursor position, scroll down contents below */
__attribute__((always_inline))
static inline void
Vt_insert_line(Vt* self)
{
    Vector_insert_VtLine(&self->lines,
                         Vector_at_VtLine(&self->lines, self->active_line),
                         VtLine_new());

    Vt_empty_line_fill_bg(self, self->active_line);

    Vector_remove_at_VtLine(&self->lines,
                            MIN(Vt_get_scroll_region_bottom(self) +1,
                                Vt_bottom_line(self)),
                            1);
}


/**
 * the same as insert line, but adds before cursor line */
__attribute__((always_inline))
static inline void
Vt_reverse_line_feed(Vt* self)
{
    Vector_remove_at_VtLine(&self->lines,
                            MIN(Vt_bottom_line(self),
                                Vt_get_scroll_region_bottom(self) +1),
                            1);
    Vector_insert_VtLine(&self->lines,
                         Vector_at_VtLine(&self->lines, self->active_line),
                         VtLine_new());
    Vt_empty_line_fill_bg(self, self->active_line);
}


/**
 * delete active line, content below scrolls up */
__attribute__((always_inline))
static inline void
Vt_delete_line(Vt* self)
{
    Vector_remove_at_VtLine(&self->lines, self->active_line, 1);

    Vector_insert_VtLine(
        &self->lines,
        Vector_at_VtLine(&self->lines,
                         MIN(Vt_get_scroll_region_bottom(self) +1,
                             Vt_bottom_line(self))),
        VtLine_new());

    Vt_empty_line_fill_bg(self,
                          MIN(Vt_get_scroll_region_bottom(self) +1,
                              Vt_bottom_line(self)));
}


__attribute__((always_inline))
static inline void
Vt_scroll_up(Vt* self)
{
    Vector_insert_VtLine(
        &self->lines,
        Vector_at_VtLine(
            &self->lines,
            MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self) +1) +1),
        VtLine_new());

    Vt_empty_line_fill_bg(
        self,
        MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self) +1) +1);

    Vector_remove_at_VtLine(&self->lines,
                            Vt_get_scroll_region_top(self) -1,
                            1);
}


__attribute__((always_inline))
static inline void
Vt_scroll_down(Vt* self)
{
    Vector_remove_at_VtLine(&self->lines,
                            MAX(Vt_top_line(self),
                                Vt_get_scroll_region_bottom(self) +1),
                            1);

    Vector_insert_VtLine(
        &self->lines,
        Vector_at_VtLine(&self->lines,
                         Vt_get_scroll_region_top(self)),
        VtLine_new());
}


/**
 * Move cursor one cell down if possible */
__attribute__((always_inline))
static inline void
Vt_cursor_down(Vt* self)
{
    if (self->active_line < Vt_bottom_line(self))
        ++self->active_line;
}


/**
 * Move cursor one cell up if possible */
__attribute__((always_inline))
static inline void
Vt_cursor_up(Vt* self)
{
    if (self->active_line > Vt_top_line(self))
        --self->active_line;
}


/**
 * Move cursor one cell to the left if possible */
__attribute__((always_inline))
static inline void
Vt_cursor_left(Vt* self)
{
    if (self->cursor_pos)
        --self->cursor_pos;
}


/**
 * Move cursor one cell to the right if possible */
__attribute__((always_inline))
static inline void
Vt_cursor_right(Vt* self)
{
    if (self->cursor_pos < self->ws.ws_col)
        ++self->cursor_pos;
}


__attribute__((always_inline))
static inline void
Vt_erase_to_end(Vt* self)
{
    for (size_t i = self->active_line +1 ; i <= Vt_bottom_line(self); ++i) {
        self->lines.buf[i].data.size = 0;
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_right(self);
}


static inline void
Pty_backspace(Vt* self)
{
    if (self->cursor_pos)
        --self->cursor_pos;
}


/**
 * Overwrite characters with colored space */
__attribute__((always_inline))
static inline void
Vt_erase_chars(Vt* self, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        size_t idx = self->cursor_pos +i;
        if (idx >= self->lines.buf[self->active_line].data.size) {
            Vector_push_VtRune(&self->lines.buf[self->active_line].data,
                               self->parser.char_state);
        } else {
            self->lines.buf[self->active_line].data.buf[idx] =
                self->parser.char_state;
        }
    }

    self->lines.buf[self->active_line].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->active_line].proxy.data);
}


/**
 * remove characters at cursor, remaining content scrolls left */
__attribute__((always_inline))
static inline void
Vt_delete_chars(Vt* self, size_t n)
{

    printf("trimby %zu (%zu)\n", n, self->active_line);
    printf("char count: %zu\n", self->lines.buf[self->active_line].data.size);
    
    /* Trim if line is longer than screen area */
    if (self->lines.buf[self->active_line].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->active_line].data,
                            self->lines.buf[self->active_line].data.size
                            - self->ws.ws_col);
    }
    
    Vector_remove_at_VtRune(
        &self->lines.buf[self->active_line].data,
        self->cursor_pos,
        MIN(self->lines.buf[self->active_line].data.size == self->cursor_pos ?
            self->lines.buf[self->active_line].data.size - self->cursor_pos :
            self->lines.buf[self->active_line].data.size,
            n));

    printf("trim 1 char count: %zu\n", self->lines.buf[self->active_line].data.size);

    /* Fill line to the cursor position with spaces with original propreties
     * before scolling so we get the expected result, when we... */
    VtRune tmp = self->parser.char_state;
    bool tmp_invert = self->parser.color_inverted;

    Vt_reset_text_attribs(self);
    self->parser.color_inverted = false;

    if (self->lines.buf[self->active_line].data.size >= 2 ) {
        self->parser.char_state.bg = self->lines.buf[self->active_line].data
            .buf[self->lines.buf[self->active_line].data.size -2].bg;
    } else {
        self->parser.char_state.bg = settings.bg;
    }

    for (size_t i = self->lines.buf[self->active_line].data.size -1;
         i < self->ws.ws_col;
         ++i)
    {
        Vector_push_VtRune(&self->lines.buf[self->active_line].data,
                           self->parser.char_state);
    }

    self->parser.char_state = tmp;
    self->parser.color_inverted = tmp_invert;

    if (self->lines.buf[self->active_line].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->active_line].data,
                             self->lines.buf[self->active_line].data.size -
                             self->ws.ws_col);
    }

    /* ...add n spaces with currently set attributes to the end */
    for (size_t i = 0; i < n && self->cursor_pos +i < self->ws.ws_col; ++i) {
        Vector_push_VtRune(&self->lines.buf[self->active_line].data,
                            self->parser.char_state);
    }

    /* Trim to screen size again */
    if (self->lines.buf[self->active_line].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->active_line].data,
                            self->lines.buf[self->active_line].data.size
                            - self->ws.ws_col);
    }



    /* Vector_remove_at_VtRune( */
    /*     &self->lines.buf[self->active_line].data, */
    /*     self->cursor_pos, */
    /*     MIN(self->lines.buf[self->active_line].data.size == self->cursor_pos ? */
    /*         self->lines.buf[self->active_line].data.size - self->cursor_pos : */
    /*         self->lines.buf[self->active_line].data.size, */
    /*         n)); */


    self->lines.buf[self->active_line].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->active_line].proxy.data);
}



__attribute__((always_inline))
static inline void
Vt_scroll_out_all_conten(Vt* self)
{
    size_t to_add = 0;
    for (size_t i = Vt_visual_bottom_line(self) -1;
         i >= Vt_visual_top_line(self);
         --i)
    {
        if (self->lines.buf[i].data.size) {
            to_add += i;
            break;
        }
    }
    to_add -= Vt_visual_top_line(self);
    to_add += 1;

    for (size_t i = 0; i < to_add; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size -1);
    }

    self->active_line += to_add;
}


__attribute__((always_inline))
static inline void
Vt_scroll_out_above(Vt* self)
{
    size_t to_add = Vt_active_screen_index(self);
    for (size_t i = 0; i < to_add; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size -1);
    }

    self->active_line += to_add;

}


__attribute__((always_inline))
static inline void
Vt_clear_above(Vt* self)
{
    for (size_t i = Vt_visual_top_line(self); i < self->active_line; ++i) {
        self->lines.buf[i].data.size = 0;
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_left(self);
}


__attribute__((always_inline))
static inline void
Vt_clear_display_and_scrollback(Vt* self)
{
    self->lines.buf[self->active_line].damaged = true;
    Vector_destroy_VtLine(&self->lines);
    self->lines = Vector_new_VtLine();
    for (size_t i = 0; i < self->ws.ws_row; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size -1);
    }
    self->active_line = 0;
}


/**
 * Clear active line left of cursor and fill it with whatever character
 * attributes are set */
__attribute__((always_inline))
static inline void
Vt_clear_left(Vt* self)
{
    for (size_t i = 0; i <= self->cursor_pos; ++i) {
        if (i < self->lines.buf[self->active_line].data.size)
            self->lines.buf[self->active_line].data.buf[i] = self->parser.char_state;
        else
            Vector_push_VtRune(&self->lines.buf[self->active_line].data,
                               self->parser.char_state);
    }

    self->lines.buf[self->active_line].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->active_line].proxy.data);
}


/**
 * Clear active line right of cursor and fill it with whatever character
  * attributes are set */
__attribute__((always_inline))
static inline void
Vt_clear_right(Vt* self)
{
    for (size_t i = self->cursor_pos; i <= self->ws.ws_col; ++i) {
        if (i +1 <= self->lines.buf[self->active_line].data.size)
            self->lines.buf[self->active_line].data.buf[i] = self->parser.char_state;
        else
            Vector_push_VtRune(&self->lines.buf[self->active_line].data,
                               self->parser.char_state);
    }

    self->lines.buf[self->active_line].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->active_line].proxy.data);
}


/**
 * Insert character literal at cursor position deal with reaching column limit */
__attribute__((always_inline, hot))
static inline void
Vt_insert_char_at_cursor(Vt* self, VtRune c)
{
    if (self->cursor_pos >= (size_t)self->ws.ws_col) {
        if (self->modes.no_auto_wrap) {
            --self->cursor_pos;
        } else {
            self->cursor_pos = 0;
            Vt_insert_new_line(self);
            self->lines.buf[self->active_line].rejoinable = true;
        }
    }

    while (self->lines.buf[self->active_line].data.size <= self->cursor_pos)
        Vector_push_VtRune(&self->lines.buf[self->active_line].data, space);

    if (self->parser.color_inverted) {
        ColorRGB tmp = c.fg;
        c.fg = ColorRGB_from_RGBA(c.bg);
        c.bg = ColorRGBA_from_RGB(tmp);
    }

    self->lines.buf[self->active_line].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->active_line].proxy.data);

    self->lines.buf[self->active_line].data.buf[self->cursor_pos] = c;

    ++self->cursor_pos;

    if (wcwidth(c.code) == 2) {
        if (self->lines.buf[self->active_line].data.size <= self->cursor_pos) {
            Vector_push_VtRune(&self->lines.buf[self->active_line].data,
                               space);
        } else {
            self->lines.buf[self->active_line]
                .data.buf[self->cursor_pos] = space;
        }

        ++self->cursor_pos;
    }
}


__attribute__((always_inline))
static inline void
Vt_empty_line_fill_bg(Vt* self, size_t idx)
{
    ASSERT(self->lines.buf[idx].data.size == 0, "line is not empty");

    self->lines.buf[self->active_line].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->active_line].proxy.data);
    
    if (!ColorRGBA_eq(self->parser.char_state.bg, settings.bg))
        for (size_t i = 0; i < self->ws.ws_col; ++i)
            Vector_push_VtRune(&self->lines.buf[idx].data,
                               self->parser.char_state);
}


/**
 * Move one line down or insert a new one, scrolls if region is set */
__attribute__((always_inline))
static inline void
Vt_insert_new_line(Vt* self)
{
    if (self->active_line == Vt_get_scroll_region_bottom(self) +1) {
        Vector_remove_at_VtLine(
            &self->lines,
            Vt_get_scroll_region_top(self), 1);
        Vector_insert_VtLine(
            &self->lines,
            Vector_at_VtLine(&self->lines, self->active_line),
            VtLine_new());
        Vt_empty_line_fill_bg(self, self->active_line);
    } else {
        if (Vt_bottom_line(self) == self->active_line) {
            Vector_push_VtLine(&self->lines, VtLine_new());
            Vt_empty_line_fill_bg(self, self->active_line +1);
        }
        ++self->active_line;
    }
}


/**
 * Move cursor to given location */
__attribute__((always_inline, hot))
static inline void
Vt_move_cursor(Vt* self, uint32_t columns, uint32_t rows)
{
    self->active_line = MIN(rows, (uint32_t)self->ws.ws_row -1) +
        Vt_top_line(self);
    self->cursor_pos = MIN(columns, (uint32_t)self->ws.ws_col);
}


__attribute__((always_inline, hot, flatten))
static inline void
Vt_handle_literal(Vt* self, char c)
{
    if (unlikely(self->parser.utf8_in_seq)) {
        self->parser.utf8_buf[self->parser.utf8_cur_seq_len++] = c;
        int64_t res = utf8_decode_validated(self->parser.utf8_buf,
                                            self->parser.utf8_cur_seq_len);
        if (unlikely(res == UTF8_CHAR_INVALID)) {
            WRN("Invalid UTF-8 sequence");
        } else if (res != UTF8_CHAR_INCOMPLETE) {
            VtRune new_char = self->parser.char_state;
            new_char.code = res;
            Vt_insert_char_at_cursor(self, new_char);
            self->parser.utf8_in_seq = false;
        }
    } else {
        switch (c) {
        case '\a':
            if (!settings.no_flash)
                gl_flash();
            break;

        case '\b':
            Pty_backspace(self);
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

        case '\t': {
            size_t cp = self->cursor_pos;

            //FIX: do this properly
            for (size_t i = 0; i < self->tabstop - (cp % self->tabstop); ++i)
                Vt_cursor_right(self);
        }
            break;

        default: {

            int64_t res = utf8_decode_validated(&c, 1);
            if (unlikely(res == UTF8_CHAR_INCOMPLETE)) {
                self->parser.utf8_in_seq = true;
                self->parser.utf8_cur_seq_len = 1;
                self->parser.utf8_buf[0] = c;
                break;
            } else if (res == UTF8_CHAR_INVALID)
                break;

            VtRune new_char = self->parser.char_state;
            new_char.code = c;
            
            if (unlikely((bool) self->charset_g0))
                new_char.code = self->charset_g0(c);
            if (unlikely((bool) self->charset_g1))
                new_char.code = self->charset_g1(c);
            
            Vt_insert_char_at_cursor(self, new_char);
        }
        }
    }
}


__attribute__((always_inline))
static inline void
Vt_handle_char(Vt* self, char c)
{
    switch (self->parser.state) {

    case PARSER_STATE_LITERAL:
        Vt_handle_literal(self, c);
        break;

        
    case PARSER_STATE_CONTROL_SEQ:
        Vt_handle_cs(self, c);
        break;


    case PARSER_STATE_ESCAPED:
        switch (expect(c, '[')) {

        /* Control sequence introduce (CSI) */
        case '[':
            self->parser.state = PARSER_STATE_CONTROL_SEQ;
            return;

        /* Operating system command (OSC) */
        case ']':
            self->parser.state = PARSER_STATE_OS_COM;
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

        case '(':
            self->parser.state = PARSER_STATE_CHARSET_G0;
            return;

        case ')':
            self->parser.state = PARSER_STATE_CHARSET_G1;
            return;

        case '*':
            self->parser.state = PARSER_STATE_CHARSET_G2;
            return;

        case '+':
            self->parser.state = PARSER_STATE_CHARSET_G3;
            return;

        case 'g':
            if (!settings.no_flash)
                gl_flash();
            break;

        /* Application Keypad (DECPAM) */
        case '=':
            self->modes.application_keypad = true;
            self->parser.state = PARSER_STATE_LITERAL;
            return;

        /* Normal Keypad (DECPNM) */
        case '>':
            self->modes.application_keypad = false;
            self->parser.state = PARSER_STATE_LITERAL;
            return;

        /* Reset initial state (RIS) */
        case 'c': 
            Vt_select_end(self);
            Vt_clear_display_and_scrollback(self);
            Vt_move_cursor(self, 0, 0);
            self->tabstop = 8;
            self->parser.state = PARSER_STATE_LITERAL;
            self->scroll_region_top = 0;
            self->scroll_region_bottom = gl_get_char_size().second;
            for (size_t* i = NULL; Vector_iter_size_t(&self->title_stack, i);)
                free((char*)*i);
            Vector_destroy_size_t(&self->title_stack);
            self->title_stack = Vector_new_size_t();
            return;

        /* Save cursor (DECSC) */
        case '7':
            self->saved_active_line = self->active_line;
            self->saved_cursor_pos = self->cursor_pos;
            return;

        /* Restore cursor (DECRC) */
        case '8':
            self->active_line = self->saved_active_line;
            self->cursor_pos = self->saved_cursor_pos;
            return;

        case '\e':
            return;

        default: {
            /* char* cs = control_char_get_pretty_string(c); */
            /* char cb[2] = { c, 0 }; */
            /* WRN("Unknown escape character:"TERMCOLOR_DEFAULT" %s " */
            /*     TERMCOLOR_YELLOW"("TERMCOLOR_DEFAULT"%d"TERMCOLOR_YELLOW")\n", */
            /*     cs ? cs : cb, c); */

            self->parser.state = PARSER_STATE_LITERAL;
            return;
        }

        }
        break;


    case PARSER_STATE_CHARSET_G0:
        self->parser.state = PARSER_STATE_LITERAL;
        switch(c) {
        case '0':
            self->charset_g0 = &char_sub_gfx;
            return;
        case 'A':
            self->charset_g0 = &char_sub_uk;
            return;
        case'B':
            self->charset_g0 = NULL;
            return;
        default:
            WRN("Unknown sequence ESC(%c\n", c);
            return;
        }
        break;


    case PARSER_STATE_CHARSET_G1:
        self->parser.state = PARSER_STATE_LITERAL;
        switch(c) {
        case '0':
            self->charset_g1 = &char_sub_gfx;
            return;
        case 'A':
            self->charset_g1 = &char_sub_uk;
            return;
        case'B':
            self->charset_g1 = NULL;
            return;
        default:
            WRN("Unknown sequence <ESC>)%c\n", c);
            return;
        }
        break;


    case PARSER_STATE_CHARSET_G2:
        self->parser.state = PARSER_STATE_LITERAL;
        switch(c) {
        case '0':
            self->charset_g2 = &char_sub_gfx;
            return;
        case 'A':
            self->charset_g2 = &char_sub_uk;
            return;
        case'B':
            self->charset_g2 = NULL;
            return;
        default:
            WRN("Unknown sequence <ESC>)%c\n", c);
            return;
        }
        break;


    case PARSER_STATE_OS_COM:
        Pty_handle_OSC(self, c);
        break;

    default:
        ASSERT_UNREACHABLE;
    }
}


__attribute__((hot))
inline bool
Vt_read(Vt* self)
{
    if (FD_ISSET(self->master, &self->rfdset)) {
        int rd = read(self->master, self->buf, sizeof(self->buf) - 2);
        if (rd >= 0 && settings.scroll_on_output) {
            Vt_visual_scroll_reset(self);
        }

        #ifdef DEBUG
        if (rd > 0) {
            self->buf[rd] = 0;
            char* out = pty_string_prettyfy(self->buf);
            LOG("PTY."TERMCOLOR_MAGENTA_LIGHT"READ("TERMCOLOR_DEFAULT"%s"
                TERMCOLOR_MAGENTA_LIGHT")"TERMCOLOR_DEFAULT
                "  ~> { bytes: %3d | %s } \n", self->dev_name, rd, out);
            free(out);
        }
        #endif

        if (rd < 0) {
            LOG("Program finished\n");
            self->is_done = true;
        } else if (rd == 0) {
            /* nothing more to read */
            return false;
        } else {
            for (int i = 0; i < rd; ++i)
                Vt_handle_char(self, self->buf[i]);
            self->repaint_required_notify(self->window_data);
            Vt_update_scrollbar_dims(self);
            if ((uint32_t) rd < (sizeof self->buf -2)) {
                /* nothing more to read */
                static bool first = true;
                if (first) {
                    first = false;
                    Pair_uint32_t px = gl_pixels(self->ws.ws_col,
                                                 self->ws.ws_row);
                    self->ws.ws_xpixel = px.first;
                    self->ws.ws_ypixel = px.second;
                    if (ioctl(self->master, TIOCSWINSZ, &self->ws) < 0)
                        WRN("IO operation failed %s\n", strerror(errno));
                }
                return false;
            }
        }
        return true;
    } else {
        /* !FD_ISSET(..) */
        Vt_update_scrollbar_vis(self);

        if (self->scrollbar.autoscroll == AUTOSCROLL_UP &&
            TimePoint_passed(self->scrollbar.autoscroll_next_step))
        {
            Vt_visual_scroll_up(self);
            self->scrollbar.autoscroll_next_step =
                TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
            Vt_update_scrollbar_dims(self);
            self->repaint_required_notify(self->window_data);
        } else if (self->scrollbar.autoscroll == AUTOSCROLL_DN &&
            TimePoint_passed(self->scrollbar.autoscroll_next_step))
        {
            Vt_visual_scroll_down(self);
            self->scrollbar.autoscroll_next_step =
                TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
            Vt_update_scrollbar_dims(self);
            self->repaint_required_notify(self->window_data);
        }
    }
    return false;
}


/**
 * write @param bytes from out buffer to pty */
__attribute__((always_inline))
static inline void
Vt_write_n(Vt* self, size_t bytes)
{
    #ifdef DEBUG
    char* str = pty_string_prettyfy(self->out_buf);
    LOG("PTY."TERMCOLOR_YELLOW"WRITE("TERMCOLOR_DEFAULT"%s"TERMCOLOR_YELLOW")"
        TERMCOLOR_DEFAULT " <~ { bytes: %3ld | %s }\n",
        self->dev_name,
        strlen(self->out_buf),
        str);
    free(str);
    #endif

    write(self->master, self->out_buf, bytes);
}


/**
 * write null-terminated string from out buffer to pty */
__attribute__((always_inline))
static inline void
Vt_write(Vt* self)
{
    Vt_write_n(self, strlen(self->out_buf));
}


void
Vt_show_lines(Vt* self,
               void(*for_line)
                    (const Vt* cont,
                     VtLine*,
                     size_t,
                     uint32_t,
                     int32_t))
{
    size_t start = Vt_visual_top_line(self);
    size_t end = self->ws.ws_row + start + (self->scrolling ? 1 : 0);
    
    for (size_t i = start; i < end; ++i) {
        for_line(self,
                 &self->lines.buf[i],
                 self->lines.buf[i].data.size,
                 i - start,
                 self->active_line == i ? (int32_t) self->cursor_pos : -1);
    }
}


void
Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end)
{
    if (out_begin)
        *out_begin = self->lines.buf + Vt_visual_top_line(self);

    if (out_end)
        *out_end = self->lines.buf + Vt_visual_bottom_line(self);
}
    

__attribute__((always_inline))
static inline const char*
normal_keypad_response(const uint32_t key)
{
    switch (key) {
    case XKB_KEY_Up   : return "\e[A";
    case XKB_KEY_Down : return "\e[B";
    case XKB_KEY_Right: return "\e[C";
    case XKB_KEY_Left : return "\e[D";
    case XKB_KEY_End  : return "\e[F";
    case XKB_KEY_Home : return "\e[H";
    default           : return NULL;
    }
}


__attribute__((always_inline))
static inline const char*
application_keypad_response(const uint32_t key)
{
    switch (key) {
    case XKB_KEY_Up          : return "\eOA";
    case XKB_KEY_Down        : return "\eOB";
    case XKB_KEY_Right       : return "\eOC";
    case XKB_KEY_Left        : return "\eOD";
    case XKB_KEY_End         : return "\eOF";
    case XKB_KEY_Home        : return "\eOH";
    case XKB_KEY_KP_Enter    : return "\eOM";
    case XKB_KEY_KP_Multiply : return "\eOj";
    case XKB_KEY_KP_Add      : return "\eOk";
    case XKB_KEY_KP_Separator: return "\eOl";
    case XKB_KEY_KP_Subtract : return "\eOm";
    case XKB_KEY_KP_Divide   : return "\eOo";
    default                  : return NULL;
    }
}


__attribute__((always_inline))
static inline const char*
normal_mod_keypad_response(const uint32_t key)
{
    switch (key) {
    case XKB_KEY_Up   : return "\e[1;%dA";
    case XKB_KEY_Down : return "\e[1;%dB";
    case XKB_KEY_Right: return "\e[1;%dC";
    case XKB_KEY_Left : return "\e[1;%dD";
    case XKB_KEY_End  : return "\e[1;%dF";
    case XKB_KEY_Home : return "\e[1;%dH";
    default           : return NULL;
    }
}


__attribute__((always_inline))
static inline const char*
application_mod_keypad_response(const uint32_t key)
{
    switch (key) {
    case XKB_KEY_Up   : return "\e[1;%dA";
    case XKB_KEY_Down : return "\e[1;%dB";
    case XKB_KEY_Right: return "\e[1;%dC";
    case XKB_KEY_Left : return "\e[1;%dD";
    case XKB_KEY_End  : return "\e[1;%dF";
    case XKB_KEY_Home : return "\e[1;%dH";
    default           : return NULL;
    }
}


/**
 * @return keypress was consumed */
__attribute__((always_inline))
static inline bool
Vt_maybe_handle_application_key(Vt* self, uint32_t key, uint32_t mods)
{
    if (FLAG_IS_SET(mods, MODIFIER_CONTROL) &&
        FLAG_IS_SET(mods, MODIFIER_SHIFT))
    {
        switch (key) {
        case 3:    // ^C
        case 25: { // ^Y
            Vector_char txt = Vt_select_region_to_string(self);
            self->window_itable->clipboard_send(self->window_data, txt.buf);
            // clipboard_send should free

            return true;
        }

        case 22:   // ^V
        case 16: { // ^P
            self->window_itable->clipboard_get(self->window_data);
            return true;
        }

        case 31: // ^_
            LOG("should decrease font size");
            return true;

        case 43: // ^+
            LOG("should enlarge font");
            return true;

        case 13:
            Vt_dump_info(self);
            return true;

        default:
            printf("%u key\n", key);
            return false;
        }
    }

    return false;
}


/**
 * @return keypress was consumed */
__attribute__((always_inline))
static inline bool
Vt_maybe_handle_keypad_key(Vt* self, uint32_t key, uint32_t mods)
{
    const char* resp = NULL;
    if (mods) {
        resp = self->modes.application_keypad ?
            application_mod_keypad_response(key):
            normal_mod_keypad_response(key);

        if (resp) {
            sprintf(self->out_buf, resp, mods +1);
            Vt_write(self);
            return true;
        }
        
    } else {
        resp = self->modes.application_keypad ? application_keypad_response(key):
            normal_keypad_response(key);

        if (resp) {
            memcpy(self->out_buf, resp, 4);
            Vt_write(self);
            return true;
        }
    }
    
    return false;
}


/**
 * @return keypress was consumed */
__attribute__((always_inline))
static inline bool
Vt_maybe_handle_function_key(Vt* self, uint32_t key, uint32_t mods)
{
    if (key >= XKB_KEY_F1 && key <= XKB_KEY_F35) {
        int f_num = key - XKB_KEY_F1;
        if (mods) {
            if (f_num < 4)
                sprintf(self->out_buf, "\e[[1;%u%c", mods +1, f_num + 'P');
            else
                sprintf(self->out_buf, "\e[%d;%u~", f_num + 12, mods +1);
        } else {
            if (f_num < 4)
                sprintf(self->out_buf, "\eO%c", f_num + 'P');
            else
                sprintf(self->out_buf, "\e[%d~", f_num + 12);
        }
        Vt_write(self);
        return true;
    } else if (key == XKB_KEY_Insert) {
        memcpy(self->out_buf, "\e[2~", 5);
        Vt_write(self);
        return true;
    } else if (key == XKB_KEY_Page_Up) {
        memcpy(self->out_buf, "\e[5~", 5);
        Vt_write(self);
        return true;
    } else if (key == XKB_KEY_Page_Down) {
        memcpy(self->out_buf, "\e[6~", 5);
        Vt_write(self);
        return true;
    } else if (key == ' ' && FLAG_IS_SET(mods, MODIFIER_CONTROL)) {
        memcpy(self->out_buf, "", 1);
        Vt_write_n(self, 1);
        return true;
    }

    return false;
}


/**
 *  substitute keypad keys with normal ones */
__attribute__((always_inline))
static inline uint32_t
numpad_key_convert(uint32_t key)
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

    case XKB_KEY_KP_0:
    case XKB_KEY_KP_1:
    case XKB_KEY_KP_2:
    case XKB_KEY_KP_3:
    case XKB_KEY_KP_4:
    case XKB_KEY_KP_5:
    case XKB_KEY_KP_6:
    case XKB_KEY_KP_7:
    case XKB_KEY_KP_8:
    case XKB_KEY_KP_9:
        return '0' + key - XKB_KEY_KP_0;

    default:
        return key;
    }
}


/**
 * Respond to key event */
void
Vt_handle_key(void* _self, uint32_t key, uint32_t mods)
{
    Vt* self = _self;

    if (!Vt_maybe_handle_application_key(self, key, mods) &&
        !Vt_maybe_handle_keypad_key(self, key, mods) &&
        !Vt_maybe_handle_function_key(self, key, mods))
    {
        key = numpad_key_convert(key);
        uint32_t seq_len;
        uint8_t offset = 0;
        if (FLAG_IS_SET(mods, MODIFIER_ALT) &&
            !self->modes.no_alt_sends_esc)
        {
            Vt_buffer(self)[0] = '\e';
            offset = 1;
        }

        if (unlikely(key == '\b') && settings.bsp_sends_del)
            key = 127;

        if ((seq_len = utf8_len(key)) != 1) {
            utf8_encode2(key, Vt_buffer(self) + offset);

            Vt_buffer(self)[seq_len +offset] = '\0';
        } else {
            Vt_buffer(self)[0 +offset] = (char)key;
            Vt_buffer(self)[1 +offset] = '\0';
        }

        Vt_write(self);
    }

    if (settings.scroll_on_key) {
        Vt_visual_scroll_reset(self);
    }

    gl_reset_action_timer();
}


/**
 * Respond to mouse button event
 * @param button  - X11 button code
 * @param state   - press/release
 * @param ammount - for non-discrete scroll
 * @param mods    - modifier keys depressed */
void
Vt_handle_button(void* _self,
                 uint32_t button,
                 bool state,
                 int32_t x,
                 int32_t y,
                 int32_t ammount,
                 uint32_t mods)
{
    Vt* self = _self;

    if (Vt_scrollbar_consume_click(self, button, state, x, y) ||
        Vt_select_consume_click(self, button, state, x, y, mods))
        return;

    bool in_window = x >= 0 && x <= self->ws.ws_xpixel &&
        y >= 0 && y <= self->ws.ws_ypixel;

    if ((self->modes.extended_report ||
         self->modes.mouse_motion_on_btn_report ||
         self->modes.mouse_btn_report) &&
        in_window)
    {

        if (!self->scrolling) {
            self->last_click_x = (double) x / self->pixels_per_cell_x;
            self->last_click_y = (double) y / self->pixels_per_cell_y;

            if (self->modes.x10_mouse_compat) {
                button += (FLAG_IS_SET(mods, MODIFIER_SHIFT) ? 4 : 0) +
                    (FLAG_IS_SET(mods, MODIFIER_ALT) ? 8 : 0) +
                    (FLAG_IS_SET(mods, MODIFIER_CONTROL) ? 16 : 0);
            }

            if (self->modes.extended_report) {
                sprintf(self->out_buf,
                        "\e[<%d;%lu;%lu%c",
                        button -1,
                        self->last_click_x +1,
                        self->last_click_y +1,
                        state ? 'M' : 'm');
            } else if (self->modes.mouse_btn_report) {
                sprintf(self->out_buf,
                        "\e[M%c%c%c",
                        32 + button -1 + !state *3,
                        (char) (32 + self->last_click_x +1),
                        (char) (32 + self->last_click_y +1));
            }
            Vt_write(self);

        }
    } else if (button == MOUSE_BTN_WHEEL_DOWN && state) {
        uint8_t lines = ammount ? ammount : settings.scroll_discrete_lines;
        for (uint8_t i = 0; i < lines; ++i) {
            Vt_visual_scroll_down(self);
        }
        Vt_update_scrollbar_dims(self);
    } else if (button == MOUSE_BTN_WHEEL_UP && state) {
        uint8_t lines = ammount ? ammount : settings.scroll_discrete_lines;
        for (uint8_t i = 0; i < lines; ++i) {
            Vt_visual_scroll_up(self);
        }
        Vt_update_scrollbar_dims(self);
    }
    
    self->repaint_required_notify(self->window_data);
}


/**
 * Respond to pointer motion event
 * @param button - button beeing held down */
void
Vt_handle_motion(void* _self, uint32_t button, int32_t x, int32_t y)
{
    Vt* self = _self;

    if (Vt_scrollbar_consume_drag(self, button, x, y) ||
        Vt_consume_drag(self, button, x, y))
        return;
    
    if (self->modes.extended_report) {
        if (!self->scrolling) {
            x = CLAMP(x, 0, self->ws.ws_xpixel);
            y = CLAMP(y, 0, self->ws.ws_ypixel);
            size_t click_x = (double) x / self->pixels_per_cell_x;
            size_t click_y = (double) y / self->pixels_per_cell_y;

            if (click_x != self->last_click_x ||
                click_y != self->last_click_y)
            {
                self->last_click_x = click_x;
                self->last_click_y = click_y;

                sprintf(self->out_buf,
                        "\e[<%d;%zu;%zuM",
                        (int) button -1 +32,
                        click_x +1,
                        click_y +1);

                Vt_write(self);
            }
        }
    }
}


/**
 * Respond to clipboard paste */
void
Vt_handle_clipboard(void* _self, const char* text)
{
    Vt* self = _self;

    if (!text)
        return;

    size_t len = strlen(text), bi = 0;
    if (self->modes.bracket_paste) {
        memcpy(self->out_buf, "\e[200~", 6);
        bi += 6;
    }
    for (size_t i = 0; i < len;) {
        int to_cpy = MIN(len -i, sizeof(self->out_buf) -bi);
        memcpy(self->out_buf +bi, text +i, to_cpy);
        i += to_cpy;
        bi += to_cpy;
        if (bi > sizeof(self->out_buf)
            -(self->modes.bracket_paste ? 7 : 1))
        {
            Vt_write(self);
            bi = 0;
        }
    }
    if (self->modes.bracket_paste)
        memcpy(self->out_buf +bi, "\e[201~", 7);
    else
        self->out_buf[bi] = 0;

    Vt_write(self);
}


