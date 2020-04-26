/* See LICENSE for license information. */

#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <utmp.h>
#include <xkbcommon/xkbcommon.h>

#include "colors.h"
#include "gui.h"
#include "settings.h"
#include "timing.h"
#include "util.h"
#include "vector.h"

typedef uint32_t Rune;

typedef struct
{
    int32_t data[4];
} VtLineProxy;

extern void (*Vt_destroy_line_proxy)(int32_t proxy[static 4]);

enum CursorType
{
    CURSOR_BLOCK = 0,
    CURSOR_BEAM,
    CURSOR_UNDERLINE,
};

enum MouseButton
{
    MOUSE_BTN_LEFT       = 1,
    MOUSE_BTN_RIGHT      = 3,
    MOUSE_BTN_MIDDLE     = 2,
    MOUSE_BTN_WHEEL_UP   = 65,
    MOUSE_BTN_WHEEL_DOWN = 66,
};

typedef struct
{
    enum CursorType type : 2;
    uint8_t         blinking : 1;
    uint8_t         hidden : 1;

} Cursor;

enum __attribute__((packed)) VtRuneStyle
{
    VT_RUNE_NORMAL = 0,
    VT_RUNE_BOLD,
    VT_RUNE_ITALIC,
};

typedef struct __attribute__((packed))
{
    Rune code;

    ColorRGB  fg;
    ColorRGB  line;
    ColorRGBA bg;

    enum VtRuneStyle state : 3;
    uint8_t          linecolornotdefault : 1;
    uint8_t          dim : 1;
    uint8_t          hidden : 1;
    uint8_t          blinkng : 1;
    uint8_t          underlined : 1;
    uint8_t          strikethrough : 1;
    uint8_t          doubleunderline : 1;
    uint8_t          curlyunderline : 1;
    uint8_t          overline : 1;

} VtRune;

DEF_VECTOR(VtRune, NULL)

DEF_VECTOR(char, NULL)

DEF_VECTOR(size_t, NULL)

DEF_VECTOR(Vector_VtRune, Vector_destroy_VtRune)

DEF_VECTOR(Vector_char, Vector_destroy_char)

typedef struct VtLine_
{

    /* Proxy resources need to be regenerated */
    bool damaged;

    /* Arbitrary data used by the renderer */
    VtLineProxy proxy;

    /* Can be split by resizing window */
    bool reflowable;

    /* Can be merged into previous line */
    bool rejoinable;

    bool was_reflown;

    Vector_VtRune data;

} VtLine;

static inline void VtLine_destroy(VtLine* self)
{
    if (Vt_destroy_line_proxy)
        Vt_destroy_line_proxy(self->proxy.data);

    Vector_destroy_VtRune(&self->data);
}

static inline VtLine VtLine_new()
{
    return (VtLine){ .damaged    = true,
                     .reflowable = true,
                     .rejoinable = false,
                     .data       = Vector_new_VtRune(),
                     .proxy      = { { 0 } } };
}

DEF_VECTOR(VtLine, VtLine_destroy)

typedef struct _Vt
{
    struct VtCallbacks
    {
        void* user_data;
        Pair_uint32_t (*on_window_size_requested)(void*);
        Pair_uint32_t (*on_window_size_from_cells_requested)(void*,
                                                             uint32_t r,
                                                             uint32_t c);
        Pair_uint32_t (*on_number_of_cells_requested)(void*);

        void (*on_window_resize_requested)(void*, uint32_t w, uint32_t h);

        Pair_uint32_t (*on_window_position_requested)(void*);

        void (*on_action_performed)(void*);
        void (*on_repaint_required)(void*);
        void (*on_bell_flash)(void*);

        void (*on_title_changed)(void*, const char*);

        void (*on_clipboard_requested)(void*);
        void (*on_clipboard_sent)(void*, const char*);

    } callbacks;

    /* Related to window interaction and gui */

    /* Controlled program has quit */
    bool is_done;

    size_t last_click_x;
    size_t last_click_y;
    double pixels_per_cell_x, pixels_per_cell_y;

    bool   scrolling;
    size_t visual_scroll_top;

    struct UnicodeInput
    {
        bool        active;
        Vector_char buffer;

    } unicode_input;

    struct Scrollbar
    {
        bool visible, dragging;

        enum AutoscrollDir
        {
            AUTOSCROLL_NONE = 0,
            AUTOSCROLL_UP   = 1,
            AUTOSCROLL_DN   = -1,

        } autoscroll;

        uint8_t   width;
        float     top;
        float     length;
        float     drag_position;
        TimePoint hide_time;
        TimePoint autoscroll_next_step;

    } scrollbar;

    struct Selection
    {

        enum SelectMode
        {
            SELECT_MODE_NONE = 0,
            SELECT_MODE_NORMAL,
            SELECT_MODE_BOX,
        } mode, // active selection mode

          next_mode; // new selection mode decided by modkeys' state during
                     // initializing click

        bool dragging;
        bool drag_can_start;

        uint8_t   click_count;
        TimePoint next_click_limit;

        /* region start point recorded when text was clicked, but
         * no drag event was received yet */
        size_t click_begin_line;
        size_t click_begin_char_idx;

        /* selected region */
        size_t begin_line;
        size_t end_line;
        size_t begin_char_idx;
        size_t end_char_idx;

    } selection;

    /* Related to terminal */
    struct winsize ws;
    struct termios tios;

    int master, slave, io;

#ifdef DEBUG
    char dev_name[64];
#endif

    fd_set wfdset, rfdset;

    pid_t pid;

    struct Parser
    {
        enum VtParserState
        {
            PARSER_STATE_LITERAL     = '\0',
            PARSER_STATE_ESCAPED     = '\e',
            PARSER_STATE_CONTROL_SEQ = '[',
            PARSER_STATE_DCS         = 'P',
            PARSER_STATE_OS_COM      = ']',
            PARSER_STATE_CHARSET_G0  = '(',
            PARSER_STATE_CHARSET_G1  = ')',
            PARSER_STATE_CHARSET_G2  = '*',
            PARSER_STATE_CHARSET_G3  = '+',
        } state;

        bool     utf8_in_seq;
        uint32_t utf8_cur_seq_len;
        uint32_t utf8_bur_buf_off;
        char     utf8_buf[4];

        VtRune char_state; // records character properties
        bool   color_inverted;

        Vector_char active_sequence;
    } parser;

    char          out_buf[128];
    char          buf[1024];
    char*         title;
    Vector_size_t title_stack;

    Rune (*charset_g0)(char);
    Rune (*charset_g1)(char);
    Rune (*charset_g2)(char);
    Rune (*charset_g3)(char);

    uint8_t tabstop;

    Vector_VtLine lines, alt_lines;

    Cursor cursor;

    size_t cursor_pos;
    size_t alt_cursor_pos;
    size_t saved_cursor_pos;

    size_t active_line;
    size_t alt_active_line;
    size_t saved_active_line;

    size_t scroll_region_top;
    size_t scroll_region_bottom;

    /* 'reverse' some modes so default is 0  */
    struct VtModes
    {
        uint8_t bracket_paste : 1;
        uint8_t del_sends_del : 1;
        uint8_t no_alt_sends_esc : 1;
        uint8_t x10_mouse_compat : 1;
        uint8_t mouse_btn_report : 1;
        uint8_t mouse_motion_on_btn_report : 1;
        uint8_t mouse_motion_report : 1;
        uint8_t window_focus_events_report : 1;
        uint8_t extended_report : 1;
        uint8_t video_reverse : 1;
        uint8_t no_auto_wrap : 1;
        uint8_t auto_repeat : 1;
        uint8_t application_keypad : 1;
    } modes;

} Vt;

Vt Vt_new(uint32_t cols, uint32_t rows);

/** set gui server connection fd to monitor for activity */
static inline void Vt_watch_fd(Vt* const self, const int fd)
{
    self->io = fd;
}

static inline char* Vt_buffer(Vt* const self)
{
    return self->out_buf;
}

/**
 * @return 1 - requires retry */
bool Vt_wait(Vt* self);

/**
 * @return 1 - requires retry */
bool Vt_read(Vt* self);

static void Vt_write(Vt* self);

void Vt_show_lines(
  Vt* self,
  void (*for_line)(const Vt* const, VtLine*, size_t, uint32_t, int32_t));

void Vt_kill_program(Vt* self);

void vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end);

void Vt_resize(Vt* self, uint32_t x, uint32_t y);

static void Vt_destroy(Vt* self)
{
    Vt_kill_program(self);

    Vector_destroy_VtLine(&self->lines);

    if (self->alt_lines.buf)
        Vector_destroy_VtLine(&self->alt_lines);

    Vector_destroy_char(&self->parser.active_sequence);

    for (size_t* i = NULL; Vector_iter_size_t(&self->title_stack, i);)
        free((char*)*i);

    Vector_destroy_size_t(&self->title_stack);
}

void Vt_handle_key(void* self, uint32_t key, uint32_t mods);

void Vt_handle_clipboard(void* self, const char* text);

void Vt_handle_button(void*    self,
                      uint32_t button,
                      bool     state,
                      int32_t  x,
                      int32_t  y,
                      int32_t  ammount,
                      uint32_t mods);

void Vt_handle_motion(void* self, uint32_t button, int32_t x, int32_t y);

static inline size_t Vt_top_line(const Vt* const self)
{
    return self->lines.size <= self->ws.ws_row
             ? 0
             : self->lines.size - self->ws.ws_row;
}

static inline size_t Vt_visual_top_line(const Vt* const self)
{
    return self->scrolling ? self->visual_scroll_top : Vt_top_line(self);
}

static inline size_t Vt_visual_bottom_line(const Vt* const self)
{
    return self->ws.ws_row + Vt_visual_top_line(self) +
           (self->scrolling ? 1 : 0);
}

void Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end);

__attribute__((always_inline, hot)) static inline bool
Vt_selection_should_highlight_char(const Vt* const self, size_t x, size_t y)
{
    switch (expect(self->selection.mode, SELECT_MODE_NONE)) {
        case SELECT_MODE_NONE:
            return false;

        case SELECT_MODE_BOX:
            return !(
              Vt_visual_top_line(self) + y <
                MIN(self->selection.end_line, self->selection.begin_line) ||
              (Vt_visual_top_line(self) + y >
               MAX(self->selection.end_line, self->selection.begin_line)) ||
              (MAX(self->selection.begin_char_idx,
                   self->selection.end_char_idx) < x) ||
              (MIN(self->selection.begin_char_idx,
                   self->selection.end_char_idx) > x));

        case SELECT_MODE_NORMAL:
            if (Vt_visual_top_line(self) + y >
                  MIN(self->selection.begin_line, self->selection.end_line) &&
                Vt_visual_top_line(self) + y <
                  MAX(self->selection.begin_line, self->selection.end_line)) {
                return true;
            } else {
                if (self->selection.begin_line == self->selection.end_line) {
                    return (self->selection.begin_line ==
                            Vt_visual_top_line(self) + y) &&
                           (x >= MIN(self->selection.begin_char_idx,
                                     self->selection.end_char_idx) &&
                            x <= MAX(self->selection.begin_char_idx,
                                     self->selection.end_char_idx));
                } else if (Vt_visual_top_line(self) + y ==
                           self->selection.begin_line) {
                    return self->selection.begin_line < self->selection.end_line
                             ? x >= self->selection.begin_char_idx
                             : x <= self->selection.begin_char_idx;

                } else if (Vt_visual_top_line(self) + y ==
                           self->selection.end_line) {
                    return self->selection.begin_line > self->selection.end_line
                             ? x >= self->selection.end_char_idx
                             : x <= self->selection.end_char_idx;
                }
            }
            return false;
    }
    return false;
}
