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

#include "config_parser.h"
#include "fontconfig.h"
#include "key.h"
#include "opts.h"
#include "settings.h"
#include "util.h"
#include "vector.h"

#ifndef CONFIG_SUBDIRECTORY_NAME
#define CONFIG_SUBDIRECTORY_NAME "wayst"
#endif

#ifndef CACHE_SUBDIRECTORY_NAME
#define CACHE_SUBDIRECTORY_NAME "wayst"
#endif

#ifndef CONFIG_FILE_NAME
#define CONFIG_FILE_NAME "config"
#endif

#ifndef FONTCONFIG_CACHE_FILE_NAME
#define FONTCONFIG_CACHE_FILE_NAME "fc-cache"
#endif

#ifndef DFT_TITLE_FMT
#define DFT_TITLE_FMT                                                                              \
    "{?sVtTitle:{sVtTitle} - }{?bCommandIsRunning && !bIsAltBufferEnabled && "                     \
    "i32CommandTimeSec > 1: "                                                                      \
    "({sRunningCommand}) }{sAppTitle}"
#endif

#ifndef DFT_TERM
#define DFT_TERM "xterm-256color"
#endif

/* point size in inches for converting font sizes */
#define PT_AS_INCH 0.0138889

static const char* const colors_default[9][18] = {
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
    {
      /* gruvbox */
      "1d2021",
      "cc241d",
      "98971a",
      "d79921",
      "458588",
      "b16286",
      "689d6a",
      "a89984",
      "928374",
      "fb4934",
      "b8bb26",
      "fabd2f",
      "93a598",
      "d3869b",
      "8ec07c",
      "ebdbb2",
      "1d2021",
      "ebdbb2",
    },
};

Settings settings;

const char* _lcd_filt_names[] = {
    [LCD_FILTER_UNDEFINED] = "Undefined",  [LCD_FILTER_NONE] = "None",
    [LCD_FILTER_H_RGB] = "Horizontal RGB", [LCD_FILTER_H_BGR] = "Horizontal BGR",
    [LCD_FILTER_V_RGB] = "Vertical RGB",   [LCD_FILTER_V_BGR] = "Vertical BGR"
};

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
            settings.bg = ColorRGBA_from_any(colors_default[idx][16], NULL);
        }
    }
    if (colors_default[idx][17]) {
        if (!(settings._explicit_colors_set && settings._explicit_colors_set[1])) {
            settings.fg = ColorRGB_from_hex(colors_default[idx][17], NULL);
        }
    }
}

static bool find_font_cached()
{
    char* xdg_cache_home = getenv("XDG_CACHE_HOME");
    char* cache_file     = NULL;
    FILE* file           = NULL;

    if (xdg_cache_home) {
        cache_file =
          asprintf("%s/" CACHE_SUBDIRECTORY_NAME "/" FONTCONFIG_CACHE_FILE_NAME, xdg_cache_home);
    } else {
        char* home = getenv("HOME");
        if (home) {
            cache_file =
              asprintf("%s/.cache/" CACHE_SUBDIRECTORY_NAME "/" FONTCONFIG_CACHE_FILE_NAME, home);
        } else {
            WRN("could not find cache directory\n");
            return true;
        }
    }

    file = fopen(cache_file, "r");
    LOG("using fontconfig cache file: %s\n", cache_file);
    free(cache_file);

    if (!file) {
        LOG("failed to open fontconfig cache file (%s)\n", strerror(errno));
        return true;
    }

    char type;
    char family_name[512];
    char file_names[4][512];

    if (unlikely(!settings.styled_fonts.size)) {
        goto abort;
    }

    for (StyledFontInfo* i = NULL; (i = Vector_iter_StyledFontInfo(&settings.styled_fonts, i));) {
        if (fscanf(file, "\n%c%511[^\n]", &type, family_name) != 2) {
            goto abort;
        }
        for (uint_fast8_t j = 0; j < 4; ++j) {
            file_names[j][0] = '\0';
            if (fscanf(file, "\t%511[^\n]", file_names[j]) != 1) {
                goto abort;
            }
        }
        if (type == 'S' && !strcmp(family_name, i->family_name) &&
            (file_names[0][0] != '-' || file_names[0][1] != 0)) {
            i->regular_file_name = strdup(file_names[0]);

            if (!strcmp(file_names[1], "-")) {
                i->bold_file_name = NULL;
            } else {
                settings.has_bold_fonts = true;
                i->bold_file_name       = strdup(file_names[1]);
            }

            if (!strcmp(file_names[2], "-")) {
                i->italic_file_name = NULL;
            } else {
                settings.has_italic_fonts = true;
                i->italic_file_name       = strdup(file_names[2]);
            }

            if (!strcmp(file_names[3], "-")) {
                i->bold_italic_file_name = NULL;
            } else {
                settings.has_bold_italic_fonts = true;
                i->bold_italic_file_name       = strdup(file_names[3]);
            }
        } else {
            goto abort;
        }
        fgetc(file); // skip \n
    }

    if (settings.symbol_fonts.size) {
        for (UnstyledFontInfo* i = NULL;
             (i = Vector_iter_UnstyledFontInfo(&settings.symbol_fonts, i));) {
            if (fscanf(file, "\n%c%511[^\n]", &type, family_name) != 2) {
                goto abort;
            }

            if (type == 'Y' && !strcmp(family_name, i->family_name) &&
                (file_names[0][0] != '-' || file_names[0][1] != 0)) {
                if (fscanf(file, "\t%511[^\n]", file_names[0]) != 1) {
                    goto abort;
                }
                i->file_name              = strdup(file_names[0]);
                settings.has_symbol_fonts = true;
            } else {
                goto abort;
            }
            fgetc(file); // skip \n
        }
    }

    if (settings.color_fonts.size) {
        for (UnstyledFontInfo* i = NULL;
             (i = Vector_iter_UnstyledFontInfo(&settings.color_fonts, i));) {
            if (fscanf(file, "\n%c%511[^\n]", &type, family_name) != 2) {
                goto abort;
            }
            if (type == 'C' && !strcmp(family_name, i->family_name) &&
                (file_names[0][0] != '-' || file_names[0][1] != 0)) {
                if (fscanf(file, "\t%511[^\n]", file_names[0]) != 1) {
                    goto abort;
                }
                i->file_name             = strdup(file_names[0]);
                settings.has_color_fonts = true;
            } else {
                goto abort;
            }
            fgetc(file); // skip \n
        }
    }

    fclose(file);
    LOG("fontconfig cache loaded succesfully\n");
    return false;

abort:
    fclose(file);
    settings.has_bold_fonts        = true;
    settings.has_italic_fonts      = true;
    settings.has_bold_italic_fonts = true;
    settings.has_symbol_fonts      = true;
    settings.has_color_fonts       = true;
    return true;
}

/**
 * Use fontconfig to get font files */
__attribute__((cold)) static void find_font()
{
    char* cache_dir      = NULL;
    char* xdg_cache_home = getenv("XDG_CACHE_HOME");

    if (xdg_cache_home) {
        cache_dir = asprintf("%s/" CACHE_SUBDIRECTORY_NAME, xdg_cache_home);
    } else {
        char* home = getenv("HOME");
        if (home) {
            cache_dir = asprintf("%s/.cache/" CACHE_SUBDIRECTORY_NAME, home);
        }
    }

    FILE* cache_file = NULL;

    if (cache_dir) {
        struct stat _st;
        if (stat(cache_dir, &_st) == -1) {
            mkdir(cache_dir, 0700);
            WRN("cache directory did not exist, created \'%s\'\n", cache_dir);
        }

        char* cache_file_name = asprintf("%s/%s", cache_dir, FONTCONFIG_CACHE_FILE_NAME);

        LOG("writing cache file: %s\n", cache_file_name);

        cache_file = fopen(cache_file_name, "w");

        if (!cache_file) {
            WRN("failed to open fontconfig cache file for writing: %s\n", strerror(errno));
        }

        free(cache_file_name);
        free(cache_dir);
    }

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
      FontconfigContext_get_file(&fc_context, NULL, NULL, settings.font_size, NULL, NULL);

    char* default_file_bold = FontconfigContext_get_file(&fc_context,
                                                         NULL,
                                                         OR(settings.font_style_bold.str, "Bold"),
                                                         settings.font_size,
                                                         NULL,
                                                         NULL);

    char* default_file_italic =
      FontconfigContext_get_file(&fc_context,
                                 NULL,
                                 OR(settings.font_style_italic.str, "Italic"),
                                 settings.font_size,
                                 NULL,
                                 NULL);

    char* default_file_bold_italic =
      FontconfigContext_get_file(&fc_context,
                                 NULL,
                                 OR(settings.font_style_italic.str, "Bold:Italic"),
                                 settings.font_size,
                                 NULL,
                                 NULL);

    if (!settings.styled_fonts.size) {
        Vector_push_StyledFontInfo(
          &settings.styled_fonts,
          (StyledFontInfo){ .size_offset       = 0,
                            .family_name       = strdup("Monospace"),
                            .regular_file_name = NULL,
                            .bold_file_name    = NULL,
                            .italic_file_name  = NULL,
                            .codepoint_ranges = (Vector_Pair_char32_t){ .buf = NULL, .size = 0 } });
    }

    int loaded_fonts = 0;

    for (StyledFontInfo* i = NULL; (i = Vector_iter_StyledFontInfo(&settings.styled_fonts, i));) {
        char* main_family = i->family_name;
        bool  exact_match;
        char* regular_file = FontconfigContext_get_file(&fc_context,
                                                        main_family,
                                                        settings.font_style_regular.str,
                                                        settings.font_size + i->size_offset,
                                                        &is_bitmap,
                                                        &exact_match);

        char* bold_file = FontconfigContext_get_file(&fc_context,
                                                     main_family,
                                                     OR(settings.font_style_bold.str, "Bold"),
                                                     settings.font_size + i->size_offset,
                                                     NULL,
                                                     &exact_match);

        if (!exact_match) {
            L_DROP_IF_SAME(bold_file, default_file);
        }
        L_DROP_IF_SAME(bold_file, default_file_bold);
        L_DROP_IF_SAME(bold_file, regular_file);
        L_DROP_IF_SAME(bold_file, default_file);
        char* italic_file = FontconfigContext_get_file(&fc_context,
                                                       main_family,
                                                       OR(settings.font_style_italic.str, "Italic"),
                                                       settings.font_size + i->size_offset,
                                                       NULL,
                                                       &exact_match);
        if (!exact_match) {
            L_DROP_IF_SAME(italic_file, default_file);
        }
        L_DROP_IF_SAME(italic_file, default_file_italic);
        L_DROP_IF_SAME(italic_file, regular_file);
        L_DROP_IF_SAME(italic_file, default_file);
        char* bold_italic_file =
          FontconfigContext_get_file(&fc_context,
                                     main_family,
                                     OR(settings.font_style_italic.str, "Bold:Italic"),
                                     settings.font_size + i->size_offset,
                                     NULL,
                                     &exact_match);
        if (!exact_match) {
            L_DROP_IF_SAME(bold_italic_file, default_file);
        }
        L_DROP_IF_SAME(bold_italic_file, default_file_bold_italic);
        L_DROP_IF_SAME(bold_italic_file, default_file_bold);
        L_DROP_IF_SAME(bold_italic_file, default_file_italic);
        L_DROP_IF_SAME(bold_italic_file, regular_file);
        L_DROP_IF_SAME(bold_italic_file, bold_file);
        L_DROP_IF_SAME(bold_italic_file, italic_file);
        L_DROP_IF_SAME(bold_italic_file, default_file);

        free(i->regular_file_name);
        i->regular_file_name = regular_file;

        free(i->bold_file_name);
        if ((i->bold_file_name = bold_file)) {
            settings.has_bold_fonts = true;
        }

        free(i->italic_file_name);
        if ((i->italic_file_name = italic_file)) {
            settings.has_italic_fonts = true;
        }

        free(i->bold_italic_file_name);
        if ((i->bold_italic_file_name = bold_italic_file)) {
            settings.has_bold_italic_fonts = true;
        }

        if (!regular_file) {
            WRN("Could not find font \'%s\'\n", i->family_name);
        } else {
            ++loaded_fonts;
        }

        if (cache_file) {
            fprintf(cache_file,
                    "S%s\n\t%s\n\t%s\n\t%s\n\t%s\n",
                    main_family,
                    OR(regular_file, "-"),
                    OR(bold_file, "-"),
                    OR(italic_file, "-"),
                    OR(bold_italic_file, "-"));
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
        bool  exact_match;
        char* file = FontconfigContext_get_file(&fc_context,
                                                i->family_name,
                                                NULL,
                                                settings.font_size,
                                                NULL,
                                                &exact_match);
        if (!exact_match) {
            L_DROP_IF_SAME(file, default_file);
        }

        free(i->file_name);
        if ((i->file_name = file)) {
            settings.has_symbol_fonts = true;
        } else {
            WRN("Could not find font \'%s\'\n", i->family_name);
        }

        if (cache_file && file) {
            fprintf(cache_file, "Y%s\n\t%s\n", i->family_name, file);
        }

        if (unlikely(settings.debug_font)) {
            printf("Loaded unstyled (symbol) font:\n  file:     %s\n", OR(file, "(none)"));
        }
    }

    for (UnstyledFontInfo* i = NULL;
         (i = Vector_iter_UnstyledFontInfo(&settings.color_fonts, i));) {
        bool  exact_match;
        char* file = FontconfigContext_get_file(&fc_context,
                                                i->family_name,
                                                NULL,
                                                settings.font_size,
                                                NULL,
                                                &exact_match);
        if (!exact_match) {
            L_DROP_IF_SAME(file, default_file);
        }

        free(i->file_name);
        if ((i->file_name = file)) {
            settings.has_color_fonts = true;
        } else {
            WRN("Could not find font \'%s\'\n", i->family_name);
        }

        if (cache_file && file) {
            fprintf(cache_file, "C%s\n\t%s\n", i->family_name, file);
        }

        if (unlikely(settings.debug_font)) {
            printf("Loaded unstyled (color) font:\n  file:     %s\n", OR(file, "(none)"));
        }
    }

    if (cache_file) {
        fclose(cache_file);
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

            [KCMD_MARK_SCROLL_UP] = (KeyCommand) {
                .key.code = KEY(Left),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_MARK_SCROLL_DN] = (KeyCommand) {
                .key.code = KEY(Right),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_COPY_OUTPUT] = (KeyCommand) {
                .key.code = KEY(x),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_HTML_DUMP] = (KeyCommand) {
                .key.code = KEY(F12),
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

            [KCMD_EXTERN_PIPE] = (KeyCommand) {
                .key.code = KEY(backslash),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },

            [KCMD_OPEN_PWD] = (KeyCommand) {
                .key.code = KEY(F10),
                .is_name = false,
                .mods = MODIFIER_SHIFT | MODIFIER_CONTROL
            },
        },

        .skip_config    = false,
        .x11_is_default = false,
        .shell_argc     = 0,

        .shell          = AString_new_uninitialized(), 
        .title_format   = AString_new_static(DFT_TITLE_FMT),
        .directory      = AString_new_uninitialized(),

        .styled_fonts = Vector_new_with_capacity_StyledFontInfo(1),
        .symbol_fonts = Vector_new_with_capacity_UnstyledFontInfo(1),
        .color_fonts  = Vector_new_with_capacity_UnstyledFontInfo(1),

        .term        = AString_new_static(DFT_TERM),
        .vte_version = AString_new_static("6201"),
        
        .locale      = AString_new_uninitialized(),
        .title       = AString_new_static(APPLICATION_NAME),
        .user_app_id = NULL,

        .uri_handler         = AString_new_static("xdg-open"),
        .extern_pipe_handler = AString_new_uninitialized(),
        .extern_pipe_source  = EXTERN_PIPE_SOURCE_COMMAND,

        .font_style_regular     = AString_new_uninitialized(),
        .font_style_bold        = AString_new_uninitialized(),
        .font_style_italic      = AString_new_uninitialized(),
        .font_style_bold_italic = AString_new_uninitialized(),
        .output_preferences     = Vector_new_output_prefs_t(),

        .lcd_exclude_ranges = Vector_new_Pair_char32_t(),

        .bsp_sends_del      = true,
        .font_size          = 9,
        .font_size_fallback = 0,
        .font_dpi           = 96,
        .general_font_dpi   = 96,
        .font_box_drawing_chars           = false,
        .font_dpi_calculate_from_phisical = false,
        .lcd_filter         = LCD_FILTER_H_RGB,
        .general_lcd_filter = LCD_FILTER_H_RGB,

        .bg        = { .r = 0,   .g = 0,   .b = 0,   .a = 240 },
        .bghl      = { .r = 50,  .g = 50,  .b = 50,  .a = 240 },
        .cursor_bg = { .r = 255, .g = 255, .b = 255, .a = 255 },
        .cursor_fg = { .r =   0, .g =   0, .b =   0 },
        .fg        = { .r = 255, .g = 255, .b = 255 },
        .fghl      = { .r = 255, .g = 255, .b = 255 },

        .background_blur = true,
        .cursor_color_static_bg = false,
        .cursor_color_static_fg = false,

        .dim_tint = { .r = 0, .g = 0, .b = 0, .a = 0 },

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
        .scrollbar_hide_delay_ms = 2000,

        .cols = 80,
        .rows = 24,

        .windowops_manip = true,
        .windowops_info  = true,

        .bold_is_bright = true,

        .colorscheme_preset   = 0,
        ._explicit_colors_set = _calloc(1, 21),

        .bell_flash = { .r = 200, .g = 200, .b = 200, .a = 30 },

        .hold_after_child_process_exit = false,
        .allow_scrollback_clear        = false,
        .scroll_on_output              = false,
        .scroll_on_key                 = true,
        .scroll_discrete_lines         = 3,

        .allow_multiple_underlines = false,

        .scrollback = 2000,

        .debug_pty = false,
        .debug_gfx = false,

        .initial_gui_pointer_mode = GUI_POINTER_MODE_HIDE,

        .enable_cursor_blink      = true,
        .cursor_blink_interval_ms = 750,
        .cursor_blink_suspend_ms  = 500,
        .cursor_blink_end_s       = 15,

        .initial_cursor_blinking = true,
        .initial_cursor_style    = CURSOR_STYLE_BLOCK,

        .decoration_style = DECORATION_STYLE_FULL,
        .decoration_theme = DECORATION_THEME_FROM_BG_IF_DARK,

        .defer_font_loading = true,
        .flush_ft_cache     = false,

        .pty_chunk_wait_delay_ns = 0,
        .pty_chunk_timeout_ms    = 5,

        .lcd_ranges_set_by_user = false,

        .vt_debug_delay_usec = 5000,

        .smooth_cursor = false,
        .incremental_windw_resize = true,
    };
}

static void settings_complete_defaults()
{
    if (unlikely(settings.flush_ft_cache || settings.debug_font || find_font_cached())) {
        find_font();
    }

    if (!settings.lcd_ranges_set_by_user && !settings.lcd_exclude_ranges.size) {
        Vector_push_Pair_char32_t(&settings.lcd_exclude_ranges,
                                  (Pair_char32_t){
                                    .first  = 0x2500, /* Box drawing block begin */
                                    .second = 0x25ff  /* Geometric shapes block end */
                                  });
        Vector_push_Pair_char32_t(
          &settings.lcd_exclude_ranges,
          (Pair_char32_t){
            .first  = 0xe0b0, /* private use area powerline symbols 'graphic' characters start */
            .second = 0xe0bf  /* private use area powerline symbols 'graphic' characters end */
          });
    }

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
    settings.general_font_dpi     = settings.font_dpi;
    settings.general_lcd_filter   = settings.general_lcd_filter;
}

static void print_help_and_exit()
{
#define MAX_OPT_PADDING 28

    printf("Usage:\n      " TERMCOLOR_BOLD "%s" TERMCOLOR_RESET
           " [options...] [-e/x command args...]\n\nOptions:\n",
           EXECUTABLE_FILE_NAME);

    for (uint32_t i = 0; i < OPT_SENTINEL_IDX; ++i) {
        if (long_options[i].has_arg != required_argument && long_options[i].val) {
            printf(" " TERMCOLOR_BOLD "-%c" TERMCOLOR_RESET ", ", long_options[i].val);
        } else {
            printf("     ");
        }
        if (long_options[i].has_arg == required_argument) {
            size_t l       = strlen(long_options[i].name) + strlen(long_options_descriptions[i][0]);
            int    padding = l >= MAX_OPT_PADDING ? 1 : MAX_OPT_PADDING - l;
            printf(" " TERMCOLOR_BOLD "--%-s " TERMCOLOR_RESET "<%s>"
                   "%-*s",
                   long_options[i].name,
                   long_options_descriptions[i][0],
                   padding,
                   "");
        } else {
            size_t l       = strlen(long_options[i].name);
            int    padding = l >= MAX_OPT_PADDING ? 1 : MAX_OPT_PADDING - l + 2;
            printf(" " TERMCOLOR_BOLD "--%s" TERMCOLOR_RESET " %*s",
                   long_options[i].name,
                   padding,
                   "");
        }
        printf("%s\n", long_options_descriptions[i][1]);
    }
    exit(EXIT_SUCCESS);
}

static void print_version_and_exit()
{
    printf("version: " VERSION
#ifdef DEBUG
           "-debug"
#endif

           "\n wayland:  "
#ifdef NOWL
           "disabled"
#else
           "enabled"
#endif
           "\n X11:      "
#ifdef NOX
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
           "\n renderer: "
#ifndef GFX_GLES
           "OpenGL 2.1"
#else
           "OpenGL ES 2.0"
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
    char *s = arg, *a_fst = NULL, *a_snd = NULL;
    if ((a_fst = strsep(&s, ".."))) {
        if (a_fst[0]) {
            if ((a_fst[0] == 'u' || a_fst[0] == 'U') && a_fst[1] == '+') {
                range.first = strtoul(a_fst + 2, NULL, 16);
            } else {
                range.first = strtoul(a_fst, NULL, 10);
            }
        }
        if ((strsep(&s, "..")) && (a_snd = strsep(&s, "..")) && a_snd[0]) {
            if ((a_snd[0] == 'u' || a_snd[0] == 'U') && a_snd[1] == '+') {
                range.second = strtoul(a_snd + 2, NULL, 16);
            } else {
                range.second = strtoul(a_snd, NULL, 10);
            }
        }
    }
    return range;
}

static void on_list_expand_syntax_error(const char* msg_fmt, va_list msg_arg)
{
    WRN("Error in config: ");
    vfprintf(stderr, msg_fmt, msg_arg);
    fputc('\n', stderr);
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
            case 'l':
                settings.flush_ft_cache = true;
                break;
            case 'o':
                settings.defer_font_loading = true;
                break;
            case 'a':
                settings.smooth_cursor = true;
                break;
            case 'H':
                settings.hold_after_child_process_exit = true;
                break;
            case 'b':
                settings.font_box_drawing_chars = true;
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
        else                                                                                       \
        {                                                                                          \
            Vector_push_char(&buf, *i);                                                            \
        }                                                                                          \
        if (!*i)                                                                                   \
            break;                                                                                 \
        }                                                                                          \
        Vector_destroy_char(&buf);

#define L_WARN_BAD_VALUE                                                                           \
    WRN("Unknown value \'%s\' for option \'%s\'\n", value, long_options[array_index].name);

#define L_WARN_BAD_VALUE_T(_type_name)                                                             \
    WRN("Failed to parse \'%s\' as " _type_name " for option \'%s\'\n",                            \
        value,                                                                                     \
        long_options[array_index].name);

#define L_WARN_BAD_COLOR                                                                           \
    WRN("Failed to parse \'%s\' as color for option \'%s\'%s.\n",                                  \
        value,                                                                                     \
        long_options[array_index].name,                                                            \
        strlen(value) ? "" : ", expected syntax =\"#rrggbb\" or =rrggbb");

#define L_ASSIGN_BOOL(_var, _dft)                                                                  \
    {                                                                                              \
        if (!value) {                                                                              \
            (_var) = (_dft);                                                                       \
        } else {                                                                                   \
            bool fail = false;                                                                     \
            bool v    = strtob2(value, &fail);                                                     \
            if (fail) {                                                                            \
                L_WARN_BAD_VALUE_T("bool");                                                        \
            } else {                                                                               \
                (_var) = v;                                                                        \
            }                                                                                      \
        }                                                                                          \
    }

        case OPT_XORG_ONLY_IDX:
            L_ASSIGN_BOOL(settings.x11_is_default, true);
            break;

        case OPT_SMOOTH_CURSOR:
            L_ASSIGN_BOOL(settings.smooth_cursor, true)
            break;

        case OPT_DYNAMIC_TITLE_IDX:
            L_ASSIGN_BOOL(settings.dynamic_title, true)
            break;

        case OPT_FLUSH_FC_CACHE_IDX:
            settings.flush_ft_cache = true;
            break;

        case OPT_PRELOAD_ALL_FONTS_IDX:
            settings.defer_font_loading = true;
            break;

        case OPT_VISUAL_BELL:
            if (!strcmp(value, "none")) {
                settings.no_flash = true;
            } else {
                bool failed         = false;
                settings.bell_flash = ColorRGBA_from_any(value, &failed);
                if (failed) {
                    L_WARN_BAD_COLOR;
                }
            }
            break;

        case OPT_DEBUG_PTY_IDX:
            settings.debug_pty = true;
            break;

        case OPT_DEBUG_VT_IDX:
            settings.debug_vt = true;
            if (value) {
                settings.vt_debug_delay_usec = atoi(value);
            }
            break;

        case OPT_HOLD:
            L_ASSIGN_BOOL(settings.hold_after_child_process_exit, true)
            break;

        case OPT_DEBUG_GFX_IDX:
            settings.debug_gfx = true;
            break;

        case OPT_DEBUG_FONT_IDX:
            settings.debug_font = true;
            break;

        case OPT_FONT_BOX_CHARS:
            settings.font_box_drawing_chars = true;
            break;

        case OPT_SCROLLBACK_IDX:
            settings.scrollback = MAX(strtol(value, NULL, 10), 0);
            break;

        case OPT_IO_CHUNK_DELAY: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0:
                settings.pty_chunk_wait_delay_ns = MAX(strtol(buf.buf, NULL, 10), 0);
                break;
            case 1:
                settings.pty_chunk_timeout_ms = MAX(strtol(buf.buf, NULL, 10), 0);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

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

        case OPT_OUTPUT_IDX: {
            bool           ok    = false;
            output_prefs_t prefs = {
                .output_name  = NULL,
                .lcd_filter   = LCD_FILTER_UNDEFINED,
                .dpi          = 0,
                .output_index = 0,
            };

            {
                L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
                case 0:
                    if (buf.size > 1 && *buf.buf != '0' &&
                        buf.size - 1 == strspn(buf.buf, "0123456789")) {
                        prefs.output_index = atoi(buf.buf);
                    } else {
                        prefs.output_name = strdup(buf.buf);
                    }
                    break;
                case 1:
                    ok = true;
                    if (!strcasecmp(buf.buf, "rgb")) {
                        prefs.lcd_filter = LCD_FILTER_H_RGB;
                    } else if (!strcasecmp(buf.buf, "bgr")) {
                        prefs.lcd_filter = LCD_FILTER_H_BGR;
                    } else if (!strcasecmp(buf.buf, "vrgb") || !strcasecmp(buf.buf, "rgbv")) {
                        prefs.lcd_filter = LCD_FILTER_V_BGR;
                    } else if (!strcasecmp(buf.buf, "vbgr") || !strcasecmp(buf.buf, "bgrv")) {
                        prefs.lcd_filter = LCD_FILTER_V_BGR;
                    } else if (!strcasecmp(buf.buf, "none")) {
                        prefs.lcd_filter = LCD_FILTER_NONE;
                    } else {
                        ok = false;
                        WRN("Unknown value \'%s\' for option \'%s\'(1)\n",
                            buf.buf,
                            long_options[array_index].name);
                    }
                    break;
                case 2:
                    if (!strcasecmp(buf.buf, "auto")) {
                        ok        = true;
                        prefs.dpi = 0;
                    } else {
                        errno     = 0;
                        prefs.dpi = strtol(buf.buf, NULL, 10);
                        ok        = true;
                        if (errno) {
                            L_WARN_BAD_VALUE;
                            prefs.dpi = 0;
                        }
                    }
                    break;
                    L_PROCESS_MULTI_ARG_PACK_END
            }

            if (ok) {
                output_prefs_t* matching_entry = NULL;
                for (output_prefs_t* i = NULL;
                     (i = Vector_iter_output_prefs_t(&settings.output_preferences, i));) {
                    if ((prefs.output_name && i->output_name &&
                         !strcmp(prefs.output_name, i->output_name)) ||
                        (prefs.output_index && prefs.output_index == i->output_index)) {
                        matching_entry = i;
                    }
                    break;
                }

                if (matching_entry) {
                    WRN("Duplicate entry for option %s output glob: '%s'\n",
                        long_options[array_index].name,
                        matching_entry->output_name);
                    *matching_entry = prefs;
                } else {
                    Vector_push_output_prefs_t(&settings.output_preferences, prefs);
                }
            }
        } break;

        case OPT_EXTERN_PIPE_HANDLER_IDX: {

            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0: {
                Vector_Vector_char values = expand_list_value(buf.buf, on_list_expand_syntax_error);
                AString_replace_with_dynamic(&settings.extern_pipe_handler, strdup(buf.buf));
                Vector_destroy_Vector_char(&values);
            } break;
            case 1:
                if (!strcasecmp(buf.buf, "buffer")) {
                    settings.extern_pipe_source = EXTERN_PIPE_SOURCE_BUFFER;
                } else if (!strcasecmp(buf.buf, "screen") || !strcasecmp(buf.buf, "viewport")) {
                    settings.extern_pipe_source = EXTERN_PIPE_SOURCE_VIEWPORT;
                } else if (!strcasecmp(buf.buf, "command")) {
                    settings.extern_pipe_source = EXTERN_PIPE_SOURCE_COMMAND;
                } else {
                    L_WARN_BAD_VALUE
                }
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

        case OPT_CURSOR_STYLE_IDX: {
            L_PROCESS_MULTI_ARG_PACK_BEGIN(value)
            case 0:
                if (!strcasecmp(buf.buf, "block")) {
                    settings.initial_cursor_style = CURSOR_STYLE_BLOCK;
                } else if (!strcasecmp(buf.buf, "beam")) {
                    settings.initial_cursor_style = CURSOR_STYLE_BEAM;
                } else if (!strcasecmp(buf.buf, "underline")) {
                    settings.initial_cursor_style = CURSOR_STYLE_UNDERLINE;
                }
                break;
            case 1:
                settings.initial_cursor_blinking = strtob(buf.buf);
                break;
                L_PROCESS_MULTI_ARG_PACK_END
        } break;

        case OPT_DIRECTORY_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.directory, strdup(values.buf[0].buf));
            Vector_destroy_Vector_char(&values);
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

        case OPT_BOLD_IS_BRIGHT:
            L_ASSIGN_BOOL(settings.bold_is_bright, true);
            break;

        case OPT_HELP_IDX:
            print_help_and_exit();
            break;

        case OPT_FONT_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
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
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
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
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
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
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.font_style_regular, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_STYLE_BOLD_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.font_style_bold, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_FONT_STYLE_ITALIC_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.font_style_italic, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_EXCLUDE_LCD_IDX: {
            settings.lcd_ranges_set_by_user = true;
            Vector_clear_Pair_char32_t(&settings.lcd_exclude_ranges);
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);

            if (values.size && values.buf->size > 3) {
                for (Vector_char* i = NULL; (i = Vector_iter_Vector_char(&values, i));) {
                    Pair_char32_t r = parse_codepoint_range(i->buf);
                    if (r.second < r.first) {
                        WRN("invalid codepoint range in %s\n", long_options[array_index].name);
                        continue;
                    }
                    Vector_push_Pair_char32_t(&settings.lcd_exclude_ranges, r);
                }
            }
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
            if (!strcasecmp(value, "auto")) {
                settings.font_dpi_calculate_from_phisical = true;
            } else {
                settings.general_font_dpi = settings.font_dpi = strtoul(value, NULL, 10);
            }
            break;

        case OPT_DECORATIONS: {
            if (!strcasecmp(value, "full")) {
                settings.decoration_style = DECORATION_STYLE_FULL;
            } else if (!strcasecmp(value, "minimal")) {
                settings.decoration_style = DECORATION_STYLE_MINIMAL;
            } else if (!strcasecmp(value, "none")) {
                settings.decoration_style = DECORATION_STYLE_NONE;
            } else {
                L_WARN_BAD_VALUE;
            }
        } break;

        case OPT_DECORATION_THEME: {
            if (!strcasecmp(value, "dark")) {
                settings.decoration_theme = DECORATION_THEME_DARK;
            } else if (!strcasecmp(value, "none")) {
                settings.decoration_theme = DECORATION_THEME_DONT_CARE;
            } else if (!strcasecmp(value, "auto")) {
                settings.decoration_theme = DECORATION_THEME_FROM_BG_IF_DARK;
            }
        } break;

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
            } else if (!strcasecmp(value, "gruvbox")) {
                settings.colorscheme_preset = 8;
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
            } else if (!strcasecmp(value, "vrgb") || !strcasecmp(value, "rgbv")) {
                settings.lcd_filter = LCD_FILTER_V_RGB;
            } else if (!strcasecmp(value, "vbgr") || !strcasecmp(value, "bgrv")) {
                settings.lcd_filter = LCD_FILTER_V_BGR;
            } else if (!strcasecmp(value, "none")) {
                settings.lcd_filter = LCD_FILTER_NONE;
            } else {
                L_WARN_BAD_VALUE;
            }

            settings.general_lcd_filter = settings.lcd_filter;
            break;

        case OPT_TITLE_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.title, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_APP_ID_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
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
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.term, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_URI_HANDLER_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            if (values.buf[0].buf[0]) {
                AString_replace_with_dynamic(&settings.uri_handler, strdup(value));
            }
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_VTE_VERSION_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
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
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.locale, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_SCROLL_LINES_IDX:
            settings.scroll_discrete_lines = MIN(strtod(value, NULL), UINT8_MAX);
            break;

        case OPT_TITLE_FORMAT_IDX: {
            Vector_Vector_char values = expand_list_value(value, on_list_expand_syntax_error);
            AString_replace_with_dynamic(&settings.title_format, strdup(value));
            Vector_destroy_Vector_char(&values);
        } break;

        case OPT_WM_BG_BLUR_IDX: {
            L_ASSIGN_BOOL(settings.background_blur, true)
        } break;

        case OPT_FORCE_WL_CSD: {
            L_ASSIGN_BOOL(settings.force_csd, true);

        } break;

        case OPT_ALWAYS_UNDERLINE_LINKS: {
            L_ASSIGN_BOOL(settings.always_underline_links, true);
        } break;

        case OPT_INC_WIN_RESIZE: {
            L_ASSIGN_BOOL(settings.incremental_windw_resize, true);
        } break;

        case OPT_BG_COLOR_IDX: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_any(value, &failed);
            if (!failed) {
                settings.bg                      = parsed;
                settings._explicit_colors_set[0] = true;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_FG_COLOR_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_any(value, &failed);
            if (!failed) {
                settings.fg                      = parsed;
                settings._explicit_colors_set[1] = true;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_H_BG_COLOR_IDX: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_any(value, &failed);
            if (!failed) {
                settings.bghl = parsed;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_H_FG_COLOR_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_any(value, &failed);
            if (!failed) {
                settings.highlight_change_fg = true;
                settings.fghl                = parsed;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_C_FG_COLOR_IDX: {
            bool failed = false;
            if (!strcasecmp(value, "none")) {
                settings.cursor_color_static_fg = false;
            } else {
                ColorRGB parsed = ColorRGB_from_any(value, &failed);
                if (!failed) {
                    settings.cursor_color_static_fg = true;
                    settings.cursor_fg              = parsed;
                } else {
                    L_WARN_BAD_COLOR
                }
            }
        } break;

        case OPT_C_BG_COLOR_IDX: {
            bool failed = false;
            if (!strcasecmp(value, "none")) {
                settings.cursor_color_static_bg = false;
            } else {
                ColorRGBA parsed = ColorRGBA_from_any(value, &failed);
                if (!failed) {
                    settings.cursor_color_static_bg = true;
                    settings.cursor_bg              = parsed;
                } else {
                    L_WARN_BAD_COLOR
                }
            }
        } break;

        case OPT_COLOR_0_IDX ... OPT_COLOR_15_IDX: {
            bool     failed = false;
            ColorRGB parsed = ColorRGB_from_any(value, &failed);

            if (!failed) {
                settings.colorscheme.color[array_index - OPT_COLOR_0_IDX] = parsed;
                ASSERT(settings._explicit_colors_set, "should already be malloced");
                settings._explicit_colors_set[array_index - OPT_COLOR_0_IDX + 2] = true;
            } else {
                L_WARN_BAD_COLOR
            }
        } break;

        case OPT_UNFOCUSED_TINT_COLOR: {
            bool      failed = false;
            ColorRGBA parsed = ColorRGBA_from_any(value, &failed);
            if (!failed) {
                settings.dim_tint = parsed;
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
                case OPT_BIND_KEY_COPY_CMD_IDX:
                    command = &settings.key_commands[KCMD_COPY_OUTPUT];
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
                case OPT_BIND_KEY_MRK_UP_IDX:
                    command = &settings.key_commands[KCMD_MARK_SCROLL_UP];
                    break;
                case OPT_BIND_KEY_MRK_DN_IDX:
                    command = &settings.key_commands[KCMD_MARK_SCROLL_DN];
                    break;
                case OPT_BIND_KEY_KSM_IDX:
                    command = &settings.key_commands[KCMD_KEYBOARD_SELECT];
                    break;
                case OPT_BIND_KEY_HTML_DUMP_IDX:
                    command = &settings.key_commands[KCMD_HTML_DUMP];
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
                case OPT_BIND_KEY_EXTERN_PIPE_IDX:
                    command = &settings.key_commands[KCMD_EXTERN_PIPE];
                    break;
                case OPT_BIND_KEY_OPEN_PWD:
                    command = &settings.key_commands[KCMD_OPEN_PWD];
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
        o        = getopt_long(argc, argv, "XctDGFhvloaHb", long_options, &opid);
        if (o == -1) {
            break;
        }
        if (cfg_file_check) {
            if (o == 'c') {
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
                    settings.shell_argv = _calloc((settings.shell_argc + 1), sizeof(char*));
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
            settings.shell_argv    = _calloc(2, sizeof(char*));
            settings.shell_argv[0] = strdup(settings.shell.str);
            settings.shell_argc    = 1;
        }
    }
}

static void handle_config_option(const char* restrict key, const char* restrict val, uint32_t line)
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

static void find_config_path()
{
    AString retval = AString_new_copy(&settings.config_path);

    if (retval.str) {
        return;
    }

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

bool on_settings_file_parse_syntax_error(uint32_t line, const char* msg_format, va_list msg_args)
{
    WRN("error in config on line %u: ", line);
    vfprintf(stderr, msg_format, msg_args);
    fputc('\n', stderr);
    return false;
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
            settings_file_parse(cfg, handle_config_option, on_settings_file_parse_syntax_error);
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
    Vector_destroy_Pair_char32_t(&settings.lcd_exclude_ranges);
    Vector_destroy_output_prefs_t(&settings.output_preferences);

    AString_destroy(&settings.config_path);
    AString_destroy(&settings.title_format);
    AString_destroy(&settings.term);
    AString_destroy(&settings.locale);
    AString_destroy(&settings.title);
    AString_destroy(&settings.uri_handler);
    AString_destroy(&settings.extern_pipe_handler);
    AString_destroy(&settings.directory);
    AString_destroy(&settings.font_style_regular);
    AString_destroy(&settings.font_style_bold);
    AString_destroy(&settings.font_style_italic);
    AString_destroy(&settings.font_style_bold_italic);

    free(settings.user_app_id);
    free(settings.user_app_id_2);
}
