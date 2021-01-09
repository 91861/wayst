/* See LICENSE for license information. */

/* Random stuff used for interacting with the Vt module, but not directly related to terminal
 * emulation  */

#define _GNU_SOURCE

#include "settings.h"
#include "util.h"

#include "vt.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#define VT_DIM_FACTOR 0.4f

Vector_char Vt_command_to_string(const Vt* self, const VtCommand* command, size_t opt_limit_lines)
{
    ASSERT(command->state == VT_COMMAND_STATE_COMPLETED, "is completed");

    size_t end;
    if (opt_limit_lines) {
        end = MIN(command->output_rows.second, command->output_rows.first + opt_limit_lines);
    } else {
        end = command->output_rows.second;
    }

    Vector_char str = Vector_new_with_capacity_char(128);
    for (size_t row = command->output_rows.first; row < end; ++row) {
        Vector_char ln = Vt_line_to_string(self, row, 0, Vt_col(self), "\n");
        Vector_pushv_char(&str, ln.buf, ln.size - 1);
        Vector_destroy_char(&ln);
    }
    Vector_push_char(&str, '\0');
    return str;
}

const char* Vt_uri_range_at(Vt*            self,
                            uint16_t       column,
                            size_t         row,
                            Pair_size_t*   out_rows,
                            Pair_uint16_t* out_columns)
{
    VtLine* base_line = Vt_line_at(self, row);
    if (!base_line || !base_line->links || column >= base_line->data.size) {
        return NULL;
    }

    uint16_t    base_link_idx = base_line->data.buf[column].hyperlink_idx;
    const char* base_link_str = Vt_uri_at(self, column, row);

    if (!base_link_idx || !base_link_str) {
        return NULL;
    }

    uint16_t start_column = column, end_column = column;
    size_t   min_row = row, max_row = row;

    /* Front */
    VtLine*  ln;
    uint16_t ln_base_idx;
    uint16_t line_start_column = start_column;

    for (size_t r = row; r + 1; --r) {
        ln = Vt_line_at(self, r);

        if (!ln || !ln->links || line_start_column >= ln->data.size) {
            min_row = r + 1;
            break;
        }

        ln_base_idx = 0;
        for (VtUri* i = NULL; (i = Vector_iter_VtUri(ln->links, i));) {
            if (!strcmp(i->uri_string, base_link_str)) {
                ln_base_idx = Vector_index_VtUri(ln->links, i) + 1;
                break;
            }
        }

        if (!ln_base_idx) {
            min_row = r + 1;
            break;
        }

        while (line_start_column) {
            if (ln->data.buf[line_start_column - 1].hyperlink_idx != ln_base_idx)
                break;
            --line_start_column;
        }

        start_column = line_start_column;
        if (line_start_column) {
            min_row = r;
            break;
        } else {
            line_start_column = Vt_col(self) - 1;
        }
    }

    /* Back */
    uint16_t line_end_column = end_column;
    for (size_t r = row; r < Vt_visual_bottom_line(self); ++r) {
        ln = Vt_line_at(self, r);

        if (!ln || !ln->links || line_end_column >= ln->data.size) {
            min_row = r - 1;
            break;
        }

        ln_base_idx = 0;
        for (VtUri* i = NULL; (i = Vector_iter_VtUri(ln->links, i));) {
            if (!strcmp(i->uri_string, base_link_str)) {
                ln_base_idx = Vector_index_VtUri(ln->links, i) + 1;
                break;
            }
        }

        if (!ln_base_idx) {
            min_row = r - 1;
            break;
        }

        while (line_end_column < Vt_col(self)) {
            if (ln->data.buf[line_end_column + 1].hyperlink_idx != ln_base_idx)
                break;
            ++line_end_column;
        }

        end_column = line_end_column;
        if (line_end_column < Vt_col(self) - 1) {
            max_row = r;
            break;
        } else {
            line_end_column = 0;
        }
    }

    if (out_rows) {
        out_rows->first  = min_row;
        out_rows->second = max_row;
    }
    if (out_columns) {
        out_columns->first  = start_column;
        out_columns->second = end_column;
    }

    return base_link_str;
}

Vector_char rune_vec_to_string(Vector_VtRune* line, size_t begin, size_t end, const char* tail)
{
    Vector_char res;
    end   = MIN((end ? end : line->size), line->size);
    begin = MIN(begin, line->size - 1);

    if (begin >= end) {
        res = Vector_new_with_capacity_char(2);
        if (tail) {
            Vector_pushv_char(&res, tail, strlen(tail) + 1);
        }
        return res;
    }
    res = Vector_new_with_capacity_char(end - begin);
    char             utfbuf[4];
    static mbstate_t mbstate;

    for (uint32_t i = begin; i < end; ++i) {
        Rune* rune = &line->buf[i].rune;

        if (rune->code == VT_RUNE_CODE_WIDE_TAIL) {
            continue;
        }

        if (rune->code > CHAR_MAX) {
            size_t bytes = c32rtomb(utfbuf, rune->code, &mbstate);
            if (bytes > 0) {
                Vector_pushv_char(&res, utfbuf, bytes);
            }
        } else if (!rune->code) {
            Vector_push_char(&res, ' ');
        } else {
            Vector_push_char(&res, rune->code);
        }

        for (int j = 0; j < VT_RUNE_MAX_COMBINE && rune->combine[j]; ++j) {
            size_t bytes = c32rtomb(utfbuf, rune->combine[j], &mbstate);
            if (bytes > 0) {
                Vector_pushv_char(&res, utfbuf, bytes);
            }
        }
    }
    if (tail) {
        Vector_pushv_char(&res, tail, strlen(tail) + 1);
    }

    return res;
}

void generate_color_palette_entry(ColorRGB* color, int16_t idx)
{
    ASSERT(idx >= 0 && idx <= 256, "index in range");
    ASSERT(color, "got color*");

    if (idx < 16) {
        /* Primary - from colorscheme */
        *color = settings.colorscheme.color[idx];
    } else if (idx < 232) {
        /* Extended */
        int16_t tmp = idx - 16;
        color->b    = (double)((tmp % 6) * 255) / 5.0;
        color->g    = (double)(((tmp /= 6) % 6) * 255) / 5.0;
        color->r    = (double)(((tmp / 6) % 6) * 255) / 5.0;
    } else {
        /* Grayscale */
        double tmp = (double)((idx - 232) * 10 + 8) / 256.0 * 255.0;

        *color = (ColorRGB){
            .r = tmp,
            .g = tmp,
            .b = tmp,
        };
    }
}

bool Vt_is_cell_selected(const Vt* const self, int32_t x, int32_t y)
{
    switch (expect(self->selection.mode, SELECT_MODE_NONE)) {
        case SELECT_MODE_NONE:
            return false;

        case SELECT_MODE_BOX:
            return !(Vt_visual_top_line(self) + y <
                       MIN(self->selection.end_line, self->selection.begin_line) ||
                     (Vt_visual_top_line(self) + y >
                      MAX(self->selection.end_line, self->selection.begin_line)) ||
                     (MAX(self->selection.begin_char_idx, self->selection.end_char_idx) < x) ||
                     (MIN(self->selection.begin_char_idx, self->selection.end_char_idx) > x));

        case SELECT_MODE_NORMAL:
            if (Vt_visual_top_line(self) + y >
                  MIN(self->selection.begin_line, self->selection.end_line) &&
                Vt_visual_top_line(self) + y <
                  MAX(self->selection.begin_line, self->selection.end_line)) {
                return true;
            } else {
                if (self->selection.begin_line == self->selection.end_line) {
                    return (self->selection.begin_line == Vt_visual_top_line(self) + y) &&
                           (x >=
                              MIN(self->selection.begin_char_idx, self->selection.end_char_idx) &&
                            x <= MAX(self->selection.begin_char_idx, self->selection.end_char_idx));
                } else if (Vt_visual_top_line(self) + y == self->selection.begin_line) {
                    return self->selection.begin_line < self->selection.end_line
                             ? x >= self->selection.begin_char_idx
                             : x <= self->selection.begin_char_idx;

                } else if (Vt_visual_top_line(self) + y == self->selection.end_line) {
                    return self->selection.begin_line > self->selection.end_line
                             ? x >= self->selection.end_char_idx
                             : x <= self->selection.end_char_idx;
                }
            }
            return false;
    }
    return false;
}

ColorRGB Vt_rune_final_fg_apply_dim(const Vt* self, const VtRune* rune, ColorRGBA bg_color)
{
    if (unlikely(rune->dim)) {
        return ColorRGB_new_from_blend(Vt_rune_fg(self, rune),
                                       ColorRGB_from_RGBA(bg_color),
                                       VT_DIM_FACTOR);
    } else {
        return Vt_rune_fg(self, rune);
    }
}

ColorRGB Vt_rune_final_fg(const Vt*     self,
                          const VtRune* rune,
                          int32_t       x,
                          int32_t       y,
                          ColorRGBA     bg_color)
{
    if (!settings.highlight_change_fg) {
        return Vt_rune_final_fg_apply_dim(self, rune, bg_color);
    } else {
        if (unlikely(Vt_is_cell_selected(self, x, y))) {
            return self->colors.highlight.fg;
        } else {
            return Vt_rune_final_fg_apply_dim(self, rune, bg_color);
        }
    }
}

ColorRGBA Vt_rune_final_bg(const Vt* self, const VtRune* rune, int32_t x, int32_t y)
{
    if (unlikely(Vt_is_cell_selected(self, x, y))) {
        return self->colors.highlight.bg;
    } else {
        return Vt_rune_bg(self, rune);
    }
}

static const char* const color_palette_names[] = {
    "Grey0",
    "NavyBlue",
    "DarkBlue",
    "Blue3",
    "Blue3",
    "Blue1",
    "DarkGreen",
    "DeepSkyBlue4",
    "DeepSkyBlue4",
    "DeepSkyBlue4",
    "DodgerBlue3",
    "DodgerBlue2",
    "Green4",
    "SpringGreen4",
    "Turquoise4",
    "DeepSkyBlue3",
    "DeepSkyBlue3",
    "DodgerBlue1",
    "Green3",
    "SpringGreen3",
    "DarkCyan",
    "LightSeaGreen",
    "DeepSkyBlue2",
    "DeepSkyBlue1",
    "Green3",
    "SpringGreen3",
    "SpringGreen2",
    "Cyan3",
    "DarkTurquoise",
    "Turquoise2",
    "Green1",
    "SpringGreen2",
    "SpringGreen1",
    "MediumSpringGreen",
    "Cyan2",
    "Cyan1",
    "DarkRed",
    "DeepPink4",
    "Purple4",
    "Purple4",
    "Purple3",
    "BlueViolet",
    "Orange4",
    "Grey37",
    "MediumPurple4",
    "SlateBlue3",
    "SlateBlue3",
    "RoyalBlue1",
    "Chartreuse4",
    "DarkSeaGreen4",
    "PaleTurquoise4",
    "SteelBlue",
    "SteelBlue3",
    "CornflowerBlue",
    "Chartreuse3",
    "DarkSeaGreen4",
    "CadetBlue",
    "CadetBlue",
    "SkyBlue3",
    "SteelBlue1",
    "Chartreuse3",
    "PaleGreen3",
    "SeaGreen3",
    "Aquamarine3",
    "MediumTurquoise",
    "SteelBlue1",
    "Chartreuse2",
    "SeaGreen2",
    "SeaGreen1",
    "SeaGreen1",
    "Aquamarine1",
    "DarkSlateGray2",
    "DarkRed",
    "DeepPink4",
    "DarkMagenta",
    "DarkMagenta",
    "DarkViolet",
    "Purple",
    "Orange4",
    "LightPink4",
    "Plum4",
    "MediumPurple3",
    "MediumPurple3",
    "SlateBlue1",
    "Yellow4",
    "Wheat4",
    "Grey53",
    "LightSlateGrey",
    "MediumPurple",
    "LightSlateBlue",
    "Yellow4",
    "DarkOliveGreen3",
    "DarkSeaGreen",
    "LightSkyBlue3",
    "LightSkyBlue3",
    "SkyBlue2",
    "Chartreuse2",
    "DarkOliveGreen3",
    "PaleGreen3",
    "DarkSeaGreen3",
    "DarkSlateGray3",
    "SkyBlue1",
    "Chartreuse1",
    "LightGreen",
    "LightGreen",
    "PaleGreen1",
    "Aquamarine1",
    "DarkSlateGray1",
    "Red3",
    "DeepPink4",
    "MediumVioletRed",
    "Magenta3",
    "DarkViolet",
    "Purple",
    "DarkOrange3",
    "IndianRed",
    "HotPink3",
    "MediumOrchid3",
    "MediumOrchid",
    "MediumPurple2",
    "DarkGoldenrod",
    "LightSalmon3",
    "RosyBrown",
    "Grey63",
    "MediumPurple2",
    "MediumPurple1",
    "Gold3",
    "DarkKhaki",
    "NavajoWhite3",
    "Grey69",
    "LightSteelBlue3",
    "LightSteelBlue",
    "Yellow3",
    "DarkOliveGreen3",
    "DarkSeaGreen3",
    "DarkSeaGreen2",
    "LightCyan3",
    "LightSkyBlue1",
    "GreenYellow",
    "DarkOliveGreen2",
    "PaleGreen1",
    "DarkSeaGreen2",
    "DarkSeaGreen1",
    "PaleTurquoise1",
    "Red3",
    "DeepPink3",
    "DeepPink3",
    "Magenta3",
    "Magenta3",
    "Magenta2",
    "DarkOrange3",
    "IndianRed",
    "HotPink3",
    "HotPink2",
    "Orchid",
    "MediumOrchid1",
    "Orange3",
    "LightSalmon3",
    "LightPink3",
    "Pink3",
    "Plum3",
    "Violet",
    "Gold3",
    "LightGoldenrod3",
    "Tan",
    "MistyRose3",
    "Thistle3",
    "Plum2",
    "Yellow3",
    "Khaki3",
    "LightGoldenrod2",
    "LightYellow3",
    "Grey84",
    "LightSteelBlue1",
    "Yellow2",
    "DarkOliveGreen1",
    "DarkOliveGreen1",
    "DarkSeaGreen1",
    "Honeydew2",
    "LightCyan1",
    "Red1",
    "DeepPink2",
    "DeepPink1",
    "DeepPink1",
    "Magenta2",
    "Magenta1",
    "OrangeRed1",
    "IndianRed1",
    "IndianRed1",
    "HotPink",
    "HotPink",
    "MediumOrchid1",
    "DarkOrange",
    "Salmon1",
    "LightCoral",
    "PaleVioletRed1",
    "Orchid2",
    "Orchid1",
    "Orange1",
    "SandyBrown",
    "LightSalmon1",
    "LightPink1",
    "Pink1",
    "Plum1",
    "Gold1",
    "LightGoldenrod2",
    "LightGoldenrod2",
    "NavajoWhite1",
    "MistyRose1",
    "Thistle1",
    "Yellow1",
    "LightGoldenrod1",
    "Khaki1",
    "Wheat1",
    "Cornsilk1",
    "Grey100",
    "Grey3",
    "Grey7",
    "Grey11",
    "Grey15",
    "Grey19",
    "Grey23",
    "Grey27",
    "Grey30",
    "Grey35",
    "Grey39",
    "Grey42",
    "Grey46",
    "Grey50",
    "Grey54",
    "Grey58",
    "Grey62",
    "Grey66",
    "Grey70",
    "Grey74",
    "Grey78",
    "Grey82",
    "Grey85",
    "Grey89",
    "Grey93",
};

int palette_color_index_from_xterm_name(const char* name)
{
    for (uint16_t i = 0; i < ARRAY_SIZE(color_palette_names); ++i) {
        if (strcasecmp(color_palette_names[i], name)) {
            return i + 16;
        }
    }
    return 0;
}

ColorRGB color_from_xterm_name(const char* name, bool* fail)
{
    for (uint16_t i = 0; i < ARRAY_SIZE(color_palette_names); ++i) {
        if (strcasecmp(color_palette_names[i], name)) {
            ColorRGB color;
            generate_color_palette_entry(&color, i + 16);
            return color;
        }
    }
    if (fail) {
        *fail = true;
    }
    return (ColorRGB){ 0 };
}

const char* name_from_color_palette_index(uint16_t index)
{
    if (index < 16 || index > 255) {
        return NULL;
    } else {
        return color_palette_names[index - 16];
    }
}

__attribute__((cold)) void Vt_dump_info(Vt* self)
{
    static int dump_index = 0;
    printf("\n====================[ STATE DUMP %2d ]====================\n", dump_index++);
    printf("parser state: ");
    if (self->parser.in_mb_seq) {
        puts("in multi-byte sequence");
    } else {
        switch (self->parser.state) {
            case PARSER_STATE_APC:
                puts("in application program command");
                break;
            case PARSER_STATE_CSI:
                puts("in control sequence");
                break;
            case PARSER_STATE_DCS:
                puts("in device control string");
                break;
            case PARSER_STATE_LITERAL:
                puts("character literal");
                break;
            case PARSER_STATE_PM:
                puts("privacy message");
                break;
            case PARSER_STATE_ESCAPED:
                puts("escape code");
                break;
            case PARSER_STATE_ESCAPED_CSI:
                puts("in control sequence escape code");
                break;
            case PARSER_STATE_DEC_SPECIAL:
                puts("DEC special command");
                break;
            case PARSER_STATE_OSC:
                puts("operating system command");
                break;
            case PARSER_STATE_TITLE:
                puts("legacy title select");
                break;
            case PARSER_STATE_CHARSET:
                puts("character set select");
                break;
            case PARSER_STATE_CHARSET_G0:
                puts("character set G0");
                break;
            case PARSER_STATE_CHARSET_G1:
                puts("character set G1");
                break;
            case PARSER_STATE_CHARSET_G2:
                puts("character set G2");
                break;
            case PARSER_STATE_CHARSET_G3:
                puts("character set G3");
                break;
        }
    }
    printf("Active character attributes:\n");
    printf("  foreground color:   " COLOR_RGB_FMT "\n",
           COLOR_RGB_AP((Vt_rune_fg(self, &self->parser.char_state))));
    printf("  background color:   " COLOR_RGBA_FMT "\n",
           COLOR_RGBA_AP((Vt_rune_bg(self, &self->parser.char_state))));
    printf("  line color uses fg: " BOOL_FMT "\n",
           BOOL_AP(!self->parser.char_state.line_color_not_default));
    printf("  line color:         " COLOR_RGB_FMT "\n",
           COLOR_RGB_AP((Vt_rune_ln_clr(self, &self->parser.char_state))));
    printf("  dim:                " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.dim));
    printf("  hidden:             " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.hidden));
    printf("  blinking:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.blinkng));
    printf("  underlined:         " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.underlined));
    printf("  strikethrough:      " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.strikethrough));
    printf("  double underline:   " BOOL_FMT "\n",
           BOOL_AP(self->parser.char_state.doubleunderline));
    printf("  curly underline:    " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.curlyunderline));
    printf("  overline:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.overline));
    printf("  inverted:           " BOOL_FMT "\n", BOOL_AP(self->parser.char_state.invert));
    printf("Tab ruler:\n");
    printf("  tabstop: %d\n  ", self->tabstop);
    for (int i = 0; i < Vt_col(self); ++i) {
        printf("%c", self->tab_ruler[i] ? '|' : '_');
    }
    printf("\nModes:\n");
    printf("  application keypad:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.application_keypad));
    printf("  application keypad cursor:        " BOOL_FMT "\n",
           BOOL_AP(self->modes.application_keypad_cursor));
    printf("  auto repeat:                      " BOOL_FMT "\n", BOOL_AP(self->modes.auto_repeat));
    printf("  bracketed paste:                  " BOOL_FMT "\n",
           BOOL_AP(self->modes.bracketed_paste));
    printf("  send DEL on delete:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.del_sends_del));
    printf("  don't send esc on alt:            " BOOL_FMT "\n",
           BOOL_AP(self->modes.no_alt_sends_esc));
    printf("  extended reporting:               " BOOL_FMT "\n",
           BOOL_AP(self->modes.extended_report));
    printf("  window focus events reporting:    " BOOL_FMT "\n",
           BOOL_AP(self->modes.window_focus_events_report));
    printf("  mouse button reporting:           " BOOL_FMT "\n",
           BOOL_AP(self->modes.mouse_btn_report));
    printf("  motion on mouse button reporting: " BOOL_FMT "\n",
           BOOL_AP(self->modes.mouse_motion_on_btn_report));
    printf("  mouse motion reporting:           " BOOL_FMT "\n",
           BOOL_AP(self->modes.mouse_motion_report));
    printf("  x10 compat mouse reporting:       " BOOL_FMT "\n",
           BOOL_AP(self->modes.x10_mouse_compat));
    printf("  no auto wrap:                     " BOOL_FMT "\n",
           BOOL_AP(self->modes.no_wraparound));
    printf("  reverse auto wrap:                " BOOL_FMT "\n",
           BOOL_AP(self->modes.reverse_wraparound));
    printf("  reverse video:                    " BOOL_FMT "\n",
           BOOL_AP(self->modes.video_reverse));

    printf("\nShell integration:\n  shell: \'%s\'\n  proto: \'%d\'\n  host: \'%s\'\n  dir: "
           "\'%s\'\n",
           self->shell_integration_shell_id,
           self->shell_integration_protocol_version,
           self->shell_integration_shell_host,
           self->shell_integration_current_dir);

    printf("  state: ");
    switch (self->shell_integration_state) {
        case VT_SHELL_INTEG_STATE_NONE:
            puts("none");
            break;
        case VT_SHELL_INTEG_STATE_PROMPT:
            puts("prompt");
            break;
        case VT_SHELL_INTEG_STATE_COMMAND:
            puts("command");
            break;
        case VT_SHELL_INTEG_STATE_OUTPUT:
            puts("output");
            break;
    }
    printf("  Command history:\n");
    for (RcPtr_VtCommand* i = NULL; (i = Vector_iter_RcPtr_VtCommand(&self->shell_commands, i));) {
        VtCommand* c = RcPtr_get_VtCommand(i);
        if (c) {
            printf("    \'%s\', exit status:%d, output lines: %zu..%zu\n",
                   c->command,
                   c->exit_status,
                   c->output_rows.first,
                   c->output_rows.second);
        }
    }

    printf("\n");
    printf("  S S | Number of lines %zu (last index: %zu)\n",
           self->lines.size,
           Vt_bottom_line(self));
    printf("  C C | Terminal size %hu x %hu\n", self->ws.ws_col, self->ws.ws_row);
    printf("V R R | \n");
    printf("I O . | Visible region: %zu - %zu\n",
           Vt_visual_top_line(self),
           Vt_visual_bottom_line(self));
    printf("E L   | \n");
    printf("W L V | Active line:  real: %zu (visible: %u)\n",
           self->cursor.row,
           Vt_cursor_row(self));
    printf("P   I | Cursor position: %u type: %d blink: %d hidden: %d\n",
           self->cursor.col,
           self->cursor.type,
           self->cursor.blinking,
           self->cursor.hidden);
    printf("O R E | Scroll region: %zu - %zu\n",
           Vt_get_scroll_region_top(self),
           Vt_get_scroll_region_bottom(self));
    printf("R E W | \n");
    printf("T G . +----------------------------------------------------\n");
    printf("| | |  BUFFER: %s\n", (self->alt_lines.buf ? "ALTERNATE" : "MAIN"));
    printf("V V V  \n");

    for (size_t i = 0; i < self->lines.size; ++i) {
        Vector_char str = rune_vec_to_string(&self->lines.buf[i].data, 0, 0, "");
        printf("%s%c %c %c %4zu%c s:%3zu dmg:%d proxy{%3d,%3d,%3d,%3d} reflow{%d,%d,%d} "
               "marks{%d,%d,%d,%d} data{%.90s%s}" TERMCOLOR_RESET "\n",
               i == self->cursor.row ? TERMCOLOR_BOLD : "",
               i == Vt_top_line(self)      ? 'v'
               : i == Vt_bottom_line(self) ? '^'
                                           : ' ',
               i == Vt_get_scroll_region_top(self) || i == Vt_get_scroll_region_bottom(self) ? '-'
                                                                                             : ' ',
               i == Vt_visual_top_line(self) || i == Vt_visual_bottom_line(self) ? '*' : ' ',
               i,
               i == self->cursor.row ? '<' : ' ',
               self->lines.buf[i].data.size,
               self->lines.buf[i].damage.type != VT_LINE_DAMAGE_NONE,
               self->lines.buf[i].proxy.data[0],
               self->lines.buf[i].proxy.data[1],
               self->lines.buf[i].proxy.data[2],
               self->lines.buf[i].proxy.data[3],
               self->lines.buf[i].reflowable,
               self->lines.buf[i].rejoinable,
               self->lines.buf[i].was_reflown,
               self->lines.buf[i].mark_command_invoke,
               self->lines.buf[i].mark_command_output_start,
               self->lines.buf[i].mark_command_output_end,
               self->lines.buf[i].mark_explicit,
               str.buf,
               (str.size > 90 ? "…" : ""));

        if (self->lines.buf[i].links) {
            for (uint16_t j = 0; j < self->lines.buf[i].links->size; ++j) {
                printf("              URI[%u]: %s\n",
                       j,
                       self->lines.buf[i].links->buf[j].uri_string);
            }
        }

        if (self->lines.buf[i].graphic_attachments &&
            self->lines.buf[i].graphic_attachments->images) {
            for (uint16_t j = 0; j < self->lines.buf[i].graphic_attachments->images->size; ++j) {
                VtLine*             ln = &self->lines.buf[i];
                VtImageSurfaceView* vu =
                  RcPtr_get_VtImageSurfaceView(&ln->graphic_attachments->images->buf[j]);
                VtImageSurface* src = RcPtr_get_VtImageSurface(&vu->source_image_surface);
                printf("              image anchor[%u] id: %u %ux%u\n",
                       j,
                       src->id,
                       src->width,
                       src->height);
            }
        }

        Vector_destroy_char(&str);
    }
}
