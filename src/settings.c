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
#include "key.h"
#include "opts.h"
#include "settings.h"
#include "util.h"
#include "vector.h"

#ifndef CONFIG_SUBDIRECTORY_NAME
#define CONFIG_SUBDIRECTORY_NAME "wayst"
#endif

#ifndef CONFIG_FILE_NAME
#define CONFIG_FILE_NAME "config"
#endif

#ifndef DFT_TITLE_FMT
#define DFT_TITLE_FMT "%2$s - %1$s"
#endif

#ifndef DFT_TERM
#define DFT_TERM "xterm-256color"
#endif

/* point size in inches for converting font sizes */
#define PT_AS_INCH 0.0138889

DEF_VECTOR(char, NULL);
DEF_VECTOR(Vector_char, Vector_destroy_char);

static Vector_Vector_char expand_list_value(const char* const list);

static const char* const colors_default[8][18] = {
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
      /* solarized */
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

static void settings_colorscheme_load_preset(uint8_t idx)
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
 * Use fontconfig to get font files */
static void find_font()
{
#define L_DROP_IF_SAME(file1, file2)                                                               \
    if (file2 && file1 && !strcmp(file1, file2)) {                                                 \
        if (unlikely(settings.debug_font)) {                                                       \
            fprintf(stderr, "Dropping redundant font file \'%s\'\n", file1);                       \
        }                                                                                          \
        free(file1);                                                                               \
        file1 = NULL;                                                                              \
    }

    FontconfigContext fc_context = FontconfigContext_new();
    bool              is_bitmap  = false;

    char* default_file =
      FontconfigContext_get_file(&fc_context, NULL, NULL, settings.font_size, NULL);
    char* default_file_bold = FontconfigContext_get_file(&fc_context,
                                                         NULL,
                                                         OR(settings.font_style_bold.str, "Bold"),
                                                         settings.font_size,
                                                         NULL);
    char* default_file_italic =
      FontconfigContext_get_file(&fc_context,
                                 NULL,
                                 OR(settings.font_style_italic.str, "Italic"),
                                 settings.font_size,
                                 NULL);
    char* default_file_bold_italic =
      FontconfigContext_get_file(&fc_context,
                                 NULL,
                                 OR(settings.font_style_italic.str, "Bold:Italic"),
                                 settings.font_size,
                                 NULL);

    if (!settings.styled_fonts.size) {
        Vector_push_StyledFontInfo(&settings.styled_fonts, (StyledFontInfo){ 0 });
        Vector_last_StyledFontInfo(&settings.styled_fonts)->family_name = strdup("Monospace");
    }

    int loaded_fonts = 0;

    for (StyledFontInfo* i = NULL; (i = Vector_iter_StyledFontInfo(&settings.styled_fonts, i));) {
        char* main_family  = i->family_name;
        char* regular_file = FontconfigContext_get_file(&fc_context,
                                                        main_family,
                                                        settings.font_style_regular.str,
                                                        settings.font_size + i->size_offset,
                                                        &is_bitmap);
        L_DROP_IF_SAME(regular_file, default_file);
        char* bold_file = FontconfigContext_get_file(&fc_context,
                                                     main_family,
                                                     OR(settings.font_style_bold.str, "Bold"),
                                                     settings.font_size + i->size_offset,
                                                     NULL);
        L_DROP_IF_SAME(bold_file, default_file);
        L_DROP_IF_SAME(bold_file, default_file_bold);
        L_DROP_IF_SAME(bold_file, regular_file);
        L_DROP_IF_SAME(bold_file, default_file);
        char* italic_file = FontconfigContext_get_file(&fc_context,
                                                       main_family,
                                                       OR(settings.font_style_italic.str, "Italic"),
                                                       settings.font_size + i->size_offset,
                                                       NULL);
        L_DROP_IF_SAME(italic_file, default_file);
        L_DROP_IF_SAME(italic_file, default_file_italic);
        L_DROP_IF_SAME(italic_file, regular_file);
        L_DROP_IF_SAME(italic_file, default_file);
        char* bold_italic_file =
          FontconfigContext_get_file(&fc_context,
                                     main_family,
                                     OR(settings.font_style_italic.str, "Bold:Italic"),
                                     settings.font_size + i->size_offset,
                                     NULL);
        L_DROP_IF_SAME(bold_italic_file, default_file);
        L_DROP_IF_SAME(bold_italic_file, default_file_bold_italic);
        L_DROP_IF_SAME(bold_italic_file, default_file_bold);
        L_DROP_IF_SAME(bold_italic_file, default_file_italic);
        L_DROP_IF_SAME(bold_italic_file, regular_file);
        L_DROP_IF_SAME(bold_italic_file, bold_file);
        L_DROP_IF_SAME(bold_italic_file, italic_file);
        L_DROP_IF_SAME(bold_italic_file, default_file);

        i->regular_file_name = regular_file;
        if ((i->bold_file_name = bold_file)) {
            settings.has_bold_fonts = true;
        }
        if ((i->italic_file_name = italic_file)) {
            settings.has_italic_fonts = true;
        }
        if ((i->bold_italic_file_name = bold_italic_file)) {
            settings.has_bold_italic_fonts = true;
        }

        if (!regular_file) {
            WRN("Could not find font \'%s\'\n", i->family_name);
        } else {
            ++loaded_fonts;
        }

        if (unlikely(settings.debug_font)) {
            printf("Loaded styled font:\n  regular:     %s\n  bold:        %s\n  italic:      %s\n "
                   " bold italic: %s\n",
                   OR(regular_file, "(none)"),
                   OR(bold_file, "(none)"),
                   OR(italic_file, "(none)"),
                   OR(bold_italic_file, "(none)"));
        }
    }

    if (!loaded_fonts) {
        ERR("Failed to load any primary font. \'" EXECUTABLE_FILE_NAME
            " --%s\' to display font info on start",
            long_options[OPT_DEBUG_FONT_IDX].name);
    }

    for (UnstyledFontInfo* i = NULL;
         (i = Vector_iter_UnstyledFontInfo(&settings.symbol_fonts, i));) {
        char* file =
          FontconfigContext_get_file(&fc_context, i->family_name, NULL, settings.font_size, NULL);
        L_DROP_IF_SAME(file, default_file);
        if ((i->file_name = file)) {
            settings.has_symbol_fonts = true;
        } else {
            WRN("Could not find font \'%s\'\n", i->family_name);
        }
        if (unlikely(settings.debug_font)) {
            printf("Loaded unstyled (symbol) font:\n  file:     %s\n", OR(file, "(none)"));
        }
    }

    for (UnstyledFontInfo* i = NULL;
         (i = Vector_iter_UnstyledFontInfo(&settings.color_fonts, i));) {
        char* file =
          FontconfigContext_get_file(&fc_context, i->family_name, NULL, settings.font_size, NULL);
        L_DROP_IF_SAME(file, default_file);
        if ((i->file_name = file)) {
            settings.has_color_fonts = true;
        } else {
            WRN("Could not find font \'%s\'\n", i->family_name);
        }
        if (unlikely(settings.debug_font)) {
            printf("Loaded unstyled (color) font:\n  file:     %s\n", OR(file, "(none)"));
        }
    }

    free(default_file);
    free(default_file_bold);
    free(default_file_italic);
    free(default_file_bold_italic);

    FontconfigContext_destroy(&fc_context);
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
                .key.code = KEY(u),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_COPY] = (KeyCommand) {
                .key.code = KEY(c),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_PASTE] = (KeyCommand) {
                .key.code = KEY(v),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_FONT_ENLARGE] = (KeyCommand) {
                .key.code = KEY(equal),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_FONT_SHRINK] = (KeyCommand) {
                .key.code = KEY(minus),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_LINE_SCROLL_UP] = (KeyCommand) {
                .key.code = KEY(Up),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_LINE_SCROLL_DN] = (KeyCommand) {
                .key.code = KEY(Down),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_PAGE_SCROLL_UP] = (KeyCommand) {
                .key.code = KEY(Page_Up),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_PAGE_SCROLL_DN] = (KeyCommand) {
                .key.code = KEY(Page_Down),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_KEYBOARD_SELECT] = (KeyCommand) {
                .key.code = KEY(k),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_DUPLICATE] = (KeyCommand) {
                .key.code = KEY(d),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_DEBUG] = (KeyCommand) {
                .key.code = KEY(slash),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
        },

        .skip_config    = false,
        .x11_is_default = false,
        .shell_argc     = 0,

        .shell          = AString_new_uninitialized(), 
        .title_format   = AString_new_static(DFT_TITLE_FMT),

        .styled_fonts = Vector_new_with_capacity_StyledFontInfo(1),
        .symbol_fonts = Vector_new_with_capacity_UnstyledFontInfo(1),
        .color_fonts  = Vector_new_with_capacity_UnstyledFontInfo(1),

        .term        = AString_new_static(DFT_TERM),
        .vte_version = AString_new_static("5602"),
        
        .locale      = AString_new_uninitialized(),
        .title       = AString_new_static(APPLICATION_NAME),
        .user_app_id = NULL,

        .font_style_regular     = AString_new_uninitialized(),
        .font_style_bold        = AString_new_uninitialized(),
        .font_style_italic      = AString_new_uninitialized(),
        .font_style_bold_italic = AString_new_uninitialized(),

        .bsp_sends_del      = true,
        .font_size          = 9,
        .font_size_fallback = 0,
        .font_dpi           = 96,
        .lcd_filter         = LCD_FILTER_H_RGB,

        .bg     = { .r = 0,   .g = 0,   .b = 0,  .a = 240 },
        .bghl   = { .r = 50,  .g = 50,  .b = 50, .a = 240 },
        .fg     = { .r = 255, .g = 255, .b = 255 },
        .fghl   = { .r = 255, .g = 255, .b = 255 },

        .highlight_change_fg = false,
        .dynamic_title       = true,

        .padding_center = true,
        .padding        = 0,
        .padd_glyph_x   = 0,
        .padd_glyph_y   = 0,
        .offset_glyph_x = 0,
        .offset_glyph_y = 0,
        .center_char    = '(',

        .scrollbar_width_px      = 10,
        .scrollbar_length_px     = 20,
        .scrollbar_fade_time_ms  = 150,
        .scrollbar_hide_delay_ms = 1500,

        .cols = 80,
        .rows = 24,

        .windowops_manip = true,
        .windowops_info  = true,

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

        .enable_cursor_blink      = true,
        .cursor_blink_interval_ms = 750,
        .cursor_blink_suspend_ms  = 500,
        .cursor_blink_end_s       = 15,
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

    settings_colorscheme_load_preset(settings.colorscheme_preset);
    free(settings._explicit_colors_set);
    settings._explicit_colors_set = NULL;
}

static void print_help_and_exit()
{
#define MAX_OPT_PADDING 28

    printf("Usage: %s [options...] [-e/x command args...]\n\nOPTIONS:\n", EXECUTABLE_FILE_NAME);
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

static void print_version_and_exit()
{
    printf("version: " VERSION
#ifdef DEBUG
           "-debug"
#endif

           "\n wayland: "
#ifdef NOWL
           "disabled"
#else
           "enabled"
#endif
           "\n X11: "
#ifdef NOWL
           "disabled"
#else
           "enabled"
#endif
           "\n utf8proc: "
#ifdef NOUTF8PROC
           "disabled"
#else
           "enabled"
#endif
           "\n");

    exit(EXIT_SUCCESS);
}

static Pair_char32_t parse_codepoint_range(char* arg)
{
    Pair_char32_t range = { .first = 0, .second = UINT_LEAST32_MAX };
    if (strnlen(arg, 3) < 3) {
        return range;
    }
    if (*arg != '.') {
        range.first = strtoul(arg, &arg, (arg[0] == 'u' || arg[0] == 'U') ? 16 : 10);
    }
    arg += 2;
    if (*arg) {
        range.second = strtoul(arg, NULL, arg[0] == 'u' ? 16 : 10);
    }
    return range;
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
                print_version_and_exit();
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

#define L_PROCESS_MULTI_ARG_PACK_BEGIN(_string)                                                    \
    int         argument_index = 0;                                                                \
    Vector_char buf            = Vector_new_with_capacity_char(15);                                \
    for (const char* i = (_string);; ++i) {                                                        \
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

#define L_WARN_BAD_COLOR                                                                           \
    WRN("Failed to parse \'%s\' as color for option \'%s\'%s.\n",                                  \
        value,                                                                                     \
        long_options[array_index].name,                                                            \
        strlen(value) ? "" : ", hint: correct config syntax is =\"#rrggbb\" or =rrggbb");

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
            print_version_and_exit();
            break;

        case OPT_BLINK_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
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
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0:
                settings.padding_center = strtob(buf.buf);
                break;
            case 1:
                settings.padding = CLAMP(strtol(buf.buf, NULL, 10), 0, UINT8_MAX);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_WINDOWOPS_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0:
                settings.windowops_manip = strtob(buf.buf);
                break;
            case 1:
                settings.windowops_info = strtob(buf.buf);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_GLYPH_PADDING_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0:
                settings.padd_glyph_x = CLAMP(strtol(buf.buf, NULL, 10), 0, INT8_MAX);
                break;
            case 1:
                settings.padd_glyph_y = CLAMP(strtol(buf.buf, NULL, 10), 0, INT8_MAX);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_SCROLLBAR_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0:
                settings.scrollbar_width_px = CLAMP(strtol(buf.buf, NULL, 10), 0, INT16_MAX);
                break;
            case 1:
                settings.scrollbar_length_px = CLAMP(strtol(buf.buf, NULL, 10), 0, INT16_MAX);
                break;
            case 2:
                settings.scrollbar_hide_delay_ms = CLAMP(strtol(buf.buf, NULL, 10), 0, INT16_MAX);
                break;
            case 3:
                settings.scrollbar_fade_time_ms = CLAMP(strtol(buf.buf, NULL, 10), 0, INT16_MAX);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_HELP_IDX:
            print_help_and_exit();
            break;

        case OPT_FONT_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            Vector_clear_StyledFontInfo(&settings.styled_fonts);
            for (Vector_char* i = NULL; (i = Vector_iter_Vector_char(&values, i));) {
                Vector_push_StyledFontInfo(&settings.styled_fonts, (StyledFontInfo){ 0 });
                StyledFontInfo* this_entry   = Vector_last_StyledFontInfo(&settings.styled_fonts);
                this_entry->codepoint_ranges = Vector_new_Pair_char32_t();

                char* str               = i->buf;
                char* arg               = strsep(&str, ":");
                this_entry->family_name = strdup(arg);

                while ((arg = strsep(&str, ":"))) {
                    if (strstr(arg, "..")) {
                        Vector_push_Pair_char32_t(&this_entry->codepoint_ranges,
                                                  parse_codepoint_range(arg));
                    } else {
                        this_entry->size_offset = CLAMP(atoi(arg), INT8_MIN, INT8_MAX);
                    }
                }
            }
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_FALLBACK_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            Vector_clear_UnstyledFontInfo(&settings.symbol_fonts);
            for (Vector_char* i = NULL; (i = Vector_iter_Vector_char(&values, i));) {
                Vector_push_UnstyledFontInfo(&settings.symbol_fonts, (UnstyledFontInfo){ 0 });
                UnstyledFontInfo* this_entry = Vector_last_UnstyledFontInfo(&settings.symbol_fonts);

                char* str               = i->buf;
                char* arg               = strsep(&str, ":");
                this_entry->family_name = strdup(arg);

                while ((arg = strsep(&str, ":"))) {
                    if (strstr(arg, "..")) {
                        Vector_push_Pair_char32_t(&this_entry->codepoint_ranges,
                                                  parse_codepoint_range(arg));
                    } else {
                        this_entry->size_offset = CLAMP(atoi(arg), INT8_MIN, INT8_MAX);
                    }
                }
            }
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_FALLBACK2_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            Vector_clear_UnstyledFontInfo(&settings.color_fonts);
            for (Vector_char* i = NULL; (i = Vector_iter_Vector_char(&values, i));) {
                Vector_push_UnstyledFontInfo(&settings.color_fonts, (UnstyledFontInfo){ 0 });
                UnstyledFontInfo* this_entry = Vector_last_UnstyledFontInfo(&settings.color_fonts);

                char* str               = i->buf;
                char* arg               = strsep(&str, ":");
                this_entry->family_name = strdup(arg);

                while ((arg = strsep(&str, ":"))) {
                    if (strstr(arg, "..")) {
                        Vector_push_Pair_char32_t(&this_entry->codepoint_ranges,
                                                  parse_codepoint_range(arg));
                    } else {
                        this_entry->size_offset = CLAMP(atoi(arg), INT8_MIN, INT8_MAX);
                    }
                }
            }
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_STYLE_REGULAR_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.font_style_regular, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_STYLE_BOLD_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.font_style_bold, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_STYLE_ITALIC_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.font_style_italic, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_SIZE_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
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

        case OPT_TITLE_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.title, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_APP_ID_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            free(settings.user_app_id);
            settings.user_app_id = strdup(values.buf[0].buf);
            if (values.size > 1) {
                free(settings.user_app_id_2);
                settings.user_app_id_2 = strdup(values.buf[1].buf);
            }
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_COLUMNS_IDX:
            settings.cols = strtol(value, NULL, 10);
            break;

        case OPT_ROWS_IDX:
            settings.rows = strtol(value, NULL, 10);
            break;

        case OPT_TERM_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.term, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_VTE_VERSION_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            if (values.buf[0].buf[0]) {
                AString_replace_with_dynamic(&settings.vte_version, strdup(values.buf[0].buf));
            } else {
                AString_destroy(&settings.vte_version);
            }
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_GLYPH_ALIGN_IDX: {
            char user_center_char = *value;
            if (isascii(user_center_char) && isgraph(user_center_char)) {
                settings.center_char = user_center_char;
            } else {
                WRN("Cannot align to \'%c\'(%d), must be graphic ASCII character\n",
                    user_center_char,
                    user_center_char);
            }
            if (value[1] != ':') {
                break;
            }
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value + 2)
            case 0:
                settings.offset_glyph_y = CLAMP(strtol(buf.buf, NULL, 10), INT8_MIN, INT8_MAX);
                break;
            case 1:
                settings.offset_glyph_x = CLAMP(strtol(buf.buf, NULL, 10), INT8_MIN, INT8_MAX);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_LOCALE_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.locale, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_SCROLL_LINES_IDX:
            settings.scroll_discrete_lines = MIN(strtod(value, NULL), UINT8_MAX);
            break;

        case OPT_TITLE_FORMAT_IDX: {
            Vector_Vector_char values = expand_list_value(value);
            AString_replace_with_dynamic(&settings.title_format, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_BG_COLOR_IDX: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_hex(value, &failed);
            if (!failed) {
                settings.bg                      = parsed;
                settings._explicit_colors_set[0] = true;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_FG_COLOR_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, &failed);
            if (!failed) {
                settings.fg                      = parsed;
                settings._explicit_colors_set[1] = true;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_H_BG_COLOR_IDX: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_hex(value, &failed);
            if (!failed) {
                settings.bghl = parsed;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_H_FG_COLOR_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, &failed);
            if (!failed) {
                settings.highlight_change_fg = true;
                settings.fghl                = parsed;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_COLOR_0_IDX ... OPT_COLOR_15_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_hex(value, &failed);

            if (!failed) {
                settings.colorscheme.color[array_index - OPT_COLOR_0_IDX] = parsed;
                ASSERT(settings._explicit_colors_set, "should already be malloced");
                settings._explicit_colors_set[array_index - OPT_COLOR_0_IDX + 2] = true;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_BIND_KEY_COPY_IDX ... OPT_BIND_KEY_QUIT_IDX: {
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
                case OPT_BIND_KEY_LN_UP_IDX:
                    command = &settings.key_commands[KCMD_LINE_SCROLL_UP];
                    break;
                case OPT_BIND_KEY_LN_DN_IDX:
                    command = &settings.key_commands[KCMD_LINE_SCROLL_DN];
                    break;
                case OPT_BIND_KEY_PG_UP_IDX:
                    command = &settings.key_commands[KCMD_PAGE_SCROLL_UP];
                    break;
                case OPT_BIND_KEY_PG_DN_IDX:
                    command = &settings.key_commands[KCMD_PAGE_SCROLL_DN];
                    break;
                case OPT_BIND_KEY_KSM_IDX:
                    command = &settings.key_commands[KCMD_KEYBOARD_SELECT];
                    break;
                case OPT_BIND_KEY_DUP_IDX:
                    command = &settings.key_commands[KCMD_DUPLICATE];
                    break;
                case OPT_BIND_KEY_DEBUG_IDX:
                    command = &settings.key_commands[KCMD_DEBUG];
                    break;
                case OPT_BIND_KEY_QUIT_IDX:
                    command = &settings.key_commands[KCMD_QUIT];
                    break;
                default:
                    ASSERT_UNREACHABLE;
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

static void handle_config_option(const char* key, const char* val, uint32_t line)
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
                    return;
                }
            }
        }
        WRN("Error in config on line %u: invalid option \'%s\'. \'%s -h\' to list options\n",
            line,
            key,
            EXECUTABLE_FILE_NAME);
    }
}

/**
 * Takes a settings value that may or may not be a list and expands it to a vector of strings.
 * Interprets list and array escape codes, so any arbitrary string values should be run through
 * this even if they are not lists.
 */
static Vector_Vector_char expand_list_value(const char* const list)
{
    Vector_Vector_char values = Vector_new_with_capacity_Vector_char(1);
    Vector_push_Vector_char(&values, Vector_new_with_capacity_char(10));

    if (!list) {
        Vector_push_char(Vector_last_Vector_char(&values), '\0');
        return values;
    }

    Vector_char whitespace = Vector_new_char();

    bool in_string    = false;
    bool in_list      = false;
    bool escaped      = false;
    bool has_brackets = false;

    const char* arr = list;
    for (char c = *arr; c; c = *(++arr)) {
        if (!escaped && !in_string && c == '[') {
            has_brackets = true;
            in_list      = true;
            escaped      = false;
            continue;
        }
        if (!escaped && !in_string && in_list && c == ']') {
            in_list = false;
            escaped = false;
            continue;
        }
        if (!escaped && c == '\"') {
            if (!in_string) {
                Vector_clear_char(&whitespace);
            }
            in_string = !in_string;
            continue;
        }
        if (!escaped && c == '\\') {
            escaped = true;
            continue;
        }
        if (!in_string && !escaped && c == ',') {
            Vector_push_char(Vector_last_Vector_char(&values), '\0');
            Vector_push_Vector_char(&values, Vector_new_with_capacity_char(10));
            Vector_clear_char(&whitespace);
            escaped = false;
            continue;
        }
        escaped = false;
        if (in_string || !isblank(c)) {
            if (isblank(c) && Vector_last_Vector_char(&values)->size) {
                /* May be whitespace inside the string, hold on to this and insert before the next
                 * character if we get one */
                Vector_push_char(&whitespace, c);
            } else {
                if (whitespace.size) {
                    Vector_pushv_char(Vector_last_Vector_char(&values),
                                      whitespace.buf,
                                      whitespace.size);
                }
                Vector_clear_char(&whitespace);
                Vector_push_char(Vector_last_Vector_char(&values), c);
            }
        }
    }
    Vector_push_char(Vector_last_Vector_char(&values), '\0');

    if (in_list) {
        WRN("List not terminated in \'%s\'\n", list);
    } else if (values.size == 1 && has_brackets) {
        WRN("\'%s\' is a single element list. Did you mean \'\\[%s\\]\'?\n",
            list,
            values.buf[0].buf);
    }
    if (in_string) {
        WRN("String not terminated in \'%s\'\n", list);
    }
    Vector_destroy_char(&whitespace);

    return values;
}

/**
 * read the open configuration file and call handle_config_option() with values formatted like
 * command line arguments (this way we can use handle_option() for dealing both config file and
 * args)
 */
static void settings_file_parse(FILE* f)
{
    ASSERT(f, "valid FILE*");

    char buf[1024 * 8] = { 0 };
    int  rd;

    Vector_char key        = Vector_new_with_capacity_char(10);
    Vector_char value      = Vector_new_with_capacity_char(30);
    Vector_char whitespace = Vector_new_char();

    bool in_list = false, in_comment = false, in_value = false, in_string = false, escaped = false;

    uint32_t key_line = 0, line = 1;

    while ((rd = fread(buf, sizeof(char), sizeof buf - 1, f))) {
        for (int i = 0; i < rd; ++i) {
            char c = buf[i];

            if (c == '\n') {
                ++line;
            } else if (c == '#' && !escaped && !in_string) {
                in_comment = true;
                continue;
            }

            if (in_comment) {
                if (c == '\n') {
                    in_comment = false;
                }
                continue;
            } else if (in_value) {
                if (c == '\\' && !escaped) {
                    escaped = true;
                    if (in_list) {
                        Vector_push_char(&value, c);
                    }
                    continue;
                } else if (c == '\"' && !escaped) {
                    if (!in_string && value.size && !in_list) {
                        Vector_push_char(&value, '\0');
                        WRN("Error in config on line %u: Unexpected characters \'%s\' before "
                            "\'\"\'. Did you mean \'\\\"\'?\n",
                            line,
                            value.buf);
                        Vector_clear_char(&value);
                    }
                    if (!in_string) {
                        Vector_clear_char(&whitespace);
                    }
                    in_string = !in_string;
                    if (in_list) {
                        Vector_push_char(&value, c);
                    }
                    continue;
                } else if (c == '[' && !in_string && !escaped) {
                    if (in_list) {
                        WRN("Error in config on lines %u-%u: Defines nested list. Did you mean "
                            "\'\\[\' ?\n",
                            key_line,
                            line);
                    }
                    in_list = true;
                } else if (c == ']' && !in_string && !escaped) {
                    if (!in_list) {
                        WRN("Error in config on line %u: Expected \'[\' before \']\'\n", line);
                    }
                    in_list = false;
                }

                if (c == '\n' && !in_list) {
                    Vector_push_char(&value, '\0');
                    handle_config_option(key.buf, value.buf, key_line);
                    Vector_clear_char(&whitespace);
                    Vector_clear_char(&key);
                    Vector_clear_char(&value);
                    in_value = false;
                    if (in_string) {
                        WRN("Error in config on line %u: Expected \'\"\' before end of line\n",
                            line - 1);
                    }
                    in_string = false;
                    continue;
                } else if (escaped && !in_list) {
                    if (c == 'n') {
                        Vector_push_char(&value, '\n');
                    }
                } else if (!iscntrl(c)) {
                    switch (c) {
                        case ']':
                        case '[':
                            if (in_string) {
                                Vector_push_char(&value, '\\');
                            }
                    }
                    if (in_string || !isblank(c)) {
                        if (whitespace.size) {
                            Vector_pushv_char(&value, whitespace.buf, whitespace.size);
                            Vector_clear_char(&whitespace);
                        }
                        Vector_push_char(&value, c);
                    } else if (value.size) {
                        Vector_push_char(&whitespace, c);
                    }
                }
                escaped = false;
            }

            /* in key */
            else if (c == '=') {
                Vector_push_char(&key, '\0');
                key_line = line;
                in_value = true;
            } else if (c == '\n' && key.size) {
                Vector_push_char(&key, '\0');
                handle_config_option(key.buf, NULL, key_line);
                Vector_clear_char(&key);
            } else {
                if (!iscntrl(c) && !isblank(c)) {
                    Vector_push_char(&key, c);
                }
            }
        }
    }

    if (key.size) {
        Vector_push_char(&key, '\0');
        Vector_push_char(&value, '\0');
        handle_config_option(key.buf, value.size > 1 ? value.buf : NULL, line);
    }

    if (in_string) {
        WRN("Error in config on line %u: Expected \'\"\' before end of file\n", line);
    }
    if (in_list) {
        WRN("Error in config on line %u: Expected \']\' before end of file\n", line);
    }
    Vector_destroy_char(&key);
    Vector_destroy_char(&value);
    Vector_destroy_char(&whitespace);
}

static void find_config_path()
{
    AString retval = AString_new_copy(&settings.config_path);
    if (retval.str)
        return;

    char* tmp = NULL;
    if ((tmp = getenv("XDG_CONFIG_HOME"))) {
        AString_replace_with_dynamic(
          &settings.config_path,
          asprintf("%s/%s/%s", tmp, CONFIG_SUBDIRECTORY_NAME, CONFIG_FILE_NAME));
    } else if ((tmp = getenv("HOME"))) {
        AString_replace_with_dynamic(
          &settings.config_path,
          asprintf("%s/.config/%s/%s", tmp, CONFIG_SUBDIRECTORY_NAME, CONFIG_FILE_NAME));
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

void settings_init(int argc, char** argv)
{
    settings_make_default();
    settings.argc = argc;
    settings.argv = argv;
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
}

void settings_cleanup()
{
    Vector_destroy_StyledFontInfo(&settings.styled_fonts);
    Vector_destroy_UnstyledFontInfo(&settings.symbol_fonts);
    Vector_destroy_UnstyledFontInfo(&settings.color_fonts);

    AString_destroy(&settings.config_path);
    AString_destroy(&settings.title_format);
    AString_destroy(&settings.term);
    AString_destroy(&settings.locale);
    AString_destroy(&settings.title);
    AString_destroy(&settings.font_style_regular);
    AString_destroy(&settings.font_style_bold);
    AString_destroy(&settings.font_style_italic);
    AString_destroy(&settings.font_style_bold_italic);

    free(settings.user_app_id);
    free(settings.user_app_id_2);
}
