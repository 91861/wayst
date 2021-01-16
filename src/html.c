#define _GNU_SOURCE

#include <stddef.h>
#include <stdio.h>
#include <uchar.h>

#include "stb_image/stb_image.h"
#include "stb_image/stb_image_write.h"
#include "wcwidth/wcwidth.h"

#include "base64.h"
#include "colors.h"
#include "util.h"
#include "vt.h"
#include "vt_sixel.h"

typedef enum
{
    UL_NONE,
    UL_NORMAL,
    UL_DOUBLE,
    UL_CURLY,
} ul_style_t;

typedef struct
{
    ColorRGB         bg;
    ColorRGB         fg;
    ColorRGB         ul;
    enum VtRuneStyle rstyle;
    ul_style_t       ulstyle;
    bool             strikethrough;
    bool             overline;
    bool             blink;
} HtmlRuneState;

HtmlRuneState HtmlRuneState_from_vt_rune(const Vt* vt, VtRune* rune)
{
    HtmlRuneState ret = {
        .bg            = ColorRGB_from_RGBA(Vt_rune_bg(vt, rune)),
        .fg            = Vt_rune_fg(vt, rune),
        .ul            = Vt_rune_ln_clr(vt, rune),
        .rstyle        = rune->rune.style,
        .ulstyle       = UL_NONE,
        .strikethrough = rune->strikethrough,
        .overline      = rune->overline,
        .blink         = rune->blinkng,
    };

    if (rune->underlined) {
        ret.ulstyle = UL_NORMAL;
    } else if (rune->doubleunderline) {
        ret.ulstyle = UL_DOUBLE;
    } else if (rune->curlyunderline) {
        ret.ulstyle = UL_CURLY;
    }

    return ret;
}

static void start_span(Vector_char*     lines,
                       const char*      opt_class,
                       ColorRGB*        opt_bg,
                       ColorRGB*        opt_fg,
                       ColorRGB*        opt_ln,
                       enum VtRuneStyle style,
                       ul_style_t       ul_style,
                       bool             strikethrough,
                       bool             overline,
                       bool             blink)
{
    Vector_pushv_char(lines, "<span", strlen("<span"));

    if (opt_class) {
        Vector_pushv_char(lines, " class=\"", strlen(" class=\'"));
        Vector_pushv_char(lines, opt_class, strlen(opt_class));
        Vector_push_char(lines, '\"');
    }

    if (blink) {
        Vector_pushv_char(lines, " class=\"blink\"", strlen(" class=\"blink\""));
    }

    if (opt_bg || opt_fg || style != VT_RUNE_NORMAL || ul_style != UL_NONE || strikethrough ||
        overline) {
        Vector_pushv_char(lines, " style=\"", strlen(" style=\""));

        if (style == VT_RUNE_BOLD || style == VT_RUNE_BOLD_ITALIC) {
            Vector_pushv_char(lines, " font-weight: bold;", strlen(" font-weight: bold;"));
        }

        if (style == VT_RUNE_ITALIC || style == VT_RUNE_BOLD_ITALIC) {
            Vector_pushv_char(lines, " font-style: italic;", strlen(" font-style: italic;"));
        }

        if (opt_bg) {
            char* bg_str = asprintf(" background: " COLOR_RGB_FMT ";", COLOR_RGB_AP((*opt_bg)));
            Vector_pushv_char(lines, bg_str, strlen(bg_str));
            free(bg_str);
        }

        if (opt_fg) {
            char* fg_str = asprintf(" color: " COLOR_RGB_FMT ";", COLOR_RGB_AP((*opt_fg)));
            Vector_pushv_char(lines, fg_str, strlen(fg_str));
            free(fg_str);
        }

        if (ul_style != UL_NONE || overline || strikethrough) {
            Vector_pushv_char(lines, " text-decoration: ", strlen(" text-decoration: "));

            switch (ul_style) {
                case UL_NORMAL:
                    Vector_pushv_char(lines, "underline solid", strlen("underline solid"));
                    break;

                case UL_DOUBLE:
                    Vector_pushv_char(lines, "underline double", strlen("underline double"));
                    break;

                case UL_CURLY:
                    Vector_pushv_char(lines, "underline dashed", strlen("underline dashed"));
                    break;

                default:;
            }

            if (strikethrough) {
                Vector_pushv_char(lines, " line-through", strlen(" line-through"));
            }

            if (overline) {
                Vector_pushv_char(lines, " overline", strlen(" overline"));
            }

            ColorRGB* ln_clr = OR(opt_ln, opt_fg);
            if (ln_clr) {
                char buf[64];
                int  len = snprintf(buf, sizeof(buf), " " COLOR_RGB_FMT, COLOR_RGB_AP((*ln_clr)));
                Vector_pushv_char(lines, buf, len);
            }
            Vector_push_char(lines, ';');
        }
        Vector_push_char(lines, '\"');
    }
    Vector_push_char(lines, '>');
}

static void end_span(Vector_char* lines)
{
    Vector_pushv_char(lines, "</span>", strlen("</span>"));
}

static void png_write_to_vec_as_b64_func(void* context, void* data, int size)
{
    if (data) {
        size_t sz  = 0;
        char*  b64 = base64_encode_alloc(data, size, &sz);
        Vector_pushv_char(context, b64, sz);
        free(b64);
    } else {
        WRN("failed to convert sixel image to png\n");
    }
}

void write_html_screen_dump(const Vt* vt, FILE* file)
{
    if (!file || !vt) {
        return;
    }

    static const char* const sixel_css = "\n"
                                         "  .sixel {\n"
                                         "    position: absolute;\n"
                                         "    overflow: hidden;\n"
                                         "  }\n";

    Vector_char sixel_html = Vector_new_char();

    for (const RcPtr_VtSixelSurface* i = NULL;
         (i = Vector_iter_const_RcPtr_VtSixelSurface(&vt->scrolled_sixels, i));) {

        const VtSixelSurface* srf = RcPtr_get_const_VtSixelSurface(i);

        if (!srf || !VtSixelSurface_is_visible(vt, srf)) {
            continue;
        }

        int32_t top = srf->anchor_global_index - Vt_top_line(vt), left = srf->anchor_cell_idx,
            height = Vt_row(vt) - (top < 0 ? top : 0), width = Vt_col(vt) - (left < 0 ? left : 0);

        static const char* sixel_image_begin_format =
          "<div z-index=2 class=\"sixel\" style=\"top: %dem; left: %dem height %dem; width: "
          "%dem;\"><img src=\"data:image/png;base64,";

        char* sixel_image_begin = asprintf(sixel_image_begin_format, top, left, height, width);
        Vector_pushv_char(&sixel_html, sixel_image_begin, strlen(sixel_image_begin));
        free(sixel_image_begin);

        stbi_write_png_compression_level = 9;
        stbi_write_png_to_func(png_write_to_vec_as_b64_func,
                               &sixel_html,
                               srf->width,
                               srf->height,
                               3,
                               srf->fragments.buf,
                               0);

        static const char* sixel_image_end = "\"></div>";
        Vector_pushv_char(&sixel_html, sixel_image_end, strlen(sixel_image_end));
    }

    static const char* const page_template =
      "<!DOCTYPE html>\n"
      "<html lang=\"en\">\n"
      "\n"
      "<head>\n"
      "  <meta charset=\"UTF-8\">\n"
      "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0\">\n"
      "  <meta name=\"generator\" content=\"" APPLICATION_NAME " v" VERSION "\">\n"
      "  <title>%s</title>\n"
      "</head>\n"
      "\n"
      "<style>\n"
      "  * {\n"
      "    margin: 0;\n"
      "    padding: 0;\n"
      "  }\n"
      "\n"
      "  .blink {\n"
      "    animation: blink-animation 1.0s linear infinite;\n"
      "  }\n"
      "\n"
      "  @keyframes blink-animation {\n"
      "    0%%   { opacity: 0.6; }\n"
      "    10%%  { opacity: 0.1; }\n"
      "    35%%  { opacity: 0.1; }\n"
      "    65%%  { opacity: 1.0; }\n"
      "    90%%  { opacity: 1.0; }\n"
      "    100%% { opacity: 0.6; }\n"
      "  }\n"
      "\n"
      "  #vt {\n"
      "    float: left;\n"
      "    font-family: %s;\n"
      "    font-size: %u;\n"
      "    underline-position: from-font;\n"
      "    background-color: " COLOR_RGB_FMT ";\n"
      "    color: " COLOR_RGB_FMT ";\n"
      "  }\n"
      "%s"
      "</style>\n"
      "\n"
      "<body>\n"
      "  <div id=\"vt\">\n"
      "    <pre>%s</pre>\n"
      "  </div>\n"
      "%s"
      "</body>\n"
      "\n"
      "</html>\n";

    Vector_char lines = Vector_new_with_capacity_char(2048);

    VtLine *begin, *end;
    Vt_get_visible_lines(vt, &begin, &end);

    for (VtLine* line = begin; line < end; ++line) {
        char* spanclass = ((line - begin) % 2) ? "ev" : "od";

        start_span(&lines,
                   spanclass,
                   NULL,
                   NULL,
                   NULL,
                   VT_RUNE_NORMAL,
                   UL_NONE,
                   false,
                   false,
                   false);

        HtmlRuneState old_rune_state = {
            .bg      = ColorRGB_from_RGBA(vt->colors.bg),
            .fg      = vt->colors.fg,
            .ul      = vt->colors.fg,
            .rstyle  = VT_RUNE_NORMAL,
            .ulstyle = UL_NONE,
        };

        int    width;
        size_t line_limit = MIN(Vt_col(vt), line->data.size);

        for (size_t c_idx = 0; c_idx < line_limit; c_idx += MAX(1, width)) {
            VtRune*       rune           = &line->data.buf[c_idx];
            HtmlRuneState new_rune_state = HtmlRuneState_from_vt_rune(vt, rune);

            if (memcmp(&old_rune_state, &new_rune_state, sizeof(new_rune_state))) {
                end_span(&lines);
                start_span(&lines,
                           NULL,
                           VtRune_bg_is_default(rune) ? NULL : &new_rune_state.bg,
                           VtRune_fg_is_default(rune) ? NULL : &new_rune_state.fg,
                           rune->line_color_not_default ? &new_rune_state.ul : NULL,
                           new_rune_state.rstyle,
                           new_rune_state.ulstyle,
                           new_rune_state.strikethrough,
                           new_rune_state.overline,
                           new_rune_state.blink);
            }

            if (rune->hidden || rune->rune.code == ' ' || !rune->rune.code) {
                Vector_push_char(&lines, ' ');
            } else {
                char      buf[5] = { 0 };
                mbstate_t mbs    = { 0 };
                size_t    len    = c32rtomb(buf, rune->rune.code, &mbs);
                if (len > 0) {
                    Vector_pushv_char(&lines, buf, len);
                }
                for (uint_fast8_t i = 0; i < VT_RUNE_MAX_COMBINE && rune->rune.combine[i]; ++i) {
                    memset(&mbs, 0, sizeof(mbs));
                    memset(buf, 0, sizeof(buf));
                    len = c32rtomb(buf, rune->rune.combine[i], &mbs);
                    if (len > 0) {
                        Vector_pushv_char(&lines, buf, len);
                    }
                }
            }
            width          = wcwidth(line->data.buf[c_idx].rune.code);
            old_rune_state = new_rune_state;
        }
        end_span(&lines);

        if (line != end) {
            Vector_push_char(&lines, '\n');
        }
    }

    Vector_push_char(&lines, '\0');
    Vector_push_char(&sixel_html, '\0');

    fprintf(file,
            page_template,
            vt->title,
            settings.styled_fonts.buf[0].family_name,
            settings.font_size,
            COLOR_RGB_AP(vt->colors.bg),
            COLOR_RGB_AP(vt->colors.fg),
            sixel_html.size > 1 ? sixel_css : "",
            lines.buf,
            sixel_html.size > 1 ? sixel_html.buf : "");

    Vector_destroy_char(&lines);
    Vector_destroy_char(&sixel_html);
}
