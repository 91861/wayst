/* See LICENSE for license information. */

#pragma once

#include "colors.h"
#include "util.h"
#include "vector.h"

#ifndef VERSION
#define VERSION "0.0.0"
#endif

// executable name
#ifndef EXE_FNAME
#define EXE_FNAME "wayst"
#endif

// application name
#ifndef APP_NAME
#define APP_NAME "Wayst"
#endif

// default lcd filter
#ifndef LCD_FILT_DFT
#define LCD_FILT_DFT LCD_FILTER_H_RGB
#endif

#define MODIFIER_SHIFT   (1 << 0)
#define MODIFIER_ALT     (1 << 1)
#define MODIFIER_CONTROL (1 << 2)

typedef struct
{
    char* family_name;
    char* file_name;
} UnstyledFontInfo;

static void UnstyledFontInfo_destroy(UnstyledFontInfo* self)
{
    free(self->family_name);
    self->family_name = NULL;
    free(self->file_name);
    self->file_name = NULL;
}

typedef struct
{
    char* family_name;
    char* regular_file_name;
    char* bold_file_name;
    char* italic_file_name;
    char* bold_italic_file_name;
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
enum KeyCommands
{
    KCMD_COPY = 0,
    KCMD_PASTE,
    KCMD_FONT_ENLARGE,
    KCMD_FONT_SHRINK,
    KCMD_UNICODE_ENTRY,
    KCMD_KEYBOARD_SELECT,
    KCMD_DUPLICATE,
    KCMD_DEBUG,
    KCMD_QUIT,

    NUM_KEY_COMMANDS, // array size
};

enum LcdFilter
{
    LCD_FILTER_UNDEFINED, // use window system if available
    LCD_FILTER_NONE,
    LCD_FILTER_H_RGB,
    LCD_FILTER_H_BGR,
    LCD_FILTER_V_RGB,
    LCD_FILTER_V_BGR,
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
    uint32_t rows, cols;

    const char** shell_argv;

    AString config_path;
    AString shell;
    AString title_format;

    AString font_style_regular, font_style_bold, font_style_italic, font_style_bold_italic;

    Vector_StyledFontInfo   styled_fonts;
    Vector_UnstyledFontInfo symbol_fonts, color_fonts;

    bool has_bold_fonts, has_italic_fonts, has_bold_italic_fonts, has_symbol_fonts, has_color_fonts;

    AString term, locale, title;

    uint16_t       font_size;
    uint16_t       font_size_fallback;
    uint16_t       font_dpi;
    enum LcdFilter lcd_filter;

    /* colors - normal, highlight */
    ColorRGBA bg, bghl;
    ColorRGB  fg, fghl;

    bool highlight_change_fg;

    int         colorscheme_preset;
    Colorscheme colorscheme;
    bool*       _explicit_colors_set;

    ColorRGBA bell_flash;
    bool      no_flash;

    bool    padding_center;
    uint8_t padding;
    int8_t  padd_glyph_x;
    int8_t  padd_glyph_y;

    bool    allow_scrollback_clear;
    bool    scroll_on_output;
    bool    scroll_on_key;
    uint8_t scroll_discrete_lines;

    bool allow_multiple_underlines;

    bool debug_pty, debug_gfx, debug_font;

    uint32_t scrollback;

    bool    enable_cursor_blink;
    int32_t cursor_blink_interval_ms;
    int32_t cursor_blink_suspend_ms;
    int32_t cursor_blink_end_s;
} Settings;

extern ColorRGB color_palette_256[257];
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
    ASSERT(settings.callbacks.keycode_from_string, "callback is NULL");

    if (cmd->is_name) {
        uint32_t code =
          settings.callbacks.keycode_from_string(settings.callbacks.user_data, cmd->key.name);
        if (!code) {
            WRN("Invalid key name \'%s\'\n", cmd->key.name);
        } else {
            LOG("Converting key name \'%s\' to keysym %u\n", cmd->key.name, code);
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
     * Those need to be compared to a 'non-shifted' keysym.
     */
    return com->mods == mods && (rawkey > 65000 ? com->key.code == key : com->key.code == rawkey);
}
