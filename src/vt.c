/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "vt.h"

#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <uchar.h>
#include <utmp.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <utmp.h>

#include <xkbcommon/xkbcommon.h>

#include "wcwidth/wcwidth.h"

VtRune space;

void (*Vt_destroy_line_proxy)(int32_t proxy[static 4]) = NULL;

static inline size_t Vt_top_line(const Vt* const self);

void Vt_visual_scroll_to(Vt* self, size_t line);

void Vt_visual_scroll_up(Vt* self);

void Vt_visual_scroll_down(Vt* self);

void Vt_visual_scroll_reset(Vt* self);

static inline size_t Vt_get_scroll_region_top(Vt* self);

static inline size_t Vt_get_scroll_region_bottom(Vt* self);

static inline bool Vt_scroll_region_not_default(Vt* self);

static void Vt_alt_buffer_on(Vt* self, bool save_mouse);

static void Vt_alt_buffer_off(Vt* self, bool save_mouse);

static void Vt_handle_prop_seq(Vt* self, Vector_char seq);

static void Vt_reset_text_attribs(Vt* self);

static void Vt_carriage_return(Vt* self);

static void Vt_clear_right(Vt* self);

static void Vt_clear_left(Vt* self);

static inline void Vt_scroll_out_all_content(Vt* self);

static void Vt_empty_line_fill_bg(Vt* self, size_t idx);

static void Vt_cursor_down(Vt* self);

static void Vt_cursor_up(Vt* self);

static void Vt_cursor_left(Vt* self);

static void Vt_cursor_right(Vt* self);

static void Vt_insert_new_line(Vt* self);

static void Vt_scroll_up(Vt* self);

static void Vt_scroll_down(Vt* self);

static void Vt_reverse_line_feed(Vt* self);

static void Vt_delete_line(Vt* self);

static void Vt_delete_chars(Vt* self, size_t n);

static void Vt_erase_chars(Vt* self, size_t n);

static void Vt_clear_above(Vt* self);

static inline void Vt_scroll_out_above(Vt* self);

static void Vt_insert_line(Vt* self);

static void Vt_clear_display_and_scrollback(Vt* self);

static void Vt_erase_to_end(Vt* self);

static void Vt_move_cursor(Vt* self, uint32_t c, uint32_t r);

static void Vt_push_title(Vt* self);

static void Vt_pop_title(Vt* self);

static inline void Vt_insert_char_at_cursor(Vt* self, VtRune c);

static inline void Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c);

static Vector_char line_to_string(Vector_VtRune* line,
                                  size_t         begin,
                                  size_t         end,
                                  const char*    tail);

/**
 * Get string from selected region */
Vector_char Vt_select_region_to_string(Vt* self)
{
    Vector_char ret, tmp;
    size_t      begin_char_idx, end_char_idx;
    size_t      begin_line =
      MIN(self->selection.begin_line, self->selection.end_line);
    size_t end_line = MAX(self->selection.begin_line, self->selection.end_line);

    if (begin_line == end_line && self->selection.mode != SELECT_MODE_NONE) {
        begin_char_idx =
          MIN(self->selection.begin_char_idx, self->selection.end_char_idx);
        end_char_idx =
          MAX(self->selection.begin_char_idx, self->selection.end_char_idx);

        return line_to_string(&self->lines.buf[begin_line].data, begin_char_idx,
                              end_char_idx + 1, "");
    } else if (self->selection.begin_line < self->selection.end_line) {
        begin_char_idx = self->selection.begin_char_idx;
        end_char_idx   = self->selection.end_char_idx;
    } else {
        begin_char_idx = self->selection.end_char_idx;
        end_char_idx   = self->selection.begin_char_idx;
    }

    if (self->selection.mode == SELECT_MODE_NORMAL) {
        ret = line_to_string(&self->lines.buf[begin_line].data, begin_char_idx,
                             0, "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i < end_line; ++i) {
            tmp = line_to_string(&self->lines.buf[i].data, 0, 0, "\n");
            Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
            Vector_destroy_char(&tmp);
        }
        tmp = line_to_string(&self->lines.buf[end_line].data, 0,
                             end_char_idx + 1, "");
        Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
        Vector_destroy_char(&tmp);
    } else if (self->selection.mode == SELECT_MODE_BOX) {
        ret = line_to_string(&self->lines.buf[begin_line].data, begin_char_idx,
                             end_char_idx + 1, "\n");
        Vector_pop_char(&ret);
        for (size_t i = begin_line + 1; i <= end_line; ++i) {
            tmp = line_to_string(&self->lines.buf[i].data, begin_char_idx,
                                 end_char_idx + 1, i == end_line ? "" : "\n");
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
    self->selection.mode = SELECT_MODE_NORMAL;
    x                    = CLAMP(x, 0, self->ws.ws_xpixel);
    y                    = CLAMP(y, 0, self->ws.ws_ypixel);
    size_t click_x       = (double)x / self->pixels_per_cell_x;
    size_t click_y       = (double)y / self->pixels_per_cell_y;

    Vector_VtRune* ln =
      &self->lines.buf[Vt_visual_top_line(self) + click_y].data;
    size_t cmax = ln->size, begin = click_x, end = click_x;

    while (begin - 1 < cmax && begin > 0 && !isspace(ln->buf[begin - 1].code))
        --begin;

    while (end + 1 < cmax && end > 0 && !isspace(ln->buf[end + 1].code))
        ++end;

    self->selection.begin_char_idx = begin;
    self->selection.end_char_idx   = end;
    self->selection.begin_line     = self->selection.end_line =
      Vt_visual_top_line(self) + click_y;
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
    self->selection.begin_line     = self->selection.end_line =
      Vt_visual_top_line(self) + click_y;
}

__attribute__((always_inline)) static inline void Vt_mark_proxy_damaged(
  Vt*    self,
  size_t idx)
{
    self->lines.buf[idx].damaged = true;
}

__attribute__((always_inline)) static inline void
Vt_mark_proxies_damaged_in_region(Vt* self, size_t begin, size_t end)
{
    size_t lo = MIN(begin, end);
    size_t hi = MAX(begin, end);
    for (size_t i = lo; i <= hi; ++i)
        Vt_mark_proxy_damaged(self, i);
}

__attribute__((always_inline)) static inline void Vt_clear_proxy(Vt*    self,
                                                                 size_t idx)
{
    if (!self->lines.buf[idx].damaged) {
        Vt_mark_proxy_damaged(self, idx);
        Vt_destroy_line_proxy(self->lines.buf[idx].proxy.data);
    }
}

__attribute__((always_inline)) static inline void
Vt_clear_proxies_in_region(Vt* self, size_t begin, size_t end)
{
    size_t lo = MIN(begin, end);
    size_t hi = MAX(begin, end);
    for (size_t i = lo; i <= hi; ++i)
        Vt_clear_proxy(self, i);
}

void Vt_clear_all_proxies(Vt* self)
{
    Vt_clear_proxies_in_region(self, 0, self->lines.size - 1);

    if (self->alt_lines.buf) {
        for (size_t i = 0; i < self->alt_lines.size - 1; ++i) {
            if (!self->alt_lines.buf[i].damaged) {
                self->alt_lines.buf[i].damaged = true;
                Vt_destroy_line_proxy(self->alt_lines.buf[i].proxy.data);
            }
        }
    }
}

__attribute__((always_inline)) static inline void
Vt_mark_proxies_damaged_in_selected_region(Vt* self)
{
    Vt_mark_proxies_damaged_in_region(self, self->selection.begin_line,
                                      self->selection.end_line);
}

/**
 * start selection
 */
void Vt_select_commit(Vt* self)
{
    if (self->selection.next_mode != SELECT_MODE_NONE) {
        self->selection.mode       = self->selection.next_mode;
        self->selection.next_mode  = SELECT_MODE_NONE;
        self->selection.begin_line = self->selection.end_line =
          self->selection.click_begin_line;
        self->selection.begin_char_idx = self->selection.end_char_idx =
          self->selection.click_begin_char_idx;

        Vt_mark_proxies_damaged_in_selected_region(self);
    }
}

/**
 * set end glyph for selection by pixel coordinates */
void Vt_select_set_end(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        x                            = CLAMP(x, 0, self->ws.ws_xpixel);
        y                            = CLAMP(y, 0, self->ws.ws_ypixel);
        size_t click_x               = (double)x / self->pixels_per_cell_x;
        size_t click_y               = (double)y / self->pixels_per_cell_y;
        Vt_select_set_end_cell(self, click_x,  click_y);
    }
}

/**
 * set end glyph for selection by cell coordinates */
void Vt_select_set_end_cell(Vt* self, int32_t x, int32_t y)
{
    if (self->selection.mode != SELECT_MODE_NONE) {
        size_t old_end               = self->selection.end_line;
        x                            = CLAMP(x, 0, self->ws.ws_col);
        y                            = CLAMP(y, 0, self->ws.ws_row);
        self->selection.end_line     = Vt_visual_top_line(self) + y;
        self->selection.end_char_idx = x;

        size_t lo = MIN(MIN(old_end, self->selection.end_line),
                        self->selection.begin_line);
        size_t hi = MAX(MAX(old_end, self->selection.end_line),
                        self->selection.begin_line);
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
    const char32_t substitutes[] = {
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
 * substitute invisible characters with readable string
 */
__attribute__((cold)) static char* control_char_get_pretty_string(const char c)
{
    switch (c) {
        case '\f':
            return TERMCOLOR_RED_LIGHT "<FF>" TERMCOLOR_DEFAULT;
        case '\n':
            return TERMCOLOR_CYAN "<LF>" TERMCOLOR_DEFAULT;
        case '\a':
            return TERMCOLOR_YELLOW "<BELL>" TERMCOLOR_DEFAULT;
        case '\r':
            return TERMCOLOR_MAGENTA "<CR>" TERMCOLOR_DEFAULT;
        case '\t':
            return TERMCOLOR_BLUE "<TAB>" TERMCOLOR_DEFAULT;
        case '\v':
            return TERMCOLOR_BLUE_LIGHT "<V-TAB>" TERMCOLOR_DEFAULT;
        case '\b':
            return TERMCOLOR_RED "<BS>" TERMCOLOR_DEFAULT;
        case '\e':
            return TERMCOLOR_GREEN_LIGHT "<ESC>" TERMCOLOR_DEFAULT;
        case 0xE:
            return TERMCOLOR_CYAN_LIGHT "<SO>" TERMCOLOR_DEFAULT;
        case 0xF:
            return TERMCOLOR_MAGENTA_LIGHT "<SI>" TERMCOLOR_DEFAULT;
        case 127:
            return TERMCOLOR_MAGENTA_LIGHT "<DEL>" TERMCOLOR_DEFAULT;
        default:
            return NULL;
    }
}

/**
 * make pty messages more readable
 */
__attribute__((cold)) static char* pty_string_prettyfy(const char* str)
{
    bool esc = false, seq = false, important = false;

    Vector_char fmt = Vector_new_char();
    for (; *str; ++str) {
        if (seq) {
            if (isalpha(*str)) {
                Vector_pushv_char(&fmt, TERMCOLOR_BG_DEFAULT,
                                  strlen(TERMCOLOR_BG_DEFAULT));
                seq       = false;
                important = true;
            }
        } else {
            if (*str == '\e') {
                esc = true;
                Vector_pushv_char(&fmt, TERMCOLOR_BG_GRAY_DARK,
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
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_GREEN,
                                          strlen(TERMCOLOR_BG_GREEN));
                        break;
                    case 'm':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_BLUE,
                                          strlen(TERMCOLOR_BG_BLUE));
                        break;
                    default:
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_RED_LIGHT,
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
                Vector_pushv_char(&fmt, TERMCOLOR_BG_WHITE TERMCOLOR_BLACK,
                                  strlen(TERMCOLOR_BG_WHITE TERMCOLOR_BLACK));
                Vector_push_char(&fmt, *str);
                Vector_pushv_char(
                  &fmt, TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT,
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
static Vector_char line_to_string(Vector_VtRune* line,
                                  size_t         begin,
                                  size_t         end,
                                  const char*    tail)
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

    // previous character was wide, skip the following space
    bool prev_wide = false;
    for (uint32_t i = begin; i < end; ++i) {
        if (prev_wide && line->buf[i].code == ' ') {
            prev_wide = false;
            continue;
        }

        if (line->buf[i].code > CHAR_MAX) {
            static mbstate_t mbstate;
            size_t bytes = c32rtomb(utfbuf, line->buf[i].code, &mbstate);
            if (bytes > 0) {
                Vector_pushv_char(&res, utfbuf, bytes);
            }
            prev_wide = wcwidth(line->buf[i].code) > 1;
        } else {
            Vector_push_char(&res, line->buf[i].code);
            prev_wide = false;
        }
    }

    if (tail)
        Vector_pushv_char(&res, tail, strlen(tail) + 1);

    return res;
}

/**
 * Split string on any character in @param symbols, filter out any character in
 * @param filter first character of returned string is the immediately preceding
 * delimiter, '\0' if none.
 */
static Vector_Vector_char string_split_on(const char* str,
                                          const char* symbols,
                                          const char* filter)
{
    Vector_Vector_char ret = Vector_new_with_capacity_Vector_char(8);
    Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
    Vector_push_char(&ret.buf[0], '\0');

    for (; *str; ++str) {
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
            Vector_push_char(&ret.buf[ret.size - 1], '\0');
            if (ret.buf[ret.size - 1].size == 2) {
                ret.buf[ret.size - 1].size = 0;
            } else {
                Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
            }
            Vector_push_char(&ret.buf[ret.size - 1], any_symbol);
        } else
            Vector_push_char(&ret.buf[ret.size - 1], *str);
    continue_outer:;
    }
    Vector_push_char(&ret.buf[ret.size - 1], '\0');

    return ret;
}

__attribute__((always_inline, hot)) static inline bool
is_csi_sequence_terminated(const char* seq, const size_t size)
{
    if (!size)
        return false;

    return isalpha(seq[size - 1]) || seq[size - 1] == '@' ||
           seq[size - 1] == '{' || seq[size - 1] == '}' ||
           seq[size - 1] == '~' || seq[size - 1] == '|';
}

static inline bool is_generic_sequence_terminated(const char*  seq,
                                                  const size_t size)
{
    if (!size)
        return false;

    return seq[size - 1] == '\a' ||
           (size > 1 && seq[size - 2] == '\e' && seq[size - 1] == '\\');
}

Vt Vt_new(uint32_t cols, uint32_t rows)
{
    Vt self;
    memset(&self, 0, sizeof(Vt));

    self.ws = (struct winsize){ .ws_col = cols, .ws_row = rows };
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

        setenv("VTE_VERSION", "5602", 1);

        setenv("TERM", settings.term, 1);

        if (execvp(settings.shell, (char* const*)settings.shell_argv)) {
            /* stdout from here will be displayed in terminal window. */
            printf(TERMCOLOR_RED "Failed to execute command: \"%s\".\n%s"
                                 "\n\narguments: ",
                   settings.shell, strerror(errno));
            for (int i = 0; i < settings.shell_argc; ++i)
                printf("%s%s", settings.shell_argv[i],
                       i == settings.shell_argc - 1 ? "" : ", ");
            puts("\nPress Ctrl-c to exit");

            for (;;)
                pause();
        }

    } else if (self.pid < 0) {
        int errnocpy = errno;
        ERR("Failed to fork process %s", strerror(errnocpy));
    }

    self.is_done          = false;
    self.parser.state     = PARSER_STATE_LITERAL;
    self.parser.in_mb_seq = false;

    self.parser.char_state = space = (VtRune){
        .code          = ' ',
        .bg            = settings.bg,
        .fg            = settings.fg,
        .state         = VT_RUNE_NORMAL,
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

    self.cursor.type     = CURSOR_BLOCK;
    self.cursor.blinking = true;
    self.cursor.col      = 0;

    self.tabstop = 8;

    self.title       = NULL;
    self.title_stack = Vector_new_size_t();

    self.unicode_input.buffer = Vector_new_char();

    return self;
}

void Vt_kill_program(Vt* self)
{
    if (self->pid > 1)
        kill(self->pid, SIGKILL);
    self->pid = 0;
}

static inline size_t Vt_top_line_alt(const Vt* const self)
{
    return self->alt_lines.size <= self->ws.ws_row
             ? 0
             : self->alt_lines.size - self->ws.ws_row;
}

static inline size_t Vt_bottom_line(const Vt* self)
{
    return Vt_top_line(self) + self->ws.ws_row - 1;
}

static inline size_t Vt_bottom_line_alt(Vt* self)
{
    return Vt_top_line_alt(self) + self->ws.ws_row - 1;
}

static inline size_t Vt_active_screen_index(Vt* self)
{
    return self->cursor.row - Vt_top_line(self);
}

static inline size_t Vt_get_scroll_region_top(Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_top;
}

static inline size_t Vt_get_scroll_region_bottom(Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_bottom - 1;
}

static inline bool Vt_scroll_region_not_default(Vt* self)
{
    return Vt_get_scroll_region_top(self) != Vt_visual_top_line(self) ||
           Vt_get_scroll_region_bottom(self) + 1 != Vt_visual_bottom_line(self);
}

void Vt_visual_scroll_up(Vt* self)
{
    if (self->scrolling) {
        if (self->visual_scroll_top)
            --self->visual_scroll_top;
    } else if (Vt_top_line(self)) {
        self->scrolling         = true;
        self->visual_scroll_top = Vt_top_line(self) - 1;
    }
}

void Vt_visual_scroll_down(Vt* self)
{
    if (self->scrolling && Vt_top_line(self) > self->visual_scroll_top) {
        ++self->visual_scroll_top;
        if (self->visual_scroll_top == Vt_top_line(self))
            self->scrolling = false;
    }
}

void Vt_visual_scroll_to(Vt* self, size_t line)
{
    line                    = MIN(line, Vt_top_line(self));
    self->visual_scroll_top = line;
    self->scrolling         = line != Vt_top_line(self);
}

void Vt_visual_scroll_reset(Vt* self)
{
    self->scrolling = false;
}

void Vt_dump_info(Vt* self)
{
    static int dump_index = 0;
    printf("\n====================[ STATE DUMP %2d ]====================\n",
           dump_index++);

    printf("Modes:\n");
    printf("  application keypad:               %d\n",
           self->modes.application_keypad);
    printf("  auto repeat:                      %d\n", self->modes.auto_repeat);
    printf("  bracketed paste:                  %d\n",
           self->modes.bracket_paste);
    printf("  send DEL on delete:               %d\n",
           self->modes.del_sends_del);
    printf("  don't send esc on alt:            %d\n",
           self->modes.no_alt_sends_esc);
    printf("  extended reporting:               %d\n",
           self->modes.extended_report);
    printf("  window focus events reporting:    %d\n",
           self->modes.window_focus_events_report);
    printf("  mouse button reporting:           %d\n",
           self->modes.mouse_btn_report);
    printf("  motion on mouse button reporting: %d\n",
           self->modes.mouse_motion_on_btn_report);
    printf("  mouse motion reporting:           %d\n",
           self->modes.mouse_motion_report);
    printf("  x10 compat mouse reporting:       %d\n",
           self->modes.x10_mouse_compat);
    printf("  no auto wrap:                     %d\n",
           self->modes.no_auto_wrap);
    printf("  reverse video:                    %d\n",
           self->modes.video_reverse);

    printf("\n");

    printf("  S | Number of lines %zu (last index: %zu)\n", self->lines.size,
           Vt_bottom_line(self));
    printf("  C | Terminal size %hu x %hu\n", self->ws.ws_col, self->ws.ws_row);
    printf("V R | \n");
    printf("I O | Visible region: %zu - %zu\n", Vt_visual_top_line(self),
           Vt_visual_bottom_line(self));
    printf("E L | \n");
    printf("W L | Active line:  real: %zu (visible: %zu)\n", self->cursor.row,
           Vt_active_screen_index(self));
    printf("P   | Cursor position: %zu type: %d blink: %d hidden: %d\n",
           self->cursor.col, self->cursor.type, self->cursor.blinking,
           self->cursor.hidden);
    printf("O R | Scroll region: %zu - %zu\n", Vt_get_scroll_region_top(self),
           Vt_get_scroll_region_bottom(self));
    printf("R E | \n");
    printf("T G +----------------------------------------------------\n");
    printf("| |  BUFFER: %s\n", (self->alt_lines.buf ? "ALTERNATIVE" : "MAIN"));
    printf("V V  \n");
    for (size_t i = 0; i < self->lines.size; ++i) {
        Vector_char str = line_to_string(&self->lines.buf[i].data, 0, 0, "");
        printf(
          "%c %c %4zu%c sz:%4zu dmg:%d proxy{%3d,%3d,%3d,%3d} reflow{%d,%d} "
          "data: %.30s\n",
          i == Vt_top_line(self) ? 'v' : i == Vt_bottom_line(self) ? '^' : ' ',
          i == Vt_get_scroll_region_top(self) ||
              i == Vt_get_scroll_region_bottom(self)
            ? '-'
            : ' ',
          i, i == self->cursor.row ? '*' : ' ', self->lines.buf[i].data.size,
          self->lines.buf[i].damaged, self->lines.buf[i].proxy.data[0],
          self->lines.buf[i].proxy.data[1], self->lines.buf[i].proxy.data[2],
          self->lines.buf[i].proxy.data[3],

          self->lines.buf[i].reflowable, self->lines.buf[i].rejoinable,

          str.buf);
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
            size_t chars_to_move = x - self->lines.buf[i].data.size;

            if (i + 1 < bottom_bound && self->lines.buf[i + 1].rejoinable) {
                chars_to_move =
                  MIN(chars_to_move, self->lines.buf[i + 1].data.size);

                Vector_pushv_VtRune(&self->lines.buf[i].data,
                                    self->lines.buf[i + 1].data.buf,
                                    chars_to_move);

                Vector_remove_at_VtRune(&self->lines.buf[i + 1].data, 0,
                                        chars_to_move);

                self->lines.buf[i].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i].proxy.data);

                self->lines.buf[i + 1].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i + 1].proxy.data);

                if (!self->lines.buf[i + 1].data.size) {
                    self->lines.buf[i].was_reflown = false;
                    Vector_remove_at_VtLine(&self->lines, i + 1, 1);
                    --self->cursor.row;
                    --bottom_bound;
                    ++removals;
                }
            }
        }
    }

    int underflow = -((int64_t)self->lines.size - self->ws.ws_row);

    if (underflow > 0) {
        for (int i = 0; i < (int)MIN(underflow, removals); ++i)
            Vector_push_VtLine(&self->lines, VtLine_new());
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

            // line below is a reflow already
            if (i + 1 < bottom_bound && self->lines.buf[i + 1].rejoinable) {
                for (size_t ii = 0; ii < chars_to_move; ++ii) {
                    Vector_insert_VtRune(&self->lines.buf[i + 1].data,
                                         self->lines.buf[i + 1].data.buf,
                                         *(self->lines.buf[i].data.buf + x +
                                           chars_to_move - ii - 1));
                }

                self->lines.buf[i + 1].damaged = true;
                Vt_destroy_line_proxy(self->lines.buf[i + 1].proxy.data);

            } else if (i < bottom_bound) {
                ++insertions_made;

                Vector_insert_VtLine(&self->lines, (self->lines.buf) + (i + 1),
                                     VtLine_new());

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

        size_t overflow = self->lines.size > self->ws.ws_row
                            ? self->lines.size - self->ws.ws_row
                            : 0;

        size_t whitespace_below = self->lines.size - 1 - self->cursor.row;

        Vector_pop_n_VtLine(
          &self->lines, MIN(overflow, MIN(whitespace_below, insertions_made)));
    }
}

/**
 * Remove extra columns from all lines
 */
static void Vt_trim_columns(Vt* self)
{
    for (size_t i = 0; i < self->lines.size; ++i) {
        if (self->lines.buf[i].data.size > (size_t)self->ws.ws_col) {
            self->lines.buf[i].damaged = true;
            Vt_destroy_line_proxy(self->lines.buf[i].proxy.data);

            size_t blanks = 0;

            size_t s = self->lines.buf[i].data.size;
            Vector_pop_n_VtRune(&self->lines.buf[i].data, s - self->ws.ws_col);

            if (self->lines.buf[i].was_reflown)
                continue;

            s = self->lines.buf[i].data.size;

            for (blanks = 0; blanks < s; ++blanks) {
                if (!(self->lines.buf[i].data.buf[s - 1 - blanks].code == ' ' &&
                      ColorRGBA_eq(
                        settings.bg,
                        self->lines.buf[i].data.buf[s - 1 - blanks].bg))) {
                    break;
                }
            }

            Vector_pop_n_VtRune(&self->lines.buf[i].data, blanks);
        }
    }
}

void Vt_resize(Vt* self, uint32_t x, uint32_t y)
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

            if (self->cursor.row + to_pop > Vt_bottom_line(self)) {
                to_pop -= self->cursor.row + to_pop - Vt_bottom_line(self);
            }

            Vector_pop_n_VtLine(&self->lines, to_pop);

            if (self->alt_lines.buf) {
                size_t to_pop_alt = self->ws.ws_row - y;

                if (self->alt_active_line + to_pop_alt >
                    Vt_bottom_line_alt(self)) {
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
    }

    Pair_uint32_t px =
      CALL_FP(self->callbacks.on_window_size_from_cells_requested,
              self->callbacks.user_data, x, y);

    self->ws = (struct winsize){
        .ws_col = x, .ws_row = y, .ws_xpixel = px.first, .ws_ypixel = px.second
    };

    self->pixels_per_cell_x = (double)self->ws.ws_xpixel / self->ws.ws_col;
    self->pixels_per_cell_y = (double)self->ws.ws_ypixel / self->ws.ws_row;

    if (ioctl(self->master, TIOCSWINSZ, &self->ws) < 0) {
        WRN("ioctl(%d, TIOCSWINSZ, winsize { %d, %d, %d, %d }) failed:  %s\n",
            self->master, self->ws.ws_col, self->ws.ws_row, self->ws.ws_xpixel,
            self->ws.ws_ypixel, strerror(errno));
    }

    self->scroll_region_top    = 0;
    self->scroll_region_bottom = self->ws.ws_row;
}

bool Vt_wait(Vt* self)
{
    FD_ZERO(&self->rfdset);
    FD_ZERO(&self->wfdset);

    FD_SET(self->master, &self->rfdset);
    FD_SET(self->master, &self->wfdset);

    if (0 > pselect(MAX(self->master, self->io) + 1, &self->rfdset,
                    &self->wfdset, NULL, NULL, NULL)) {
        if (errno == EINTR || errno == EAGAIN) {
            errno = 0;
            return true;
        } else {
            WRN("pselect failed: %s\n", strerror(errno));
        }
    }

    return false;
}

__attribute__((always_inline, flatten)) static inline int32_t
short_sequence_get_int_argument(const char* seq)
{
    return *seq == 0 || seq[1] == 0 ? 1 : strtol(seq, NULL, 10);
}

static inline void Vt_handle_dec_mode(Vt* self, int code, bool on)
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

        /* Show toolbar (rxvt) */
        case 10:
            break;

        /* Start Blinking Cursor (AT&T 610) */
        case 12:
            break;

            /* hide/show cursor (DECTCEM) */
        case 25:
            self->cursor.hidden = on;
            break;

            /* Very visible cursor (CVVIS) */
            // case 12:
            // break;

        /* X11 xterm mouse protocol. */
        case 1000:
            self->modes.mouse_btn_report = !on;
            break;

        /* Hilite mouse tracking, xterm */
        case 1001:
            WRN("Hilite mouse tracking not implemented\n");
            break;

        /* xterm cell motion mouse tracking */
        case 1002:
            self->modes.mouse_motion_on_btn_report = !on;
            break;

        /* xterm all motion tracking */
        case 1003:
            self->modes.mouse_motion_report = on;
            break;

        case 1004:
            self->modes.window_focus_events_report = !on;
            break;

        /* utf8 Mouse Mode */
        case 1005:
            WRN("utf8 mouse mode not implemented\n");
            break;

        /* SGR mouse mode */
        case 1006:
            self->modes.extended_report = !on;
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
            self->modes.no_alt_sends_esc = !on;
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
                Vt_alt_buffer_off(self, code == 1049);
            } else {
                Vt_alt_buffer_on(self, code == 1049);
            }
            break;

        case 2004:
            self->modes.bracket_paste = on;
            break;

        case 1051: // Sun function-key mode, xterm.
        case 1052: // HP function-key mode, xterm.
        case 1053: // SCO function-key mode, xterm.
        case 1060: // legacy keyboard emulation, i.e, X11R6,
        case 1061: // VT220 keyboard emulation, xterm.
            WRN("Unimplemented keyboard option\n");
            break;

        default:
            WRN("Unknown DECSET/DECRST code: " TERMCOLOR_DEFAULT "%d\n", code);
    }
}

static inline void Vt_handle_CSI(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_csi_sequence_terminated(self->parser.active_sequence.buf,
                                   self->parser.active_sequence.size)) {

        Vector_push_char(&self->parser.active_sequence, '\0');
        char* seq       = self->parser.active_sequence.buf;
        char  last_char = self->parser.active_sequence
                           .buf[self->parser.active_sequence.size - 2];
        bool is_single_arg = !strchr(seq, ';') && !strchr(seq, ':');

#define MULTI_ARG_IS_ERROR                                                     \
    if (!is_single_arg) {                                                      \
        WRN("Unexpected additional arguments for CSI "                         \
            "sequence " TERMCOLOR_DEFAULT "\'%s\'\n",                          \
            seq);                                                              \
        break;                                                                 \
    }

        if (*seq != '?') {

            /* sequence without question mark */
            switch (last_char) {

                /* <ESC>[ Ps ; ... m - change one or more text attributes (SGR)
                 */
                case 'm': {
                    Vector_pop_n_char(&self->parser.active_sequence, 2);
                    Vector_push_char(&self->parser.active_sequence, '\0');
                    Vt_handle_prop_seq(self, self->parser.active_sequence);
                } break;

                /* <ESC>[ Ps K - clear(erase) line right of cursor (EL)
                 * none/0 - right 1 - left 2 - all */
                case 'K': {
                    MULTI_ARG_IS_ERROR
                    int arg =
                      *seq == 'K' ? 0 : short_sequence_get_int_argument(seq);
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
                            WRN("Unknown CSI(EL) sequence: " TERMCOLOR_DEFAULT
                                "%s\n",
                                seq);
                    }
                } break;

                /* <ECS>[ Ps @ - Insert Ps Chars (ICH) */
                case '@': {
                    MULTI_ARG_IS_ERROR // TODO: (SL), ECMA-48
                      int arg = short_sequence_get_int_argument(seq);
                    for (int i = 0; i < arg; ++i) {
                        Vt_insert_char_at_cursor_with_shift(self, space);
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
                    MULTI_ARG_IS_ERROR // TODO: (SL), ECMA-48
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
                    self->cursor.col =
                      MIN(short_sequence_get_int_argument(seq) - 1,
                          self->ws.ws_col);
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
                    Vt_move_cursor(self, self->cursor.col,
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
                        top = 0;
                        bottom =
                          CALL_FP(self->callbacks.on_number_of_cells_requested,
                                  self->callbacks.user_data)
                            .second;
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

                /* no args: 1:1 */
                /* <ESC>[ Py ; Px f - move cursor to Px-Py (HVP) */
                case 'f':
                /* <ESC>[ Py ; Px H - move cursor to Px-Py (CUP) */
                case 'H': {
                    uint32_t x = 0, y = 0;
                    if (*seq != 'H') {
                        if (sscanf(seq, "%u;%u", &y, &x) == EOF) {
                            WRN("invalid CSI(CUP) sequence %s\n", seq);
                            break;
                        }
                        --x;
                        --y;
                    }
                    Vt_move_cursor(self, x, y);
                } break;

                /* <ESC>[...c - Send device attributes (Primary DA) */
                case 'c': {
                    memcpy(self->out_buf, "\e[?6c", 6); /* report VT 102 */
                    Vt_write(self);
                } break;

                /* <ESC>[...n - Device status report (DSR) */
                case 'n': {
                    MULTI_ARG_IS_ERROR
                    int arg = short_sequence_get_int_argument(seq);
                    if (arg == 5) {
                        /* 5 - is terminal ok
                         *  ok - 0, not ok - 3 */
                        memcpy(self->out_buf, "\e[0n", 5);
                        Vt_write(self);
                    } else if (arg == 6) {
                        /* 6 - report cursor position */
                        sprintf(self->out_buf, "\e[%zu;%zuR",
                                Vt_active_screen_index(self) + 1,
                                self->cursor.col + 1);
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

                /* <ESC>[ Ps i - Local printing related commands */
                case 'i':
                    break;

                /* <ESC>[ Ps q - Set cursor style (DECSCUSR) */
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
                            WRN("Unknown DECSCUR code:" TERMCOLOR_DEFAULT
                                " %d\n",
                                arg);
                    }
                } break;

                /* <ESC>[  Pm... l - Reset Mode (RM) */
                case 'l': {
                    MULTI_ARG_IS_ERROR
                    switch (short_sequence_get_int_argument(seq)) {
                        case 4:
                            // TODO: turn off IRM
                            break;
                    }
                } break;

                /* <ESC>[ Ps ; ... t - WindowOps */
                case 't': {
                    int nargs;
                    int args[4];

                    for (nargs = 0; seq && nargs < 4 && *seq != 't'; ++nargs) {
                        *(args + nargs) = strtol(seq, NULL, 10);
                        seq             = strstr(seq, ";");
                        if (seq)
                            ++seq;
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
                                WRN("Invalid CSI(WindowOps) "
                                    "sequence:" TERMCOLOR_DEFAULT " %s\n",
                                    seq);
                            }
                        } break;

                        /* Report iconification state */
                        case 11:
                            /* if (iconified) */
                            /*     write(CSI 1 t) */
                            /* else */
                            /*     write(CSI 2 t) */
                            break;

                        /* Report window position */
                        case 13: {
                            Pair_uint32_t pos = CALL_FP(
                              self->callbacks.on_window_position_requested,
                              self->callbacks.user_data);
                            snprintf(self->out_buf, sizeof self->out_buf,
                                     "\e[3;%d;%d;t", pos.first, pos.second);
                            Vt_write(self);
                        } break;

                        /* Report window size in pixels */
                        case 14: {
                            snprintf(self->out_buf, sizeof self->out_buf,
                                     "\e[4;%d;%d;t", self->ws.ws_xpixel,
                                     self->ws.ws_ypixel);
                            Vt_write(self);
                        } break;

                        /* Report text area size in chars */
                        case 18:
                            snprintf(self->out_buf, sizeof self->out_buf,
                                     "\e[8;%d;%d;t", self->ws.ws_col,
                                     self->ws.ws_row);
                            Vt_write(self);
                            break;

                        /* Report window size in chars */
                        case 19:
                            snprintf(self->out_buf, sizeof self->out_buf,
                                     "\e[9;%d;%d;t", self->ws.ws_col,
                                     self->ws.ws_row);
                            Vt_write(self);
                            break;

                        /* Report icon name */
                        case 20:
                        /* Report window title */
                        case 21:
                            snprintf(self->out_buf, sizeof self->out_buf,
                                     "\e]L%s\e\\", self->title);
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
                            // int arg = short_sequence_get_int_argument(seq);
                            // uint32_t ypixels = gfx_pixels(arg, 0).first;
                        }
                    }

                } break;

                default:
                    WRN("Unknown CSI sequence: " TERMCOLOR_DEFAULT "%s\n", seq);

            } // end switch

        } else {

            /* sequence starts with question mark */
            switch (last_char) {

                /* DEC Private Mode Set (DECSET) */
                case 'h':
                /* DEC Private Mode Reset (DECRST) */
                case 'l': {
                    bool               is_enable = last_char == 'l';
                    Vector_Vector_char tokens =
                      string_split_on(seq + 1, ";:", NULL);
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
            }
        }

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_simple_prop_cmd(Vt* self, char* command)
{
    int cmd = *command ? strtol(command, NULL, 10) : 0;

#define MAYBE_DISABLE_ALL_UNDERLINES                                           \
    if (!settings.allow_multiple_underlines) {                                 \
        self->parser.char_state.underlined      = false;                       \
        self->parser.char_state.doubleunderline = false;                       \
        self->parser.char_state.curlyunderline  = false;                       \
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

static inline void Vt_alt_buffer_on(Vt* self, bool save_mouse)
{
    Vt_visual_scroll_reset(self);
    self->alt_lines = self->lines;
    self->lines     = Vector_new_VtLine();
    for (size_t i = 0; i < self->ws.ws_row; ++i)
        Vector_push_VtLine(&self->lines, VtLine_new());
    if (save_mouse) {
        self->alt_cursor_pos  = self->cursor.col;
        self->alt_active_line = self->cursor.row;
    }
    self->cursor.row = 0;
}

static inline void Vt_alt_buffer_off(Vt* self, bool save_mouse)
{
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
        self->scroll_region_bottom = self->ws.ws_row;
        Vt_visual_scroll_reset(self);
    }
}

/**
 * SGR codes are separated by one or multiple ';' or ':',
 * some values require a set number of following 'arguments'.
 * 'Commands' may be combined into a single sequence.
 */
static void Vt_handle_prop_seq(Vt* self, Vector_char seq)
{
    Vector_Vector_char tokens = string_split_on(seq.buf, ";:", NULL);

    for (Vector_char* token = NULL;
         (token = Vector_iter_Vector_char(&tokens, token));) {
        Vector_char* args[] = { token, NULL, NULL, NULL, NULL };

        /* color change 'commands' */
        if (!strcmp(token->buf + 1, "38") /* foreground */ ||
            !strcmp(token->buf + 1, "48") /* background */ ||
            !strcmp(token->buf + 1, "58") /* underline  */) {
            /* next argument determines how the color will be set and final
             * number of args */

            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                (args[2] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                if (!strcmp(args[1]->buf + 1, "5")) {
                    /* from 256 palette (one argument) */
                    long idx = MIN(strtol(args[2]->buf + 1, NULL, 10), 255);

                    if (args[0]->buf[1] == '3')
                        self->parser.char_state.fg = color_palette_256[idx];

                    else if (args[0]->buf[1] == '4')
                        self->parser.char_state.bg =
                          ColorRGBA_from_RGB(color_palette_256[idx]);

                    else if (args[0]->buf[1] == '5') {
                        self->parser.char_state.linecolornotdefault = true;
                        self->parser.char_state.line = color_palette_256[idx];
                    }

                } else if (!strcmp(args[1]->buf + 1, "2")) {
                    /* sent as 24-bit rgb (three arguments) */
                    if ((args[3] =
                           (token = Vector_iter_Vector_char(&tokens, token))) &&
                        (args[4] =
                           (token = Vector_iter_Vector_char(&tokens, token)))) {
                        long c[3] = {
                            MIN(strtol(args[2]->buf + 1, NULL, 10), 255),
                            MIN(strtol(args[3]->buf + 1, NULL, 10), 255),
                            MIN(strtol(args[4]->buf + 1, NULL, 10), 255)
                        };

                        if (args[0]->buf[1] == '3') {
                            self->parser.char_state.fg =
                              (ColorRGB){ .r = c[0], .g = c[1], .b = c[2] };
                        } else if (args[0]->buf[1] == '4') {
                            self->parser.char_state.bg = (ColorRGBA){
                                .r = c[0], .g = c[1], .b = c[2], .a = 255
                            };
                        } else if (args[0]->buf[1] == '5') {
                            self->parser.char_state.linecolornotdefault = true;
                            self->parser.char_state.line =
                              (ColorRGB){ .r = c[0], .g = c[1], .b = c[2] };
                        }
                    }
                }
            }
        } else if (!strcmp(token->buf + 1, "4")) {

            // possible curly underline
            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token)))) {

                // enable this only on "4:3" not "4;3"
                if (!strcmp(args[1]->buf, ":3")) {

                    if (!settings.allow_multiple_underlines) {
                        self->parser.char_state.underlined      = false;
                        self->parser.char_state.doubleunderline = false;
                    }

                    self->parser.char_state.curlyunderline = true;
                } else {
                    Vt_handle_simple_prop_cmd(self, args[0]->buf + 1);
                    Vt_handle_simple_prop_cmd(self, args[1]->buf + 1);
                }
            } else {
                Vt_handle_simple_prop_cmd(self, args[0]->buf + 1);
                break; // end of sequence
            }

        } else {
            Vt_handle_simple_prop_cmd(self, token->buf + 1);
        }
    } // end for

    Vector_destroy_Vector_char(&tokens);
}

static void Vt_handle_APC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_generic_sequence_terminated(self->parser.active_sequence.buf,
                                       self->parser.active_sequence.size)) {
        Vector_push_char(&self->parser.active_sequence, '\0');

        const char* seq = self->parser.active_sequence.buf;
        char*       str = pty_string_prettyfy(seq);
        WRN("Unknown application programming command:" TERMCOLOR_DEFAULT
            " %s\n",
            str);
        free(str);

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_DCS(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_generic_sequence_terminated(self->parser.active_sequence.buf,
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

            default:;
        }

        char* str = pty_string_prettyfy(self->parser.active_sequence.buf);
        WRN("Unknown device control string:" TERMCOLOR_DEFAULT " %s\n", str);
        free(str);

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_PM(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_generic_sequence_terminated(self->parser.active_sequence.buf,
                                       self->parser.active_sequence.size)) {

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_OSC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_generic_sequence_terminated(self->parser.active_sequence.buf,
                                       self->parser.active_sequence.size)) {
        Vector_push_char(&self->parser.active_sequence, '\0');

        char*              seq    = self->parser.active_sequence.buf;
        Vector_Vector_char tokens = string_split_on(seq, ";:", "\a\b\n\t\v");

        int arg = strtol(tokens.buf[0].buf + 1, NULL, 10);

        switch (arg) {
            /* Change Icon Name and Window Title */
            case 0:
            /* Change Icon Name */
            case 1:
            /* Change Window Title */
            case 2:
                /* Set title */
                if (tokens.size >= 2) {
                    free(self->title);
                    self->title = strdup(tokens.buf[1].buf + 1);
                    CALL_FP(self->callbacks.on_title_changed,
                            self->callbacks.user_data, tokens.buf[1].buf + 1);
                }
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
                    while (*uri && *uri != '/')
                        ++uri; // skip hostname
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

            /* sets dynamic colors for xterm colorOps */
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            case 19:
                WRN("Dynamic colors not implemented\n");
                break;

            /* coresponding colorOps resets */
            case 110:
            case 111:
            case 112:
            case 113:
            case 114:
            case 115:
            case 116:
            case 117:
            case 118:
            case 119:
                // TODO: reset things, when there are things to reset
                break;

            case 50:
                WRN("xterm fontOps not implemented\n");
                break;

            /* Send desktop notification */
            case 777:
                WRN("OSC 777 notifications not implemented\n");
                break;

            default:
                WRN("Unknown OSC:" TERMCOLOR_DEFAULT " %s\n",
                    self->parser.active_sequence.buf);
        }

        Vector_destroy_Vector_char(&tokens);
        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

// TODO: figure out how this should work
static inline void Vt_push_title(Vt* self)
{
    Vector_push_size_t(&self->title_stack, (size_t)self->title);
    self->title = NULL;
}

static inline void Vt_pop_title(Vt* self)
{
    if (self->title)
        free(self->title);

    if (self->title_stack.size) {
        self->title = (char*)self->title_stack.buf[self->title_stack.size - 1];
        Vector_pop_size_t(&self->title_stack);
    } else {
        self->title = NULL;
    }
}

static inline void Vt_reset_text_attribs(Vt* self)
{
    memset(&self->parser.char_state, 0, sizeof(self->parser.char_state));
    self->parser.char_state.code = ' ';
    self->parser.char_state.bg   = settings.bg;
    self->parser.char_state.fg   = settings.fg;
    self->parser.color_inverted  = false;
}

/**
 * Move cursor to first column */
__attribute__((always_inline)) static inline void Vt_carriage_return(Vt* self)
{
    self->cursor.col = 0;
}

/**
 * make a new empty line at cursor position, scroll down contents below */
__attribute__((always_inline)) static inline void Vt_insert_line(Vt* self)
{
    Vector_insert_VtLine(&self->lines,
                         Vector_at_VtLine(&self->lines, self->cursor.row),
                         VtLine_new());

    Vt_empty_line_fill_bg(self, self->cursor.row);

    Vector_remove_at_VtLine(
      &self->lines,
      MIN(Vt_get_scroll_region_bottom(self) + 1, Vt_bottom_line(self)), 1);
}

/**
 * the same as insert line, but adds before cursor line */
__attribute__((always_inline)) static inline void Vt_reverse_line_feed(Vt* self)
{
    Vector_remove_at_VtLine(
      &self->lines,
      MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self) + 1), 1);
    Vector_insert_VtLine(&self->lines,
                         Vector_at_VtLine(&self->lines, self->cursor.row),
                         VtLine_new());
    Vt_empty_line_fill_bg(self, self->cursor.row);
}

/**
 * delete active line, content below scrolls up */
__attribute__((always_inline)) static inline void Vt_delete_line(Vt* self)
{
    Vector_remove_at_VtLine(&self->lines, self->cursor.row, 1);

    Vector_insert_VtLine(
      &self->lines,
      Vector_at_VtLine(&self->lines, MIN(Vt_get_scroll_region_bottom(self) + 1,
                                         Vt_bottom_line(self))),
      VtLine_new());

    Vt_empty_line_fill_bg(
      self, MIN(Vt_get_scroll_region_bottom(self) + 1, Vt_bottom_line(self)));
}

__attribute__((always_inline)) static inline void Vt_scroll_up(Vt* self)
{
    Vector_insert_VtLine(
      &self->lines,
      Vector_at_VtLine(
        &self->lines,
        MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self) + 1) + 1),
      VtLine_new());

    Vt_empty_line_fill_bg(
      self,
      MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self) + 1) + 1);

    Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_top(self) - 1,
                            1);
}

__attribute__((always_inline)) static inline void Vt_scroll_down(Vt* self)
{
    Vector_remove_at_VtLine(
      &self->lines,
      MAX(Vt_top_line(self), Vt_get_scroll_region_bottom(self) + 1), 1);

    Vector_insert_VtLine(
      &self->lines,
      Vector_at_VtLine(&self->lines, Vt_get_scroll_region_top(self)),
      VtLine_new());
}

/**
 * Move cursor one cell down if possible */
__attribute__((always_inline)) static inline void Vt_cursor_down(Vt* self)
{
    if (self->cursor.row < Vt_bottom_line(self))
        ++self->cursor.row;
}

/**
 * Move cursor one cell up if possible */
__attribute__((always_inline)) static inline void Vt_cursor_up(Vt* self)
{
    if (self->cursor.row > Vt_top_line(self))
        --self->cursor.row;
}

/**
 * Move cursor one cell to the left if possible */
__attribute__((always_inline)) static inline void Vt_cursor_left(Vt* self)
{
    if (self->cursor.col)
        --self->cursor.col;
}

/**
 * Move cursor one cell to the right if possible */
__attribute__((always_inline)) static inline void Vt_cursor_right(Vt* self)
{
    if (self->cursor.col < self->ws.ws_col)
        ++self->cursor.col;
}

static inline void Vt_erase_to_end(Vt* self)
{
    for (size_t i = self->cursor.row + 1; i <= Vt_bottom_line(self); ++i) {
        self->lines.buf[i].data.size = 0;
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_right(self);
}

static inline void Pty_backspace(Vt* self)
{
    if (self->cursor.col)
        --self->cursor.col;
}

/**
 * Overwrite characters with colored space */
static inline void Vt_erase_chars(Vt* self, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        size_t idx = self->cursor.col + i;
        if (idx >= self->lines.buf[self->cursor.row].data.size) {
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data,
                               self->parser.char_state);
        } else {
            self->lines.buf[self->cursor.row].data.buf[idx] =
              self->parser.char_state;
        }
    }

    self->lines.buf[self->cursor.row].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->cursor.row].proxy.data);
}

/**
 * remove characters at cursor, remaining content scrolls left */
static void Vt_delete_chars(Vt* self, size_t n)
{
    /* Trim if line is longer than screen area */
    if (self->lines.buf[self->cursor.row].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->lines.buf[self->cursor.row].data.size -
                              self->ws.ws_col);
    }

    Vector_remove_at_VtRune(
      &self->lines.buf[self->cursor.row].data, self->cursor.col,
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
        self->parser.char_state.bg =
          self->lines.buf[self->cursor.row]
            .data.buf[self->lines.buf[self->cursor.row].data.size - 2]
            .bg;
    } else {
        self->parser.char_state.bg = settings.bg;
    }

    for (size_t i = self->lines.buf[self->cursor.row].data.size - 1;
         i < self->ws.ws_col; ++i) {
        Vector_push_VtRune(&self->lines.buf[self->cursor.row].data,
                           self->parser.char_state);
    }

    self->parser.char_state     = tmp;
    self->parser.color_inverted = tmp_invert;

    if (self->lines.buf[self->cursor.row].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->lines.buf[self->cursor.row].data.size -
                              self->ws.ws_col);
    }

    /* ...add n spaces with currently set attributes to the end */
    for (size_t i = 0; i < n && self->cursor.col + i < self->ws.ws_col; ++i) {
        Vector_push_VtRune(&self->lines.buf[self->cursor.row].data,
                           self->parser.char_state);
    }

    /* Trim to screen size again */
    if (self->lines.buf[self->cursor.row].data.size > self->ws.ws_col) {
        Vector_pop_n_VtRune(&self->lines.buf[self->cursor.row].data,
                            self->lines.buf[self->cursor.row].data.size -
                              self->ws.ws_col);
    }

    self->lines.buf[self->cursor.row].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->cursor.row].proxy.data);
}

static inline void Vt_scroll_out_all_content(Vt* self)
{
    int64_t to_add = 0;
    for (size_t i = Vt_visual_bottom_line(self) - 1;
         i >= Vt_visual_top_line(self); --i) {
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
    size_t to_add = Vt_active_screen_index(self);
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
    self->lines.buf[self->cursor.row].damaged = true;
    Vector_destroy_VtLine(&self->lines);
    self->lines = Vector_new_VtLine();
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
            self->lines.buf[self->cursor.row].data.buf[i] =
              self->parser.char_state;
        else
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data,
                               self->parser.char_state);
    }

    self->lines.buf[self->cursor.row].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->cursor.row].proxy.data);
}

/**
 * Clear active line right of cursor and fill it with whatever character
 * attributes are set */
static inline void Vt_clear_right(Vt* self)
{
    for (size_t i = self->cursor.col; i <= self->ws.ws_col; ++i) {
        if (i + 1 <= self->lines.buf[self->cursor.row].data.size)
            self->lines.buf[self->cursor.row].data.buf[i] =
              self->parser.char_state;
        else
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data,
                               self->parser.char_state);
    }

    self->lines.buf[self->cursor.row].damaged = true;
    Vt_destroy_line_proxy(self->lines.buf[self->cursor.row].proxy.data);
}

/**
 * Insert character literal at cursor position, deal with reaching column limit
 */
__attribute__((always_inline, hot)) static inline void Vt_insert_char_at_cursor(
  Vt*    self,
  VtRune c)
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

    while (self->lines.buf[self->cursor.row].data.size <= self->cursor.col)
        Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, space);

    if (unlikely(self->parser.color_inverted)) {
        ColorRGB tmp = c.fg;
        c.fg         = ColorRGB_from_RGBA(c.bg);
        c.bg         = ColorRGBA_from_RGB(tmp);
    }

    Vt_mark_proxy_damaged(self, self->cursor.row);

    self->lines.buf[self->cursor.row].data.buf[self->cursor.col] = c;

    ++self->cursor.col;

    if (unlikely(wcwidth(c.code) == 2)) {
        if (self->lines.buf[self->cursor.row].data.size <= self->cursor.col) {
            Vector_push_VtRune(&self->lines.buf[self->cursor.row].data, space);
        } else {
            self->lines.buf[self->cursor.row].data.buf[self->cursor.col] =
              space;
        }

        ++self->cursor.col;
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

    VtRune* insert_point = Vector_at_VtRune(
      &self->lines.buf[self->cursor.row].data, self->cursor.col);
    Vector_insert_VtRune(&self->lines.buf[self->cursor.row].data, insert_point,
                         c);

    Vt_mark_proxy_damaged(self, self->cursor.row);
}

static inline void Vt_empty_line_fill_bg(Vt* self, size_t idx)
{
    ASSERT(self->lines.buf[idx].data.size == 0, "line is not empty");

    self->lines.buf[idx].damaged = true;

    Vt_destroy_line_proxy(self->lines.buf[idx].proxy.data);

    if (!ColorRGBA_eq(self->parser.char_state.bg, settings.bg))
        for (size_t i = 0; i < self->ws.ws_col; ++i)
            Vector_push_VtRune(&self->lines.buf[idx].data,
                               self->parser.char_state);
}

/**
 * Move one line down or insert a new one, scrolls if region is set */
static inline void Vt_insert_new_line(Vt* self)
{
    if (self->cursor.row == Vt_get_scroll_region_bottom(self) + 1) {
        Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_top(self),
                                1);
        Vector_insert_VtLine(&self->lines,
                             Vector_at_VtLine(&self->lines, self->cursor.row),
                             VtLine_new());
        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else {
        if (Vt_bottom_line(self) == self->cursor.row) {
            Vector_push_VtLine(&self->lines, VtLine_new());
            Vt_empty_line_fill_bg(self, self->cursor.row + 1);
        }
        ++self->cursor.row;
    }
}

/**
 * Move cursor to given location */
__attribute__((always_inline, hot)) static inline void
Vt_move_cursor(Vt* self, uint32_t columns, uint32_t rows)
{
    self->cursor.row =
      MIN(rows, (uint32_t)self->ws.ws_row - 1) + Vt_top_line(self);
    self->cursor.col = MIN(columns, (uint32_t)self->ws.ws_col);
}

__attribute__((always_inline, hot, flatten)) static inline void
Vt_handle_literal(Vt* self, char c)
{
    if (unlikely(self->parser.in_mb_seq)) {

        char32_t res;
        size_t   rd = mbrtoc32(&res, &c, 1, &self->parser.input_mbstate);

        if (unlikely(rd == (size_t)-1)) {
            // encoding error
            WRN("%s\n", strerror(errno));
            errno = 0;

            // zero-initialized mbstate_t always represents the initial
            // conversion state
            memset(&self->parser.input_mbstate, 0, sizeof(mbstate_t));
        } else if (rd != (size_t)-2) {
            // sequence is complete
            VtRune new_rune = self->parser.char_state;
            new_rune.code   = res;
            Vt_insert_char_at_cursor(self, new_rune);
            self->parser.in_mb_seq = false;
        }
    } else {
        switch (c) {
            case '\a':
                if (!settings.no_flash) {
                    CALL_FP(self->callbacks.on_bell_flash,
                            self->callbacks.user_data);
                }
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
                size_t cp = self->cursor.col;

                // TODO: do this properly
                for (size_t i = 0; i < self->tabstop - (cp % self->tabstop);
                     ++i)
                    Vt_cursor_right(self);
            } break;

            default: {

                if (c & (1 << 7)) {
                    mbrtoc32(NULL, &c, 1, &self->parser.input_mbstate);
                    self->parser.in_mb_seq = true;
                    break;
                }

                VtRune new_char = self->parser.char_state;
                new_char.code   = c;

                if (unlikely((bool)self->charset_g0))
                    new_char.code = self->charset_g0(c);
                if (unlikely((bool)self->charset_g1))
                    new_char.code = self->charset_g1(c);

                Vt_insert_char_at_cursor(self, new_char);
            }
        }
    }
}

__attribute__((always_inline, hot)) static inline void Vt_handle_char(Vt*  self,
                                                                      char c)
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
                    if (!settings.no_flash) {
                        CALL_FP(self->callbacks.on_bell_flash,
                                self->callbacks.user_data);
                    }
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

                /* Reset initial state (RIS) */
                case 'c':
                    Vt_select_end(self);
                    Vt_clear_display_and_scrollback(self);
                    Vt_move_cursor(self, 0, 0);
                    self->tabstop           = 8;
                    self->parser.state      = PARSER_STATE_LITERAL;
                    self->scroll_region_top = 0;

                    self->scroll_region_bottom =
                      CALL_FP(self->callbacks.on_number_of_cells_requested,
                              self->callbacks.user_data)
                        .second;

                    for (size_t* i = NULL;
                         Vector_iter_size_t(&self->title_stack, i);)
                        free((char*)*i);
                    Vector_destroy_size_t(&self->title_stack);
                    self->title_stack = Vector_new_size_t();
                    return;

                /* Save cursor (DECSC) */
                case '7':
                    self->saved_active_line = self->cursor.row;
                    self->saved_cursor_pos  = self->cursor.col;
                    return;

                /* Restore cursor (DECRC) */
                case '8':
                    self->cursor.row = self->saved_active_line;
                    self->cursor.col = self->saved_cursor_pos;
                    return;

                case '\e':
                    return;

                default: {
                    char* cs    = control_char_get_pretty_string(c);
                    char  cb[2] = { c, 0 };
                    WRN("Unknown escape sequence:" TERMCOLOR_DEFAULT
                        " %s " TERMCOLOR_YELLOW "(" TERMCOLOR_DEFAULT
                        "%d" TERMCOLOR_YELLOW ")\n",
                        cs ? cs : cb, c);

                    self->parser.state = PARSER_STATE_LITERAL;
                    return;
                }
            }
            break;

        case PARSER_STATE_CHARSET_G0:
            self->parser.state = PARSER_STATE_LITERAL;
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
                    WRN("Unknown sequence ESC(%c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G1:
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
                    WRN("Unknown sequence <ESC>)%c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G2:
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
                    WRN("Unknown sequence <ESC>)%c\n", c);
                    return;
            }
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

        default:
            ASSERT_UNREACHABLE;
    }
}

static inline void Vt_shrink_scrollback(Vt* self)
{
    // alt buffer is active
    if (self->alt_lines.buf)
        return;

    size_t ln_cnt = self->lines.size;
    if (unlikely(ln_cnt > MAX(settings.scrollback * 1.1, self->ws.ws_row))) {
        size_t to_remove = ln_cnt - settings.scrollback;
        Vector_remove_at_VtLine(&self->lines, 0, to_remove);
        self->cursor.row -= to_remove;
    }
}

static inline void Vt_clear_proxies(Vt* self)
{
    if (self->scrolling) {
        if (self->visual_scroll_top > self->ws.ws_row * 5) {
            Vt_clear_proxies_in_region(
              self, Vt_visual_bottom_line(self) + 3 * self->ws.ws_row,
              self->lines.size - 1);
        }
    } else if (self->lines.size > self->ws.ws_row) {
        Vt_clear_proxies_in_region(self, 0, Vt_visual_top_line(self));
    }
}

__attribute__((hot)) inline bool Vt_read(Vt* self)
{
    if (FD_ISSET(self->master, &self->rfdset)) {
        int rd = read(self->master, self->buf, sizeof(self->buf) - 2);

        if (rd >= 0 && settings.scroll_on_output) {
            Vt_visual_scroll_reset(self);
        }

#ifdef DEBUG
        if (rd > 0) {
            self->buf[rd] = 0;
            char* out     = pty_string_prettyfy(self->buf);
            LOG("PTY." TERMCOLOR_MAGENTA_LIGHT "READ(" TERMCOLOR_DEFAULT
                "%s" TERMCOLOR_MAGENTA_LIGHT ")" TERMCOLOR_DEFAULT
                "  ~> { bytes: %3d | %s } \n",
                self->dev_name, rd, out);
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
            for (int i = 0; i < rd; ++i) {
                Vt_handle_char(self, self->buf[i]);
            }

            CALL_FP(self->callbacks.on_repaint_required,
                    self->callbacks.user_data);

            if ((uint32_t)rd < (sizeof self->buf - 2)) {
                /* nothing more to read */
                static bool first = true;
                if (first) {
                    first = false;

                    Pair_uint32_t px = CALL_FP(
                      self->callbacks.on_window_size_from_cells_requested,
                      self->callbacks.user_data, self->ws.ws_col,
                      self->ws.ws_row);

                    self->ws.ws_xpixel = px.first;
                    self->ws.ws_ypixel = px.second;
                    if (ioctl(self->master, TIOCSWINSZ, &self->ws) < 0)
                        WRN("IO operation failed %s\n", strerror(errno));
                }

                Vt_shrink_scrollback(self);
                Vt_clear_proxies(self);
                return false;
            }
        }
        return true;
    } else {
        /* !FD_ISSET(..) */
    }
    return false;
}

/**
 * write @param bytes from out buffer to pty */
__attribute__((always_inline)) static inline void Vt_write_n(Vt*    self,
                                                             size_t bytes)
{
#ifdef DEBUG
    char* str = pty_string_prettyfy(self->out_buf);
    LOG("PTY." TERMCOLOR_YELLOW "WRITE(" TERMCOLOR_DEFAULT "%s" TERMCOLOR_YELLOW
        ")" TERMCOLOR_DEFAULT " <~ { bytes: %3ld | %s }\n",
        self->dev_name, strlen(self->out_buf), str);
    free(str);
#endif

    write(self->master, self->out_buf, bytes);
}

/**
 * Write null-terminated string from out buffer to pty */
__attribute__((always_inline)) static inline void Vt_write(Vt* self)
{
    Vt_write_n(self, strlen(self->out_buf));
}

void Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end)
{
    if (out_begin) {
        *out_begin = self->lines.buf + Vt_visual_top_line(self);
    }

    if (out_end) {
        *out_end = self->lines.buf + Vt_visual_bottom_line(self);
    }
}

__attribute__((always_inline)) static inline const char* normal_keypad_response(
  const uint32_t key)
{
    switch (key) {
        case XKB_KEY_Up:
            return "\e[A\0";
        case XKB_KEY_Down:
            return "\e[B\0";
        case XKB_KEY_Right:
            return "\e[C\0";
        case XKB_KEY_Left:
            return "\e[D\0";
        case XKB_KEY_End:
            return "\e[F\0";
        case XKB_KEY_Home:
            return "\e[H\0";
        case 127:
            return "\e[P\0";
        default:
            return NULL;
    }
}

__attribute__((always_inline)) static inline const char*
application_keypad_response(const uint32_t key)
{
    switch (key) {
        case XKB_KEY_Up:
            return "\eOA\0";
        case XKB_KEY_Down:
            return "\eOB\0";
        case XKB_KEY_Right:
            return "\eOC\0";
        case XKB_KEY_Left:
            return "\eOD\0";
        case XKB_KEY_End:
            return "\eOF\0";
        case XKB_KEY_Home:
            return "\eOH\0";
        case XKB_KEY_KP_Enter:
            return "\eOM\0";
        case XKB_KEY_KP_Multiply:
            return "\eOj\0";
        case XKB_KEY_KP_Add:
            return "\eOk\0";
        case XKB_KEY_KP_Separator:
            return "\eOl\0";
        case XKB_KEY_KP_Subtract:
            return "\eOm\0";
        case XKB_KEY_KP_Divide:
            return "\eOo\0";
        case 127:
            return "\e[3~";
        default:
            return NULL;
    }
}

/**
 * Get response format string in normal keypad mode */
__attribute__((always_inline)) static inline const char*
normal_mod_keypad_response(const uint32_t key)
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
__attribute__((always_inline)) static inline const char*
application_mod_keypad_response(const uint32_t key)
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
            char32_t result = strtol(self->unicode_input.buffer.buf, NULL, 16);
            Vector_clear_char(&self->unicode_input.buffer);
            if (result) {
                LOG("unicode input \'%s\' -> %d\n",
                    self->unicode_input.buffer.buf, result);

                static mbstate_t mbstate;
                size_t seq_len = c32rtomb(Vt_buffer(self), result, &mbstate);
                if (seq_len) {
                    Vt_buffer(self)[seq_len] = '\0';
                    Vt_write(self);
                }
            } else {
                WRN("Failed to parse \'%s\'\n", self->unicode_input.buffer.buf);
            }
        } else if (key == 27) {
            // Escape
            self->unicode_input.buffer.size = 0;
            self->unicode_input.active      = false;
            CALL_FP(self->callbacks.on_repaint_required,
                    self->callbacks.user_data);
        } else if (key == 8) {
            // Backspace
            if (self->unicode_input.buffer.size) {
                Vector_pop_char(&self->unicode_input.buffer);
            } else {
                self->unicode_input.buffer.size = 0;
                self->unicode_input.active      = false;
            }
            CALL_FP(self->callbacks.on_repaint_required,
                    self->callbacks.user_data);
        } else if (isxdigit(key)) {
            if (self->unicode_input.buffer.size > 8) {
                CALL_FP(self->callbacks.on_bell_flash,
                        self->callbacks.user_data);
            } else {
                Vector_push_char(&self->unicode_input.buffer, key);
                CALL_FP(self->callbacks.on_repaint_required,
                        self->callbacks.user_data);
            }
        } else {
            CALL_FP(self->callbacks.on_bell_flash, self->callbacks.user_data);
        }
        return true;
    }
    return false;
}

/**
 * Respond to key event if it is a keypad key
 * @return keypress was consumed */
static inline bool Vt_maybe_handle_keypad_key(Vt*      self,
                                              uint32_t key,
                                              uint32_t mods)
{
    const char* resp = NULL;
    if (mods) {
        resp = self->modes.application_keypad
                 ? application_mod_keypad_response(key)
                 : normal_mod_keypad_response(key);

        if (resp) {
            sprintf(self->out_buf, resp, mods + 1);
            Vt_write(self);
            return true;
        }

    } else {
        resp = self->modes.application_keypad ? application_keypad_response(key)
                                              : normal_keypad_response(key);

        if (resp) {
            memcpy(self->out_buf, resp, 5);
            Vt_write(self);
            return true;
        }
    }

    return false;
}

/**
 * Respond to key event if it is a function key
 * @return keypress was consumed */
static inline bool Vt_maybe_handle_function_key(Vt*      self,
                                                uint32_t key,
                                                uint32_t mods)
{
    if (key >= XKB_KEY_F1 && key <= XKB_KEY_F35) {
        int f_num = key - XKB_KEY_F1;
        if (mods) {
            if (f_num < 4)
                sprintf(self->out_buf, "\e[[1;%u%c", mods + 1, f_num + 'P');
            else
                sprintf(self->out_buf, "\e[%d;%u~", f_num + 12, mods + 1);
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
 *  Substitute keypad keys with normal ones */
__attribute__((always_inline)) static inline uint32_t numpad_key_convert(
  uint32_t key)
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
void Vt_handle_key(void* _self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    Vt* self = _self;

    if (!Vt_maybe_handle_unicode_input_key(self, key, rawkey, mods) &&
        !Vt_maybe_handle_keypad_key(self, key, mods) &&
        !Vt_maybe_handle_function_key(self, key, mods)) {
        key = numpad_key_convert(key);

        uint8_t offset = 0;
        if (FLAG_IS_SET(mods, MODIFIER_ALT) && !self->modes.no_alt_sends_esc) {
            Vt_buffer(self)[0] = '\e';
            offset             = 1;
        }

        if (unlikely(key == '\b') && settings.bsp_sends_del)
            key = 127;

        static mbstate_t mbstate;
        size_t mb_len = c32rtomb(Vt_buffer(self) + offset, key, &mbstate);

        if (mb_len) {
            Vt_buffer(self)[mb_len] = '\0';
            Vt_write(self);
        }
    }

    if (settings.scroll_on_key) {
        Vt_visual_scroll_reset(self);
    }

    CALL_FP(self->callbacks.on_action_performed, self->callbacks.user_data);
}

/**
 * Respond to mouse button event
 * @param button  - X11 button code
 * @param state   - press/release
 * @param ammount - for non-discrete scroll
 * @param mods    - modifier keys depressed */
void Vt_handle_button(void*    _self,
                      uint32_t button,
                      bool     state,
                      int32_t  x,
                      int32_t  y,
                      int32_t  ammount,
                      uint32_t mods)
{
    Vt* self = _self;

    bool in_window =
      x >= 0 && x <= self->ws.ws_xpixel && y >= 0 && y <= self->ws.ws_ypixel;

    if ((self->modes.extended_report ||
         self->modes.mouse_motion_on_btn_report ||
         self->modes.mouse_btn_report) &&
        in_window) {

        if (!self->scrolling) {
            self->last_click_x = (double)x / self->pixels_per_cell_x;
            self->last_click_y = (double)y / self->pixels_per_cell_y;

            if (self->modes.x10_mouse_compat) {
                button += (FLAG_IS_SET(mods, MODIFIER_SHIFT) ? 4 : 0) +
                          (FLAG_IS_SET(mods, MODIFIER_ALT) ? 8 : 0) +
                          (FLAG_IS_SET(mods, MODIFIER_CONTROL) ? 16 : 0);
            }

            if (self->modes.extended_report) {
                sprintf(self->out_buf, "\e[<%u;%lu;%lu%c", button - 1,
                        self->last_click_x + 1, self->last_click_y + 1,
                        state ? 'M' : 'm');
            } else if (self->modes.mouse_btn_report) {
                sprintf(self->out_buf, "\e[M%c%c%c",
                        32 + button - 1 + !state * 3,
                        (char)(32 + self->last_click_x + 1),
                        (char)(32 + self->last_click_y + 1));
            }
            Vt_write(self);
        }
    }

    CALL_FP(self->callbacks.on_repaint_required, self->callbacks.user_data);
}

/**
 * Respond to pointer motion event
 * @param button - button beeing held down */
void Vt_handle_motion(void* _self, uint32_t button, int32_t x, int32_t y)
{
    Vt* self = _self;
    if (self->modes.extended_report) {
        if (!self->scrolling) {
            x              = CLAMP(x, 0, self->ws.ws_xpixel);
            y              = CLAMP(y, 0, self->ws.ws_ypixel);
            size_t click_x = (double)x / self->pixels_per_cell_x;
            size_t click_y = (double)y / self->pixels_per_cell_y;

            if (click_x != self->last_click_x ||
                click_y != self->last_click_y) {
                self->last_click_x = click_x;
                self->last_click_y = click_y;

                sprintf(self->out_buf, "\e[<%d;%zu;%zuM", (int)button - 1 + 32,
                        click_x + 1, click_y + 1);

                Vt_write(self);
            }
        }
    }
}

/**
 * Respond to clipboard paste */
void Vt_handle_clipboard(void* _self, const char* text)
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
        int to_cpy = MIN(len - i, sizeof(self->out_buf) - bi);
        memcpy(self->out_buf + bi, text + i, to_cpy);
        i += to_cpy;
        bi += to_cpy;
        if (bi > sizeof(self->out_buf) - (self->modes.bracket_paste ? 7 : 1)) {
            Vt_write(self);
            bi = 0;
        }
    }

    if (self->modes.bracket_paste)
        memcpy(self->out_buf + bi, "\e[201~", 7);
    else
        self->out_buf[bi] = 0;

    Vt_write(self);
}
