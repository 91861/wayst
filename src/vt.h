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

#ifndef VT_RUNE_MAX_COMBINE
#define VT_RUNE_MAX_COMBINE 2
#endif

typedef struct
{
    char* s;
} DynStr;

static void DynStr_destroy(DynStr* self)
{
    free(self->s);
    self->s = NULL;
}

DEF_VECTOR(DynStr, DynStr_destroy)

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

    uint8_t  blinking : 1;
    uint8_t  hidden : 1;
    size_t   row;
    uint16_t col;
} Cursor;

#define VT_RUNE_CODE_WIDE_TAIL 27

typedef struct
{
    char32_t code;
    char32_t combine[VT_RUNE_MAX_COMBINE];
    enum VtRuneStyle
    {
        VT_RUNE_NORMAL = 0,
        VT_RUNE_BOLD,
        VT_RUNE_ITALIC,
        VT_RUNE_BOLD_ITALIC,
        TV_RUNE_UNSTYLED,
    } style : 3;
} Rune;

#define VT_RUNE_PALETTE_INDEX_TERM_DEFAULT (-1)

/**
 * Represents a single character */
typedef struct
{
    Rune rune;

    union vt_rune_rgb_color_variant_t
    {
        ColorRGB rgb;
        int16_t  index;
    } ln_clr_data, fg_data;

    union vt_rune_rgba_color_variant_t
    {
        ColorRGBA rgba;
        int16_t   index;
    } bg_data;

    bool bg_is_palette_entry : 1;
    bool fg_is_palette_entry : 1;
    bool ln_clr_is_palette_entry : 1;
    bool line_color_not_default : 1;
    bool invert : 1;
    bool dim : 1;
    bool hidden : 1;
    bool blinkng : 1;
    bool underlined : 1;
    bool strikethrough : 1;
    bool doubleunderline : 1;
    bool curlyunderline : 1;
    bool overline : 1;

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
    uint32_t data[6];
} VtLineProxy;

typedef struct
{
    /* Characters */
    Vector_VtRune data;

    /* Arbitrary data used by the renderer */
    VtLineProxy proxy;

    /* Clickable link ranges */
    // Vector_VtUriRange uris;

    struct VtLineDamage
    {
        /* Range of cells that should be repainted if type == RANGE or
         * not repainted if type == SHIFT */
        uint32_t front, end;

        /* Number of cells the existing contents should be moved right */
        int8_t shift;

        enum __attribute__((packed)) VtLineDamageType
        {
            /* Proxy objects are up to date */
            VT_LINE_DAMAGE_NONE = 0,

            /* The entire line needs to be refreshed */
            VT_LINE_DAMAGE_FULL,

            /* Line contents were shifted 'shift' number of cells. Cells before 'front' and after
               'end' may have changed */
            VT_LINE_DAMAGE_SHIFT,

            /* The characters between 'front' and 'end' need to be refreshed */
            VT_LINE_DAMAGE_RANGE,
        } type;
    } damage;

    /* Can be split by resizing window */
    bool reflowable : 1;

    /* Can be merged into previous line */
    bool rejoinable : 1;

    /* Part of this line was moved to the next one */
    bool was_reflown : 1;
} VtLine;

static void VtLine_copy(VtLine* dest, VtLine* source)
{
    memcpy(dest, source, sizeof(VtLine));
    dest->was_reflown = false;
    dest->reflowable  = false;
    dest->rejoinable  = false;
    dest->damage.type = VT_LINE_DAMAGE_FULL;
    memset(&dest->proxy, 0, sizeof(VtLineProxy));
    dest->data = Vector_new_with_capacity_VtRune(source->data.size);
    Vector_pushv_VtRune(&dest->data, source->data.buf, source->data.size);
}

static inline void VtLine_destroy(void* vt_, VtLine* self);

DEF_VECTOR_DA(VtLine, VtLine_destroy, void)

typedef struct _Vt
{
    struct vt_callbacks_t
    {
        void* user_data;

        Pair_uint32_t (*on_window_size_requested)(void*);
        Pair_uint32_t (*on_text_area_size_requested)(void*);
        Pair_uint32_t (*on_window_size_from_cells_requested)(void*, uint32_t r, uint32_t c);
        Pair_uint32_t (*on_number_of_cells_requested)(void*);
        void (*on_window_resize_requested)(void*, uint32_t w, uint32_t h);
        Pair_uint32_t (*on_window_position_requested)(void*);
        bool (*on_minimized_state_requested)(void*);
        bool (*on_fullscreen_state_requested)(void*);
        void (*on_action_performed)(void*);
        void (*on_repaint_required)(void*);
        void (*on_visual_bell)(void*);
        void (*on_desktop_notification_sent)(void*, const char* opt_title, const char* text);
        void (*on_window_maximize_state_set)(void*, bool);
        void (*on_window_fullscreen_state_set)(void*, bool);
        void (*on_window_dimensions_set)(void*, int32_t, int32_t);
        void (*on_text_area_dimensions_set)(void*, int32_t, int32_t);
        void (*on_title_changed)(void*, const char*);
        void (*on_clipboard_requested)(void*);
        void (*on_font_reload_requseted)(void*);
        void (*on_clipboard_sent)(void*, const char*);
        void (*destroy_proxy)(void*, VtLineProxy*);
    } callbacks;

    uint32_t last_click_x;
    uint32_t last_click_y;
    double   pixels_per_cell_x, pixels_per_cell_y;

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

    bool wrap_next;

    struct Parser
    {
        enum VtParserState
        {
            PARSER_STATE_LITERAL = 0,
            PARSER_STATE_ESCAPED,
            PARSER_STATE_ESCAPED_CSI,
            PARSER_STATE_CSI,
            PARSER_STATE_DCS,
            PARSER_STATE_APC,
            PARSER_STATE_OSC,
            PARSER_STATE_PM,
            PARSER_STATE_CHARSET,
            PARSER_STATE_CHARSET_G0,
            PARSER_STATE_CHARSET_G1,
            PARSER_STATE_CHARSET_G2,
            PARSER_STATE_CHARSET_G3,
            PARSER_STATE_TITLE,
            PARSER_STATE_DEC_SPECIAL,
        } state;

        bool      in_mb_seq;
        mbstate_t input_mbstate;

        VtRune char_state; // records currently selected character properties

        Vector_char active_sequence;
    } parser;

    char*         title;
    char*         work_dir;
    Vector_DynStr title_stack;

    struct terminal_colors_t
    {
        ColorRGBA bg;
        ColorRGB  fg;

        // TODO:
        /* struct terminal_cursor_colors_t */
        /* { */
        /*     bool enabled; */
        /*     ColorRGBA bg; */
        /*     ColorRGB  fg; */
        /* } cursor; */

        struct terminal_highlight_colors_t
        {
            ColorRGBA bg;
            ColorRGB  fg;
        } highlight;

        ColorRGB palette_256[256];
    } colors;

    /**
     * Character set is composed of C0 (7-bit control characters), C1 (8-bit control characters), GL
     * - graphics left (7-bit graphic characters), GR - graphics right (8-bit graphic characters).
     *
     * The program can use SCS sequences to designate graphic sets G0-G3. This allows mapping them
     * to GL and GR with `locking shifts` - LS sequences or `single shifts` - SS sequences.
     * Locking shifts stay active until modified by another LS sequence or RIS. Single shifts only
     * affect the following character.
     * By default G0 is designated as GL and G1 as GR.
     *
     * GR has no effect in UTF-8 mode. C1 is used only if S8C1T is enabled. In UTF-8 mode C1 can be
     * accesed only with escape sequences.
     *
     * Historically this was used to input language specific characters or symbols (without
     * multi-byte sequences). Even though there is no need for this when using UTF-8, some modern
     * programs will enable the `DEC Special` set to optimize drawing line/box-drawing characters.
     */
    char32_t (*charset_g0)(char);
    char32_t (*charset_g1)(char);
    char32_t (*charset_g2)(char); // not available on VT100
    char32_t (*charset_g3)(char); // not available on VT100
    char32_t (**charset_gl)(char);
    char32_t (**charset_gr)(char);           // VT200 only
    char32_t (**charset_single_shift)(char); // not available on VT100

    uint8_t tabstop;
    bool*   tab_ruler;

    Vector_VtLine lines, alt_lines;

    VtRune*  last_interted;
    char32_t last_codepoint;
#ifndef NOUTF8PROC
    int32_t utf8proc_state;
#endif

    VtRune blank_space;

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
        uint8_t no_wraparound : 1;
        uint8_t reverse_wraparound : 1;
        uint8_t origin : 1;
        uint8_t allow_column_size_switching : 1;
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
        uint8_t auto_repeat : 1;
        uint8_t application_keypad : 1;
        uint8_t application_keypad_cursor : 1;
        uint8_t pop_on_bell : 1;
        uint8_t urgency_on_bell : 1;
    } modes;

#define VT_XT_MODIFY_KEYBOARD_DFT 0
    int8_t xterm_modify_keyboard;

#define VT_XT_MODIFY_CURSOR_KEYS_DFT 2
    int8_t xterm_modify_cursor_keys;

#define VT_XT_MODIFY_FUNCTION_KEYS_DFT 2
    int8_t xterm_modify_function_keys;

#define VT_XT_MODIFY_OTHER_KEYS_DFT 0
    int8_t xterm_modify_other_keys;

} Vt;

static ColorRGB Vt_rune_fg_no_invert(const Vt* self, const VtRune* rune);

static ColorRGBA Vt_rune_bg_no_invert(const Vt* self, const VtRune* rune)
{
    if (!rune->bg_is_palette_entry) {
        return rune->bg_data.rgba;
    } else if (rune->bg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT) {
        return self->colors.bg;
    } else {
        return ColorRGBA_from_RGB(self->colors.palette_256[rune->bg_data.index]);
    }
}

static ColorRGBA Vt_rune_bg(const Vt* self, const VtRune* rune)
{
    if (rune->invert) {
        return ColorRGBA_from_RGB(Vt_rune_fg_no_invert(self, rune));
    } else {
        return Vt_rune_bg_no_invert(self, rune);
    }
}

static ColorRGB Vt_rune_fg_no_invert(const Vt* self, const VtRune* rune)
{
    if (!rune->fg_is_palette_entry) {
        return rune->fg_data.rgb;
    } else if (rune->fg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT) {
        return self->colors.fg;
    } else {
        return self->colors.palette_256[rune->fg_data.index];
    }
}

static ColorRGB Vt_rune_fg(const Vt* self, const VtRune* rune)
{
    if (rune->invert) {
        return ColorRGB_from_RGBA(Vt_rune_bg_no_invert(self, rune));
    } else {
        return Vt_rune_fg_no_invert(self, rune);
    }
}

static ColorRGB Vt_rune_ln_clr(const Vt* self, const VtRune* rune)
{
    if (rune->line_color_not_default) {
        if (rune->ln_clr_is_palette_entry) {
            return self->colors.palette_256[rune->ln_clr_data.index];
        } else {
            return rune->ln_clr_data.rgb;
        }
    } else {
        return Vt_rune_fg(self, rune);
    }
}

static inline void VtLine_destroy(void* vt_, VtLine* self)
{
    Vt* vt = vt_;
    CALL_FP(vt->callbacks.destroy_proxy, vt->callbacks.user_data, &self->proxy);
    Vector_destroy_VtRune(&self->data);
}

/**
 * Get index of the last line */
static inline size_t Vt_max_line(const Vt* const self)
{
    return self->lines.size - 1;
}

/**
 * Get number of terminal columns */
static inline uint16_t Vt_col(const Vt* const self)
{
    return self->ws.ws_col;
}

/**
 * Get number of terminal rows */
static inline uint16_t Vt_row(const Vt* const self)
{
    return self->ws.ws_row;
}

/**
 * Get line at global index if it exists */
static inline VtLine* Vt_line_at(Vt* self, size_t row)
{
    if (row > Vt_max_line(self)) {
        return NULL;
    }
    return &self->lines.buf[row];
}

/**
 * Get cell at global position if it exists */
static inline VtRune* Vt_at(Vt* self, size_t column, size_t row)
{
    VtLine* line = Vt_line_at(self, row);
    if (!line || column >= line->data.size) {
        return NULL;
    }
    return &line->data.buf[column];
}

/**
 * Get line under terminal cursor */
static inline VtLine* Vt_cursor_line(Vt* self)
{
    return &self->lines.buf[self->cursor.row];
}

/**
 * Get cell under terminal cursor */
static inline VtRune* Vt_cursor_cell(Vt* self)
{
    VtLine* cursor_line = Vt_cursor_line(self);
    if (self->cursor.col >= cursor_line->data.size) {
        return NULL;
    }
    return &cursor_line->data.buf[self->cursor.col];
}

/**
 * Make a new interpreter with a given size */
void Vt_init(Vt* self, uint32_t cols, uint32_t rows);

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

/**
 * Get line index at the top of the real viewport */
static inline size_t Vt_top_line(const Vt* const self)
{
    return self->lines.size <= self->ws.ws_row ? 0 : self->lines.size - self->ws.ws_row;
}

/**
 * Get line index at the bottom of the real viewport */
static inline size_t Vt_bottom_line(const Vt* self)
{
    return Vt_top_line(self) + Vt_row(self) - 1;
}

/**
 * Terminal is displaying the scrollback buffer */
static inline bool Vt_is_scrolling_visual(const Vt* self)
{
    return self->scrolling_visual;
}

/**
 * Get line index at the top of the viewport (takes visual scroling into account) */
static inline size_t Vt_visual_top_line(const Vt* const self)
{
    return self->scrolling_visual ? self->visual_scroll_top : Vt_top_line(self);
}

/**
 * Get line index at the bottom of the viewport (takes visual scroling into account) */
static inline size_t Vt_visual_bottom_line(const Vt* const self)
{
    return self->ws.ws_row + Vt_visual_top_line(self) - 1;
}

/**
 * Move the first visible line of the visual viewport to global line index (out of range values are
 * clamped) starts/stops scrolling */
void Vt_visual_scroll_to(Vt* self, size_t line);

/**
 * Move visual viewport one line up and start visual scrolling
 * @return can scroll more */
bool Vt_visual_scroll_up(Vt* self);

/**
 * Move visual viewport one page up and start visual scrolling */
static inline void Vt_visual_scroll_page_up(Vt* self)
{
    size_t tgt_pos =
      (Vt_visual_top_line(self) > Vt_row(self)) ? Vt_visual_top_line(self) - Vt_row(self) : 0;
    Vt_visual_scroll_to(self, tgt_pos);
}

/**
 * Move visual viewport one line down and stop scrolling if lowest position
 * @return can scroll more  */
bool Vt_visual_scroll_down(Vt* self);

/**
 * Move visual viewport one page down and stop scrolling if lowest position */
static inline void Vt_visual_scroll_page_down(Vt* self)
{
    Vt_visual_scroll_to(self, self->visual_scroll_top + Vt_row(self));
}

/**
 * Reset the visual viewport and stop scrolling if lowest position */
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
 * Terminal listens for scroll wheel button presses */
static bool Vt_reports_mouse(Vt* self)
{
    return self->modes.extended_report || self->modes.mouse_motion_on_btn_report ||
           self->modes.mouse_btn_report;
}

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
