/* See LICENSE for license information. */


#pragma once

#include "util.h"
#include "colors.h"

#ifndef VERSION
    #define VERSION "unknown"
#endif

typedef struct
{
    ColorRGB color[16];

} Colorscheme;

typedef struct
{
    const char* config_path;
    bool skip_config;

    /* Prefer x11 over wayland */
    bool x11_is_default;

    /* Shell */
    char* shell;
    int shell_argc;
    const char** shell_argv;

    /* TERM value */
    char* term;

    /* override locale */
    char* locale;

    /* main title (application name) */
    char* title;

    /* allow applications to set their own window title (uses title format) */
    bool dynamic_title;

    bool bsp_sends_del;

    /* printf-style format string used when window title is set by the
     * shell/program. Has two string arguments, title and title set by the
     * shell.
     *
     * title_format = "%2$s"            -> user@host:~
     * title_format = "%s - %s"         -> wayst - user@host:~
     * title_format = "Terminal (%2$s)" -> Terminal (user@host:~)
     */
    char* title_format;

    /* initial window size */
    uint32_t rows, cols;

    /* font family */
    char* font;


    /* font family for symbol font */
    char* font_fallback;
    char* font_fallback2;

    /* font files */
    char* font_name;
    char* font_name_bold;
    char* font_name_italic;
    char* font_name_fallback;
    char* font_name_fallback2;
    uint16_t font_size;
    uint16_t font_dpi;

    /* colors - normal, highlight */
    ColorRGBA bg, bghl;
    ColorRGB fg, fghl;

    /* color for 'dim' text */
    ColorRGB fg_dim;

    bool highlight_change_fg;

    int colorscheme_preset;
    Colorscheme colorscheme;
    bool* _explicit_colors_set;

    int text_blink_interval;

    ColorRGBA bell_flash;
    bool  no_flash;

    bool allow_scrollback_clear;
    bool scroll_on_output;
    bool scroll_on_key;
    uint8_t scroll_discrete_lines;

    bool allow_multiple_underlines;

} Settings;


extern ColorRGB color_palette_256[257];
extern Settings settings;

void
settings_init(const int argc, char* const* argv);


void
settings_cleanup();
