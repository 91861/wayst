/* See LICENSE for license information. */


#define _GNU_SOURCE

#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <locale.h>

#include <fontconfig/fontconfig.h>

#include "settings.h"
#include "util.h"
#include "vector.h"

#ifndef CFG_SDIR_NAME
#define CFG_SDIR_NAME "wayst"
#endif

#ifndef CFG_FNAME
#define CFG_FNAME "config"
#endif


ColorRGB color_palette_256[257];

static const char* const arg_path = "path";
static const char* const arg_int = "number";
static const char* const arg_color = "#RRGGBB";
static const char* const arg_color_a = "#RRGGBBAA";
static const char* const arg_string = "string";

// -e and -x are reserved
static struct option
long_options[] =
{
    { "config-file",                 required_argument, 0, 'i' },
    { "skip-config",                 no_argument,       0, 'C' },
    { "xorg-only",                   no_argument,       0, 'X' },
    { "term",                        required_argument, 0, 'r' },
    { "title",                       required_argument, 0, 't' },
    { "no-dynamic-title",            no_argument,       0, 'T' },
    { "title-format",                required_argument, 0, 'o' },
    { "locale",                      required_argument, 0, 'l' },
    { "rows",                        required_argument, 0, 'R' },
    { "columns",                     required_argument, 0, 'c' },
    { "bg-color",                    required_argument, 0, '0' },
    { "fg-color",                    required_argument, 0, '1' },
    { "color-0",                     required_argument, 0, '~' },
    { "color-1",                     required_argument, 0, '~' },
    { "color-2",                     required_argument, 0, '~' },
    { "color-3",                     required_argument, 0, '~' },
    { "color-4",                     required_argument, 0, '~' },
    { "color-5",                     required_argument, 0, '~' },
    { "color-6",                     required_argument, 0, '~' },
    { "color-7",                     required_argument, 0, '~' },
    { "color-8",                     required_argument, 0, '~' },
    { "color-9",                     required_argument, 0, '~' },
    { "color-10",                    required_argument, 0, '~' },
    { "color-11",                    required_argument, 0, '~' },
    { "color-12",                    required_argument, 0, '~' },
    { "color-13",                    required_argument, 0, '~' },
    { "color-14",                    required_argument, 0, '~' },
    { "color-15",                    required_argument, 0, '~' },
    { "fg-color-dim",                required_argument, 0, '2' },
    { "h-bg-color",                  required_argument, 0, '3' },
    { "h-fg-color",                  required_argument, 0, '4' },
    { "h-change-fg",                 no_argument,       0, 'f' },
    { "no-flash",                    no_argument,       0, 'F' },
    { "colorscheme",                 required_argument, 0, 's' },
    { "font",                        required_argument, 0, 'Y' },
    { "font-size",                   required_argument, 0, 'S' },
    { "dpi",                         required_argument, 0, 'D' },
    { "scroll-lines",                required_argument, 0, 'y' },
    { "version",                     no_argument,       0, 'v' },
    { "help",                        no_argument,       0, 'h' },
    { 0 }
};


static const char* long_options_descriptions[][2] =
{
    { arg_path    , "use configuration file"                                },
    { NULL        , "skip default configuration file"                       },
    { NULL        , "always use X11"                                        },
    { arg_string  , "TERM value"                                            },
    { arg_string  , "window title and application class name"               },
    { NULL        , "don't allow programs to change window title"           },
    { arg_string  , "window title format string"                            },
    { arg_string  , "override locale"                                       },
    { arg_int     , "number of rows"                                        },
    { arg_int     , "number of columns"                                     },
    { arg_color_a , "background color"                                      },
    { arg_color   , "foreground color"                                      },
    { arg_color   , "palette color black"                                   },
    { arg_color   , "palette color red"                                     },
    { arg_color   , "palette color green"                                   },
    { arg_color   , "palette color yellow"                                  },
    { arg_color   , "palette color blue"                                    },
    { arg_color   , "palette color magenta"                                 },
    { arg_color   , "palette color cyan"                                    },
    { arg_color   , "palette color grey"                                    },
    { arg_color   , "palette color bright black"                            },
    { arg_color   , "palette color bright red"                              },
    { arg_color   , "palette color bright green"                            },
    { arg_color   , "palette color bright yellow"                           },
    { arg_color   , "palette color bright blue"                             },
    { arg_color   , "palette color bright magenta"                          },
    { arg_color   , "palette color bright cyan"                             },
    { arg_color   , "palette color bright grey"                             },
    { arg_color   , "dim/faint text color"                                  },
    { arg_color_a , "highlighted text background color"                     },
    { arg_color   , "highlighted text foreground color"                     },
    { NULL        , "highligting text changes foreground color"             },
    { NULL        , "disable visual flash"                                  },
    { "name"      , "colorscheme preset: wayst, linux, xterm, rxvt, yaru, tan"
                    "go, orchis, solarized"                                 },
    { "name"      , "font family"                                           },
    { arg_int     , "font size"                                             },
    { arg_int     , "dpi"                                                   },
    { arg_int     , "lines scrolled per whell click"                        },
    { NULL        , "show version"                                          },
    { NULL        , "show this message"                                     },
    { 0 }
};


const char* const
colors_default[8][18] =
{
    {   /* wayst */
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
    {   /* linux console */
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
    {   /* xterm */
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
    {   /* rxvt */
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
    {   /* yaru */
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
        "FFFFFF"
    },
    {   /* tango */
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
    {   /* orchis */
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
    },  /* solarized */
    {
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
        "839496"
    }
};


Settings
settings;


/** get default colorscheme  */
static Colorscheme
Colorscheme_default(int idx)
{
    if (idx >= 8)
        idx = 0;

    Colorscheme c;

    for (uint32_t i = 0; i < 16; ++i)
        c.color[i] = ColorRGB_from_hex(colors_default[idx][i], NULL);

    if (colors_default[idx][16])
        settings.bg = ColorGRBA_from_hex(colors_default[idx][16], NULL);

    if (colors_default[idx][17])
        settings.fg = ColorRGB_from_hex(colors_default[idx][17], NULL);

    return c;
}


/** Initialize 256color colorpallete */
static void
init_color_palette()
{
    for (int_fast16_t i = 0; i < 257; ++i) {
        if (i < 16) {
            /* Primary - from colorscheme */
            color_palette_256[i] = settings.colorscheme.color[i];
        } else if (i < 232) {
            /* Extended */
            int_fast16_t tmp = i -16;
            color_palette_256[i].b = (double) (( tmp       % 6) *255) /5.0;
            color_palette_256[i].g = (double) (((tmp /= 6) % 6) *255) /5.0;
            color_palette_256[i].r = (double) (((tmp / 6)  % 6) *255) /5.0;
        } else {
            /* Grayscale */
            double tmp = (double) ((i -232) *10 +8) /256.0 *255.0;
            color_palette_256[i] = (ColorRGB) {
                .r = tmp,
                .g = tmp,
                .b = tmp,
            };
        }
    }
}


/** Use fontconfig to get font files */
static void
find_font()
{
    FcConfig* cfg = FcInitLoadConfigAndFonts();

    FcPattern* pat = FcNameParse((const FcChar8*) settings.font);

    FcObjectSet* os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, NULL);
    FcFontSet* fs = FcFontList(cfg, pat, os);

    for (int_fast32_t i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];
        FcChar8 *file, *style;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch)
        {
            if (!strcmp((const char*)style, "Regular"))
                settings.font_name = strdup((char*) file);

            if (!strcmp((const char*)style, "Bold"))
                settings.font_name_bold = strdup((char*) file);

            if (!strcmp((const char*)style, "Italic"))
                settings.font_name_italic = strdup((char*) file);
        }
    }

    FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);

    pat = FcNameParse((const FcChar8*) settings.font_fallback);
    os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, NULL);
    fs = FcFontList(cfg, pat, os);

    for (int_fast32_t i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];
        FcChar8 *file, *style;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch)
        {
            free(settings.font_name_fallback);
            settings.font_name_fallback = strdup((char*) file);
        }
    }

    FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);


    pat = FcNameParse((const FcChar8*) settings.font_fallback2);
    os = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_FILE, NULL);
    fs = FcFontList(cfg, pat, os);

    for (int_fast32_t i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];
        FcChar8 *file, *style;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch)
        {
            settings.font_name_fallback2 = strdup((char*) file);
        }
    }

    FcFontSetDestroy(fs);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);

    FcConfigDestroy(cfg);


    if (!settings.font_name)
        ERR("Failed to locate font files for \"%s\"", settings.font);

    if (!settings.font_name_bold)
        WRN("Selected font has no bold style\n");

    if (!settings.font_name_italic)
        WRN("Selected font has no italic style\n");

    if (!settings.font_name_fallback)
        WRN("Fallback font could not be found\n");

    if (!settings.font_name_fallback2)
        WRN("Fallback font could not be found\n");

    LOG("font files:\n  normal: %s\n  bold: %s\n  italic: %s\n"
        "  fallback/symbol: %s\n  fallback/symbol: %s\n",
        settings.font_name,
        settings.font_name_bold ? settings.font_name_bold : "(none)",
        settings.font_name_italic ? settings.font_name_italic : "(none)",
        settings.font_name_fallback ? settings.font_name_fallback : "(none)",
        settings.font_name_fallback2 ? settings.font_name_fallback2 : "(none)");

}


static void
settings_make_default()
{
    settings = (Settings)
    {
        .config_path = NULL,
        .skip_config = false,
        .x11_is_default = false,
        .shell = NULL,
        .shell_argc = 0,
        .shell_argv = NULL,
        .term = "xterm-256color",
        .locale = NULL,
        .bsp_sends_del = true,

        .font = "Noto Sans Mono",
        .font_fallback = "FontAwesome",
        .font_fallback2 = "NotoColorEmoji",
        .font_size = 10,
        .font_dpi = 96,

        .bg =     { .r = 0,   .g = 0,   .b = 0,   .a = 240 },
        .bghl =   { .r =  50, .g =  50, .b =  50, .a = 240 },
        .fg =     { .r = 255, .g = 255, .b = 255 },
        .fghl =   { .r = 255, .g = 255, .b = 255 },
        .fg_dim = { .r = 150, .g = 150, .b = 150 },

        .highlight_change_fg = false,
        .title = "Wayst",
        .dynamic_title = true,
        .title_format = "%2$s - %1$s",

        .cols = 80,
        .rows = 24,

        .colorscheme_preset = 0,

        .text_blink_interval = 750,

        .bell_flash = { .r = 20, .g = 20, .b = 20, .a = 240 },

        .allow_scrollback_clear = false,
        .scroll_on_output = false,
        .scroll_on_key = true,
        .scroll_discrete_lines = 3,
    };
}


static void
settings_complete_defaults()
{
    setlocale(LC_ALL, settings.locale ? settings.locale : "");
    settings.colorscheme =
        Colorscheme_default(settings.colorscheme_preset);

    find_font();
}


static void
print_help(char* const* argv)
{
    printf("Usage: %s [options...] [-e/x command args...]\n", argv[0] +2);

    for (uint32_t i = 0;
         i < sizeof(long_options) / sizeof(long_options[0]) -1;
         ++i)
    {
        if (long_options[i].has_arg == no_argument &&
            long_options[i].val)
        {
            printf(" -%c, ", long_options[i].val);
        } else {
            printf("     ");
        }

        if (long_options[i].has_arg == required_argument) {
            printf(" --%-s <%s>%-*s",
                    long_options[i].name,
                    long_options_descriptions[i][0],
                    (int)(20 - strlen(long_options[i].name)
                          - strlen(long_options_descriptions[i][0])),
                    "");
        } else {
            printf(" --%s %*s",
                    long_options[i].name,
                    (int)(22 - strlen(long_options[i].name)),
                    "");
        }

        puts(long_options_descriptions[i][1]);
    }

    exit(0);
}


static void
handle_option(const char opt,
              const char* value,
              const int argc,
              char* const* argv)
{
    switch (opt) {

    case 'X':
        settings.x11_is_default = true;
        break;

    case 'v':
        printf("version: "VERSION"\n");
        break;

    case 'h':
        print_help(argv);
        break;

    case 'T':
        settings.dynamic_title = false;
        break;

    case 'f':
        settings.highlight_change_fg = true;
        break;

    case 'F':
        settings.no_flash = true;
        break;
                    
    case 'Y':
        settings.font = strdup(value);
        break;
                
    case 'S':
        settings.font_size = strtoul(value, NULL, 10);
        break;

    case 'D':
        settings.font_dpi = strtoul(value, NULL, 10);
        break;

    case 's':
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
            settings.colorscheme_preset = MIN(strtoul(value, NULL, 10),
                                              sizeof(colors_default) -1);
        break;

    case 't':
        settings.title = strdup(value);
        break;

    case 'c':
        settings.cols = strtol(value, NULL, 10);
        break;

    case 'R':
        settings.rows = strtol(value, NULL, 10);
        break;

    case 'r':
        settings.term = strdup(value);
        break;

    case 'l':
        settings.locale = strdup(value);
        break;

    case 'y':
        settings.scroll_discrete_lines = MIN(strtod(value, NULL), UINT8_MAX);
        break;

    case 'o':
        settings.title_format = strdup(value);
        break;

    case '0':
        settings.bg = ColorGRBA_from_hex(value, NULL);
        break;

    case '1':
        settings.fg = ColorRGB_from_hex(value, NULL);
        break;

    case '2':
        settings.fg_dim = ColorRGB_from_hex(value, NULL);
        break;

    case '3':
        settings.bghl = ColorGRBA_from_hex(value, NULL);
        break;

    case '4':
        settings.fghl = ColorRGB_from_hex(value, NULL);
        break;
    }
}

static void
settings_get_opts(const int argc,
                  char* const* argv,
                  const bool cfg_file_check)
{

    optind = 1;
    for (int o; (optind < argc && optind >= 0 &&
                 strcmp(argv[optind], "-e") &&
                 strcmp(argv[optind], "-x"));)
    {
        /* print 'invalid option' error message only once */
        opterr = cfg_file_check;

        int opid = 0;
        o = getopt_long(argc, argv, "XCTfFhv", long_options, &opid);

        if (o == -1)
            break;

        if (cfg_file_check) {
            if (o == 'C')
                settings.skip_config = true;
            else if (o == 'i')
                settings.config_path = strdup(optarg);
        } else
            handle_option(o, optarg, argc, argv);
    }

    /* get program name and args for it */
    if (!cfg_file_check) {
        for (int i = 0; i < argc; ++i) {
            if (!strcmp(argv[i], "-e") || !strcmp(argv[i], "-x")) {
                if (argc > (i++ +1)) {
                    settings.shell = strdup(argv[i]);
                    settings.shell_argc = argc - i;
                    settings.shell_argv =
                        calloc((settings.shell_argc +1), sizeof(char*));

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
            settings.shell_argv = calloc(2, sizeof(char*));
            settings.shell_argv[0] = strdup(settings.shell);
            settings.shell_argc = 1;
        }
    }
}


static void
handle_config_option(const char* key,
                     const char* val,
                     const int argc,
                     char* const* argv)
{
    if (val && key)
        for (struct option* opt = long_options; opt->name; ++opt)
            if (!strcmp(key, opt->name))
                if (opt->has_arg == required_argument ||
                        !strcasecmp(val, "true"))
                    handle_option(opt->val, val, argc, argv);
}


DEF_VECTOR(char, NULL);
static void
settings_file_parse(FILE* f, const int argc, char* const* argv)
{
    if (!f)
        return;

    bool in_comment = false;
    bool in_value = false;
    bool in_string = false;
    bool escaped = false;

    char buf[512] = { 0 };
    int rd;

    Vector_char key = Vector_new_with_capacity_char(30);
    Vector_char value = Vector_new_with_capacity_char(30);

    while ((rd = fread(buf, sizeof(char), sizeof buf -1, f))) {

        for (int i = 0; i < rd; ++i) {
            if (in_comment) {
                if (buf[i] == '\n') {
                    in_comment = false;
                    in_string = false;
                }
                continue;
            } else if (in_value) {

                if (!in_string && buf[i] == '#') {
                    in_comment = true;
                    in_value = false;
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
                    in_value = false;
                    in_string = false;
                    Vector_push_char(&value, 0);
                    handle_config_option(key.buf, value.buf, argc, argv);
                    value.size = 0;
                } else {
                    if (!isblank(buf[i]) || in_string || escaped) {
                        Vector_push_char(
                            &value,
                            (buf[i] == 'n' && escaped) ? '\n' : buf[i]);
                    }
                }

            } else {
                if (buf[i] == '=' && !escaped) {
                    Vector_push_char(&key, 0);
                    key.size = 0;
                    in_value = true;
                } else if(buf[i] == '\n' && !escaped) {
                    key.size = 0;
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


static const char*
find_config_path()
{
    if (settings.config_path)
        return settings.config_path;

    char* tmp = NULL;
    if ((tmp = getenv("XDG_CONFIG_HOME"))) {
        return asprintf("%s/%s/%s", tmp, CFG_SDIR_NAME, CFG_FNAME);
    } else if ((tmp = getenv("HOME"))) {
        return asprintf("%s/.config/%s/%s", tmp, CFG_SDIR_NAME, CFG_FNAME);;
    } else {
        WRN("Could not find config directory\n");
        return NULL;
    }
}


static FILE*
open_config(const char* path)
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


void
settings_init(const int argc, char* const* argv)
{
    settings_make_default();
    settings_get_opts(argc, argv, true);
    if (!settings.skip_config) {
        const char* cfg_path = find_config_path();
        FILE* cfg = open_config(cfg_path);
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


void
settings_cleanup()
{
    if (settings.font_name)
        free(settings.font_name);

    if (settings.font_name_bold)
        free(settings.font_name_bold);

    if (settings.font_name_italic)
        free(settings.font_name_italic);

    if (settings.font_name_fallback)
        free(settings.font_name_fallback);

    if (settings.font_name_fallback2)
        free(settings.font_name_fallback2);
}

