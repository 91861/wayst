/* See LICENSE for license information. */

/**
 * Options
 **/

#pragma once

#include <assert.h>
#include <getopt.h>
#include <stddef.h>

#include "util.h"

static const char* const arg_path    = "path";
static const char* const arg_int     = "int";
static const char* const arg_color   = "#RRGGBB";
static const char* const arg_color_a = "#RRGGBBAA";
static const char* const arg_string  = "string";
static const char* const arg_key     = "key";
static const char* const arg_name    = "name";
static const char* const arg_bool    = "bool";

// -e and -x are reserved
static struct option long_options[] = {

#define OPT_CONFIG_FILE_IDX 0
    [OPT_CONFIG_FILE_IDX] = { "config-file", required_argument, 0, 0 },

#define OPT_SKIP_CONFIG_IDX 1
    [OPT_SKIP_CONFIG_IDX] = { "skip-config", no_argument, 0, 'c' },

#define OPT_XORG_ONLY_IDX 2
    [OPT_XORG_ONLY_IDX] = { "xorg-only", no_argument, 0, 'X' },

#define OPT_TERM_IDX 3
    [OPT_TERM_IDX] = { "term", required_argument, 0, 0 },

#define OPT_VTE_VERSION_IDX 4
    [OPT_VTE_VERSION_IDX] = { "vte-version", required_argument, 0, 0 },

#define OPT_TITLE_IDX 5
    [OPT_TITLE_IDX] = { "title", required_argument, 0, 0 },

#define OPT_HOLD 6
    [OPT_HOLD] = { "hold", optional_argument, 0, 'H' },

#define OPT_DYNAMIC_TITLE_IDX 7
    [OPT_DYNAMIC_TITLE_IDX] = { "no-dynamic-title", no_argument, 0, 't' },

#define OPT_TITLE_FORMAT_IDX 8
    [OPT_TITLE_FORMAT_IDX] = { "title-format", required_argument, 0, 0 },

#define OPT_APP_ID_IDX 9
    [OPT_APP_ID_IDX] = { "app-id", required_argument, 0, 0 },

#define OPT_LOCALE_IDX 10
    [OPT_LOCALE_IDX] = { "locale", required_argument, 0, 0 },

#define OPT_ROWS_IDX 11
    [OPT_ROWS_IDX] = { "rows", required_argument, 0, 0 },

#define OPT_DECORATIONS 12
    [OPT_DECORATIONS] = { "decorations", required_argument, 0, 0 },

#define OPT_HIDE_POINTER 13
    [OPT_HIDE_POINTER] = { "hide-pointer", required_argument, 0, 0 },

#define OPT_SMOOTH_CURSOR 14
    [OPT_SMOOTH_CURSOR] = { "cursor-animations", optional_argument, 0, 'a' },

#define OPT_COLUMNS_IDX 15
    [OPT_COLUMNS_IDX] = { "columns", required_argument, 0, 0 },

#define OPT_DIRECTORY_IDX 16
    [OPT_DIRECTORY_IDX] = { "directory", required_argument, 0, 0 },

#define OPT_WINDOWOPS_IDX 17
    [OPT_WINDOWOPS_IDX] = { "windowops", required_argument, 0, 0 },

#define OPT_IO_CHUNK_DELAY 18
    [OPT_IO_CHUNK_DELAY] = { "io-chunk-delay", required_argument, 0, 0 },

#define OPT_BG_COLOR_IDX 19
    [OPT_BG_COLOR_IDX] = { "bg-color", required_argument, 0, 0 },

#define OPT_FG_COLOR_IDX 20
    [OPT_FG_COLOR_IDX] = { "fg-color", required_argument, 0, 0 },

#define OPT_COLOR_0_IDX 21
    [OPT_COLOR_0_IDX] = { "color-0", required_argument, 0, 0 },

#define OPT_COLOR_1_IDX 22
    [OPT_COLOR_1_IDX] = { "color-1", required_argument, 0, 0 },

#define OPT_COLOR_2_IDX 23
    [OPT_COLOR_2_IDX] = { "color-2", required_argument, 0, 0 },

#define OPT_COLOR_3_IDX 24
    [OPT_COLOR_3_IDX] = { "color-3", required_argument, 0, 0 },

#define OPT_COLOR_4_IDX 25
    [OPT_COLOR_4_IDX] = { "color-4", required_argument, 0, 0 },

#define OPT_COLOR_5_IDX 26
    [OPT_COLOR_5_IDX] = { "color-5", required_argument, 0, 0 },

#define OPT_COLOR_6_IDX 27
    [OPT_COLOR_6_IDX] = { "color-6", required_argument, 0, 0 },

#define OPT_COLOR_7_IDX 28
    [OPT_COLOR_7_IDX] = { "color-7", required_argument, 0, 0 },

#define OPT_COLOR_8_IDX 29
    [OPT_COLOR_8_IDX] = { "color-8", required_argument, 0, 0 },

#define OPT_COLOR_9_IDX 30
    [OPT_COLOR_9_IDX] = { "color-9", required_argument, 0, 0 },

#define OPT_COLOR_10_IDX 31
    [OPT_COLOR_10_IDX] = { "color-10", required_argument, 0, 0 },

#define OPT_COLOR_11_IDX 32
    [OPT_COLOR_11_IDX] = { "color-11", required_argument, 0, 0 },

#define OPT_COLOR_12_IDX 33
    [OPT_COLOR_12_IDX] = { "color-12", required_argument, 0, 0 },

#define OPT_COLOR_13_IDX 34
    [OPT_COLOR_13_IDX] = { "color-13", required_argument, 0, 0 },

#define OPT_COLOR_14_IDX 35
    [OPT_COLOR_14_IDX] = { "color-14", required_argument, 0, 0 },

#define OPT_COLOR_15_IDX 36
    [OPT_COLOR_15_IDX] = { "color-15", required_argument, 0, 0 },

#define OPT_C_BG_COLOR_IDX 37
    [OPT_C_BG_COLOR_IDX] = { "cursor-bg-color", required_argument, 0, 0 },

#define OPT_C_FG_COLOR_IDX 38
    [OPT_C_FG_COLOR_IDX] = { "cursor-fg-color", required_argument, 0, 0 },

#define OPT_H_BG_COLOR_IDX 39
    [OPT_H_BG_COLOR_IDX] = { "highlihgt-bg-color", required_argument, 0, 0 },

#define OPT_H_FG_COLOR_IDX 40
    [OPT_H_FG_COLOR_IDX] = { "highlight-fg-color", required_argument, 0, 0 },

#define OPT_VISUAL_BELL 41
    [OPT_VISUAL_BELL] = { "visual-bell", required_argument, 0, 0 },

#define OPT_BOLD_IS_BRIGHT 42
    [OPT_BOLD_IS_BRIGHT] = { "bold-is-bright", required_argument, 0, 0 },

#define OPT_COLORSCHEME_IDX 43
    [OPT_COLORSCHEME_IDX] = { "colorscheme", required_argument, 0, 0 },

#define OPT_UNFOCUSED_TINT_COLOR 44
    [OPT_UNFOCUSED_TINT_COLOR] = { "unfocused-tint", required_argument, 0, 0 },

#define OPT_FONT_IDX 45
    [OPT_FONT_IDX] = { "font", required_argument, 0, 0 },

#define OPT_FONT_STYLE_REGULAR_IDX 46
    [OPT_FONT_STYLE_REGULAR_IDX] = { "style-regular", required_argument, 0, 0 },

#define OPT_FONT_STYLE_BOLD_IDX 47
    [OPT_FONT_STYLE_BOLD_IDX] = { "style-bold", required_argument, 0, 0 },

#define OPT_FONT_STYLE_ITALIC_IDX 48
    [OPT_FONT_STYLE_ITALIC_IDX] = { "style-italic", required_argument, 0, 0 },

#define OPT_FONT_STYLE_BOLD_ITALIC_IDX 49
    [OPT_FONT_STYLE_BOLD_ITALIC_IDX] = { "style-bolditalic", required_argument, 0, 0 },

#define OPT_FONT_FALLBACK_IDX 50
    [OPT_FONT_FALLBACK_IDX] = { "font-symbol", required_argument, 0, 0 },

#define OPT_FONT_FALLBACK2_IDX 51
    [OPT_FONT_FALLBACK2_IDX] = { "font-color", required_argument, 0, 0 },

#define OPT_FLUSH_FC_CACHE_IDX 52
    [OPT_FLUSH_FC_CACHE_IDX] = { "flush-fc-cache", no_argument, 0, 'l' },

#define OPT_PRELOAD_ALL_FONTS_IDX 53
    [OPT_PRELOAD_ALL_FONTS_IDX] = { "preload-all-fonts", no_argument, 0, 'o' },

#define OPT_EXCLUDE_LCD_IDX 54
    [OPT_EXCLUDE_LCD_IDX] = { "exclude-lcd", required_argument, 0, 0 },

#define OPT_FONT_SIZE_IDX 55
    [OPT_FONT_SIZE_IDX] = { "font-size", required_argument, 0, 0 },

#define OPT_DPI_IDX 56
    [OPT_DPI_IDX] = { "dpi", required_argument, 0, 0 },

#define OPT_GLYPH_PADDING_IDX 57
    [OPT_GLYPH_PADDING_IDX] = { "glyph-padding", required_argument, 0, 0 },

#define OPT_GLYPH_ALIGN_IDX 58
    [OPT_GLYPH_ALIGN_IDX] = { "glyph-align", required_argument, 0, 0 },

#define OPT_LCD_ORDER_IDX 59
    [OPT_LCD_ORDER_IDX] = { "lcd-order", required_argument, 0, 0 },

#define OPT_CURSOR_STYLE_IDX 60
    [OPT_CURSOR_STYLE_IDX] = { "cursor-style", required_argument, 0, 0 },

#define OPT_BLINK_IDX 61
    [OPT_BLINK_IDX] = { "blink", required_argument, 0, 0 },

#define OPT_PADDING_IDX 62
    [OPT_PADDING_IDX] = { "padding", required_argument, 0, 0 },

#define OPT_SCROLLBAR_IDX 63
    [OPT_SCROLLBAR_IDX] = { "scrollbar", required_argument, 0, 0 },

#define OPT_SCROLL_LINES_IDX 64
    [OPT_SCROLL_LINES_IDX] = { "scroll-lines", required_argument, 0, 0 },

#define OPT_SCROLLBACK_IDX 65
    [OPT_SCROLLBACK_IDX] = { "scrollback", required_argument, 0, 0 },

#define OPT_URI_HANDLER_IDX 66
    [OPT_URI_HANDLER_IDX] = { "uri-handler", required_argument, 0, 0 },

#define OPT_EXTERN_PIPE_HANDLER_IDX 67
    [OPT_EXTERN_PIPE_HANDLER_IDX] = { "extern-pipe", required_argument, 0, 0 },

#define OPT_BIND_KEY_COPY_IDX 68
    [OPT_BIND_KEY_COPY_IDX] = { "bind-key-copy", required_argument, 0, 0 },

#define OPT_BIND_KEY_PASTE_IDX 69
    [OPT_BIND_KEY_PASTE_IDX] = { "bind-key-paste", required_argument, 0, 0 },

#define OPT_BIND_KEY_ENLARGE_IDX 70
    [OPT_BIND_KEY_ENLARGE_IDX] = { "bind-key-enlarge", required_argument, 0, 0 },

#define OPT_BIND_KEY_SHRINK_IDX 71
    [OPT_BIND_KEY_SHRINK_IDX] = { "bind-key-shrink", required_argument, 0, 0 },

#define OPT_BIND_KEY_UNI_IDX 72
    [OPT_BIND_KEY_UNI_IDX] = { "bind-key-unicode", required_argument, 0, 0 },

#define OPT_BIND_KEY_PG_UP_IDX 73
    [OPT_BIND_KEY_PG_UP_IDX] = { "bind-key-pg-up", required_argument, 0, 0 },

#define OPT_BIND_KEY_PG_DN_IDX 74
    [OPT_BIND_KEY_PG_DN_IDX] = { "bind-key-pg-down", required_argument, 0, 0 },

#define OPT_BIND_KEY_LN_UP_IDX 75
    [OPT_BIND_KEY_LN_UP_IDX] = { "bind-key-ln-up", required_argument, 0, 0 },

#define OPT_BIND_KEY_LN_DN_IDX 76
    [OPT_BIND_KEY_LN_DN_IDX] = { "bind-key-ln-down", required_argument, 0, 0 },

#define OPT_BIND_KEY_MRK_UP_IDX 77
    [OPT_BIND_KEY_MRK_UP_IDX] = { "bind-key-mark-up", required_argument, 0, 0 },

#define OPT_BIND_KEY_MRK_DN_IDX 78
    [OPT_BIND_KEY_MRK_DN_IDX] = { "bind-key-mark-down", required_argument, 0, 0 },

#define OPT_BIND_KEY_COPY_CMD_IDX 79
    [OPT_BIND_KEY_COPY_CMD_IDX] = { "bind-key-copy-output", required_argument, 0, 0 },

#define OPT_BIND_KEY_EXTERN_PIPE_IDX 80
    [OPT_BIND_KEY_EXTERN_PIPE_IDX] = { "bind-key-extern-pipe", required_argument, 0, 0 },

#define OPT_BIND_KEY_KSM_IDX 81
    [OPT_BIND_KEY_KSM_IDX] = { "bind-key-kbd-select", required_argument, 0, 0 },

#define OPT_BIND_KEY_OPEN_PWD 82
    [OPT_BIND_KEY_OPEN_PWD] = { "bind-key-open-pwd", required_argument, 0, 0 },

#define OPT_BIND_KEY_HTML_DUMP_IDX 83
    [OPT_BIND_KEY_HTML_DUMP_IDX] = { "bind-key-html-dump", required_argument, 0, 0 },

#define OPT_BIND_KEY_DUP_IDX 84
    [OPT_BIND_KEY_DUP_IDX] = { "bind-key-duplicate", required_argument, 0, 0 },

#define OPT_BIND_KEY_DEBUG_IDX 85
    [OPT_BIND_KEY_DEBUG_IDX] = { "bind-key-debug", required_argument, 0, 0 },

#define OPT_BIND_KEY_QUIT_IDX 86
    [OPT_BIND_KEY_QUIT_IDX] = { "bind-key-quit", required_argument, 0, 0 },

#define OPT_DEBUG_PTY_IDX 87
    [OPT_DEBUG_PTY_IDX] = { "debug-pty", no_argument, 0, 'D' },

#define OPT_DEBUG_VT_IDX 88
    [OPT_DEBUG_VT_IDX] = { "debug-vt", required_argument, 0, 0 },

#define OPT_DEBUG_GFX_IDX 89
    [OPT_DEBUG_GFX_IDX] = { "debug-gfx", no_argument, 0, 'G' },

#define OPT_DEBUG_FONT_IDX 90
    [OPT_DEBUG_FONT_IDX] = { "debug-font", no_argument, 0, 'F' },

#define OPT_VERSION_IDX 91
    [OPT_VERSION_IDX] = { "version", no_argument, 0, 'v' },

#define OPT_HELP_IDX 92
    [OPT_HELP_IDX] = { "help", no_argument, 0, 'h' },

#define OPT_SENTINEL_IDX 93
    [OPT_SENTINEL_IDX] = { 0 }
};

static const char* long_options_descriptions[][2] = {
    [OPT_CONFIG_FILE_IDX] = { arg_path, "Use configuration file" },
    [OPT_SKIP_CONFIG_IDX] = { NULL, "Ignore default configuration file" },
    [OPT_XORG_ONLY_IDX]   = { NULL, "Always use X11" },
    [OPT_TERM_IDX]        = { arg_string, "TERM value (default: xterm-256color)" },
    [OPT_VTE_VERSION_IDX] = { arg_string,
                              "Report VTE compatibility, might break some programs (empty to "
                              "disable) (default: 6201)" },
    [OPT_HOLD]            = { "bool?", "Retain window after program exit" },

    [OPT_TITLE_IDX]         = { arg_string, "Window title" },
    [OPT_DYNAMIC_TITLE_IDX] = { NULL, "Do not allow programs to change the window title" },
    [OPT_TITLE_FORMAT_IDX]  = { arg_string, "Window title format string" },
    [OPT_APP_ID_IDX]        = { "[string, string?]", "Application ID / WM_CLASS" },
    [OPT_LOCALE_IDX]        = { arg_string, "Override locale" },

    [OPT_COLUMNS_IDX] = { arg_int, "Number of columns (default: 80)" },
    [OPT_ROWS_IDX]    = { arg_int, "Number of rows (default: 24)" },

    [OPT_DECORATIONS] = { arg_name, "Window decoration type: full, minimal, none (default: full)" },

    [OPT_HIDE_POINTER] = { arg_name,
                           "Hide pointer: true, false, force_true, force_false, if_not_reporting "
                           "(default: true)" },

    [OPT_SMOOTH_CURSOR] = { "bool?", "Use cursor animations" },

    [OPT_DIRECTORY_IDX] = { arg_path, "Start in directory" },

    [OPT_WINDOWOPS_IDX] = { "bool:bool?",
                            "Allow window interactions - manipulation:information (default: "
                            "true:true)" },

    [OPT_IO_CHUNK_DELAY] = { "int:int?",
                             "Wait for following chunks - time[usec]:timeout[ms] (default: 0:5)" },

    [OPT_BG_COLOR_IDX] = { arg_color_a, "Background color" },
    [OPT_FG_COLOR_IDX] = { arg_color, "Foreground color" },
    [OPT_COLOR_0_IDX]  = { arg_color, "Palette color black" },
    [OPT_COLOR_1_IDX]  = { arg_color, "Palette color red" },
    [OPT_COLOR_2_IDX]  = { arg_color, "Palette color green" },
    [OPT_COLOR_3_IDX]  = { arg_color, "Palette color yellow" },
    [OPT_COLOR_4_IDX]  = { arg_color, "Palette color blue" },
    [OPT_COLOR_5_IDX]  = { arg_color, "Palette color magenta" },
    [OPT_COLOR_6_IDX]  = { arg_color, "Palette color cyan" },
    [OPT_COLOR_7_IDX]  = { arg_color, "Palette color gray" },
    [OPT_COLOR_8_IDX]  = { arg_color, "Palette color bright black" },
    [OPT_COLOR_9_IDX]  = { arg_color, "Palette color bright red" },
    [OPT_COLOR_10_IDX] = { arg_color, "Palette color bright green" },
    [OPT_COLOR_11_IDX] = { arg_color, "Palette color bright yellow" },
    [OPT_COLOR_12_IDX] = { arg_color, "Palette color bright blue" },
    [OPT_COLOR_13_IDX] = { arg_color, "Palette color bright magenta" },
    [OPT_COLOR_14_IDX] = { arg_color, "Palette color bright cyan" },
    [OPT_COLOR_15_IDX] = { arg_color, "Palette color bright gray" },

    [OPT_C_BG_COLOR_IDX] = { "#RRGGBBAA/none", "Cursor background color" },
    [OPT_C_FG_COLOR_IDX] = { "#RRGGBB/none", "Cursor foreground color" },

    [OPT_H_BG_COLOR_IDX] = { arg_color_a, "Highlighted text background color" },
    [OPT_H_FG_COLOR_IDX] = { arg_color, "Highlighted text foreground color" },

    [OPT_VISUAL_BELL]     = { "#RRGGBBAA/name", "Visual bell effect - overlay color/none" },
    [OPT_COLORSCHEME_IDX] = { "name/int",
                              "Colorscheme name/index: wayst, linux, xterm, rxvt, yaru, tango, "
                              "orchis, "
                              "solarized" },

    [OPT_BOLD_IS_BRIGHT] = { arg_bool, "Show bold text in bright colors (default: true)" },

    [OPT_UNFOCUSED_TINT_COLOR] = { arg_color_a, "Tint window when out of focus" },

    [OPT_FONT_IDX]               = { "[font, ...]", "Primary fonts (default: Monospace)" },
    [OPT_FONT_STYLE_REGULAR_IDX] = { arg_name, "Font style to use as default (default: Regular)" },
    [OPT_FONT_STYLE_BOLD_IDX]    = { arg_name, "Font style to use as bold (default: Bold)" },
    [OPT_FONT_STYLE_ITALIC_IDX]  = { arg_name, "Font style to use as italic (default: Italic)" },
    [OPT_FONT_STYLE_BOLD_ITALIC_IDX] = { arg_name,
                                         "Font style to use as bold italic (default: "
                                         "Bold:Italic)" },
    [OPT_FONT_FALLBACK_IDX]          = { "[font, ...]", "Symbol fonts" },
    [OPT_FONT_FALLBACK2_IDX]         = { "[font, ...]", "Color bitmap fonts" },

    [OPT_FLUSH_FC_CACHE_IDX]    = { NULL, "Rebuild fontconfig cache" },
    [OPT_PRELOAD_ALL_FONTS_IDX] = { NULL, "Load all fonts on startup" },

    [OPT_EXCLUDE_LCD_IDX] = { "[chr..chr, ...]", "Ranges of characters without lcd filtering" },

    [OPT_FONT_SIZE_IDX]     = { arg_int, "Font size" },
    [OPT_DPI_IDX]           = { arg_int, "Font dpi (default: 96)" },
    [OPT_GLYPH_PADDING_IDX] = { "int:int?", "Glyph padding - horizontal[px]:vertical[px]" },
    [OPT_GLYPH_ALIGN_IDX]   = { "char:int?:int?",
                              "Adjust glyph alignment - center character:y offset[px]:x offset[px] "
                              "(default: (:0:0)" },
    [OPT_LCD_ORDER_IDX]     = { arg_name, "Force LCD subpixel order: none, rgb, bgr, vrgb, vbgr" },

    [OPT_CURSOR_STYLE_IDX] = { "name:bool?",
                               "Set initial cursor style - block/beam/underline:blinking (default: "
                               "block:true)" },
    [OPT_BLINK_IDX]        = { "bool:int?:int?:int?",
                        "Blinking cursor - enable:rate[ms]:suspend[ms]:end[s](<0 never)" },

    [OPT_SCROLL_LINES_IDX]        = { arg_int, "Lines scrolled per wheel click (default: 3)" },
    [OPT_SCROLLBACK_IDX]          = { arg_int, "Scrollback buffer size (default: 2000)" },
    [OPT_URI_HANDLER_IDX]         = { arg_string, "URI handler program (default: xdg-open)" },
    [OPT_EXTERN_PIPE_HANDLER_IDX] = { "string:name?",
                                      "Extern pipe handler and mode - "
                                      "executable:command/screen/buffer "
                                      "(default: none:command)" },
    [OPT_PADDING_IDX]             = { "bool:int?",
                          "Pad screen content: center:extra padding[px] (default: true:0)" },
    [OPT_SCROLLBAR_IDX]           = { "int:int?:int?:int?",
                            "width[px]:min length[px]:hide delay[ms]:fade time[ms] (default: "
                            "10:20:1500:150)" },

    [OPT_BIND_KEY_COPY_IDX]    = { arg_key, "Copy key command (default: C+S+c)" },
    [OPT_BIND_KEY_PASTE_IDX]   = { arg_key, "Paste key command (default: C+S+v)" },
    [OPT_BIND_KEY_ENLARGE_IDX] = { arg_key, "Enlagre font key command (default: C+S+equal)" },
    [OPT_BIND_KEY_SHRINK_IDX]  = { arg_key, "Shrink font key command (default: C+S+minus)" },
    [OPT_BIND_KEY_UNI_IDX]     = { arg_key,
                               "Unicode entry mode activation key command (default: C+S+u)" },

    [OPT_BIND_KEY_LN_UP_IDX] = { arg_key, "Scroll up by line key command (default: C+S+Up)" },
    [OPT_BIND_KEY_LN_DN_IDX] = { arg_key, "Scroll down by line key command (default: C+S+Down)" },
    [OPT_BIND_KEY_PG_UP_IDX] = { arg_key, "Scroll up by page key command (default: C+S+Page_Up)" },
    [OPT_BIND_KEY_PG_DN_IDX] = { arg_key,
                                 "Scroll down by page key command (default: C+S+Page_Down)" },

    [OPT_BIND_KEY_MRK_UP_IDX] = { arg_key,
                                  "Scroll up to previous mark or command output key command "
                                  "(default: "
                                  "C+S+Left)" },
    [OPT_BIND_KEY_MRK_DN_IDX] = { arg_key,
                                  "Scroll down to next mark or command output key command "
                                  "(default: "
                                  "C+S+Right)" },

    [OPT_BIND_KEY_COPY_CMD_IDX]    = { arg_key,
                                    "Copy last command output to clipboard key command (default: "
                                    "C+S+x)" },
    [OPT_BIND_KEY_EXTERN_PIPE_IDX] = { arg_key,
                                       "Pipe content to external program (default: "
                                       "C+S+backslash)" },

    [OPT_BIND_KEY_KSM_IDX] = { arg_key, "Enter keyboard select mode key command (default: C+S+k)" },
    [OPT_BIND_KEY_HTML_DUMP_IDX] = { arg_key, "HTML screen dump key command (default: C+S+F12)" },
    [OPT_BIND_KEY_OPEN_PWD]      = { arg_key, "Open working directory as URI (default: C+S+F10)" },
    [OPT_BIND_KEY_DUP_IDX]       = { arg_key,
                               "New instance in work directory key command (default: C+S+d)" },

    [OPT_BIND_KEY_DEBUG_IDX] = { arg_key, "Debug info key command (default: C+S+slash)" },
    [OPT_BIND_KEY_QUIT_IDX]  = { arg_key, "Quit key command" },

    [OPT_DEBUG_PTY_IDX]  = { NULL, "Output pty communication to stderr" },
    [OPT_DEBUG_VT_IDX]   = { "int?", "Slow down the interpreter to usec/byte (default: 5000)" },
    [OPT_DEBUG_GFX_IDX]  = { NULL, "Run renderer in debug mode" },
    [OPT_DEBUG_FONT_IDX] = { NULL, "Show font information" },
    [OPT_VERSION_IDX]    = { NULL, "Show version" },
    [OPT_HELP_IDX]       = { NULL, "Show this message" },

    [OPT_SENTINEL_IDX] = { NULL, NULL }
};

static_assert(ARRAY_SIZE(long_options) == ARRAY_SIZE(long_options_descriptions),
              "all_options_have_descriptions");
