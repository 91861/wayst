/* See LICENSE for license information. */

#pragma once

#ifdef __linux
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#include <termios.h>
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#include <termios.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uchar.h>
#include <unistd.h>

#include "colors.h"
#include "monitor.h"
#include "settings.h"
#include "timing.h"
#include "util.h"
#include "vector.h"

enum MouseButton
{
    MOUSE_BTN_LEFT       = 1,
    MOUSE_BTN_RIGHT      = 3,
    MOUSE_BTN_MIDDLE     = 2,
    MOUSE_BTN_WHEEL_UP   = 65,
    MOUSE_BTN_WHEEL_DOWN = 66,
};

typedef struct Cursor
{
    enum CursorType
    {
        CURSOR_BLOCK = 0,
        CURSOR_BEAM,
        CURSOR_UNDERLINE,
    } type : 2;

    uint8_t blinking : 1;
    uint8_t hidden : 1;
    size_t  col, row;
} Cursor;

/**
 * Represents a single character */
typedef struct __attribute__((packed))
{
    char32_t code;

    ColorRGB  fg;
    ColorRGB  line;
    ColorRGBA bg;

    enum __attribute__((packed)) VtRuneStyle
    {
        VT_RUNE_NORMAL = 0,
        VT_RUNE_BOLD,
        VT_RUNE_ITALIC,
        VT_RUNE_BOLD_ITALIC,
    } state : 3;

    uint8_t linecolornotdefault : 1;
    uint8_t dim : 1;
    uint8_t hidden : 1;
    uint8_t blinkng : 1;
    uint8_t underlined : 1;
    uint8_t strikethrough : 1;
    uint8_t doubleunderline : 1;
    uint8_t curlyunderline : 1;
    uint8_t overline : 1;

} VtRune;

DEF_VECTOR(VtRune, NULL)

DEF_VECTOR(char, NULL)

DEF_VECTOR(size_t, NULL)

DEF_VECTOR(Vector_VtRune, Vector_destroy_VtRune)

DEF_VECTOR(Vector_char, Vector_destroy_char)

/**
 * represents a clickable range of text linked to a URL */
typedef struct
{
    size_t begin, end;
    char*  uri_string;
} VtUriRange;

static void VtUriRange_destroy(VtUriRange* self)
{
    free(self->uri_string);
}

DEF_VECTOR(VtUriRange, VtUriRange_destroy)

typedef struct
{
    int32_t data[4];
} VtLineProxy;

extern void (*Vt_destroy_line_proxy)(int32_t proxy[static 4]);

typedef struct
{
    /* Characters */
    Vector_VtRune data;

    /* Arbitrary data used by the renderer */
    VtLineProxy proxy;

    /* Clickable link ranges */
    Vector_VtUriRange uris;

    /* Can be split by resizing window */
    bool reflowable : 1;

    /* Can be merged into previous line */
    bool rejoinable : 1;

    /* Part of this line was moved to the next one */
    bool was_reflown : 1;

    /* Proxy resources need to be regenerated */
    bool damaged : 1;

    /*
     * TODO: something like this
     * struct Damage {
     *     enum Type { NONE, FULL, PARTIAL } type;
     *     int8_t shift, region_front, region_end;
     * };
     */

} VtLine;

/* TODO: Make a version of Vector that can bind an additional destructor argument, so
 * Vt_destroy_line_proxy doesn't have to be a global */
static inline void VtLine_destroy(VtLine* self)
{
    if (Vt_destroy_line_proxy)
        Vt_destroy_line_proxy(self->proxy.data);

    Vector_destroy_VtRune(&self->data);
}

DEF_VECTOR(VtLine, VtLine_destroy)

typedef struct _Vt
{
    struct VtCallbacks
    {
        void* user_data;

        Pair_uint32_t (*on_window_size_requested)(void*);
        Pair_uint32_t (*on_window_size_from_cells_requested)(void*, uint32_t r, uint32_t c);
        Pair_uint32_t (*on_number_of_cells_requested)(void*);
        void (*on_window_resize_requested)(void*, uint32_t w, uint32_t h);
        Pair_uint32_t (*on_window_position_requested)(void*);
        void (*on_action_performed)(void*);
        void (*on_repaint_required)(void*);
        void (*on_bell_flash)(void*);

        void (*on_title_changed)(void*, const char*);

        void (*on_clipboard_requested)(void*);
        void (*on_font_reload_requseted)(void*);
        void (*on_clipboard_sent)(void*, const char*);

    } callbacks;

    size_t last_click_x;
    size_t last_click_y;
    double pixels_per_cell_x, pixels_per_cell_y;

    bool   scrolling_visual;
    size_t visual_scroll_top;

    struct UnicodeInput
    {
        bool        active;
        Vector_char buffer;

    } unicode_input;

    struct Selection
    {
        enum SelectMode
        {
            SELECT_MODE_NONE = 0,
            SELECT_MODE_NORMAL,
            SELECT_MODE_BOX,
        } mode, /* active selection mode */

          /** new selection mode decided by modkeys' state during
           * initializing click */
          next_mode;

        /* region start point recorded when text was clicked, but
         * no drag event was received yet (at that point a previous selection
         * region may still be valid) */
        size_t click_begin_line;
        size_t click_begin_char_idx;

        /* selected region */
        size_t  begin_line;
        size_t  end_line;
        int32_t begin_char_idx;
        int32_t end_char_idx;

    } selection;

    /* Related to terminal */
    struct winsize ws;
    struct termios tios;
    int            master_fd;
    Vector_char    output;

    struct Parser
    {
        enum VtParserState
        {
            PARSER_STATE_LITERAL = 0,
            PARSER_STATE_ESCAPED,
            PARSER_STATE_CSI,
            PARSER_STATE_DCS,
            PARSER_STATE_APC,
            PARSER_STATE_OSC,
            PARSER_STATE_PM,
            PARSER_STATE_CHARSET_G0,
            PARSER_STATE_CHARSET_G1,
            PARSER_STATE_CHARSET_G2,
            PARSER_STATE_CHARSET_G3,
        } state;

        bool      in_mb_seq;
        mbstate_t input_mbstate;

        VtRune char_state; // records currently selected character properties
        bool   color_inverted;

        Vector_char active_sequence;
    } parser;

    char*         title;
    char*         work_dir;
    Vector_size_t title_stack;

    char32_t (*charset_g0)(char);
    char32_t (*charset_g1)(char);
    char32_t (*charset_g2)(char);
    char32_t (*charset_g3)(char);

    uint8_t tabstop;

    Vector_VtLine lines, alt_lines;

    Cursor cursor;

    size_t alt_cursor_pos;
    size_t saved_cursor_pos;
    size_t alt_active_line;
    size_t saved_active_line;
    size_t scroll_region_top;
    size_t scroll_region_bottom;

    /* 'reverse' some modes so default is 0  */
    struct VtModes
    {
        uint8_t bracketed_paste : 1;
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

/**
 * Make a new interpreter with a given size */
Vt Vt_new(uint32_t cols, uint32_t rows);

/**
 * Interpret a range od bytes */
void Vt_interpret(Vt* self, char* buf, size_t bytes);

/**
 * Ger response message */
void Vt_get_output(Vt* self, char** out_buf, size_t* out_bytes);

/**
 * Get lines that should be visible */
void vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end);

/**
 * Change terminal size */
void Vt_resize(Vt* self, uint32_t x, uint32_t y);

/**
 * Destroy all renderer line 'proxy' objects */
void Vt_clear_all_proxies(Vt* self);

/**
 * Print state info to stdout */
void Vt_dump_info(Vt* self);

/**
 * Enable unicode input prompt */
void Vt_start_unicode_input(Vt* self);

/**
 * Respond to keypress event */
void Vt_handle_key(void* self, uint32_t key, uint32_t rawkey, uint32_t mods);

/**
 * Respond to clipboard paste */
void Vt_handle_clipboard(void* self, const char* text);

/**
 * Respond to mouse button event
 * @param button  - X11 button code
 * @param state   - press/release
 * @param ammount - for non-discrete scroll
 * @param mods    - modifier keys depressed */
void Vt_handle_button(void*    self,
                      uint32_t button,
                      bool     state,
                      int32_t  x,
                      int32_t  y,
                      int32_t  ammount,
                      uint32_t mods);

/**
 * Respond to pointer motion event
 * @param button - button being held down */
void Vt_handle_motion(void* self, uint32_t button, int32_t x, int32_t y);

static inline size_t Vt_top_line(const Vt* const self)
{
    return self->lines.size <= self->ws.ws_row ? 0 : self->lines.size - self->ws.ws_row;
}

static inline size_t Vt_visual_top_line(const Vt* const self)
{
    return self->scrolling_visual ? self->visual_scroll_top : Vt_top_line(self);
}

static inline size_t Vt_visual_bottom_line(const Vt* const self)
{
    return self->ws.ws_row + Vt_visual_top_line(self) - 1;
}

void Vt_visual_scroll_to(Vt* self, size_t line);
void Vt_visual_scroll_up(Vt* self);
void Vt_visual_scroll_down(Vt* self);
void Vt_visual_scroll_reset(Vt* self);

/**
 * Get a range of lines that should be visible */
void Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end);

/**
 * Initialize selection region to word by pixel in screen coordinates */
void Vt_select_init_word(Vt* self, int32_t x, int32_t y);

/**
 * Initialize selection region to line by pixel in screen coordinates */
void Vt_select_init_line(Vt* self, int32_t y);

/**
 * Initialize selection region to character by pixel in screen coordinates */
void Vt_select_init(Vt* self, enum SelectMode mode, int32_t x, int32_t y);

/**
 * Initialize selection region to character by cell in screen coordinates */
void Vt_select_init_cell(Vt* self, enum SelectMode mode, int32_t x, int32_t y);

/**
 * Replace existing selection with the initialized selection */
void Vt_select_commit(Vt* self);

/**
 * Get selected text as utf8 string */
Vector_char Vt_select_region_to_string(Vt* self);

/**
 * Set selection begin point to pixel in screen coordinates */
void Vt_select_set_front(Vt* self, int32_t x, int32_t y);

/**
 * Set selection begin point to cell in screen coordinates */
void Vt_select_set_front_cell(Vt* self, int32_t x, int32_t y);

/**
 * Set selection end point to pixel in screen coordinates */
void Vt_select_set_end(Vt* self, int32_t x, int32_t y);

/**
 * Set selection end point to cell in screen coordinates */
void Vt_select_set_end_cell(Vt* self, int32_t x, int32_t y);

/**
 * End selection */
void Vt_select_end(Vt* self);

/**
 * Destroy the interpreter */
void Vt_destroy(Vt* self);

/**
 * Should a cell (in screen coordinates) be visually highlighted as selected */
__attribute__((always_inline, hot)) static inline bool Vt_is_cell_selected(const Vt* const self,
                                                                           int32_t         x,
                                                                           int32_t         y)
{
    switch (expect(self->selection.mode, SELECT_MODE_NONE)) {
        case SELECT_MODE_NONE:
            return false;

        case SELECT_MODE_BOX:
            return !(Vt_visual_top_line(self) + y <
                       MIN(self->selection.end_line, self->selection.begin_line) ||
                     (Vt_visual_top_line(self) + y >
                      MAX(self->selection.end_line, self->selection.begin_line)) ||
                     (MAX(self->selection.begin_char_idx, self->selection.end_char_idx) < x) ||
                     (MIN(self->selection.begin_char_idx, self->selection.end_char_idx) > x));

        case SELECT_MODE_NORMAL:
            if (Vt_visual_top_line(self) + y >
                  MIN(self->selection.begin_line, self->selection.end_line) &&
                Vt_visual_top_line(self) + y <
                  MAX(self->selection.begin_line, self->selection.end_line)) {
                return true;
            } else {
                if (self->selection.begin_line == self->selection.end_line) {
                    return (self->selection.begin_line == Vt_visual_top_line(self) + y) &&
                           (x >=
                              MIN(self->selection.begin_char_idx, self->selection.end_char_idx) &&
                            x <= MAX(self->selection.begin_char_idx, self->selection.end_char_idx));
                } else if (Vt_visual_top_line(self) + y == self->selection.begin_line) {
                    return self->selection.begin_line < self->selection.end_line
                             ? x >= self->selection.begin_char_idx
                             : x <= self->selection.begin_char_idx;

                } else if (Vt_visual_top_line(self) + y == self->selection.end_line) {
                    return self->selection.begin_line > self->selection.end_line
                             ? x >= self->selection.end_char_idx
                             : x <= self->selection.end_char_idx;
                }
            }
            return false;
    }
    return false;
}
