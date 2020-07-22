/* See LICENSE for license information. */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <fontconfig/fontconfig.h>

#include "fontconfig.h"
#include "opts.h"
#include "settings.h"
#include "util.h"
#include "vector.h"

// configuration file subdirectory
#ifndef CFG_SDIR_NAME
#define CFG_SDIR_NAME "wayst"
#endif

// configuration file name
#ifndef CFG_FNAME
#define CFG_FNAME "config"
#endif

// executable name
#ifndef EXE_FNAME
#define EXE_FNAME "wayst"
#endif

// application name
#ifndef APP_NAME
#define APP_NAME "Wayst"
#endif

// default title format
#ifndef DFT_TITLE_FMT
#define DFT_TITLE_FMT "%2$s - %1$s"
#endif

// default term value
#ifndef DFT_TERM
#define DFT_TERM "xterm-256color"
#endif

// point size in inches for converting font sizes
#define PT_AS_INCH 0.0138889

DEF_VECTOR(char, NULL);

ColorRGB color_palette_256[257];

const char* const colors_default[8][18] = {
    {
      /* wayst */
      "000000",
      "AB1F00",
      "D2FF00",
      "FF7D00",
      "00518C",
      "B7006F",
      "00AEA0",
      "AAAAAA",
      "545454",
      "BB3939",
      "AFFF52",
      "FFA855",
      "107BC9",
      "FF368A",
      "40FFEF",
      "FFFFFF",
      "000000EE",
      "FFFFFF",
    },
    {
      /* linux tty */
      "000000",
      "AA0000",
      "00AA00",
      "AA5500",
      "0000AA",
      "AA00AA",
      "00AAAA",
      "AAAAAA",
      "555555",
      "FF5555",
      "55FF55",
      "FFFF55",
      "5555FF",
      "FF55FF",
      "55FFFF",
      "FFFFFF",
      "000000",
      "FFFFFF",
    },
    {
      /* xterm */
      "000000",
      "CD0000",
      "00CD00",
      "CDCD00",
      "0000EE",
      "CD00CD",
      "00CDCD",
      "E5E5E5",
      "7F7F7F",
      "FF0000",
      "00FF00",
      "FFFF00",
      "5C5CFF",
      "FF00FF",
      "00FFFF",
      "FFFFFF",
      "000000",
      "FFFFFF",
    },
    {
      /* rxvt */
      "000000",
      "CD0000",
      "00CD00",
      "CDCD00",
      "0000CD",
      "CD00CD",
      "00CDCD",
      "FAEBD7",
      "404040",
      "FF0000",
      "00FF00",
      "FFFF00",
      "0000FF",
      "FF00FF",
      "00FFFF",
      "FFFFFF",
      "000000",
      "FFFFFF",
    },
    {
      /* yaru */
      "2E3436",
      "CC0000",
      "4E9A06",
      "C4A000",
      "3465A4",
      "75507B",
      "06989A",
      "D3D7CF",
      "555753",
      "EF2929",
      "8AE234",
      "FCE94F",
      "729FCF",
      "AD7FA8",
      "34E2E2",
      "EEEEEC",
      "300A24",
      "FFFFFF",
    },
    {
      /* tango */
      "000000",
      "CC0000",
      "4D9A05",
      "C3A000",
      "3464A3",
      "754F7B",
      "05979A",
      "D3D6CF",
      "545652",
      "EF2828",
      "89E234",
      "FBE84F",
      "729ECF",
      "AC7EA8",
      "34E2E2",
      "EDEDEB",
      "2D2D2D",
      "EEEEEC",
    },
    {
      /* orchis */
      "000000",
      "CC0000",
      "4D9A05",
      "C3A000",
      "3464A3",
      "754F7B",
      "05979A",
      "D3D6CF",
      "545652",
      "EF2828",
      "89E234",
      "FBE84F",
      "729ECF",
      "AC7EA8",
      "34E2E2",
      "EDEDEB",
      "303030",
      "EFEFEF",
    },
    {
      // solarized
      "073642",
      "DC322F",
      "859900",
      "B58900",
      "268BD2",
      "D33682",
      "2AA198",
      "EEE8D5",
      "002B36",
      "CB4B16",
      "586E75",
      "657B83",
      "839496",
      "6C71C4",
      "93A1A1",
      "FDF6E3",
      "002B36",
      "839496",
    },
};

Settings settings;

/**
 * Set default colorscheme  */
static void settings_colorscheme_default(uint8_t idx)
{
    if (idx >= ARRAY_SIZE(colors_default)) {
        idx = 0;
        ASSERT_UNREACHABLE
    }
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(settings._explicit_colors_set && settings._explicit_colors_set[i + 2])) {
            settings.colorscheme.color[i] = ColorRGB_from_hex(colors_default[idx][i], NULL);
        }
    }
    if (colors_default[idx][16]) {
        if (!(settings._explicit_colors_set && settings._explicit_colors_set[0])) {
            settings.bg = ColorRGBA_from_hex(colors_default[idx][16], NULL);
        }
    }
    if (colors_default[idx][17]) {
        if (!(settings._explicit_colors_set && settings._explicit_colors_set[1])) {
            settings.fg = ColorRGB_from_hex(colors_default[idx][17], NULL);
        }
    }
}

/**
 * Initialize 256 color palette */
static void init_color_palette()
{
    for (int_fast16_t i = 0; i < 257; ++i) {
        if (i < 16) {
            /* Primary - from colorscheme */
            color_palette_256[i] = settings.colorscheme.color[i];
        } else if (i < 232) {
            /* Extended */
            int_fast16_t tmp       = i - 16;
            color_palette_256[i].b = (double)((tmp % 6) * 255) / 5.0;
            color_palette_256[i].g = (double)(((tmp /= 6) % 6) * 255) / 5.0;
            color_palette_256[i].r = (double)(((tmp / 6) % 6) * 255) / 5.0;
        } else {
            /* Grayscale */
            double tmp           = (double)((i - 232) * 10 + 8) / 256.0 * 255.0;
            color_palette_256[i] = (ColorRGB){
                .r = tmp,
                .g = tmp,
                .b = tmp,
            };
        }
    }
}

/**
 * Use fontconfig to get font files */
static void find_font()
{
#define L_DROP_IF_SAME(file1, file2)                                                               \
    if (file2 && file1 && !strcmp(file1, file2)) {                                                 \
        free(file1);                                                                               \
        file1 = NULL;                                                                              \
    }

    FontconfigContext fc_context = FontconfigContext_new();
    bool              is_bitmap  = false;

    char* default_file =
      FontconfigContext_get_file(&fc_context, NULL, NULL, settings.font_size, NULL);

    char* main_family = OR(settings.font.str, "Monospace");

    char* regular_file = FontconfigContext_get_file(&fc_context,
                                                    main_family,
                                                    settings.font_style_regular.str,
                                                    settings.font_size,
                                                    &is_bitmap);
    if (is_bitmap) {
        settings.lcd_filter = LCD_FILTER_NONE;
    }

    char* bold_file = FontconfigContext_get_file(&fc_context,
                                                 main_family,
                                                 OR(settings.font_style_bold.str, "Bold"),
                                                 settings.font_size,
                                                 NULL);
    L_DROP_IF_SAME(bold_file, regular_file);

    char* italic_file = FontconfigContext_get_file(&fc_context,
                                                   main_family,
                                                   OR(settings.font_style_italic.str, "Italic"),
                                                   settings.font_size,
                                                   NULL);
    L_DROP_IF_SAME(italic_file, regular_file);

    char* bold_italic_file =
      FontconfigContext_get_file(&fc_context,
                                 main_family,
                                 OR(settings.font_style_italic.str, "Bold:Italic"),
                                 settings.font_size,
                                 NULL);
    L_DROP_IF_SAME(bold_italic_file, regular_file);
    L_DROP_IF_SAME(bold_italic_file, bold_file);
    L_DROP_IF_SAME(bold_italic_file, italic_file);

    char* fallback_file = FontconfigContext_get_file(&fc_context,
                                                     settings.font_fallback.str,
                                                     NULL,
                                                     settings.font_size,
                                                     NULL);
    L_DROP_IF_SAME(fallback_file, regular_file);
    L_DROP_IF_SAME(fallback_file, default_file);

    char* fallback2_file = FontconfigContext_get_file(&fc_context,
                                                      settings.font_fallback2.str,
                                                      NULL,
                                                      settings.font_size,
                                                      NULL);
    L_DROP_IF_SAME(fallback2_file, regular_file);
    L_DROP_IF_SAME(fallback2_file, default_file);

    if (fallback_file) {
        L_DROP_IF_SAME(fallback2_file, fallback_file);
    }

    free(default_file);
    FontconfigContext_destroy(&fc_context);

    if (regular_file) {
        AString_replace_with_dynamic(&settings.font_file_name_regular, regular_file);
    }
    if (bold_file) {
        AString_replace_with_dynamic(&settings.font_file_name_bold, bold_file);
    }
    if (italic_file) {
        AString_replace_with_dynamic(&settings.font_file_name_italic, italic_file);
    }
    if (bold_italic_file) {
        AString_replace_with_dynamic(&settings.font_file_name_bold_italic, bold_italic_file);
    }
    if (fallback_file) {
        AString_replace_with_dynamic(&settings.font_file_name_fallback, fallback_file);
    }
    if (fallback2_file) {
        AString_replace_with_dynamic(&settings.font_file_name_fallback2, fallback2_file);
    }
    if (unlikely(settings.debug_font)) {
        printf("Loaded font files:\n  regular:     %s\n  bold:        %s\n  italic:      %s\n  bold italic: %s\n  "
               "symbol:      %s\n  color:       %s\n  glyph padding: %dpx-%dpx\n",
               OR(settings.font_file_name_regular.str, "(none)"),
               OR(settings.font_file_name_bold.str, "(none)"),
               OR(settings.font_file_name_italic.str, "(none)"),
               OR(settings.font_file_name_bold_italic.str, "(none)"),
               OR(settings.font_file_name_fallback.str, "(none)"),
               OR(settings.font_file_name_fallback2.str, "(none)"),
               settings.padd_glyph_x,
               settings.padd_glyph_y);
    }
}

static void settings_make_default()
{
    settings = (Settings){
        .key_commands = {
            [KCMD_QUIT] = (KeyCommand) {
                .key.code = 0, // NoSymbol
                .is_name = false,
                .mods = 0,
            },
            [KCMD_UNICODE_ENTRY] = (KeyCommand) {
                .key.code = 117, // u
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
            [KCMD_COPY] = (KeyCommand) {
                .key.code = 99, // c
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
            [KCMD_PASTE] = (KeyCommand) {
                .key.code = 118, // v
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
            [KCMD_FONT_ENLARGE] = (KeyCommand) {
                .key.code = 61, // equal
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
            [KCMD_FONT_SHRINK] = (KeyCommand) {
                .key.code = 45, // minus
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
            [KCMD_KEYBOARD_SELECT] = (KeyCommand) {
                .key.code = 107, // k
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
            [KCMD_DEBUG] = (KeyCommand) {
                .key.code = 100, // d
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
        },

        .skip_config    = false,
        .x11_is_default = false,
        .shell_argc     = 0,

        .shell          = AString_new_uninitialized(), 
        .title_format   = AString_new_static(DFT_TITLE_FMT),
        .font           = AString_new_uninitialized(),
        .font_fallback  = AString_new_uninitialized(),
        .font_fallback2 = AString_new_uninitialized(),
        .term           = AString_new_static(DFT_TERM),
        .locale         = AString_new_uninitialized(),
        .title          = AString_new_static(APP_NAME),

        .font_style_regular     = AString_new_uninitialized(),
        .font_style_bold        = AString_new_uninitialized(),
        .font_style_italic      = AString_new_uninitialized(),
        .font_style_bold_italic = AString_new_uninitialized(),

        .font_file_name_regular     = AString_new_uninitialized(),
        .font_file_name_bold        = AString_new_uninitialized(),
        .font_file_name_italic      = AString_new_uninitialized(),
        .font_file_name_bold_italic = AString_new_uninitialized(),
        .font_file_name_fallback    = AString_new_uninitialized(),
        .font_file_name_fallback2   = AString_new_uninitialized(),

        .bsp_sends_del      = true,
        .font_size          = 9,
        .font_size_fallback = 0,
        .font_dpi           = 96,
        .lcd_filter         = LCD_FILTER_H_RGB,

        .bg     = { .r = 0, .g = 0, .b = 0, .a = 240 },
        .bghl   = { .r = 50, .g = 50, .b = 50, .a = 240 },
        .fg     = { .r = 255, .g = 255, .b = 255 },
        .fghl   = { .r = 255, .g = 255, .b = 255 },

        .highlight_change_fg = false,
        .dynamic_title       = true,

        .padding_center = true,
        .padding = 0,
        .padd_glyph_x = 0,
        .padd_glyph_y = 0,

        .cols = 80,
        .rows = 24,

        .colorscheme_preset   = 0,
        ._explicit_colors_set = calloc(1, 21),

        .bell_flash = { .r = 20, .g = 20, .b = 20, .a = 240 },

        .allow_scrollback_clear = false,
        .scroll_on_output       = false,
        .scroll_on_key          = true,
        .scroll_discrete_lines  = 3,

        .allow_multiple_underlines = false,

        .scrollback = 2000,

        .debug_pty = false,
        .debug_gfx = false,

        .enable_cursor_blink = true,
        .cursor_blink_interval_ms = 750,
        .cursor_blink_suspend_ms = 500,
        .cursor_blink_end_s = 15,
    };
}

static void settings_complete_defaults()
{
    find_font();

    // set up locale
    if (!settings.locale.str) {
        char* locale;
        locale = getenv("LC_ALL");
        if (!locale || !*locale)
            locale = getenv("LC_CTYPE");
        if (!locale || !*locale)
            locale = getenv("LANG");
        if (!locale || !*locale)
            locale = "C";

        AString_replace_with_static(&settings.locale, locale);
    }

    setlocale(LC_CTYPE, settings.locale.str);
    LOG("Using locale: %s\n", settings.locale.str);

    settings_colorscheme_default(settings.colorscheme_preset);
    free(settings._explicit_colors_set);
    settings._explicit_colors_set = NULL;
}

static void print_help_and_exit()
{
#define MAX_OPT_PADDING 25

    printf("Usage: %s [options...] [-e/x command args...]\n", EXE_FNAME);
    for (uint32_t i = 0; i < OPT_SENTINEL_IDX; ++i) {
        if (long_options[i].has_arg == no_argument && long_options[i].val) {
            printf(" -%c, ", long_options[i].val);
        } else {
            printf("     ");
        }
        if (long_options[i].has_arg == required_argument) {
            printf(" --%-s <%s>%-*s",
                   long_options[i].name,
                   long_options_descriptions[i][0],
                   (int)(MAX_OPT_PADDING - strlen(long_options[i].name) -
                         strlen(long_options_descriptions[i][0])),
                   "");
        } else {
            printf(" --%s %*s",
                   long_options[i].name,
                   (int)((MAX_OPT_PADDING + 2) - strlen(long_options[i].name)),
                   "");
        }
        puts(long_options_descriptions[i][1]);
    }
    exit(EXIT_SUCCESS);
}

static void handle_option(const char opt, const int array_index, const char* value)
{
    LOG("settings[%d] (%s) = %s\n", array_index, long_options[array_index].name, value);

    // short options
    if (opt) {
        switch (opt) {
            case 'X':
                settings.x11_is_default = true;
                break;
            case 't':
                settings.dynamic_title = false;
                break;
            case 'f':
                settings.no_flash = true;
                break;
            case 'v':
                printf("version: " VERSION "\n");
                exit(EXIT_SUCCESS);
                break;
            case 'D':
                settings.debug_pty = true;
                break;
            case 'G':
                settings.debug_gfx = true;
                break;
            case 'F':
                settings.debug_font = true;
                break;
            case 'h':
                print_help_and_exit();
                break;
        }
        return;
    }

    // long options
    switch (array_index) {

#define L_UNEXPECTED_EXTRA_ARG_FOR_LONG_OPT(_value)                                                \
    WRN("Unexpected extra argument \'%s\' for option \'%s\'\n",                                    \
        (_value),                                                                                  \
        long_options[array_index].name);

#define L_PROCESS_MULTI_ARG_PACK_BEGIN                                                             \
    int         argument_index = 0;                                                                \
    Vector_char buf            = Vector_new_with_capacity_char(15);                                \
    for (const char* i = value;; ++i) {                                                            \
        if (*i == ':' || *i == '\0') {                                                             \
            Vector_push_char(&buf, '\0');                                                          \
            switch (argument_index) {

#define L_PROCESS_MULTI_ARG_PACK_END                                                               \
    default:                                                                                       \
        L_UNEXPECTED_EXTRA_ARG_FOR_LONG_OPT(buf.buf);                                              \
        }                                                                                          \
        Vector_clear_char(&buf);                                                                   \
        ++argument_index;                                                                          \
        }                                                                                          \
        else { Vector_push_char(&buf, *i); }                                                       \
        if (!*i)                                                                                   \
            break;                                                                                 \
        }                                                                                          \
        Vector_destroy_char(&buf);

#define L_WARN_BAD_VALUE                                                                           \
    WRN("Unknown value \'%s\' for option \'%s\'\n", value, long_options[array_index].name);

        case OPT_XORG_ONLY_IDX:
            settings.x11_is_default = value ? strtob(value) : true;
            break;

        case OPT_DYNAMIC_TITLE_IDX:
            settings.dynamic_title = value ? strtob(value) : true;
            break;

        case OPT_NO_FLASH_IDX:
            settings.no_flash = value ? strtob(value) : true;
            break;

        case OPT_DEBUG_PTY_IDX:
            settings.debug_pty = true;
            break;

        case OPT_DEBUG_GFX_IDX:
            settings.debug_gfx = true;
            break;

        case OPT_DEBUG_FONT_IDX:
            settings.debug_font = true;
            break;

        case OPT_SCROLLBACK_IDX:
            settings.scrollback = MAX(strtol(value, NULL, 10), 0);
            break;

        case OPT_VERSION_IDX:
            printf("version: " VERSION "\n");
            exit(EXIT_SUCCESS);
            break;

        case OPT_BLINK_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN
            case 0:
                settings.enable_cursor_blink = strtob(buf.buf);
                break;
            case 1:
                settings.cursor_blink_interval_ms = strtol(buf.buf, NULL, 10);
                break;
            case 2:
                settings.cursor_blink_suspend_ms = strtol(buf.buf, NULL, 10);
                break;
            case 3:
                settings.cursor_blink_end_s = strtol(buf.buf, NULL, 10);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_PADDING_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN
            case 0:
                settings.padding_center = strtob(buf.buf);
                break;
            case 1:
                settings.padding = CLAMP(strtol(buf.buf, NULL, 10), 0, UINT8_MAX);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_GLYPH_PADDING_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN
            case 0:
                settings.padd_glyph_x = CLAMP(strtol(buf.buf, NULL, 10), 0, INT8_MAX);
                break;
            case 1:
                settings.padd_glyph_y = CLAMP(strtol(buf.buf, NULL, 10), 0, INT8_MAX);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_HELP_IDX:
            print_help_and_exit();
            break;

        case OPT_FONT_IDX:
            AString_replace_with_dynamic(&settings.font, strdup(value));
            break;

        case OPT_FONT_FALLBACK_IDX:
            AString_replace_with_dynamic(&settings.font_fallback, strdup(value));
            break;

        case OPT_FONT_FALLBACK2_IDX:
            AString_replace_with_dynamic(&settings.font_fallback2, strdup(value));
            break;

        case OPT_FONT_STYLE_REGULAR_IDX:
            AString_replace_with_dynamic(&settings.font_style_regular, strdup(value));
            break;

        case OPT_FONT_STYLE_BOLD_IDX:
            AString_replace_with_dynamic(&settings.font_style_bold, strdup(value));
            break;

        case OPT_FONT_STYLE_ITALIC_IDX:
            AString_replace_with_dynamic(&settings.font_style_italic, strdup(value));
            break;

        case OPT_FONT_SIZE_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN
            case 0:
                settings.font_size = strtoul(buf.buf, NULL, 10);
                break;
            case 1:
                settings.font_size_fallback = strtoul(buf.buf, NULL, 10);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_DPI_IDX:
            settings.font_dpi = strtoul(value, NULL, 10);
            break;

        case OPT_COLORSCHEME_IDX:
            if (!strcasecmp(value, "wayst")) {
                settings.colorscheme_preset = 0;
            } else if (!strcasecmp(value, "linux")) {
                settings.colorscheme_preset = 1;
            } else if (!strcasecmp(value, "xterm")) {
                settings.colorscheme_preset = 2;
            } else if (!strcasecmp(value, "rxvt")) {
                settings.colorscheme_preset = 3;
            } else if (!strcasecmp(value, "yaru")) {
                settings.colorscheme_preset = 4;
            } else if (!strcasecmp(value, "tango")) {
                settings.colorscheme_preset = 5;
            } else if (!strcasecmp(value, "orchis")) {
                settings.colorscheme_preset = 6;
            } else if (!strcasecmp(value, "solarized")) {
                settings.colorscheme_preset = 7;
            } else {
                settings.colorscheme_preset =
                  MIN(strtoul(value, NULL, 10), sizeof(colors_default) - 1);
            }
            break;

        case OPT_LCD_ORDER_IDX:
            if (!strcasecmp(value, "rgb")) {
                settings.lcd_filter = LCD_FILTER_H_RGB;
            } else if (!strcasecmp(value, "bgr")) {
                settings.lcd_filter = LCD_FILTER_H_BGR;
            } else if (!strcasecmp(value, "vrgb")) {
                settings.lcd_filter = LCD_FILTER_V_RGB;
            } else if (!strcasecmp(value, "vbgr")) {
                settings.lcd_filter = LCD_FILTER_V_BGR;
            } else if (!strcasecmp(value, "none")) {
                settings.lcd_filter = LCD_FILTER_NONE;
            } else {
                L_WARN_BAD_VALUE;
            }
            break;

        case OPT_TITLE_IDX:
            AString_replace_with_dynamic(&settings.title, strdup(value));
            break;

        case OPT_COLUMNS_IDX:
            settings.cols = strtol(value, NULL, 10);
            break;

        case OPT_ROWS_IDX:
            settings.rows = strtol(value, NULL, 10);
            break;

        case OPT_TERM_IDX:
            AString_replace_with_dynamic(&settings.term, strdup(value));
            break;

        case OPT_LOCALE_IDX:
            AString_replace_with_dynamic(&settings.locale, strdup(value));
            break;

        case OPT_SCROLL_LINES_IDX:
            settings.scroll_discrete_lines = MIN(strtod(value, NULL), UINT8_MAX);
            break;

        case OPT_TITLE_FORMAT_IDX:
            AString_replace_with_dynamic(&settings.title_format, strdup(value));
            break;

        case OPT_BG_COLOR_IDX: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_hex(value, NULL);
            if (!failed) {
                settings.bg                      = parsed;
                settings._explicit_colors_set[0] = true;
            } else {
                WRN("Failed to parse \'%s\' as RGBA color\n", value);
            }
        } break;

        case OPT_FG_COLOR_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, NULL);
            if (!failed) {
                settings.fg                      = parsed;
                settings._explicit_colors_set[1] = true;
            } else {
                WRN("Failed to parse \'%s\' as RGB color\n", value);
            }
        } break;

        case OPT_H_BG_COLOR_IDX: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_hex(value, NULL);
            if (!failed) {
                settings.bghl = parsed;
            } else {
                WRN("Failed to parse \'%s\' as RGBA color\n", value);
            }
        } break;

        case OPT_H_FG_COLOR_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, NULL);
            if (!failed) {
                settings.highlight_change_fg = true;
                settings.fghl                = parsed;
            } else {
                WRN("Failed to parse \'%s\' as RGB color\n", value);
            }
        } break;

        case OPT_COLOR_0_IDX ... OPT_COLOR_15_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, &failed);

            if (!failed) {
                settings.colorscheme.color[array_index - OPT_COLOR_0_IDX] = parsed;
                ASSERT(settings._explicit_colors_set, "not malloced");
                settings._explicit_colors_set[array_index - OPT_COLOR_0_IDX + 2] = true;
            } else {
                WRN("Failed to parse \'%s\' as RGB color\n", value);
            }
        } break;

        case OPT_BIND_KEY_COPY_IDX:
        case OPT_BIND_KEY_PASTE_IDX:
        case OPT_BIND_KEY_ENLARGE_IDX:
        case OPT_BIND_KEY_SHRINK_IDX:
        case OPT_BIND_KEY_UNI_IDX:
        case OPT_BIND_KEY_KSM_IDX:
        case OPT_BIND_KEY_DEBUG_IDX:
        case OPT_BIND_KEY_QUIT_IDX: {
            // point 'name_start' to after the last '+'
            const char* name_start = value;
            while (*name_start)
                ++name_start;
            while (*name_start != '+' && name_start >= value)
                --name_start;

            // get modifiers from between 'value' and 'name_start'
            uint32_t    mods          = 0;
            const char* current_begin = value;
            for (const char* i = value; i <= name_start; ++i) {
                if (*i == '+') {
                    static const char* ctrl_names[]  = { "C", "Ctrl", "Control" };
                    static const char* alt_names[]   = { "A", "M", "Alt", "Meta" };
                    static const char* shift_names[] = { "S", "Shift" };

                    size_t slice_range = i - current_begin;

#define CHECK_NAMES(_array, _mask)                                                                 \
    for (uint_fast8_t j = 0; j < ARRAY_SIZE(_array); ++j)                                          \
        if (strneqci(_array[j], current_begin, slice_range)) {                                     \
            mods |= _mask;                                                                         \
            LOG("parsed %.*s as " #_mask "\n", (int)slice_range, current_begin);                   \
            goto done_parsing;                                                                     \
        }

                    CHECK_NAMES(ctrl_names, MODIFIER_CONTROL)
                    CHECK_NAMES(alt_names, MODIFIER_ALT)
                    CHECK_NAMES(shift_names, MODIFIER_SHIFT)
                    WRN("Invalid modifier key name \'%.*s\'\n", (int)slice_range, current_begin);
                done_parsing:;
                    current_begin = i + 1;
                }
            }

            KeyCommand* command;
            switch (array_index) {
                case OPT_BIND_KEY_COPY_IDX:
                    command = &settings.key_commands[KCMD_COPY];
                    break;
                case OPT_BIND_KEY_PASTE_IDX:
                    command = &settings.key_commands[KCMD_PASTE];
                    break;
                case OPT_BIND_KEY_ENLARGE_IDX:
                    command = &settings.key_commands[KCMD_FONT_ENLARGE];
                    break;
                case OPT_BIND_KEY_SHRINK_IDX:
                    command = &settings.key_commands[KCMD_FONT_SHRINK];
                    break;
                case OPT_BIND_KEY_UNI_IDX:
                    command = &settings.key_commands[KCMD_UNICODE_ENTRY];
                    break;
                case OPT_BIND_KEY_KSM_IDX:
                    command = &settings.key_commands[KCMD_KEYBOARD_SELECT];
                    break;
                case OPT_BIND_KEY_DEBUG_IDX:
                    command = &settings.key_commands[KCMD_DEBUG];
                    break;
                case OPT_BIND_KEY_QUIT_IDX:
                    command = &settings.key_commands[KCMD_QUIT];
                    break;
            }
            command->mods     = mods;
            command->is_name  = true;
            command->key.name = strdup(name_start + 1);
        } break;
    }
}

static void settings_get_opts(const int argc, char* const* argv, const bool cfg_file_check)
{
    optind = 1;
    for (int o; (optind < argc && optind >= 0 && strcmp(argv[optind], "-e") &&
                 strcmp(argv[optind], "-x"));) {

        /* print 'invalid option' error message only once */
        opterr   = cfg_file_check;
        int opid = 0;
        o        = getopt_long(argc, argv, "XctDGFfhv", long_options, &opid);
        if (o == -1) {
            break;
        }
        if (cfg_file_check) {
            if (o == 'C') {
                settings.skip_config = true;
            } else if (opid == OPT_CONFIG_FILE_IDX && optarg) {
                AString_replace_with_dynamic(&settings.config_path, strdup(optarg));
            }
        } else {
            handle_option(o, opid, optarg);
        }
    }

    /* get program name and args for it */
    if (!cfg_file_check) {
        for (int i = 0; i < argc; ++i) {
            if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "-x")) {
                if (argc > (i++ + 1)) {
                    AString_replace_with_dynamic(&settings.shell, strdup(argv[i]));
                    settings.shell_argc = argc - i;
                    settings.shell_argv = calloc((settings.shell_argc + 1), sizeof(char*));
                    for (int j = 0; j < settings.shell_argc; ++j) {
                        settings.shell_argv[j] = strdup(argv[i + j]);
                    }
                }
                break;
            }
        }

        /* no -e or -x option was passed - find shell from env or use default */
        if (!settings.shell.str) {
            AString_replace_with_static(&settings.shell, getenv("SHELL"));
            if (!settings.shell.str) {
                AString_replace_with_static(&settings.shell, "/bin/sh");
            }
            settings.shell_argv    = calloc(2, sizeof(char*));
            settings.shell_argv[0] = strdup(settings.shell.str);
            settings.shell_argc    = 1;
        }
    }
}

static void handle_config_option(const char* key, const char* val)
{
    if (key) {
        for (int i = 0;; ++i) {
            struct option* opt = &long_options[i];
            if (!opt->name)
                break;
            if (!strcmp(key, opt->name)) {
                if (opt->has_arg == required_argument || !val || !strcasecmp(val, "true") ||
                    !strcmp(val, "1")) {
                    handle_option(opt->val, i, val);
                }
            }
        }
    }
}

static void settings_file_parse(FILE* f)
{
    ASSERT(f, "valid FILE*");

    bool in_comment = false;
    bool in_value   = false;
    bool in_string  = false;
    bool escaped    = false;

    char buf[512] = { 0 };
    int  rd;

    Vector_char key   = Vector_new_with_capacity_char(30);
    Vector_char value = Vector_new_with_capacity_char(30);

    while ((rd = fread(buf, sizeof(char), sizeof buf - 1, f))) {

        for (int i = 0; i < rd; ++i) {
            if (in_comment) {
                if (buf[i] == '\n') {
                    in_comment = false;
                    in_string  = false;
                }
                continue;
            } else if (in_value) {

                if (!in_string && buf[i] == '#') {
                    in_comment = true;
                    in_value   = false;
                    Vector_push_char(&value, 0);
                    handle_config_option(key.buf, value.buf);
                    value.size = 0;
                    continue;
                }

                if (buf[i] == '\\' && !escaped) {
                    escaped = true;
                    continue;
                }
                if (buf[i] == '\"' && !escaped) {
                    in_string = !in_string;
                } else if (buf[i] == '\n' && !escaped) {
                    in_value  = false;
                    in_string = false;
                    Vector_push_char(&value, 0);
                    handle_config_option(key.buf, value.buf);
                    value.size = 0;
                } else {
                    if (!isblank(buf[i]) || in_string || escaped) {
                        Vector_push_char(&value, (buf[i] == 'n' && escaped) ? '\n' : buf[i]);
                    }
                }

            } else {
                if (buf[i] == '=' && !escaped) {
                    Vector_push_char(&key, 0);
                    key.size = 0;
                    in_value = true;
                } else if (buf[i] == '\n' && !escaped) {
                    if (key.size) {
                        Vector_push_char(&key, '\0');
                        handle_config_option(key.buf, NULL);
                    }
                    Vector_clear_char(&key);
                } else {
                    if (buf[i] == '#' && !escaped) {
                        in_comment = true;
                    } else if (!isblank(buf[i])) {
                        Vector_push_char(&key, buf[i]);
                    }
                }
            }
            escaped = false;
        }
    }

    Vector_destroy_char(&key);
    Vector_destroy_char(&value);
}

static void find_config_path()
{
    AString retval = AString_new_copy(&settings.config_path);
    if (retval.str)
        return;

    char* tmp = NULL;
    if ((tmp = getenv("XDG_CONFIG_HOME"))) {
        AString_replace_with_dynamic(&settings.config_path,
                                     asprintf("%s/%s/%s", tmp, CFG_SDIR_NAME, CFG_FNAME));
    } else if ((tmp = getenv("HOME"))) {
        AString_replace_with_dynamic(&settings.config_path,
                                     asprintf("%s/.config/%s/%s", tmp, CFG_SDIR_NAME, CFG_FNAME));
    } else {
        WRN("Could not find config directory\n");
    }
}

static FILE* open_config(const char* path)
{
    if (path) {
        FILE* f = fopen(path, "r");
        if (!f) {
            WRN("\'%s\' - %s\n", path, strerror(errno));
        }
        return f;
    }
    return NULL;
}

void settings_init(const int argc, char* const* argv)
{
    settings_make_default();
    settings_get_opts(argc, argv, true);
    find_config_path();
    if (!settings.skip_config && settings.config_path.str) {
        FILE* cfg = open_config(settings.config_path.str);
        if (cfg) {
            settings_file_parse(cfg);
            fclose(cfg);
        }
    }
    settings_get_opts(argc, argv, false);
    settings_complete_defaults();
    init_color_palette();
}

void settings_cleanup()
{
    AString_destroy(&settings.config_path);
    AString_destroy(&settings.title_format);
    AString_destroy(&settings.font);
    AString_destroy(&settings.font_fallback);
    AString_destroy(&settings.font_fallback2);
    AString_destroy(&settings.term);
    AString_destroy(&settings.locale);
    AString_destroy(&settings.title);
    AString_destroy(&settings.font_file_name_regular);
    AString_destroy(&settings.font_style_regular);
    AString_destroy(&settings.font_style_bold);
    AString_destroy(&settings.font_style_italic);
    AString_destroy(&settings.font_style_bold_italic);
    AString_destroy(&settings.font_file_name_bold);
    AString_destroy(&settings.font_file_name_italic);
    AString_destroy(&settings.font_file_name_bold_italic);
    AString_destroy(&settings.font_file_name_fallback);
    AString_destroy(&settings.font_file_name_fallback2);
}
