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

// default font families
#ifndef DFT_FONT_NAME
#define DFT_FONT_NAME "Noto Sans Mono"
#endif

#ifndef DFT_FONT_NAME_FALLBACK
#define DFT_FONT_NAME_FALLBACK NULL
#endif

#ifndef DFT_FONT_NAME_FALLBACK2
#define DFT_FONT_NAME_FALLBACK2 NULL
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

#define OPT_VERSION_IDX 53
    [OPT_VERSION_IDX] = { "version", no_argument, 0, 'v' },

#define OPT_HELP_IDX 54
    [OPT_HELP_IDX] = { "help", no_argument, 0, 'h' },

#define OPT_SENTINEL_IDX 55
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
    [OPT_VERSION_IDX]   = { NULL, "Show version" },
    [OPT_HELP_IDX]      = { NULL, "Show this message" },

    [OPT_SENTINEL_IDX] = { NULL, NULL }
};

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
      /* linux console */
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
    { /* yaru */
      "2E3436", "CC0000", "4E9A06", "C4A000", "3465A4", "75507B", "06989A", "D3D7CF", "555753",
      "EF2929", "8AE234", "FCE94F", "729FCF", "AD7FA8", "34E2E2", "EEEEEC", "300A24", "FFFFFF" },
    {
      /* tango */ "000000",
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
      // orchis
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
    { // solarized
      "073642", "DC322F", "859900", "B58900", "268BD2", "D33682", "2AA198", "EEE8D5", "002B36",
      "CB4B16", "586E75", "657B83", "839496", "6C71C4", "93A1A1", "FDF6E3", "002B36", "839496" },
};

Settings settings;

/** set default colorscheme  */
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

    if (colors_default[idx][16])
        if (!(settings._explicit_colors_set && settings._explicit_colors_set[0])) {
            settings.bg = ColorRGBA_from_hex(colors_default[idx][16], NULL);
        }

    if (colors_default[idx][17])
        if (!(settings._explicit_colors_set && settings._explicit_colors_set[1])) {
            settings.fg = ColorRGB_from_hex(colors_default[idx][17], NULL);
        }
}

/** Initialize 256color colorpallete */
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

/** Use fontconfig to get font files */
static void find_font()
{
    FcConfig* cfg = FcInitLoadConfigAndFonts();

    FcPattern* pat = FcNameParse((const FcChar8*)settings.font);

    FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, FC_PIXEL_SIZE, NULL);
    FcFontSet*   fs = FcFontList(cfg, pat, os);

    char*  regular_alternative = NULL;
    bool   is_bitmap           = false;
    double pix_sz_regular = 0, pix_sz_bold = 0, pix_sz_italic = 0;
    double desired_pix_size = (double)settings.font_size * PT_AS_INCH * settings.font_dpi;

    bool load_bold = (settings.font_style_regular && settings.font_style_bold)
                       ? strcmp(settings.font_style_regular, settings.font_style_bold)
                       : settings.font_style_bold && !settings.font_style_regular
                           ? (!strcmp(settings.font_style_bold, "Regular"))
                           : true;

    bool load_italic = (settings.font_style_regular && settings.font_style_italic)
                         ? strcmp(settings.font_style_regular, settings.font_style_italic)
                         : settings.font_style_italic && !settings.font_style_regular
                             ? (!strcmp(settings.font_style_italic, "Regular"))
                             : true;

    for (int_fast32_t i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];
        FcChar8 *  file, *style;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {

            double pix_size = 0;
            FcPatternGetDouble(font, FC_PIXEL_SIZE, 0, &pix_size);

            if (pix_size)
                is_bitmap = true;

#define SZ_DIFF(_ps) fabs(desired_pix_size - (_ps))

            if (!strcmp((const char*)style,
                        settings.font_style_regular ? settings.font_style_regular : "Regular")) {
                if (is_bitmap && SZ_DIFF(pix_sz_regular) < SZ_DIFF(pix_size))
                    continue;
                free(settings.font_name);
                settings.font_name = strdup((char*)file);
                pix_sz_regular     = pix_size;
            }

            if ((!strcmp((const char*)style, "Text") || !strcmp((const char*)style, "Medium")) &&
                !settings.font_style_regular) {
                if (is_bitmap && SZ_DIFF(pix_sz_regular) < SZ_DIFF(pix_size))
                    continue;
                free(regular_alternative);
                regular_alternative = strdup((char*)file);
                pix_sz_regular      = pix_size;
            }

            if (load_bold) {
                if (!strcmp((const char*)style,
                            settings.font_style_bold ? settings.font_style_bold : "Bold")) {
                    if (is_bitmap && SZ_DIFF(pix_sz_bold) < SZ_DIFF(pix_size))
                        continue;
                    free(settings.font_name_bold);
                    settings.font_name_bold = strdup((char*)file);
                    pix_sz_bold             = pix_size;
                }
            }

            if (load_italic) {
                if (!strcmp((const char*)style,
                            settings.font_style_italic ? settings.font_name_italic : "Italic")) {
                    if (is_bitmap && SZ_DIFF(pix_sz_italic) < SZ_DIFF(pix_size))
                        continue;
                    free(settings.font_name_italic);
                    settings.font_name_italic = strdup((char*)file);
                    pix_sz_italic             = pix_size;
                }
            }
        }
    }

    if (is_bitmap) {
        settings.lcd_filter = LCD_FILTER_NONE;
    }

    FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);

    if (settings.font_fallback) {
        pat = FcNameParse((const FcChar8*)settings.font_fallback);
        os  = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, NULL);
        fs  = FcFontList(cfg, pat, os);

        for (int_fast32_t i = 0; fs && i < fs->nfont; ++i) {
            FcPattern* font = fs->fonts[i];
            FcChar8 *  file, *style;

            if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
                FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
                free(settings.font_name_fallback);
                settings.font_name_fallback = strdup((char*)file);
            }
        }

        FcFontSetDestroy(fs);
        FcObjectSetDestroy(os);
        FcPatternDestroy(pat);
    }

    if (settings.font_fallback2) {
        pat = FcNameParse((const FcChar8*)settings.font_fallback2);
        os  = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, NULL);
        fs  = FcFontList(cfg, pat, os);

        for (int_fast32_t i = 0; fs && i < fs->nfont; ++i) {
            FcPattern* font = fs->fonts[i];
            FcChar8 *  file, *style;

            if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
                FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
                free(settings.font_name_fallback2);
                settings.font_name_fallback2 = strdup((char*)file);
            }
        }

        FcFontSetDestroy(fs);
        FcObjectSetDestroy(os);
        FcPatternDestroy(pat);
    }

    FcConfigDestroy(cfg);

    if (!settings.font_name) {
        if (regular_alternative) {
            settings.font_name = regular_alternative;
        } else {
            if (settings.font_style_regular) {
                ERR("No style \'%s\' found for \'%s\'", settings.font_style_regular, settings.font);
            } else {
                ERR("Failed to load font \'%s\'", settings.font);
            }
        }
    } else if (regular_alternative) {
        free(regular_alternative);
    }

    if (!settings.font_name_bold && settings.font_style_bold && load_bold) {
        WRN("No style \'%s\' found for \'%s\'\n", settings.font_style_bold, settings.font);
    }

    if (!settings.font_name_italic && settings.font_style_italic && load_italic) {
        WRN("No style \'%s\' found for \'%s\'\n", settings.font_style_italic, settings.font);
    }

    if (!settings.font_name_fallback && settings.font_fallback)
        WRN("Failed to load font \'%s\'\n", settings.font_fallback);

    if (!settings.font_name_fallback2 && settings.font_fallback2) {
        WRN("Failed to load font \'%s\'\n", settings.font_fallback2);
    } else if (!settings.font_name_fallback) {
        // if fallback is not found but fallback2 is, swap them
        settings.font_name_fallback  = settings.font_name_fallback2;
        settings.font_name_fallback2 = NULL;
    }

    LOG("font files:\n  normal: %s\n  bold: %s\n  italic: %s\n"
        "  fallback/symbol: %s\n  fallback/symbol: %s\n",
        settings.font_name, settings.font_name_bold ? settings.font_name_bold : "(none)",
        settings.font_name_italic ? settings.font_name_italic : "(none)",
        settings.font_name_fallback ? settings.font_name_fallback : "(none)",
        settings.font_name_fallback2 ? settings.font_name_fallback2 : "(none)");
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

        .config_path    = NULL,
        .skip_config    = false,
        .x11_is_default = false,
        .shell          = NULL,
        .shell_argc     = 0,
        .shell_argv     = NULL,
        .term           = NULL,
        .locale         = NULL,
        .bsp_sends_del  = true,

        .font           = NULL,
        .font_fallback  = NULL,
        .font_fallback2 = NULL,
        .font_size      = 10,
        .font_dpi       = 96,
        .lcd_filter     = LCD_FILTER_UNDEFINED,

        .bg     = { .r = 0, .g = 0, .b = 0, .a = 240 },
        .bghl   = { .r = 50, .g = 50, .b = 50, .a = 240 },
        .fg     = { .r = 255, .g = 255, .b = 255 },
        .fghl   = { .r = 255, .g = 255, .b = 255 },
        .fg_dim = { .r = 150, .g = 150, .b = 150 },

        .highlight_change_fg = false,
        .title               = NULL,
        .dynamic_title       = true,
        .title_format        = NULL,

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

        .enable_cursor_blink = true,
        .cursor_blink_interval_ms = 750,
        .cursor_blink_suspend_ms = 500,
        .cursor_blink_end_s = 15,
    };
}

static void settings_complete_defaults()
{
    // set up fonts
    if (!settings.font)
        settings.font = DFT_FONT_NAME;

    if (!settings.font_fallback)
        settings.font_fallback = DFT_FONT_NAME_FALLBACK;

    if (!settings.font_fallback2)
        settings.font_fallback2 = DFT_FONT_NAME_FALLBACK2;

    find_font();

    // other strings
    if (!settings.title_format)
        settings.title_format = DFT_TITLE_FMT;

    if (!settings.term)
        settings.term = DFT_TERM;

    if (!settings.title)
        settings.title = APP_NAME;

    // set up locale
    if (!settings.locale) {
        char* locale;
        locale = getenv("LC_ALL");
        if (!locale || !*locale)
            locale = getenv("LC_CTYPE");
        if (!locale || !*locale)
            locale = getenv("LANG");
        if (!locale || !*locale)
            locale = "C";
        settings.locale = locale;
    }

    setlocale(LC_CTYPE, settings.locale);
    LOG("Using locale: %s\n", settings.locale);

    // load colorscheme
    settings_colorscheme_default(settings.colorscheme_preset);
    free(settings._explicit_colors_set);
    settings._explicit_colors_set = NULL;
}

static void print_help()
{
    printf("Usage: %s [options...] [-e/x command args...]\n", EXE_FNAME);

    for (uint32_t i = 0; i < OPT_SENTINEL_IDX; ++i) {
        if (long_options[i].has_arg == no_argument && long_options[i].val) {
            printf(" -%c, ", long_options[i].val);
        } else {
            printf("     ");
        }

        if (long_options[i].has_arg == required_argument) {
            printf(
              " --%-s <%s>%-*s", long_options[i].name, long_options_descriptions[i][0],
              (int)(20 - strlen(long_options[i].name) - strlen(long_options_descriptions[i][0])),
              "");
        } else {
            printf(" --%s %*s", long_options[i].name, (int)(22 - strlen(long_options[i].name)), "");
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

            case 'T':
                settings.dynamic_title = false;
                break;

            case 'F':
                settings.no_flash = true;
                break;

            case 'f':
                settings.highlight_change_fg = true;
                break;

            case 'v':
                printf("version: " VERSION "\n");
                break;

            case 'D':
                settings.debug_pty = true;
                break;

            case 'h':
                print_help();
                break;
        }
        return;
    }

    // long options
    switch (array_index) {
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

        case OPT_SCROLLBACK_IDX:
            settings.scrollback = MAX(strtol(value, NULL, 10), 0);
            break;

        case OPT_VERSION_IDX:
            printf("version: " VERSION "\n");
            break;

        case OPT_BLINK_IDX: {
            int         argument_index = 0;
            Vector_char buf            = Vector_new_with_capacity_char(2);
            for (const char* i = value;; ++i) {
                if (*i == ':' || *i == '\0') {
                    Vector_push_char(&buf, '\0');
                    switch (argument_index) {
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
                        default:
                            WRN("Extra argument \'%s\' for option \'blink\'", buf.buf);
                    }
                    Vector_clear_char(&buf);
                    ++argument_index;
                } else {
                    Vector_push_char(&buf, *i);
                }

                if (!*i)
                    break;
            }
            Vector_destroy_char(&buf);
        } break;

        case OPT_HELP_IDX:
            print_help();
            break;

        case OPT_FONT_IDX:
            free(settings.font);
            settings.font = strdup(value);
            break;

        case OPT_FONT_FALLBACK_IDX:
            free(settings.font_fallback);
            settings.font_fallback = strdup(value);
            break;

        case OPT_FONT_FALLBACK2_IDX:
            free(settings.font_fallback2);
            settings.font_fallback2 = strdup(value);
            break;

        case OPT_FONT_STYLE_REGULAR_IDX:
            free(settings.font_style_regular);
            settings.font_style_regular = strdup(value);
            break;

        case OPT_FONT_STYLE_BOLD_IDX:
            free(settings.font_style_bold);
            settings.font_style_bold = strdup(value);
            break;

        case OPT_FONT_STYLE_ITALIC_IDX:
            free(settings.font_style_italic);
            settings.font_style_italic = strdup(value);
            break;

        case OPT_FONT_SIZE_IDX:
            settings.font_size = strtoul(value, NULL, 10);
            break;

        case OPT_DPI_IDX:
            settings.font_dpi = strtoul(value, NULL, 10);
            break;

        case OPT_COLORSCHEME_IDX:
            if (!strcasecmp(value, "wayst"))
                settings.colorscheme_preset = 0;
            else if (!strcasecmp(value, "linux"))
                settings.colorscheme_preset = 1;
            else if (!strcasecmp(value, "xterm"))
                settings.colorscheme_preset = 2;
            else if (!strcasecmp(value, "rxvt"))
                settings.colorscheme_preset = 3;
            else if (!strcasecmp(value, "yaru"))
                settings.colorscheme_preset = 4;
            else if (!strcasecmp(value, "tango"))
                settings.colorscheme_preset = 5;
            else if (!strcasecmp(value, "orchis"))
                settings.colorscheme_preset = 6;
            else if (!strcasecmp(value, "solarized"))
                settings.colorscheme_preset = 7;
            else
                settings.colorscheme_preset =
                  MIN(strtoul(value, NULL, 10), sizeof(colors_default) - 1);
            break;

        case OPT_TITLE_IDX:
            free(settings.title);
            settings.title = strdup(value);
            break;

        case OPT_COLUMNS_IDX:
            settings.cols = strtol(value, NULL, 10);
            break;

        case OPT_ROWS_IDX:
            settings.rows = strtol(value, NULL, 10);
            break;

        case OPT_TERM_IDX:
            free(settings.term);
            settings.term = strdup(value);
            break;

        case OPT_LOCALE_IDX:
            free(settings.locale);
            settings.locale = strdup(value);
            break;

        case OPT_SCROLL_LINES_IDX:
            settings.scroll_discrete_lines = MIN(strtod(value, NULL), UINT8_MAX);
            break;

        case OPT_TITLE_FORMAT_IDX:
            free(settings.title_format);
            settings.title_format = strdup(value);
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

        case OPT_FG_COLOR_DIM_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, NULL);
            if (!failed) {
                settings.fg_dim = parsed;
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

        case OPT_COLOR_0_IDX:
        case OPT_COLOR_1_IDX:
        case OPT_COLOR_2_IDX:
        case OPT_COLOR_3_IDX:
        case OPT_COLOR_4_IDX:
        case OPT_COLOR_5_IDX:
        case OPT_COLOR_6_IDX:
        case OPT_COLOR_7_IDX:
        case OPT_COLOR_8_IDX:
        case OPT_COLOR_9_IDX:
        case OPT_COLOR_10_IDX:
        case OPT_COLOR_11_IDX:
        case OPT_COLOR_12_IDX:
        case OPT_COLOR_13_IDX:
        case OPT_COLOR_14_IDX:
        case OPT_COLOR_15_IDX: {
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
        opterr = cfg_file_check;

        int opid = 0;
        o        = getopt_long(argc, argv, "XCTDFhv", long_options, &opid);

        if (o == -1)
            break;

        if (cfg_file_check) {
            if (o == 'C')
                settings.skip_config = true;
            else if (opid == OPT_CONFIG_FILE_IDX && optarg)
                settings.config_path = strdup(optarg);
        } else {
            handle_option(o, opid, optarg);
        }
    }

    /* get program name and args for it */
    if (!cfg_file_check) {
        for (int i = 0; i < argc; ++i) {
            if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "-x")) {
                if (argc > (i++ + 1)) {
                    settings.shell      = strdup(argv[i]);
                    settings.shell_argc = argc - i;
                    settings.shell_argv = calloc((settings.shell_argc + 1), sizeof(char*));

                    for (int i2 = 0; i2 < settings.shell_argc; ++i2) {
                        settings.shell_argv[i2] = strdup(argv[i + i2]);
                    }
                }
                break;
            }
        }

        /* no -e or -x option was passed - find shell from env or use default */
        if (!settings.shell) {
            settings.shell = getenv("SHELL");
            if (!settings.shell)
                settings.shell = "/bin/sh";
            settings.shell_argv    = calloc(2, sizeof(char*));
            settings.shell_argv[0] = strdup(settings.shell);
            settings.shell_argc    = 1;
        }
    }
}

static void handle_config_option(const char*  key,
                                 const char*  val,
                                 const int    argc,
                                 char* const* argv)
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

static void settings_file_parse(FILE* f, const int argc, char* const* argv)
{
    if (!f)
        return;

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
                    handle_config_option(key.buf, value.buf, argc, argv);
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
                    handle_config_option(key.buf, value.buf, argc, argv);
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
                        handle_config_option(key.buf, NULL, argc, argv);
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

static const char* find_config_path()
{
    if (settings.config_path)
        return settings.config_path;

    char* tmp = NULL;
    if ((tmp = getenv("XDG_CONFIG_HOME"))) {
        return asprintf("%s/%s/%s", tmp, CFG_SDIR_NAME, CFG_FNAME);
    } else if ((tmp = getenv("HOME"))) {
        return asprintf("%s/.config/%s/%s", tmp, CFG_SDIR_NAME, CFG_FNAME);
        ;
    } else {
        WRN("Could not find config directory\n");
        return NULL;
    }
}

static FILE* open_config(const char* path)
{
    if (path) {
        FILE* f = fopen(path, "r");

        if (!f) {
            WRN("\"%s\" - %s\n", path, strerror(errno));
        }

        return f;
    }
    return NULL;
}

void settings_init(const int argc, char* const* argv)
{
    settings_make_default();
    settings_get_opts(argc, argv, true);

    if (!settings.skip_config) {
        const char* cfg_path = find_config_path();
        FILE*       cfg      = open_config(cfg_path);
        settings_file_parse(cfg, argc, argv);
        if (cfg)
            fclose(cfg);
        if (cfg_path)
            free((void*)cfg_path);
    }

    settings_get_opts(argc, argv, false);
    settings_complete_defaults();
    init_color_palette();
}

void settings_cleanup() {}
