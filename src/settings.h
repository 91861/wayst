/* See LICENSE for license information. */

#pragma once

#include "colors.h"
#include "util.h"
#include "vector.h"

#ifndef VERSION
#define VERSION "0.0.0"
#endif

#ifndef EXECUTABLE_FILE_NAME
#define EXECUTABLE_FILE_NAME "wayst"
#endif

#ifndef APPLICATION_NAME
#define APPLICATION_NAME "Wayst"
#endif

// default lcd filter
#ifndef LCD_FILT_DFT
#define LCD_FILT_DFT LCD_FILTER_H_RGB
#endif

#define MODIFIER_SHIFT   (1 << 0)
#define MODIFIER_ALT     (1 << 1)
#define MODIFIER_CONTROL (1 << 2)

DEF_VECTOR(Pair_char32_t, NULL);

typedef struct
{
    char*                family_name;
    char*                file_name;
    int8_t               size_offset;
    Vector_Pair_char32_t codepoint_ranges;
} UnstyledFontInfo;

static void UnstyledFontInfo_destroy(UnstyledFontInfo* self)
{
    free(self->family_name);
    self->family_name = NULL;
    free(self->file_name);
    self->file_name = NULL;
    Vector_destroy_Pair_char32_t(&self->codepoint_ranges);
}

typedef struct
{
    char*                family_name;
    char*                regular_file_name;
    char*                bold_file_name;
    char*                italic_file_name;
    char*                bold_italic_file_name;
    int8_t               size_offset;
    Vector_Pair_char32_t codepoint_ranges;
} StyledFontInfo;

static void StyledFontInfo_destroy(StyledFontInfo* self)
{
    free(self->family_name);
    self->family_name = NULL;
    free(self->regular_file_name);
    self->regular_file_name = NULL;
    free(self->bold_file_name);
    self->bold_file_name = NULL;
    free(self->italic_file_name);
    self->italic_file_name = NULL;
    free(self->bold_italic_file_name);
    self->bold_italic_file_name = NULL;
    Vector_destroy_Pair_char32_t(&self->codepoint_ranges);
}

DEF_VECTOR(StyledFontInfo, StyledFontInfo_destroy);
DEF_VECTOR(UnstyledFontInfo, UnstyledFontInfo_destroy);

typedef struct
{
    ColorRGB color[16];

} Colorscheme;

typedef struct
{
    union key_data
    {
        uint32_t code;
        char*    name; // malloced
    } key;

    bool is_name;

    uint32_t mods;
} KeyCommand;

// indicies into array in settings
enum key_command_e
{
    KCMD_COPY = 0,
    KCMD_COPY_OUTPUT,
    KCMD_PASTE,
    KCMD_FONT_ENLARGE,
    KCMD_FONT_SHRINK,
    KCMD_UNICODE_ENTRY,

    KCMD_LINE_SCROLL_UP,
    KCMD_LINE_SCROLL_DN,
    KCMD_PAGE_SCROLL_UP,
    KCMD_PAGE_SCROLL_DN,
    KCMD_MARK_SCROLL_UP,
    KCMD_MARK_SCROLL_DN,

    KCMD_EXTERN_PIPE,
    KCMD_KEYBOARD_SELECT,
    KCMD_HTML_DUMP,
    KCMD_OPEN_PWD,
    KCMD_DUPLICATE,
    KCMD_DEBUG,
    KCMD_QUIT,

    NUM_KEY_COMMANDS, // array size
};

enum lcd_filter_e
{
    LCD_FILTER_UNDEFINED, // use window system if available
    LCD_FILTER_NONE,
    LCD_FILTER_H_RGB,
    LCD_FILTER_H_BGR,
    LCD_FILTER_V_RGB,
    LCD_FILTER_V_BGR,
};

enum extern_pipe_source_e
{
    EXTERN_PIPE_SOURCE_BUFFER,
    EXTERN_PIPE_SOURCE_VIEWPORT,
    EXTERN_PIPE_SOURCE_COMMAND,
};

typedef struct
{
    struct external_data
    {
        void* user_data;
        uint32_t (*keycode_from_string)(void* self, char* string);
    } callbacks;

    int    argc;
    char** argv;

    KeyCommand key_commands[NUM_KEY_COMMANDS];

    bool skip_config;
    bool x11_is_default;
    int  shell_argc;

    bool     dynamic_title;
    bool     bsp_sends_del;
    bool     windowops_manip;
    bool     windowops_info;
    uint32_t rows, cols;

    const char** shell_argv;

    AString config_path;
    AString shell;
    AString title_format;

    AString font_style_regular, font_style_bold, font_style_italic, font_style_bold_italic;

    Vector_StyledFontInfo   styled_fonts;
    Vector_UnstyledFontInfo symbol_fonts, color_fonts;

    bool has_bold_fonts, has_italic_fonts, has_bold_italic_fonts, has_symbol_fonts, has_color_fonts;

    bool                 lcd_ranges_set_by_user;
    Vector_Pair_char32_t lcd_exclude_ranges;

    AString term, vte_version, locale, title, directory, uri_handler, extern_pipe_handler;

    char* user_app_id;
    char* user_app_id_2;

    uint16_t          font_size;
    uint16_t          font_size_fallback;
    uint16_t          font_dpi;
    enum lcd_filter_e lcd_filter;

    /* colors - normal, highlight */
    ColorRGBA bg, bghl, cursor_bg;
    ColorRGB  fg, fghl, cursor_fg;

    bool cursor_color_static_bg, cursor_color_static_fg;

    ColorRGBA dim_tint;

    bool highlight_change_fg;

    int         colorscheme_preset;
    Colorscheme colorscheme;

    /* bool[] that records if a given color index was set by user configuration. This is needed
     * because the colorscheme option may be be received after color options and in that case it
     * should not overwrite user settings */
    bool* _explicit_colors_set;

    ColorRGBA bell_flash;
    bool      no_flash;

    bool    padding_center;
    uint8_t padding;
    int8_t  padd_glyph_x, padd_glyph_y;
    char    center_char;
    int8_t  offset_glyph_x, offset_glyph_y;

    uint16_t scrollbar_width_px, scrollbar_length_px;
    uint16_t scrollbar_hide_delay_ms;
    uint16_t scrollbar_fade_time_ms;

    bool    allow_scrollback_clear;
    bool    scroll_on_output;
    bool    scroll_on_key;
    bool    hold_after_child_process_exit;
    uint8_t scroll_discrete_lines;

    bool allow_multiple_underlines;

    bool     debug_pty, debug_gfx, debug_font, debug_vt;
    uint32_t vt_debug_delay_usec;

    uint32_t scrollback;

    bool    enable_cursor_blink;
    int32_t cursor_blink_interval_ms;
    int32_t cursor_blink_suspend_ms;
    int32_t cursor_blink_end_s;

    bool initial_cursor_blinking;
    bool bold_is_bright;

    enum decoration_style_e
    {
        DECORATION_STYLE_FULL,
        DECORATION_STYLE_MINIMAL,
        DECORATION_STYLE_NONE,
    } decoration_style;

    enum decoration_theme_e
    {
        DECORATION_THEME_DONT_CARE = 0,
        DECORATION_THEME_FROM_BG,
        DECORATION_THEME_FROM_BG_IF_DARK,
        DECORATION_THEME_DARK,
        DECORATION_THEME_LIGHT,
    } decoration_theme;

    enum initial_gui_pointer_mode_e
    {
        GUI_POINTER_MODE_FORCE_SHOW,
        GUI_POINTER_MODE_FORCE_HIDE,
        GUI_POINTER_MODE_HIDE,
        GUI_POINTER_MODE_SHOW,
        GUI_POINTER_MODE_SHOW_IF_REPORTING,
    } initial_gui_pointer_mode;

    enum initial_cursor_style_e
    {
        CURSOR_STYLE_BLOCK,
        CURSOR_STYLE_BEAM,
        CURSOR_STYLE_UNDERLINE,
    } initial_cursor_style;

    enum extern_pipe_source_e extern_pipe_source;

    bool defer_font_loading;
    bool flush_ft_cache;

    uint32_t pty_chunk_wait_delay_ns;
    uint32_t pty_chunk_timeout_ms;

    bool smooth_cursor;

} Settings;

extern Settings settings;

void settings_init(int argc, char** argv);

static inline void KeyCommand_name_to_code(KeyCommand* cmd);

static inline void settings_after_window_system_connected()
{
    for (int_fast8_t i = 0; i < NUM_KEY_COMMANDS; ++i) {
        KeyCommand_name_to_code(&settings.key_commands[i]);
    }
    if (settings.lcd_filter == LCD_FILTER_UNDEFINED) {
        settings.lcd_filter = LCD_FILT_DFT;
    }
}

void settings_cleanup();

static inline void KeyCommand_name_to_code(KeyCommand* cmd)
{
    if (cmd->is_name) {
        uint32_t code =
          CALL(settings.callbacks.keycode_from_string, settings.callbacks.user_data, cmd->key.name);
        if (!code) {
            WRN("Invalid key name \'%s\'\n", cmd->key.name);
        } else {
            LOG("settings::key name \'%s\' -> keysym %u\n", cmd->key.name, code);
        }
        free(cmd->key.name);
        cmd->key.code = code;
        cmd->is_name  = false;
    }
}

static bool KeyCommand_is_active(KeyCommand* com, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    /*
     * For whatever stupid reason keysym from string returns keysyms obtainable
     * by converting keycodes using a keyboard map for SOME, BUT NOT ALL KEYS!
     * Those need to be compared to a non-shifted 'raw' keysym (we can't just use keycodes instead
     * because this would ignote window system key remaps like 'caps:swapescape').
     */
    return com->mods == mods && (rawkey > 65000 ? com->key.code == key : com->key.code == rawkey);
}
