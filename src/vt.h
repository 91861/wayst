/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#ifdef __linux
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__)
#include <termios.h>
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#include <termios.h>
#endif

#ifndef NOUTF8PROC
#include <utf8proc.h>
#define C_WIDTH(c) ((uint8_t)utf8proc_charwidth((char32_t)(c)))
#else
#include "wcwidth/wcwidth.h"
#define C_WIDTH(c) ((uint8_t)wcwidth((c)))
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
#include "map.h"
#include "monitor.h"
#include "rcptr.h"
#include "settings.h"
#include "timing.h"
#include "util.h"
#include "vector.h"

#define VT_DIM_FACTOR                     0.4f
#define VT_SYNCHRONIZED_UPDATE_TIMEOUT_MS 100

typedef struct
{
    int32_t  z_layer;
    uint32_t cell_width;
    uint32_t cell_height;
    uint32_t anchor_offset_x;
    uint32_t anchor_offset_y;
    uint32_t sample_offset_x;
    uint32_t sample_offset_y;
    uint32_t sample_width;
    uint32_t sample_height;
} vt_image_proto_display_args_t;

typedef enum
{
    VT_IMAGE_PROTO_ACTION_TRANSMIT,

    /* The terminal emulator will try to load the image and respond with either
       OK or an error, but it will not replace an existing image with the
       same id, nor will it store the image. */
    VT_IMAGE_PROTO_ACTION_QUERY,
    VT_IMAGE_PROTO_ACTION_DISPLAY,
    VT_IMAGE_PROTO_ACTION_TRANSMIT_AND_DISPLAY,
    VT_IMAGE_PROTO_ACTION_DELETE,
} vt_image_proto_action_t;

typedef enum
{
    VT_IMAGE_PROTO_TRANSMISSION_DIRECT,
    VT_IMAGE_PROTO_TRANSMISSION_FILE,
    VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE,
    VT_IMAGE_PROTO_TRANSMISSION_SHARED_MEM,
} vt_image_proto_transmission_t;

typedef enum
{
    VT_IMAGE_PROTO_COMPRESSION_NONE,
    VT_IMAGE_PROTO_COMPRESSION_ZLIB,
} vt_image_proto_compression_t;

typedef enum
{
    VT_IMAGE_SURFACE_INCOMPLETE,
    VT_IMAGE_SURFACE_READY,
    VT_IMAGE_SURFACE_FAIL,
    VT_IMAGE_SURFACE_DESTROYED,
} vt_image_surface_state_t;

typedef enum
{
    VT_GUI_POINTER_MODE_FORCE_HIDE,
    VT_GUI_POINTER_MODE_FORCE_SHOW,
    VT_GUI_POINTER_MODE_HIDE,
    VT_GUI_POINTER_MODE_SHOW,
    VT_GUI_POINTER_MODE_SHOW_IF_REPORTING,
} vt_gui_pointer_mode_t;

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

DEF_VECTOR(DynStr, DynStr_destroy);

enum MouseButton
{
    MOUSE_BTN_LEFT       = 1,
    MOUSE_BTN_RIGHT      = 3,
    MOUSE_BTN_MIDDLE     = 2,
    MOUSE_BTN_WHEEL_UP   = 65,
    MOUSE_BTN_WHEEL_DOWN = 66,
};

typedef struct VtCursor
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
} VtCursor;

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
        VT_RUNE_UNSTYLED,
    } style : 3;
} Rune;

/* Get total grapheme cluster width (in cells) */
static inline uint8_t Rune_width(Rune r)
{
    uint8_t base  = C_WIDTH(r.code);
    uint8_t extra = 0;

    for (int i = 0; i < VT_RUNE_MAX_COMBINE; ++i) {
        extra = MAX(extra, r.combine[i]);
    }

    return base + extra;
}

/* Get total maximum possible visual grapheme cluster width (in cells).
 (Ambiguous width characters (unicode private use block can be used for custom symbols like
 icons or logos and differs system to system) may actually use double width glyphs, but should
 only advance the cursor by a single cell) */
static inline uint8_t Rune_width_spill(Rune r)
{
    uint8_t base  = C_WIDTH(r.code);
    uint8_t extra = 0;

    for (int i = 0; i < VT_RUNE_MAX_COMBINE; ++i) {
        extra = MAX(extra, r.combine[i]);
    }

    return unicode_is_ambiguous_width(r.code) ? (MAX(2, base) + extra) : (base + extra);
}

static inline bool Rune_is_blank(Rune r)
{
    for (int i = 0; i < VT_RUNE_MAX_COMBINE; ++i) {
        if (r.combine[i])
            return false;
    }

    return r.code == ' ';
}

#define VT_RUNE_PALETTE_INDEX_TERM_DEFAULT (-1)

/**
 * Represents a single terminal cell */
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

    int16_t hyperlink_idx;

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

DEF_VECTOR(VtRune, NULL);

DEF_VECTOR(char, NULL);

DEF_VECTOR(size_t, NULL);

DEF_VECTOR(Vector_VtRune, Vector_destroy_VtRune);

DEF_VECTOR(Vector_char, Vector_destroy_char);

typedef struct
{
    ColorRGB palette[256];
    ColorRGB active_color;
} graphic_color_registers_t;

typedef enum
{
    VT_COMMAND_STATE_TYPING,
    VT_COMMAND_STATE_RUNNING,
    VT_COMMAND_STATE_COMPLETED,
} vt_command_state_t;

/* Shell integration command */
typedef struct
{
    char* command;

    /* command_end_row = output_rows.first -1 */
    size_t             command_start_row;
    Pair_size_t        output_rows;
    TimeSpan           execution_time;
    int                exit_status;
    uint16_t           command_start_column;
    vt_command_state_t state;
    bool               is_vte_protocol : 1;
} VtCommand;

static void VtCommand_destroy(VtCommand* self)
{
    free(self->command);
    self->command = NULL;
}

DEF_RC_PTR(VtCommand, VtCommand_destroy);
DEF_VECTOR(RcPtr_VtCommand, RcPtr_destroy_VtCommand);

typedef struct
{
    uint32_t data[6];
} VtImageSurfaceProxy;

DEF_VECTOR(uint8_t, NULL);

typedef struct
{
    vt_image_surface_state_t      state;
    bool                          png_data_transmission;
    bool                          display_on_transmission_completed;
    vt_image_proto_display_args_t display_args;
    uint32_t                      id; /* 0 if not specified by client */
    Vector_uint8_t                fragments;
    uint8_t                       bytes_per_pixel;
    uint32_t                      width, height;
    VtImageSurfaceProxy           proxy;
} VtImageSurface;

static inline void VtImageSurface_destroy(void* vt_, VtImageSurface* self);

DEF_RC_PTR_DA(VtImageSurface, VtImageSurface_destroy, void);

DEF_VECTOR(RcPtr_VtImageSurface, RcPtr_destroy_VtImageSurface);

typedef struct
{
    uint32_t data[4];
} VtImageSurfaceViewProxy;

typedef struct
{
    RcPtr_VtImageSurface source_image_surface;

    size_t        anchor_global_index;
    uint16_t      anchor_cell_idx;
    Pair_uint32_t anchor_offset_px;

    Pair_uint32_t sample_offset_px;
    Pair_uint32_t sample_dims_px;

    Pair_uint16_t cell_scale_rect;
    Pair_uint16_t cell_size;

    int32_t z_layer;

    VtImageSurfaceViewProxy proxy;
} VtImageSurfaceView;

typedef struct
{
    size_t              line;
    VtImageSurfaceView* view;
} vt_image_surface_view_delete_action_t;

DEF_VECTOR(vt_image_surface_view_delete_action_t, NULL);

static inline void VtImageSurfaceView_destroy(void* vt_, VtImageSurfaceView* self);

DEF_RC_PTR_DA(VtImageSurfaceView, VtImageSurfaceView_destroy, void);

DEF_VECTOR(RcPtr_VtImageSurfaceView, RcPtr_destroy_VtImageSurfaceView);

typedef struct
{
    uint32_t data[4];
} VtSixelSurfaceProxy;

typedef struct
{
    size_t   anchor_global_index;
    uint16_t anchor_cell_idx;

    uint32_t            width, height;
    VtSixelSurfaceProxy proxy;
    Vector_uint8_t      fragments;
} VtSixelSurface;

static void VtSixelSurface_destroy(void* _vt, VtSixelSurface* self);

DEF_VECTOR_DA(VtSixelSurface, VtSixelSurface_destroy, void);
DEF_RC_PTR_DA(VtSixelSurface, VtSixelSurface_destroy, void);
DEF_VECTOR(RcPtr_VtSixelSurface, RcPtr_destroy_VtSixelSurface);

/**
 * represents a clickable range of text linked to a URI */
typedef struct
{
    char* uri_string;
} VtUri;

static void VtUri_destroy(VtUri* self)
{
    free(self->uri_string);
    self->uri_string = NULL;
}

DEF_VECTOR(VtUri, VtUri_destroy);

typedef struct
{
    uint32_t data[4];
} VtLineProxy;

typedef struct
{
    Vector_RcPtr_VtImageSurfaceView* images;
    Vector_RcPtr_VtSixelSurface*     sixels;
} VtGraphicLineAttachments;

static VtGraphicLineAttachments VtGraphicLineAttachments_clone(VtGraphicLineAttachments* source)
{
    VtGraphicLineAttachments dest = { NULL, NULL };

    if (source->images) {
        dest.images  = _malloc(sizeof(Vector_RcPtr_VtImageSurfaceView));
        *dest.images = Vector_new_with_capacity_RcPtr_VtImageSurfaceView(source->images->size);
        for (RcPtr_VtImageSurfaceView* i = NULL;
             (i = Vector_iter_RcPtr_VtImageSurfaceView(source->images, i));) {
            Vector_push_RcPtr_VtImageSurfaceView(dest.images,
                                                 RcPtr_new_shared_VtImageSurfaceView(i));
        }
    }

    if (source->sixels) {
        dest.sixels  = _malloc(sizeof(Vector_RcPtr_VtSixelSurface));
        *dest.sixels = Vector_new_with_capacity_RcPtr_VtSixelSurface(source->sixels->size);
        for (RcPtr_VtSixelSurface* i = NULL;
             (i = Vector_iter_RcPtr_VtSixelSurface(dest.sixels, i));) {
            Vector_push_RcPtr_VtSixelSurface(dest.sixels, RcPtr_new_shared_VtSixelSurface(i));
        }
    }

    return dest;
}

static void VtGraphicLineAttachments_destroy(VtGraphicLineAttachments* self)
{
    if (self->images) {
        Vector_destroy_RcPtr_VtImageSurfaceView(self->images);
        free(self->images);
        self->images = NULL;
    }

    if (self->sixels) {
        Vector_destroy_RcPtr_VtSixelSurface(self->sixels);
        free(self->sixels);
        self->sixels = NULL;
    }
}

typedef enum
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

    /* The proxy object (and damage model) for this line was moved to its clone in the synchronized
     * update state. vt_line_damage_t.front contains the index into synchronized_update_state.lines
     * where the original proxy is now stored.
     *
     * A line with this danage model should never be presented for rendering by
     * Vt_get_visible_lines() */
    VT_LINE_DAMAGE_PROXIES_MOVED_TO_CLONE,
} vt_line_damage_type_e;

typedef struct
{
    /* Range of cells that should be repainted if type == RANGE or
     * not repainted if type == SHIFT */
    uint16_t front, end;

    /* Number of cells the existing contents should be moved right */
    int8_t shift;

    vt_line_damage_type_e type;
} vt_line_damage_t;

typedef struct
{
    /* Characters */
    Vector_VtRune data;

    /* Arbitrary data used by the renderer */
    VtLineProxy proxy;

    /* Clickable link adresses */
    Vector_VtUri* links;

    /* Images attached to this line */
    VtGraphicLineAttachments* graphic_attachments;

    /* Ref to command info if this is an output/invocation of a shell command */
    RcPtr_VtCommand linked_command;

    vt_line_damage_t damage;

    /* Can be split by resizing window */
    bool reflowable : 1;

    /* Can be merged into previous line */
    bool rejoinable : 1;

    /* Part of this line was moved to the next one */
    bool was_reflown : 1;

    /* Jump-to mark was explicitly set by the shell */
    bool mark_explicit : 1;

    /* This is line was used to invoke a command, contains the prompt/command body or both */
    bool mark_command_invoke : 1;

    /* This line starts a command output block */
    bool mark_command_output_start : 1;

    /* This line ends a command output block */
    bool mark_command_output_end : 1;
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

static VtLine VtLine_clone(VtLine* source)
{
    VtLine dest;
    memcpy(&dest, source, sizeof(VtLine));

    dest.data = Vector_new_with_capacity_VtRune(source->data.size);
    Vector_pushv_VtRune(&dest.data, source->data.buf, source->data.size);

    if (RcPtr_get_VtCommand(&source->linked_command)) {
        dest.linked_command = RcPtr_new_shared_VtCommand(&source->linked_command);
    }

    if (source->links) {
        dest.links  = _malloc(sizeof(Vector_VtUri));
        *dest.links = Vector_new_with_capacity_VtUri(source->links->size);
        Vector_pushv_VtUri(dest.links, source->links->buf, source->links->size);

        for (VtUri* i = NULL; (i = Vector_iter_VtUri(dest.links, i));) {
            i->uri_string = strdup(i->uri_string);
        }
    }

    if (source->graphic_attachments) {
        dest.graphic_attachments  = _malloc(sizeof(VtGraphicLineAttachments));
        *dest.graphic_attachments = VtGraphicLineAttachments_clone(source->graphic_attachments);
    }

    return dest;
}

static inline void VtLine_destroy(void* vt_, VtLine* self);

DEF_VECTOR_DA(VtLine, VtLine_destroy, void);

/* typedef struct */
/* { */
/*     Vector_VtLine                   lines; */
/*     Vector_RcPtr_VtImageSurfaceView image_views; */
/*     Vector_RcPtr_VtSixelSurface     scrolled_sixels; */
/*     // TODO: Vector_VtSixelSurface           static_sixels; */
/* } VtScreenBuffer; */

/* static void VtScreenBuffer_destroy(VtScreenBuffer* self) */
/* { */
/*     Vector_destroy_VtLine(&self->lines); */
/*     Vector_destroy_RcPtr_VtImageSurfaceView(&self->image_views); */
/*     Vector_destroy_RcPtr_VtSixelSurface(&self->scrolled_sixels); */
/* } */

/* 'reverse' some modes so default is 0  */
typedef struct
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
    uint8_t no_insert_replace_mode : 1;
    uint8_t margin_bell : 1;

    /* VT300:
     * When sixel display mode is enabled, the sixel active position begins at the upper-left
     * corner of the ANSI text active position. Scrolling occurs when the sixel active position
     * reaches the bottom margin of the graphics page. When sixel mode is exited, the text
     * cursor is set to the current sixel cursor position.
     *
     * When sixel scrolling is disabled, the sixel active position begins at the upper-left
     * corner of the active graphics page. The terminal ignores any commands that attempt to
     * advance the active position below the bottom margin of the graphics page. When sixel mode
     * is exited, the text cursor does not change from the position it was in when sixel mode
     * was entered. */
    uint8_t sixel_scrolling : 1;

    /* Use private color registers for each sixel graphic */
    uint8_t sixel_private_color_registers : 1;

    /* Sixel scrolling leaves cursor to right of graphic. */
    uint8_t sixel_scrolling_move_cursor_right : 1;

    /* Send/receive mode aka local echo mode - SRM (VT102)
     *
     * This control function turns local echo on or off. When local echo is on, the
     * terminal sends keyboard characters to the screen. The host does not have
     * to send (echo) the characters back to the terminal display. When local
     * echo is off, the terminal only sends characters to the host. It is up to the
     * host to echo characters back to the screen.
     */
    uint8_t send_receive_mode : 1;

    /* New Line Mode - NLM (VT102)
     *
     * If LNM is set, then the cursor moves to the first column on the next line when the terminal
     * receives an LF, FF, or VT character. When you press Return, the terminal sends both a
     * carriage return (CR) and line feed (LF). If LNM is reset, then the cursor moves to the
     * current column on the next line when the terminal receives an LF, FF, or VT character. When
     * you press Return, the terminal sends only a carriage return (CR) character.
     */
    uint8_t new_line_mode : 1;

    /* Vertical Split Screen Mode - DECVSSM (VT400)
     *
     * This control function defines whether or not the set left and right margins
     * (DECSLRM) control function can set margins.
     */
    uint8_t vertical_split_screen_mode : 1;
} vt_modes_t;

typedef struct
{
    bool   alive;
    size_t global_index;
} vt_synchronized_update_origin_t;

DEF_VECTOR(vt_synchronized_update_origin_t, NULL);

typedef struct
{
    Pair_uint16_t                          snapshot_display_size;
    TimePoint                              snapshot_create_time;
    Vector_VtLine                          lines;
    Vector_vt_synchronized_update_origin_t origins;
} vt_synchronized_update_state_t;

typedef struct
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
        void (*on_select_end)(void*);
        void (*on_desktop_notification_sent)(void*, const char* opt_title, const char* text);
        void (*on_window_maximize_state_set)(void*, bool);
        void (*on_window_fullscreen_state_set)(void*, bool);
        void (*on_window_dimensions_set)(void*, uint32_t, uint32_t);
        void (*on_text_area_dimensions_set)(void*, int32_t, int32_t);
        void (*on_title_changed)(void*, const char*);
        void (*on_clipboard_requested)(void*);
        void (*on_font_reload_requseted)(void*);
        void (*on_clipboard_sent)(void*, const char*);
        void (*on_urgency_set)(void*);
        void (*on_restack_to_front)(void*);
        void (*on_command_state_changed)(void*);
        void (*on_gui_pointer_mode_changed)(void*);
        void (*on_buffer_changed)(void*);
        void (*on_visual_scroll_reset)(void*);
        void (*on_mouse_report_state_changed)(void*);
        const char* (*on_application_hostname_requested)(void*);

        void (*destroy_proxy)(void*, VtLineProxy*);
        void (*destroy_image_proxy)(void*, VtImageSurfaceProxy*);
        void (*destroy_image_view_proxy)(void*, VtImageSurfaceViewProxy*);
        void (*destroy_sixel_proxy)(void*, VtSixelSurfaceProxy*);
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
    Vector_char    output, staged_output;

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

        // TODO: SGR stack
        VtRune char_state; // records currently selected character properties

        Vector_char active_sequence;
    } parser;

    struct VtUriMatcher
    {
        Vector_char match;
        uint16_t    start_column;
        size_t      start_row;

        enum VtUriMatcherState
        {
            VT_URI_MATCHER_EMPTY,

            /* in a valid scheme */
            VT_URI_MATCHER_SCHEME,

            /* scheme was ended with ':' */
            VT_URI_MATCHER_SCHEME_COMPLETE,

            /* scheme:/ */
            VT_URI_MATCHER_FST_LEADING_SLASH,

            /* scheme://aut */
            VT_URI_MATCHER_AUTHORITY,

            /* scheme://authority/path */
            VT_URI_MATCHER_PATH,

            /* www.stuffgoeshere */
            VT_URI_MATCHER_SUFFIX_REFERENCE,
        } state;
    } uri_matcher;

    enum VtShellIntegrationState
    {
        VT_SHELL_INTEG_STATE_NONE,
        VT_SHELL_INTEG_STATE_PROMPT,
        VT_SHELL_INTEG_STATE_COMMAND,
        VT_SHELL_INTEG_STATE_OUTPUT,
    } shell_integration_state;

    vt_synchronized_update_state_t synchronized_update_state;

    RcPtr_VtImageSurface            manipulated_image;
    Vector_RcPtr_VtImageSurface     images;
    Vector_RcPtr_VtImageSurfaceView image_views, alt_image_views;
    Vector_RcPtr_VtSixelSurface     scrolled_sixels, alt_scrolled_sixels;

    Vector_RcPtr_VtCommand shell_commands;

    char* shell_integration_shell_id;
    int   shell_integration_protocol_version;
    char* shell_integration_shell_host;
    char* shell_integration_current_dir;

    char*         title;
    char*         client_host;
    char*         work_dir;
    Vector_DynStr title_stack;

    struct
    {
        bool action_performed;
        bool repaint;
    } defered_events;

    vt_gui_pointer_mode_t gui_pointer_mode;

    struct terminal_colors_t
    {
        ColorRGBA bg;
        ColorRGB  fg;

        struct terminal_cursor_colors_t
        {
            bool      enabled;
            ColorRGBA bg;
        } cursor;

        struct terminal_highlight_colors_t
        {
            ColorRGBA bg;
            ColorRGB  fg;
        } highlight;

        ColorRGB palette_256[256];

        graphic_color_registers_t global_graphic_color_registers;

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

    char* active_hyperlink;

    VtRune last_inserted;
    bool   has_last_inserted_rune;

    char32_t last_codepoint;
#ifndef NOUTF8PROC
    int32_t utf8proc_state;
#endif

    VtRune blank_space;

    VtCursor cursor;

    size_t alt_cursor_pos;
    size_t saved_cursor_pos;
    size_t alt_active_line;
    size_t saved_active_line;

    /* top margin in screen coordinates */
    uint16_t scroll_region_top;

    /* bottom margin in screen coordinates */
    uint16_t scroll_region_bottom;

    uint16_t scroll_region_left;
    uint16_t scroll_region_right;

    vt_modes_t modes;

#define VT_XT_MODIFY_KEYBOARD_DFT 0
    int8_t xterm_modify_keyboard;

#define VT_XT_MODIFY_CURSOR_KEYS_DFT 2
    int8_t xterm_modify_cursor_keys;

#define VT_XT_MODIFY_FUNCTION_KEYS_DFT 2
    int8_t xterm_modify_function_keys;

#define VT_XT_MODIFY_OTHER_KEYS_DFT 0
    int8_t xterm_modify_other_keys;

} Vt;

static const VtCommand* Vt_get_last_completed_command(Vt* self)
{
    for (RcPtr_VtCommand* ptr = NULL;
         (ptr = Vector_iter_back_RcPtr_VtCommand(&self->shell_commands, ptr));) {
        VtCommand* c = RcPtr_get_VtCommand(ptr);
        if (c && c->state == VT_COMMAND_STATE_COMPLETED) {
            return c;
        }
    }
    return NULL;
}

static inline const VtCommand* Vt_get_last_command(const Vt* self)
{
    return RcPtr_get_const_VtCommand(Vector_last_const_RcPtr_VtCommand(&self->shell_commands));
}

static inline bool VtRune_fg_is_default(const VtRune* rune)
{
    return rune->fg_is_palette_entry && rune->fg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT;
}

static inline bool VtRune_bg_is_default(const VtRune* rune)
{
    return rune->bg_is_palette_entry && rune->bg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT;
}

static ColorRGB Vt_rune_fg_no_invert(const Vt* self, const VtRune* rune);

static inline ColorRGBA Vt_rune_bg_no_invert(const Vt* self, const VtRune* rune)
{
    if (!rune) {
        return self->colors.bg;
    }

    if (!rune->bg_is_palette_entry) {
        return rune->bg_data.rgba;
    } else if (rune->bg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT) {
        return self->colors.bg;
    } else {
        return ColorRGBA_from_RGB(self->colors.palette_256[rune->bg_data.index]);
    }
}

static inline ColorRGBA Vt_rune_bg(const Vt* self, const VtRune* rune)
{
    if (!rune) {
        return self->colors.bg;
    }

    if (rune->invert) {
        return ColorRGBA_from_RGB(Vt_rune_fg_no_invert(self, rune));
    } else {
        return Vt_rune_bg_no_invert(self, rune);
    }
}

static inline ColorRGB Vt_rune_fg_no_invert(const Vt* self, const VtRune* rune)
{
    if (!rune) {
        return self->colors.fg;
    }

    if (!rune->fg_is_palette_entry) {
        return rune->fg_data.rgb;
    } else if (rune->fg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT) {
        return self->colors.fg;
    } else {
        int16_t idx =
          rune->fg_data.index + 8 * (settings.bold_is_bright && rune->rune.style == VT_RUNE_BOLD &&
                                     rune->fg_data.index <= 7 && rune->fg_data.index >= 0);
        return self->colors.palette_256[idx];
    }
}

static inline ColorRGB Vt_rune_fg(const Vt* self, const VtRune* rune)
{
    if (!rune) {
        return self->colors.fg;
    }

    if (rune->invert) {
        return ColorRGB_from_RGBA(Vt_rune_bg_no_invert(self, rune));
    } else {
        return Vt_rune_fg_no_invert(self, rune);
    }
}

static inline ColorRGB Vt_rune_ln_clr(const Vt* self, const VtRune* rune)
{
    if (!rune) {
        return self->colors.fg;
    }

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

static inline ColorRGB Vt_rune_cursor_fg(const Vt* self, const VtRune* rune)
{
    return (settings.cursor_color_static_fg || !rune)
             ? settings.cursor_fg
             : ColorRGB_from_RGBA(rune ? Vt_rune_bg(self, rune) : self->colors.bg);
}

static inline ColorRGBA Vt_rune_cursor_bg(const Vt* self, const VtRune* rune)
{
    return self->colors.cursor.enabled ? self->colors.cursor.bg
           : (settings.cursor_color_static_bg || !rune)
             ? settings.cursor_bg
             : ColorRGBA_from_RGB(Vt_rune_fg(self, rune));
}

static ColorRGB Vt_rune_cursor_ln_clr(const Vt* self, const VtRune* rune)
{
    if (rune->line_color_not_default) {
        if (rune->ln_clr_is_palette_entry) {
            return self->colors.palette_256[rune->ln_clr_data.index];
        } else {
            return rune->ln_clr_data.rgb;
        }
    } else {
        return Vt_rune_cursor_fg(self, rune);
    }
}

static inline void VtLine_destroy(void* vt_, VtLine* self)
{
    Vt* vt = vt_;
    if (unlikely(self->links)) {
        Vector_destroy_VtUri(self->links);
        free(self->links);
        self->links = NULL;
    }

    if (unlikely(self->graphic_attachments)) {
        if (self->graphic_attachments->images) {
            Vector_destroy_RcPtr_VtImageSurfaceView(self->graphic_attachments->images);
            free(self->graphic_attachments->images);
            /* self->graphic_attachments->images = NULL; */
        }
        if (self->graphic_attachments->sixels) {
            Vector_destroy_RcPtr_VtSixelSurface(self->graphic_attachments->sixels);
            free(self->graphic_attachments->sixels);
            /* self->graphic_attachments->sixels = NULL; */
        }

        free(self->graphic_attachments);
        self->graphic_attachments = NULL;
    }

    CALL(vt->callbacks.destroy_proxy, vt->callbacks.user_data, &self->proxy);
    Vector_destroy_VtRune(&self->data);
    RcPtr_destroy_VtCommand(&self->linked_command);
}

static void VtImageSurface_destroy(void* vt_, VtImageSurface* self)
{
    Vt* vt = vt_;
    Vector_destroy_uint8_t(&self->fragments);
    self->id    = 0;
    self->state = VT_IMAGE_SURFACE_DESTROYED;
    CALL(vt->callbacks.destroy_image_proxy, vt->callbacks.user_data, &self->proxy);
}

static void VtImageSurfaceView_destroy(void* vt_, VtImageSurfaceView* self)
{
    Vt* vt = vt_;
    RcPtr_destroy_VtImageSurface(&self->source_image_surface);
    CALL(vt->callbacks.destroy_image_view_proxy, vt->callbacks.user_data, &self->proxy);
}

static void VtSixelSurface_destroy(void* _vt, VtSixelSurface* self)
{
    Vt* vt = _vt;
    Vector_destroy_uint8_t(&self->fragments);
    CALL(vt->callbacks.destroy_sixel_proxy, vt->callbacks.user_data, &self->proxy);
}

static uint16_t VtLine_add_link(VtLine* self, const char* link)
{
    if (!self->links) {
        self->links  = _malloc(sizeof(*self->links));
        *self->links = Vector_new_with_capacity_VtUri(1);
        Vector_push_VtUri(self->links, (VtUri){ .uri_string = strdup(link) });
        return 0;
    }

    for (VtUri* i = NULL; (i = Vector_iter_VtUri(self->links, i));) {
        if (i->uri_string && !strcmp(i->uri_string, link)) {
            return Vector_index_VtUri(self->links, i);
        }
    }

    Vector_push_VtUri(self->links, (VtUri){ .uri_string = strdup(link) });
    return self->links->size - 1;
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

static inline bool Vt_synchronized_update_is_active(const Vt* self)
{
    return self->synchronized_update_state.snapshot_display_size.first ||
           self->synchronized_update_state.snapshot_display_size.second;
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
static inline VtRune* Vt_at(Vt* self, uint16_t column, size_t row)
{
    VtLine* line = Vt_line_at(self, row);
    if (!line || column >= line->data.size) {
        return NULL;
    }
    return &line->data.buf[column];
}

static inline Pair_uint16_t Vt_pixels_to_cells(Vt* self, int32_t x, int32_t y)
{
    x = CLAMP(x, 0, self->ws.ws_xpixel);
    y = CLAMP(y, 0, self->ws.ws_ypixel);
    return (Pair_uint16_t){
        .first  = (double)x / self->pixels_per_cell_x,
        .second = (double)y / self->pixels_per_cell_y,
    };
}

/**
 * Get location of an entire link including wrapped lines at cell in global coordinates */
const char* Vt_uri_range_at(Vt*            self,
                            uint16_t       column,
                            size_t         row,
                            Pair_size_t*   out_rows,
                            Pair_uint16_t* out_columns);

#ifdef DEBUG
static inline VtLine* _ERRVt_cursor_line(int ln, const Vt* self)
{
    ERR("line count overflow on line %d. line cnt %zu cursor pos %zu\n",
        ln,
        self->lines.size,
        self->cursor.row);
    return NULL;
}

static inline VtLine* _Vt_cursor_line(const Vt* self)
{
    return &self->lines.buf[self->cursor.row];
}

#define Vt_cursor_line(_s)                                                                         \
    (((_s)->lines.size < (_s)->cursor.row) ? _ERRVt_cursor_line(__LINE__, (_s))                    \
                                           : _Vt_cursor_line((_s)))

#else
/**
 * Get line under terminal cursor */
static inline VtLine* Vt_cursor_line(const Vt* self)
{
    return &self->lines.buf[self->cursor.row];
}
#endif

/**
 * Get cell under terminal cursor */
static inline VtRune* Vt_cursor_cell(const Vt* self)
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
 * Get pty response data up to given size of @param len. */
void Vt_peek_output(Vt* self, size_t len, char** out_buf, size_t* out_size);

/**
 * Get size of pending pty response data waiting to be written */
static inline size_t Vt_get_output_size(const Vt* self)
{
    return self->output.size;
}

/**
 * Remove @param len bytes of output data from internal buffer. */
void Vt_consumed_output(Vt* self, size_t len);

/**
 * Get lines that should be visible */
void Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end);

VtLine* Vt_get_visible_line(const Vt* self, size_t idx);

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
 * Is the alternate screen buffer beeing displayed */
static inline bool Vt_alt_buffer_enabled(Vt* self)
{
    return self->alt_lines.buf;
}

/**
 * Get line index at the top of the client viewport */
static inline size_t Vt_top_line(const Vt* const self)
{
    return self->lines.size <= self->ws.ws_row ? 0 : self->lines.size - self->ws.ws_row;
}

/**
 * Get line index at the bottom of the client viewport */
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
 * Get line index at the top of the visual viewport (takes visual scroling into account) */
static inline size_t Vt_visual_top_line(const Vt* const self)
{
    return self->scrolling_visual ? self->visual_scroll_top : Vt_top_line(self);
}

/**
 * Get line index at the bottom of the visual viewport (takes visual scroling into account) */
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
 * Move visual viewport to previous line mark and start visual scrolling */
static inline bool Vt_visual_scroll_mark_up(Vt* self)
{
    if (!Vt_visual_top_line(self) || Vt_alt_buffer_enabled(self)) {
        return false;
    }

    for (size_t i = Vt_visual_top_line(self); i + 1; --i) {
        VtLine* ln = Vt_line_at(self, i);

        if (ln && (ln->mark_explicit || ln->mark_command_output_start)) {
            Vt_visual_scroll_to(self, i ? (i - 1) : i);
            return true;
        }
    }
    return false;
}

/**
 * Move visual viewport to next line mark and start visual scrolling */
static inline bool Vt_visual_scroll_mark_down(Vt* self)
{
    if (Vt_alt_buffer_enabled(self)) {
        return false;
    }

    bool found_first = false;
    for (size_t i = Vt_visual_top_line(self) + 1; i < Vt_max_line(self); ++i) {
        VtLine* ln = Vt_line_at(self, i);
        if (ln && (ln->mark_explicit || ln->mark_command_output_start)) {
            if (found_first) {
                Vt_visual_scroll_to(self, i ? (i - 1) : i);
                return true;
            } else {
                found_first = true;
            }
        }
    }

    if (Vt_visual_top_line(self) != Vt_top_line(self)) {
        Vt_visual_scroll_to(self, Vt_top_line(self));
        return true;
    } else {
        return false;
    }
}

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
 * Terminal listens for scroll wheel or button presses */
static bool Vt_reports_mouse(Vt* self)
{
    return self->modes.extended_report || self->modes.mouse_motion_on_btn_report ||
           self->modes.mouse_btn_report;
}

/**
 * Destroy the interpreter */
void Vt_destroy(Vt* self);

/**
 * Generate color for given 256 palette index */
void generate_color_palette_entry(ColorRGB* color, int16_t idx);

/**
 * Get xterm 256 palette index from X11 color name */
int palette_color_index_from_xterm_name(const char* name);

/**
 * Get xterm 256 palette color by X11 color name */
ColorRGB color_from_xterm_name(const char* name, bool* fail);

bool _vt_is_cell_selected(const Vt* const self, int32_t x, int32_t y);

/**
 * Should a cell (in screen coordinates) be visually highlighted as selected */
static inline bool Vt_is_cell_selected(const Vt* const self, int32_t x, int32_t y)
{
    if (likely(self->selection.mode == SELECT_MODE_NONE)) {
        return false;
    } else {
        return _vt_is_cell_selected(self, x, y);
    }
}

/**
 * Get cursor row in screen coordinates */
static inline uint16_t Vt_cursor_row(const Vt* self)
{
    return self->cursor.row - Vt_top_line(self);
}

static inline uint16_t Vt_visual_cursor_row(const Vt* self)
{
    return self->cursor.row - Vt_visual_top_line(self);
}

/**
 * Get scroll region top line in global coordinates */
static inline size_t Vt_get_scroll_region_top(const Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_top;
}

/**
 * Get scroll region bottom line in global coordinates */
static inline size_t Vt_get_scroll_region_bottom(const Vt* self)
{
    return Vt_top_line(self) + self->scroll_region_bottom;
}

/**
 * Is terminal scroll region set to default */
static inline bool Vt_scroll_region_not_default(const Vt* self)
{
    return Vt_get_scroll_region_top(self) != Vt_top_line(self) ||
           Vt_get_scroll_region_bottom(self) != Vt_bottom_line(self);
}

/**
 * Get UTF-8 encoded string from Vector_VtRune in a given range
 * @param tail - append string to the end */
Vector_char rune_vec_to_string(Vector_VtRune* line, size_t begin, size_t end, const char* opt_tail);

/**
 * Get UTF-8 encoded string from VtLine in a given range
 * @param tail - append string to the end */
static inline Vector_char VtLine_to_string(VtLine* line, size_t begin, size_t end, const char* tail)
{
    return rune_vec_to_string(&line->data, begin, end, tail);
}

/**
 * Get UTF-8 encoded string from a terminal line at global index in a given range
 * @param tail - append string to the end */
static Vector_char Vt_line_to_string(const Vt*   self,
                                     size_t      idx,
                                     size_t      begin,
                                     size_t      end,
                                     const char* tail)
{
    return VtLine_to_string(&self->lines.buf[idx], begin, end, tail);
}

/**
 * Get UTF-8 encoded string from a range of lines */
Vector_char Vt_region_to_string(Vt* self, size_t begin_line, size_t end_line);

/**
 * Get current work directory of client program */
static const char* Vt_get_work_directory(const Vt* self)
{
    return OR(self->work_dir, self->shell_integration_current_dir);
}

/**
 * Get hostname of client program */
static const char* Vt_get_client_host(const Vt* self)
{
    return OR(self->client_host, self->shell_integration_shell_host);
}

/**
 * Get if client program is running on localhost, when in doubt assumes true */
static bool Vt_client_host_is_local(const Vt* self)
{
    const char* application_host =
      CALL(self->callbacks.on_application_hostname_requested, self->callbacks.user_data);
    const char* client_host = Vt_get_client_host(self);

    LOG("Vt::host_is_local{ application: %s, client: %s }\n", application_host, client_host);

    return (!application_host || !client_host) ? true : !strcmp(application_host, client_host);
}

void Vt_shrink_scrollback(Vt* self);

void Vt_clear_scrollback(Vt* self);

Vector_char Vt_command_to_string(const Vt* self, const VtCommand* command, size_t opt_limit_lines);

static bool Vt_ImageSurfaceView_is_visual_visible(const Vt* self, VtImageSurfaceView* view)
{
    return Vt_visual_top_line(self) <= view->anchor_global_index + view->cell_size.second &&
           Vt_visual_bottom_line(self) >= view->anchor_global_index;
}

static VtCommand* Vt_shell_integration_get_active_command(Vt* self)
{
    RcPtr_VtCommand* cmd_ptr = Vector_last_RcPtr_VtCommand(&self->shell_commands);
    VtCommand*       cmd     = NULL;
    if (!cmd_ptr || !(cmd = RcPtr_get_VtCommand(cmd_ptr))) {
        return NULL;
    }
    return cmd->state == VT_COMMAND_STATE_RUNNING ? cmd : NULL;
}

static inline ColorRGB Vt_rune_final_fg_apply_dim(const Vt*     self,
                                                  const VtRune* rune,
                                                  ColorRGBA     bg_color,
                                                  bool          is_cursor)
{
    if (!rune) {
        return ColorRGB_new_from_blend(settings.fg, ColorRGB_from_RGBA(bg_color), VT_DIM_FACTOR);
    }

    if (unlikely(rune->dim)) {
        return ColorRGB_new_from_blend(is_cursor ? Vt_rune_cursor_fg(self, rune)
                                                 : Vt_rune_fg(self, rune),
                                       ColorRGB_from_RGBA(bg_color),
                                       VT_DIM_FACTOR);
    } else {
        return is_cursor ? Vt_rune_cursor_fg(self, rune) : Vt_rune_fg(self, rune);
    }
}

static inline ColorRGBA Vt_rune_final_bg(const Vt*     self,
                                         const VtRune* rune,
                                         int32_t       x,
                                         int32_t       y,
                                         bool          is_cursor)
{
    if (unlikely(Vt_is_cell_selected(self, x, y))) {
        return self->colors.highlight.bg;
    } else if (rune) {
        return is_cursor ? Vt_rune_cursor_bg(self, rune) : Vt_rune_bg(self, rune);
    } else {
        return settings.bg;
    }
}

static inline ColorRGB Vt_rune_final_fg(const Vt*     self,
                                        const VtRune* rune,
                                        int32_t       x,
                                        int32_t       y,
                                        ColorRGBA     bg_color,
                                        bool          is_cursor)
{
    if (!settings.highlight_change_fg) {
        return Vt_rune_final_fg_apply_dim(self, rune, bg_color, is_cursor);
    } else {
        if (unlikely(Vt_is_cell_selected(self, x, y))) {
            return self->colors.highlight.fg;
        } else {
            return Vt_rune_final_fg_apply_dim(self, rune, bg_color, is_cursor);
        }
    }
}

static inline bool Vt_is_reporting_mouse(const Vt* self)
{
    return self->modes.mouse_btn_report || self->modes.mouse_motion_report ||
           self->modes.mouse_motion_on_btn_report || self->modes.extended_report;
}

/**
 * Get URI at cell in global coordinates */
static inline const char* Vt_uri_at(Vt* self, uint16_t column, size_t row)
{
    VtLine* line;
    if (unlikely(Vt_synchronized_update_is_active(self) && row >= Vt_top_line(self))) {
        line =
          &self->synchronized_update_state.lines
             .buf[MIN(row - Vt_top_line(self), self->synchronized_update_state.lines.size - 1)];
    } else {
        line = Vt_line_at(self, row);
    }

    if (!line || !line->links || column >= line->data.size) {
        return NULL;
    }

    VtRune* rune = &line->data.buf[column];

    if (!rune->hyperlink_idx || rune->hyperlink_idx > (int16_t)line->links->size) {
        return NULL;
    }

    return Vector_at_VtUri(line->links, rune->hyperlink_idx - 1)->uri_string;
}
