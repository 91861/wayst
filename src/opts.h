/* See LICENSE for license information. */

/**
 * Options
 **/

#pragma once

#include <getopt.h>
#include <stddef.h>

static const char* const arg_path    = "path";
static const char* const arg_int     = "number";
static const char* const arg_color   = "#RRGGBB";
static const char* const arg_color_a = "#RRGGBBAA";
static const char* const arg_string  = "string";
static const char* const arg_key     = "key";
static const char* const arg_name    = "name";

// -e and -x are reserved
static struct option long_options[] = {

#define OPT_CONFIG_FILE_IDX 0
    [OPT_CONFIG_FILE_IDX] = { "config-file", required_argument, 0, 0 },

#define OPT_SKIP_CONFIG_IDX 1
    [OPT_SKIP_CONFIG_IDX] = { "skip-config", no_argument, 0, 'C' },

#define OPT_XORG_ONLY_IDX 2
    [OPT_XORG_ONLY_IDX] = { "xorg-only", no_argument, 0, 'X' },

#define OPT_TERM_IDX 3
    [OPT_TERM_IDX] = { "term", required_argument, 0, 0 },

#define OPT_TITLE_IDX 4
    [OPT_TITLE_IDX] = { "title", required_argument, 0, 0 },

#define OPT_DYNAMIC_TITLE_IDX 5
    [OPT_DYNAMIC_TITLE_IDX] = { "no-dynamic-title", no_argument, 0, 'T' },

#define OPT_TITLE_FORMAT_IDX 6
    [OPT_TITLE_FORMAT_IDX] = { "title-format", required_argument, 0, 0 },

#define OPT_LOCALE_IDX 7
    [OPT_LOCALE_IDX] = { "locale", required_argument, 0, 0 },

#define OPT_ROWS_IDX 8
    [OPT_ROWS_IDX] = { "rows", required_argument, 0, 0 },

#define OPT_COLUMNS_IDX 9
    [OPT_COLUMNS_IDX] = { "columns", required_argument, 0, 0 },

#define OPT_BG_COLOR_IDX 10
    [OPT_BG_COLOR_IDX] = { "bg-color", required_argument, 0, 0 },

#define OPT_FG_COLOR_IDX 11
    [OPT_FG_COLOR_IDX] = { "fg-color", required_argument, 0, 0 },

#define OPT_COLOR_0_IDX 12
    [OPT_COLOR_0_IDX] = { "color-0", required_argument, 0, 0 },

#define OPT_COLOR_1_IDX 13
    [OPT_COLOR_1_IDX] = { "color-1", required_argument, 0, 0 },

#define OPT_COLOR_2_IDX 14
    [OPT_COLOR_2_IDX] = { "color-2", required_argument, 0, 0 },

#define OPT_COLOR_3_IDX 15
    [OPT_COLOR_3_IDX] = { "color-3", required_argument, 0, 0 },

#define OPT_COLOR_4_IDX 16
    [OPT_COLOR_4_IDX] = { "color-4", required_argument, 0, 0 },

#define OPT_COLOR_5_IDX 17
    [OPT_COLOR_5_IDX] = { "color-5", required_argument, 0, 0 },

#define OPT_COLOR_6_IDX 18
    [OPT_COLOR_6_IDX] = { "color-6", required_argument, 0, 0 },

#define OPT_COLOR_7_IDX 19
    [OPT_COLOR_7_IDX] = { "color-7", required_argument, 0, 0 },

#define OPT_COLOR_8_IDX 20
    [OPT_COLOR_8_IDX] = { "color-8", required_argument, 0, 0 },

#define OPT_COLOR_9_IDX 21
    [OPT_COLOR_9_IDX] = { "color-9", required_argument, 0, 0 },

#define OPT_COLOR_10_IDX 22
    [OPT_COLOR_10_IDX] = { "color-10", required_argument, 0, 0 },

#define OPT_COLOR_11_IDX 23
    [OPT_COLOR_11_IDX] = { "color-11", required_argument, 0, 0 },

#define OPT_COLOR_12_IDX 24
    [OPT_COLOR_12_IDX] = { "color-12", required_argument, 0, 0 },

#define OPT_COLOR_13_IDX 25
    [OPT_COLOR_13_IDX] = { "color-13", required_argument, 0, 0 },

#define OPT_COLOR_14_IDX 26
    [OPT_COLOR_14_IDX] = { "color-14", required_argument, 0, 0 },

#define OPT_COLOR_15_IDX 27
    [OPT_COLOR_15_IDX] = { "color-15", required_argument, 0, 0 },

#define OPT_FG_COLOR_DIM_IDX 28
    [OPT_FG_COLOR_DIM_IDX] = { "fg-color-dim", required_argument, 0, 0 },

#define OPT_H_BG_COLOR_IDX 29
    [OPT_H_BG_COLOR_IDX] = { "h-bg-color", required_argument, 0, 0 },

#define OPT_H_FG_COLOR_IDX 30
    [OPT_H_FG_COLOR_IDX] = { "h-fg-color", required_argument, 0, 0 },

#define OPT_NO_FLASH_IDX 31
    [OPT_NO_FLASH_IDX] = { "no-flash", no_argument, 0, 'F' },

#define OPT_COLORSCHEME_IDX 32
    [OPT_COLORSCHEME_IDX] = { "colorscheme", required_argument, 0, 0 },

#define OPT_FONT_IDX 33
    [OPT_FONT_IDX] = { "font", required_argument, 0, 0 },

#define OPT_FONT_STYLE_REGULAR_IDX 34
    [OPT_FONT_STYLE_REGULAR_IDX] = { "style-regular", required_argument, 0, 0 },

#define OPT_FONT_STYLE_BOLD_IDX 35
    [OPT_FONT_STYLE_BOLD_IDX] = { "style-bold", required_argument, 0, 0 },

#define OPT_FONT_STYLE_ITALIC_IDX 36
    [OPT_FONT_STYLE_ITALIC_IDX] = { "style-italic", required_argument, 0, 0 },

#define OPT_FONT_FALLBACK_IDX 37
    [OPT_FONT_FALLBACK_IDX] = { "font-fallback", required_argument, 0, 0 },

#define OPT_FONT_FALLBACK2_IDX 38
    [OPT_FONT_FALLBACK2_IDX] = { "font-fallback2", required_argument, 0, 0 },

#define OPT_FONT_SIZE_IDX 39
    [OPT_FONT_SIZE_IDX] = { "font-size", required_argument, 0, 0 },

#define OPT_DPI_IDX 40
    [OPT_DPI_IDX] = { "dpi", required_argument, 0, 0 },

#define OPT_BLINK_IDX 41
    [OPT_BLINK_IDX] = { "blink", required_argument, 0, 0 },

#define OPT_SCROLL_LINES_IDX 42
    [OPT_SCROLL_LINES_IDX] = { "scroll-lines", required_argument, 0, 0 },

#define OPT_SCROLLBACK_IDX 43
    [OPT_SCROLLBACK_IDX] = { "scrollback", required_argument, 0, 0 },

#define OPT_BIND_KEY_COPY_IDX 44
    [OPT_BIND_KEY_COPY_IDX] = { "bind-key-copy", required_argument, 0, 0 },

#define OPT_BIND_KEY_PASTE_IDX 45
    [OPT_BIND_KEY_PASTE_IDX] = { "bind-key-paste", required_argument, 0, 0 },

#define OPT_BIND_KEY_ENLARGE_IDX 46
    [OPT_BIND_KEY_ENLARGE_IDX] = { "bind-key-enlarge", required_argument, 0, 0 },

#define OPT_BIND_KEY_SHRINK_IDX 47
    [OPT_BIND_KEY_SHRINK_IDX] = { "bind-key-shrink", required_argument, 0, 0 },

#define OPT_BIND_KEY_UNI_IDX 48
    [OPT_BIND_KEY_UNI_IDX] = { "bind-key-uni", required_argument, 0, 0 },

#define OPT_BIND_KEY_KSM_IDX 49
    [OPT_BIND_KEY_KSM_IDX] = { "bind-key-ksm", required_argument, 0, 0 },

#define OPT_BIND_KEY_DEBUG_IDX 50
    [OPT_BIND_KEY_DEBUG_IDX] = { "bind-key-debug", required_argument, 0, 0 },

#define OPT_BIND_KEY_QUIT_IDX 51
    [OPT_BIND_KEY_QUIT_IDX] = { "bind-key-quit", required_argument, 0, 0 },

#define OPT_DEBUG_PTY_IDX 52
    [OPT_DEBUG_PTY_IDX] = { "debug-pty", no_argument, 0, 'D' },

#define OPT_DEBUG_GFX_IDX 53
    [OPT_DEBUG_GFX_IDX] = { "debug-gfx", no_argument, 0, 'G' },

#define OPT_VERSION_IDX 54
    [OPT_VERSION_IDX] = { "version", no_argument, 0, 'v' },

#define OPT_HELP_IDX 55
    [OPT_HELP_IDX] = { "help", no_argument, 0, 'h' },

#define OPT_SENTINEL_IDX 56
    [OPT_SENTINEL_IDX] = { 0 }
};

static const char* long_options_descriptions[][2] = {
    [OPT_CONFIG_FILE_IDX]   = { arg_path, "Use configuration file" },
    [OPT_SKIP_CONFIG_IDX]   = { NULL, "Ignore default configuration file" },
    [OPT_XORG_ONLY_IDX]     = { NULL, "Always use X11" },
    [OPT_TERM_IDX]          = { arg_string, "TERM value" },
    [OPT_TITLE_IDX]         = { arg_string, "Window title and application class name" },
    [OPT_DYNAMIC_TITLE_IDX] = { NULL, "Do not allow programs to change the "
                                      "window title" },
    [OPT_TITLE_FORMAT_IDX]  = { arg_string, "Window title format string" },
    [OPT_LOCALE_IDX]        = { arg_string, "Override locale" },
    [OPT_ROWS_IDX]          = { arg_int, "Number of rows" },
    [OPT_COLUMNS_IDX]       = { arg_int, "Number of columns" },
    [OPT_BG_COLOR_IDX]      = { arg_color_a, "Background color" },
    [OPT_FG_COLOR_IDX]      = { arg_color, "Foreground color" },
    [OPT_COLOR_0_IDX]       = { arg_color, "Palette color black" },
    [OPT_COLOR_1_IDX]       = { arg_color, "Palette color red" },
    [OPT_COLOR_2_IDX]       = { arg_color, "Palette color green" },
    [OPT_COLOR_3_IDX]       = { arg_color, "Palette color yellow" },
    [OPT_COLOR_4_IDX]       = { arg_color, "Palette color blue" },
    [OPT_COLOR_5_IDX]       = { arg_color, "Palette color magenta" },
    [OPT_COLOR_6_IDX]       = { arg_color, "Palette color cyan" },
    [OPT_COLOR_7_IDX]       = { arg_color, "Palette color grey" },
    [OPT_COLOR_8_IDX]       = { arg_color, "Palette color bright black" },
    [OPT_COLOR_9_IDX]       = { arg_color, "Palette color bright red" },
    [OPT_COLOR_10_IDX]      = { arg_color, "Palette color bright green" },
    [OPT_COLOR_11_IDX]      = { arg_color, "Palette color bright yellow" },
    [OPT_COLOR_12_IDX]      = { arg_color, "Palette color bright blue" },
    [OPT_COLOR_13_IDX]      = { arg_color, "Palette color bright magenta" },
    [OPT_COLOR_14_IDX]      = { arg_color, "Palette color bright cyan" },
    [OPT_COLOR_15_IDX]      = { arg_color, "Palette color bright grey" },
    [OPT_FG_COLOR_DIM_IDX]  = { arg_color, "Dim/faint text color" },
    [OPT_H_BG_COLOR_IDX]    = { arg_color_a, "Highlighted text background color" },
    [OPT_H_FG_COLOR_IDX]    = { arg_color, "Highlighted text foreground color" },
    [OPT_NO_FLASH_IDX]      = { NULL, "Disable visual bell" },
    [OPT_COLORSCHEME_IDX]   = { arg_name, "Colorscheme preset: wayst, linux, xterm, rxvt, yaru, "
                                        "tango, orchis, solarized" },

    [OPT_FONT_IDX]               = { arg_name, "Primary font family" },
    [OPT_FONT_STYLE_REGULAR_IDX] = { arg_name, "Font style to use as default" },
    [OPT_FONT_STYLE_BOLD_IDX]    = { arg_name, "Font style to use as bold" },
    [OPT_FONT_STYLE_ITALIC_IDX]  = { arg_name, "Font style to use as italic " },
    [OPT_FONT_FALLBACK_IDX]      = { arg_name, "Fallback font family" },
    [OPT_FONT_FALLBACK2_IDX]     = { arg_name, "Second fallback font family" },

    [OPT_FONT_SIZE_IDX] = { arg_int, "Font size" },
    [OPT_DPI_IDX]       = { arg_int, "Font dpi" },
    [OPT_BLINK_IDX]     = { "bool:R?:S?:E?",
                        "Blinking cursor enable:rate[ms]:suspend[ms]:end[s](<0 to disable)" },

    [OPT_SCROLL_LINES_IDX] = { arg_int, "Lines scrolled per wheel click" },
    [OPT_SCROLLBACK_IDX]   = { arg_int, "Size of scrollback buffer" },

    [OPT_BIND_KEY_COPY_IDX]    = { arg_key, "Copy key command" },
    [OPT_BIND_KEY_PASTE_IDX]   = { arg_key, "Paste key command" },
    [OPT_BIND_KEY_ENLARGE_IDX] = { arg_key, "Enlagre font key command" },
    [OPT_BIND_KEY_SHRINK_IDX]  = { arg_key, "Shrink font key command" },
    [OPT_BIND_KEY_UNI_IDX]     = { arg_key, "Unicode entry mode activation "
                                        "key command" },
    [OPT_BIND_KEY_KSM_IDX]     = { arg_key, "Enter keyboard select mode key command" },
    [OPT_BIND_KEY_DEBUG_IDX]   = { arg_key, "Debug info key command" },
    [OPT_BIND_KEY_QUIT_IDX]    = { arg_key, "Quit key command" },

    [OPT_DEBUG_PTY_IDX] = { NULL, "Output pty communication to stderr" },
    [OPT_DEBUG_GFX_IDX] = { NULL, "Run renderer in debug mode" },
    [OPT_VERSION_IDX]   = { NULL, "Show version" },
    [OPT_HELP_IDX]      = { NULL, "Show this message" },

    [OPT_SENTINEL_IDX] = { NULL, NULL }
};
