/* See LICENSE for license information. */

#define _GNU_SOURCE

#include "settings.h"
#include "util.h"

#include "vt.h"
#include "vt_private.h"
#include "vt_shell.h"
#include "vt_sixel.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <uchar.h>

#include "vt_img_proto.h"

#include "key.h"

static void Vt_shift_global_line_index_refs(Vt* self, size_t point, int64_t change, bool refs_only);
static inline size_t Vt_top_line(const Vt* const self);
void                 Vt_visual_scroll_to(Vt* self, size_t line);
void                 Vt_visual_scroll_reset(Vt* self);
static void          Vt_alt_buffer_on(Vt* self, bool save_mouse);
static void          Vt_alt_buffer_off(Vt* self, bool save_mouse);
static void          Vt_handle_multi_argument_SGR(Vt* self, Vector_char seq, VtRune* opt_target);
static void          Vt_reset_text_attribs(Vt* self, VtRune* opt_target);
static void          Vt_carriage_return(Vt* self);
static void          Vt_clear_right(Vt* self);
static void          Vt_clear_left(Vt* self);
static inline void   Vt_scroll_out_all_content(Vt* self);
static void          Vt_empty_line_fill_bg(Vt* self, size_t idx);
static void          Vt_insert_new_line(Vt* self);
static void          Vt_scroll_up(Vt* self);
static void          Vt_scroll_down(Vt* self);
static void          Vt_reverse_line_feed(Vt* self);
static void          Vt_delete_line(Vt* self);
static void          Vt_delete_chars(Vt* self, size_t n);
static void          Vt_erase_chars(Vt* self, size_t n);
static void          Vt_clear_above(Vt* self);
static inline void   Vt_scroll_out_above(Vt* self);
static void          Vt_insert_line(Vt* self);
static void          Vt_clear_display_and_scrollback(Vt* self);
static void          Vt_erase_to_end(Vt* self);
static void          Vt_move_cursor(Vt* self, uint16_t c, uint16_t r);
static void          Vt_set_title(Vt* self, const char* title);
static void          Vt_push_title(Vt* self);
static void          Vt_pop_title(Vt* self);
static void          Vt_insert_char_at_cursor(Vt* self, VtRune c);
static void          Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c);
static bool          Vt_alt_buffer_enabled(Vt* self);
static inline void   Vt_mark_proxy_fully_damaged(Vt* self, size_t idx);
static void          Vt_mark_proxy_damaged_cell(Vt* self, size_t line, size_t rune);
static void          Vt_init_tab_ruler(Vt* self);
static void          Vt_reset_tab_ruler(Vt* self);

static inline VtLine VtLine_new()
{
    VtLine line;
    memset(&line, 0, sizeof(VtLine));

    line.damage.type = VT_LINE_DAMAGE_FULL;
    line.reflowable  = true;
    line.data        = Vector_new_VtRune();

    return line;
}

static inline void VtLine_strip_blanks(VtLine* self)
{
    for (VtRune* i = NULL; (i = Vector_last_VtRune(&self->data));) {
        if ((i->rune.code == ' ' || i->rune.code == '\0') && !i->rune.combine[0] &&
            !i->hyperlink_idx && !i->invert && !i->underlined && !i->blinkng &&
            !i->doubleunderline && !i->strikethrough && i->bg_is_palette_entry &&
            i->bg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT && i->fg_is_palette_entry &&
            i->fg_data.index == VT_RUNE_PALETTE_INDEX_TERM_DEFAULT) {
            Vector_pop_VtRune(&self->data);
        } else {
            break;
        }
    }
}

void Vt_output(Vt* self, const char* buf, size_t len)
{
    Vector_pushv_char(&self->output, buf, len);
}

static void Vt_bell(Vt* self)
{
    if (!settings.no_flash)
        CALL(self->callbacks.on_visual_bell, self->callbacks.user_data);
    if (self->modes.pop_on_bell)
        CALL(self->callbacks.on_restack_to_front, self->callbacks.user_data);
    if (self->modes.urgency_on_bell)
        CALL(self->callbacks.on_urgency_set, self->callbacks.user_data);
}

static inline size_t Vt_top_line_alt(const Vt* const self)
{
    return self->alt_lines.size <= Vt_row(self) ? 0 : self->alt_lines.size - Vt_row(self);
}

static inline size_t Vt_bottom_line_alt(Vt* self)
{
    return Vt_top_line_alt(self) + Vt_row(self) - 1;
}

static void Vt_command_output_interrupted(Vt* self)
{
    self->shell_integration_state = VT_SHELL_INTEG_STATE_NONE;
}

static void Vt_uri_complete(Vt* self)
{
    if (self->uri_matcher.start_row == self->cursor.row && self->cursor.col) {
        for (uint16_t i = self->uri_matcher.start_column; i < self->cursor.col; ++i) {
            VtRune* r = Vt_at(self, i, self->cursor.row);
            if (r) {
                r->hyperlink_idx =
                  VtLine_add_link(Vt_cursor_line(self), self->uri_matcher.match.buf) + 1;
            }
        }
    } else {
        for (uint16_t i = self->uri_matcher.start_column; i < Vt_col(self); ++i) {
            VtRune* r = Vt_at(self, i, self->uri_matcher.start_row);
            if (r) {
                r->hyperlink_idx = VtLine_add_link(Vt_line_at(self, self->uri_matcher.start_row),
                                                   self->uri_matcher.match.buf) +
                                   1;
            }
        }
        for (size_t row = self->uri_matcher.start_row + 1; row < self->cursor.row; ++row) {
            for (uint16_t i = 0; i < self->cursor.col; ++i) {
                VtRune* r = Vt_at(self, i, row);
                if (r) {
                    r->hyperlink_idx =
                      VtLine_add_link(Vt_line_at(self, row), self->uri_matcher.match.buf) + 1;
                }
            }
        }
        for (uint16_t i = 0; i < self->cursor.col; ++i) {
            VtRune* r = Vt_at(self, i, self->cursor.row);
            if (r) {
                r->hyperlink_idx =
                  VtLine_add_link(Vt_cursor_line(self), self->uri_matcher.match.buf) + 1;
            }
        }
    }

    LOG("Vt::uri_match: %s\n", self->uri_matcher.match.buf);
}

static bool isurl(char32_t c)
{
    if (c > 255)
        return false;

    switch (c) {
        case '-':
        case '.':
        case '_':
        case '~':
        case ':':
        case '/':
        case '?':
        case '#':
        case '[':
        case ']':
        case '@':
        case '!':
        case '$':
        case '&':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case '%':
        case '\'':
            return true;
    }

    return isalnum(c);
}

static void Vt_uri_break_match(Vt* self)
{
    if (self->uri_matcher.state == VT_URI_MATCHER_PATH) {
        Vector_push_char(&self->uri_matcher.match, '\0');
        Vt_uri_complete(self);
    } else if (self->uri_matcher.state == VT_URI_MATCHER_SUFFIX_REFERENCE) {
        Vector_push_char(&self->uri_matcher.match, '\0');
        if (streq_glob(self->uri_matcher.match.buf, "www.*.*")) {
            Vt_uri_complete(self);
        }
    } else if (self->uri_matcher.state == VT_URI_MATCHER_AUTHORITY) {
        Vector_push_char(&self->uri_matcher.match, '\0');
        if (strstr(self->uri_matcher.match.buf, ".")) {
            Vt_uri_complete(self);
        }
    }
    self->uri_matcher.state = VT_URI_MATCHER_EMPTY;
    Vector_clear_char(&self->uri_matcher.match);
}

static void Vt_uri_next_char(Vt* self, char32_t c)
{
    switch (self->uri_matcher.state) {
        case VT_URI_MATCHER_EMPTY: {
            if (c <= CHAR_MAX && isalpha(c)) {
                Vector_push_char(&self->uri_matcher.match, c);
                self->uri_matcher.state        = VT_URI_MATCHER_SCHEME;
                self->uri_matcher.start_column = self->cursor.col;
                self->uri_matcher.start_row    = self->cursor.row;
            }
        } break;

        case VT_URI_MATCHER_SCHEME: {
            /* We care if we should use it, not if it's valid. Drop '+' '-' '.' */
            if (isalnum(c) && self->uri_matcher.match.size < 10) {
                Vector_push_char(&self->uri_matcher.match, c);
            } else if (c == ':') {
                static const char* const SUPPORTED_SCHEMES[] = {
                    "file", "http",   "https",  "shttp",  "irc",    "smb",     "udp",
                    "xmpp", "xri",    "magnet", "mailto", "callto", "message", "mumble",
                    "ssh",  "telnet", "imap",   "pop",    "ftp",    "sftp",    "tftp",
                    "nfs",  "fish",   "git",    "svn",    "jar",    "mvn",     "vnc",
                    "rdp",  "spice",  "nx",     "cvs",    "admin",  "app",
                };

                Vector_push_char(&self->uri_matcher.match, '\0');
                bool is_supported = false;
                for (uint_fast8_t i = 0; i < ARRAY_SIZE(SUPPORTED_SCHEMES); ++i) {
                    if (!strcasecmp(SUPPORTED_SCHEMES[i], self->uri_matcher.match.buf)) {
                        is_supported = true;
                    }
                }
                if (is_supported) {
                    Vector_pop_char(&self->uri_matcher.match);
                    Vector_push_char(&self->uri_matcher.match, c);
                    self->uri_matcher.state = VT_URI_MATCHER_SCHEME_COMPLETE;
                } else {
                    Vt_uri_break_match(self);
                }

            } else if (c == '.') {
                Vector_push_char(&self->uri_matcher.match, '\0');
                if (!strcmp("www", self->uri_matcher.match.buf)) {
                    Vector_pop_char(&self->uri_matcher.match);
                    Vector_push_char(&self->uri_matcher.match, c);
                    self->uri_matcher.state = VT_URI_MATCHER_SUFFIX_REFERENCE;
                } else {
                    Vt_uri_break_match(self);
                }
            } else {
                Vt_uri_break_match(self);
            }
        } break;

        case VT_URI_MATCHER_SCHEME_COMPLETE: {
            if (c == '/') {
                Vector_push_char(&self->uri_matcher.match, c);
                self->uri_matcher.state = VT_URI_MATCHER_FST_LEADING_SLASH;
            } else {
                Vt_uri_break_match(self);
            }
        } break;

        case VT_URI_MATCHER_FST_LEADING_SLASH: {
            if (c == '/') {
                Vector_push_char(&self->uri_matcher.match, c);
                self->uri_matcher.state = VT_URI_MATCHER_AUTHORITY;
            } else {
                Vt_uri_break_match(self);
            }
        } break;

        case VT_URI_MATCHER_AUTHORITY: {
            if (c == '/') {
                Vector_push_char(&self->uri_matcher.match, c);
                self->uri_matcher.state = VT_URI_MATCHER_PATH;
            } else {
                Vector_push_char(&self->uri_matcher.match, c);
            }
        } break;

        case VT_URI_MATCHER_PATH:
        case VT_URI_MATCHER_SUFFIX_REFERENCE: {
            if (isurl(c)) {
                Vector_push_char(&self->uri_matcher.match, c);
            } else {
                Vt_uri_break_match(self);
            }
        } break;
    }
}

static inline void Vt_about_to_delete_line_by_scroll_up(Vt* self, size_t idx)
{
    VtLine* src = &self->lines.buf[idx];
    VtLine* tgt = &self->lines.buf[idx + 1];

    if (unlikely(src->graphic_attachments && src->graphic_attachments->images)) {
        for (RcPtr_VtImageSurfaceView* i = NULL;
             (i = Vector_iter_RcPtr_VtImageSurfaceView(src->graphic_attachments->images, i));) {
            VtImageSurfaceView* view = RcPtr_get_VtImageSurfaceView(i);
            if (view && view->cell_size.second > 1) {
                VtImageSurfaceView new_view  = Vt_crop_VtImageSurfaceView_top_by_line(self, view);
                new_view.anchor_global_index = idx + 1;
                RcPtr_VtImageSurfaceView new_ptr        = RcPtr_new_VtImageSurfaceView(self);
                *RcPtr_get_VtImageSurfaceView(&new_ptr) = new_view;
                RcPtr_VtImageSurfaceView new_ptr2 = RcPtr_new_shared_VtImageSurfaceView(&new_ptr);

                if (!tgt->graphic_attachments) {
                    tgt->graphic_attachments = calloc(1, sizeof(VtGraphicLineAttachments));
                }

                if (!tgt->graphic_attachments->images) {
                    tgt->graphic_attachments->images =
                      malloc(sizeof(Vector_RcPtr_VtImageSurfaceView));
                    *tgt->graphic_attachments->images = Vector_new_RcPtr_VtImageSurfaceView();
                }

                Vector_push_RcPtr_VtImageSurfaceView(tgt->graphic_attachments->images, new_ptr);
                Vector_push_RcPtr_VtImageSurfaceView(&self->image_views, new_ptr2);
            }
        }
    }
}

static void Vt_about_to_delete_line_by_scroll_down(Vt* self, size_t idx)
{
    for (RcPtr_VtImageSurfaceView* i = NULL;
         (i = Vector_iter_RcPtr_VtImageSurfaceView(&self->image_views, i));) {
        VtImageSurfaceView* view = RcPtr_get_VtImageSurfaceView(i);
        if (view) {
            while (view->cell_size.second > 1 && VtImageSurfaceView_spans_line(view, idx)) {
                Vt_crop_VtImageSurfaceView_bottom_by_line(self, view);
            }
        }
    }
}

static void Vt_grapheme_break(Vt* self)
{
#ifndef NOUTF8PROC
    self->utf8proc_state = 0;
#endif
    self->last_codepoint = 0;
}

static void Vt_set_fg_color_custom(Vt* self, ColorRGB color, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->fg_is_palette_entry = false;
    r->fg_data.rgb         = color;
}

static void Vt_set_fg_color_palette(Vt* self, int16_t index, VtRune* opt_target)
{
    ASSERT(index >= 0 && index <= 256, "in palette range");
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->fg_is_palette_entry = true;
    r->fg_data.index       = index;
}

static void Vt_set_fg_color_default(Vt* self, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->fg_is_palette_entry = true;
    r->fg_data.index       = VT_RUNE_PALETTE_INDEX_TERM_DEFAULT;
}

static void Vt_set_bg_color_custom(Vt* self, ColorRGBA color, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->bg_is_palette_entry = false;
    r->bg_data.rgba        = color;
}

static void Vt_set_bg_color_palette(Vt* self, int16_t index, VtRune* opt_target)
{
    ASSERT(index >= 0 && index <= 256, "in palette range");
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->bg_is_palette_entry = true;
    r->bg_data.index       = index;
}

static void Vt_set_bg_color_default(Vt* self, VtRune* opt_target)
{
    VtRune* r              = OR(opt_target, &self->parser.char_state);
    r->bg_is_palette_entry = true;
    r->bg_data.index       = VT_RUNE_PALETTE_INDEX_TERM_DEFAULT;
}

static void Vt_set_line_color_custom(Vt* self, ColorRGB color, VtRune* opt_target)
{
    VtRune* r                  = OR(opt_target, &self->parser.char_state);
    r->line_color_not_default  = true;
    r->ln_clr_is_palette_entry = false;
    r->ln_clr_data.rgb         = color;
}

static void Vt_set_line_color_palette(Vt* self, int16_t index, VtRune* opt_target)
{
    ASSERT(index >= 0 && index <= 256, "in palette range");
    VtRune* r                  = OR(opt_target, &self->parser.char_state);
    r->ln_clr_is_palette_entry = true;
    r->ln_clr_data.index       = index;
}

static void Vt_set_line_color_default(Vt* self, VtRune* opt_target)
{
    VtRune* r                 = OR(opt_target, &self->parser.char_state);
    r->line_color_not_default = false;
}

static ColorRGB Vt_active_fg_color(const Vt* self)
{
    return Vt_rune_fg(self, &self->parser.char_state);
}

static ColorRGBA Vt_active_bg_color(const Vt* self)
{
    return Vt_rune_bg(self, &self->parser.char_state);
}

static ColorRGB Vt_active_line_color(const Vt* self)
{
    return Vt_rune_ln_clr(self, &self->parser.char_state);
}

void Vt_clear_all_proxies(Vt* self)
{
    Vt_clear_proxies_in_region(self, 0, self->lines.size - 1);
    if (Vt_alt_buffer_enabled(self)) {
        for (size_t i = 0; i < self->alt_lines.size - 1; ++i) {
            Vt_clear_line_proxy(self, &self->alt_lines.buf[i]);
        }
    }
}

void Vt_clear_all_image_proxies(Vt* self)
{
    for (RcPtr_VtImageSurfaceView* i = NULL;
         (i = Vector_iter_RcPtr_VtImageSurfaceView(&self->image_views, i));) {
        VtImageSurfaceView* srf = RcPtr_get_VtImageSurfaceView(i);
        CALL(self->callbacks.destroy_image_view_proxy, self->callbacks.user_data, &srf->proxy);
    }

    for (RcPtr_VtSixelSurface* i = NULL;
         (i = Vector_iter_RcPtr_VtSixelSurface(&self->scrolled_sixels, i));) {
        VtSixelSurface* srf = RcPtr_get_VtSixelSurface(i);
        CALL(self->callbacks.destroy_sixel_proxy, self->callbacks.user_data, &srf->proxy);
    }
}

Vector_char Vt_region_to_string(Vt* self, size_t begin_line, size_t end_line)
{
    Vector_char tmp, ret = Vt_line_to_string(self,
                                             begin_line,
                                             0,
                                             Vt_col(self),
                                             Vt_line_at(self, begin_line)->was_reflown ? "" : "\n");
    Vector_pop_char(&ret);
    for (size_t i = begin_line + 1; i < end_line; ++i) {
        tmp =
          Vt_line_to_string(self, i, 0, Vt_col(self), self->lines.buf[i].was_reflown ? "" : "\n");
        Vector_pushv_char(&ret, tmp.buf, tmp.size - 1);
        Vector_destroy_char(&tmp);
    }

    tmp = Vt_line_to_string(self, end_line, 0, Vt_col(self), "");
    Vector_pushv_char(&ret, tmp.buf, tmp.size);
    Vector_destroy_char(&tmp);

    return ret;
}

static inline char32_t char_sub_uk(char original)
{
    return original == '#' ? 0xa3 /* £ */ : original;
}

static inline char32_t char_sub_gfx(char original)
{
    static const char32_t substitutes[] = {
        0x2592, // ▒
        0x2409, // ␉
        0x240c, // ␌
        0x240d, // ␍
        0x240a, // ␊
        0x00b0, // °
        0x00b1, // ±
        0x2424, // ␤
        0x240b, // ␋
        0x2518, // ┘
        0x2510, // ┐
        0x250c, // ┌
        0x2514, // └
        0x253c, // ┼
        0x23ba, // ⎺
        0x23bb, // ⎻
        0x2500, // ─
        0x23BC, // ⎼
        0x23BD, // ⎽
        0x251C, // ├
        0x2524, // ┤
        0x2534, // ┴
        0x252C, // ┬
        0x2502, // │
        0x2264, // ≤
        0x2265, // ≥
        0x03C0, // π
        0x00A3, // £
        0x2260, // ≠
        0x22C5, // ⋅
        0x2666, // ♦
    };

    if (unlikely(original >= 'a' && original <= '~')) {
        return substitutes[original - 'a'];
    } else {
        return original;
    }
}

/**
 * substitute invisible characters with a readable string */
__attribute__((cold)) static const char* control_char_get_pretty_string(const char c)
{
    static const char* const strings[] = {
        TERMCOLOR_BG_BLACK TERMCOLOR_RED "␀" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_YELLOW "␁" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_YELLOW_LIGHT "␂" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␃" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␄" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␅" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␆" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_YELLOW "␇" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_RED "␈" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_BLUE "␉" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␊" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_BLUE_LIGHT "␋" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_RED_LIGHT "␌" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␍" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␎" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA_LIGHT "␏" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␐" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA_LIGHT "␑" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␒" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␓" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␔" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN "␕" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␖" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN "␗" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_BLUE_LIGHT "␘" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN "␙" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN_LIGHT "␚" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_GREEN_LIGHT "␛" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_RED_LIGHT "␜" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␝" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA "␞" TERMCOLOR_DEFAULT,
        TERMCOLOR_BG_BLACK TERMCOLOR_CYAN_LIGHT "␟" TERMCOLOR_DEFAULT,
    };

    if ((uint32_t)c < ARRAY_SIZE(strings)) {
        return strings[(int)c];
    } else if (c == 127) {
        return TERMCOLOR_BG_BLACK TERMCOLOR_MAGENTA_LIGHT "␡" TERMCOLOR_DEFAULT;
    } else {
        return NULL;
    }
}

/**
 * make pty messages more readable */
__attribute__((cold)) char* pty_string_prettyfy(const char* str, int32_t max)
{
    bool esc = false, seq = false, important = false;

    Vector_char fmt = Vector_new_char();
    for (const char* s = str; *s && ((s - str) < max); ++s) {
        if (seq) {
            if (!isdigit(*s) && *s != '?' && *s != ';' && *s != ':') {
                Vector_pushv_char(&fmt, TERMCOLOR_BG_DEFAULT, strlen(TERMCOLOR_BG_DEFAULT));
                seq       = false;
                important = true;
            }
        } else {
            if (*s == '\e') {
                esc = true;
                Vector_pushv_char(&fmt, TERMCOLOR_BG_GRAY_DARK, strlen(TERMCOLOR_BG_GRAY_DARK));
            } else if (*s == '[' && esc) {
                seq = true;
                esc = false;
            }
        }

        const char* ctr = control_char_get_pretty_string(*s);
        if (ctr) {
            Vector_pushv_char(&fmt, ctr, strlen(ctr));
        } else {
            if (important) {
                switch (*s) {
                    case 'H':
                    case 'G':
                    case 'f':
                    case '`':
                    case 'd':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_GREEN, strlen(TERMCOLOR_BG_GREEN));
                        break;

                    case 'm':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_BLUE, strlen(TERMCOLOR_BG_BLUE));
                        break;

                    case 'B':
                    case 'C':
                    case 'e':
                    case 'a':
                    case 'D':
                    case 'E':
                    case 'F':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_CYAN, strlen(TERMCOLOR_BG_CYAN));
                        break;

                    case 'M':
                    case 'T':
                    case 'X':
                    case 'S':
                    case '@':
                    case 'L':
                    case 'P':
                        Vector_pushv_char(&fmt,
                                          TERMCOLOR_BG_MAGENTA_LIGHT,
                                          strlen(TERMCOLOR_BG_MAGENTA_LIGHT));
                        break;

                    case 'I':
                    case 'Z':
                    case 'g':
                        Vector_pushv_char(&fmt, TERMCOLOR_BG_MAGENTA, strlen(TERMCOLOR_BG_MAGENTA));
                        break;

                    default:
                        Vector_pushv_char(&fmt,
                                          TERMCOLOR_BG_RED_LIGHT,
                                          strlen(TERMCOLOR_BG_RED_LIGHT));
                }
                Vector_push_char(&fmt, *s);
                Vector_pushv_char(&fmt, TERMCOLOR_RESET, strlen(TERMCOLOR_RESET));

            } else if (*s == ';' && seq) {
                Vector_pushv_char(&fmt, TERMCOLOR_RED_LIGHT, strlen(TERMCOLOR_RED_LIGHT));
                Vector_push_char(&fmt, *s);
                Vector_pushv_char(&fmt, TERMCOLOR_DEFAULT, strlen(TERMCOLOR_DEFAULT));
            } else if (isdigit(*s) && seq) {
                Vector_pushv_char(&fmt,
                                  TERMCOLOR_BG_WHITE TERMCOLOR_BLACK,
                                  strlen(TERMCOLOR_BG_WHITE TERMCOLOR_BLACK));
                Vector_push_char(&fmt, *s);
                Vector_pushv_char(&fmt,
                                  TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT,
                                  strlen(TERMCOLOR_BG_GRAY_DARK TERMCOLOR_DEFAULT));
            } else {
                Vector_push_char(&fmt, *s);
            }
        }
        important = false;
    }
    Vector_pushv_char(&fmt, TERMCOLOR_BG_DEFAULT, strlen(TERMCOLOR_BG_DEFAULT));
    Vector_push_char(&fmt, '\0');
    return fmt.buf;
}

/**
 * Split string on any character in @param delimiters, filter out any character in @param filter.
 * first character of returned string is the immediately preceding delimiter, '\0' if none. Multiple
 * @param collapsable_delimiters are treated as a single delimiter */
static Vector_Vector_char string_split_on(const char* str,
                                          const char* delimiters,
                                          const char* collapsable_delimiters,
                                          const char* filter)
{
    Vector_Vector_char ret = Vector_new_with_capacity_Vector_char(8);
    Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
    Vector_push_char(&ret.buf[0], '\0');

    for (; *str; ++str) {
        /* skip filtered */
        for (const char* i = filter; filter && *i; ++i)
            if (*str == *i)
                goto continue_outer;

        bool is_non_greedy = false;
        char any_symbol    = 0;
        if (delimiters) {
            for (const char* i = delimiters; *i; ++i) {
                if (*i == *str) {
                    is_non_greedy = true;
                    any_symbol    = *i;
                    break;
                }
            }
        }
        if (collapsable_delimiters) {
            for (const char* i = collapsable_delimiters; *i; ++i) {
                if (*i == *str) {
                    any_symbol = *i;
                    break;
                }
            }
        }

        if (any_symbol) {
            Vector_push_char(&ret.buf[ret.size - 1], '\0');

            if (ret.buf[ret.size - 1].size == 2 && !is_non_greedy) {
                ret.buf[ret.size - 1].size = 0;
            } else {
                Vector_push_Vector_char(&ret, Vector_new_with_capacity_char(8));
            }
            Vector_push_char(&ret.buf[ret.size - 1], any_symbol);
        } else {
            /* record delimiter */
            Vector_push_char(&ret.buf[ret.size - 1], *str);
        }
    continue_outer:;
    }
    Vector_push_char(&ret.buf[ret.size - 1], '\0');

    return ret;
}

static inline bool is_csi_sequence_terminated(const char* seq, const size_t size)
{
    if (!size) {
        return false;
    }

    return isalpha(seq[size - 1]) || seq[size - 1] == '@' || seq[size - 1] == '{' ||
           seq[size - 1] == '}' || seq[size - 1] == '~' || seq[size - 1] == '|';
}

static inline bool is_string_sequence_terminated(const char* seq, const size_t size)
{
    if (!size) {
        return false;
    }

    return seq[size - 1] == '\a' || (size > 1 && seq[size - 2] == '\e' && seq[size - 1] == '\\');
}

static void Vt_reset_color_palette_entry(Vt* self, int16_t idx)
{
    generate_color_palette_entry(&self->colors.palette_256[idx], idx);
}

static void Vt_init_color_palette(Vt* self)
{
    for (int16_t i = 0; i < 256; ++i) {
        Vt_reset_color_palette_entry(self, i);
    }
}

/**
 * Parse a color form xterm name or as XParseColor() */
static void set_rgb_color_from_xterm_string(ColorRGB* color, const char* string)
{
    bool     failed = false;
    ColorRGB c;
    if (strstr(string, "rgbi:")) {
        c = ColorRGB_from_xorg_rgb_intensity_specification(string, &failed);
    } else if (strstr(string, "rgb:")) {
        c = ColorRGB_from_xorg_rgb_specification(string, &failed);
    } else if (*string == '#') {
        c = ColorRGB_from_xorg_old_rgb_specification(string, &failed);
    } else {
        c = color_from_xterm_name(string, &failed);
    }

    if (!failed) {
        *color = c;
    } else {
        WRN("Failed to parse \'%s\' as color\n", string);
    }
}

static void set_rgba_color_from_xterm_string(ColorRGBA* color, const char* string)
{
    ColorRGB c;
    set_rgb_color_from_xterm_string(&c, string);
    *color = ColorRGBA_from_RGB(c);
}

static void Vt_hard_reset(Vt* self)
{
    memset(&self->modes, 0, sizeof(self->modes));
    Vt_alt_buffer_off(self, false);
    Vt_select_end(self);
    Vt_clear_display_and_scrollback(self);
    Vector_clear_RcPtr_VtSixelSurface(&self->alt_scrolled_sixels);
    Vector_clear_RcPtr_VtImageSurfaceView(&self->alt_image_views);
    Vt_move_cursor(self, 0, 0);

    self->parser.state = PARSER_STATE_LITERAL;

    self->charset_g0             = NULL;
    self->charset_g1             = NULL;
    self->charset_g2             = NULL;
    self->charset_g3             = NULL;
    self->charset_single_shift   = NULL;
    self->has_last_inserted_rune = false;

    self->scroll_region_top    = 0;
    self->scroll_region_bottom = Vt_row(self) - 1;
    self->scroll_region_left   = 0;
    self->scroll_region_right  = Vt_col(self) - 1;

    Vector_clear_DynStr(&self->title_stack);
    free(self->title);
    self->title = NULL;

    self->colors.bg = settings.bg;
    self->colors.fg = settings.fg;

    self->colors.highlight.bg = settings.bghl;
    self->colors.highlight.fg = settings.fghl;

    Vt_init_color_palette(self);

    self->tabstop = 8;
    Vt_reset_tab_ruler(self);

    Vt_uri_break_match(self);

    // TODO: Clear DECUDK
}

static void Vt_soft_reset(Vt* self)
{
    Vt_alt_buffer_off(self, false);
    Vt_move_cursor(self, 0, 0);
    self->tabstop                = 8;
    self->parser.state           = PARSER_STATE_LITERAL;
    self->charset_g0             = NULL;
    self->charset_g1             = NULL;
    self->charset_g2             = NULL;
    self->charset_g3             = NULL;
    self->charset_single_shift   = NULL;
    self->has_last_inserted_rune = false;
    self->scroll_region_top      = 0;
    self->scroll_region_bottom   = Vt_row(self) - 1;
    self->scroll_region_left     = 0;
    self->scroll_region_right    = Vt_col(self) - 1;
    Vt_uri_break_match(self);
    Vector_clear_DynStr(&self->title_stack);
}

void Vt_init(Vt* self, uint32_t cols, uint32_t rows)
{
    memset(self, 0, sizeof(Vt));
    self->ws = (struct winsize){ .ws_col = cols, .ws_row = rows };

    self->scroll_region_bottom = rows - 1;
    self->scroll_region_right  = cols - 1;
    self->parser.state         = PARSER_STATE_LITERAL;
    self->parser.in_mb_seq     = false;

    self->colors.bg = settings.bg;
    self->colors.fg = settings.fg;

    self->colors.highlight.bg = settings.bghl;
    self->colors.highlight.fg = settings.fghl;

    Vt_reset_text_attribs(self, NULL);

    memcpy(&self->blank_space, &self->parser.char_state, sizeof(VtRune));

    self->parser.active_sequence = Vector_new_char();
    self->output                 = Vector_new_char();
    self->staged_output          = Vector_new_char();
    self->lines                  = Vector_new_VtLine(self);

    for (size_t i = 0; i < self->ws.ws_row; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
    }

    switch (settings.initial_cursor_style) {
        case CURSOR_STYLE_BEAM:
            self->cursor.type = CURSOR_BEAM;
            break;
        case CURSOR_STYLE_UNDERLINE:
            self->cursor.type = CURSOR_UNDERLINE;
            break;
        default:
        case CURSOR_STYLE_BLOCK:
            self->cursor.type = CURSOR_BLOCK;
            break;
    }

    switch (settings.initial_gui_pointer_mode) {
        case GUI_POINTER_MODE_FORCE_HIDE:
            self->gui_pointer_mode = VT_GUI_POINTER_MODE_FORCE_HIDE;
            break;
        case GUI_POINTER_MODE_FORCE_SHOW:
            self->gui_pointer_mode = VT_GUI_POINTER_MODE_FORCE_SHOW;
            break;
        case GUI_POINTER_MODE_HIDE:
            self->gui_pointer_mode = VT_GUI_POINTER_MODE_HIDE;
            break;
        case GUI_POINTER_MODE_SHOW:
            self->gui_pointer_mode = VT_GUI_POINTER_MODE_SHOW;
            break;
        case GUI_POINTER_MODE_SHOW_IF_REPORTING:
            self->gui_pointer_mode = VT_GUI_POINTER_MODE_SHOW_IF_REPORTING;
            break;
        default:
            ASSERT_UNREACHABLE;
    }

    self->cursor.blinking = settings.initial_cursor_blinking;
    self->cursor.col      = 0;

    self->tabstop = 8;
    Vt_init_tab_ruler(self);

    self->title       = NULL;
    self->title_stack = Vector_new_DynStr();

    self->uri_matcher.state = VT_URI_MATCHER_EMPTY;
    self->uri_matcher.match = Vector_new_with_capacity_char(128);

    self->images      = Vector_new_RcPtr_VtImageSurface();
    self->image_views = Vector_new_RcPtr_VtImageSurfaceView();

    // self->static_sixels   = Vector_new_VtSixelSurface(self);
    self->scrolled_sixels = Vector_new_RcPtr_VtSixelSurface();

    self->shell_commands = Vector_new_RcPtr_VtCommand();

    self->unicode_input.buffer = Vector_new_char();

    self->xterm_modify_keyboard      = VT_XT_MODIFY_KEYBOARD_DFT;
    self->xterm_modify_cursor_keys   = VT_XT_MODIFY_CURSOR_KEYS_DFT;
    self->xterm_modify_function_keys = VT_XT_MODIFY_FUNCTION_KEYS_DFT;
    self->xterm_modify_other_keys    = VT_XT_MODIFY_OTHER_KEYS_DFT;

    Vt_init_color_palette(self);
}

static void Vt_init_tab_ruler(Vt* self)
{
    free(self->tab_ruler);
    self->tab_ruler = calloc(1, Vt_col(self) + 1);
    Vt_reset_tab_ruler(self);
}

static void Vt_reset_tab_ruler(Vt* self)
{
    for (uint16_t i = 0; i < Vt_col(self); ++i) {
        self->tab_ruler[i] = i % self->tabstop == 0;
    }
}

static void Vt_clear_all_tabstops(Vt* self)
{
    memset(self->tab_ruler, false, Vt_col(self));
}

bool Vt_visual_scroll_up(Vt* self)
{
    if (self->scrolling_visual) {
        if (self->visual_scroll_top) {
            --self->visual_scroll_top;
        } else {
            return true;
        }
    } else if (Vt_top_line(self)) {
        self->scrolling_visual  = true;
        self->visual_scroll_top = Vt_top_line(self) - 1;
    }
    return false;
}

bool Vt_visual_scroll_down(Vt* self)
{
    if (self->scrolling_visual && Vt_top_line(self) > self->visual_scroll_top) {
        ++self->visual_scroll_top;
        if (self->visual_scroll_top == Vt_top_line(self)) {
            self->scrolling_visual = false;
            return true;
        }
    }
    return false;
}

void Vt_visual_scroll_to(Vt* self, size_t line)
{
    line                    = MIN(line, Vt_top_line(self));
    self->visual_scroll_top = line;
    self->scrolling_visual  = line != Vt_top_line(self);
}

void Vt_visual_scroll_reset(Vt* self)
{
    self->scrolling_visual = false;
}

static void Vt_reflow_expand(Vt* self, uint32_t x)
{
    size_t bottom_bound = self->cursor.row;

    int removals = 0;

    while (bottom_bound > 0 && self->lines.buf[bottom_bound].rejoinable) {
        --bottom_bound;
    }

    for (size_t i = 0; i < bottom_bound; ++i) {
        VtLine* srcline = &self->lines.buf[i + 1];
        VtLine* tgtline = &self->lines.buf[i];

        if (tgtline->data.size < x && tgtline->reflowable) {
            int32_t chars_to_move = x - tgtline->data.size;
            if (i + 1 < bottom_bound && srcline->rejoinable) {
                chars_to_move = MIN(chars_to_move, (int32_t)srcline->data.size);

                /* Copy uri strings to target line and convert uri idx-es to the new line */
                if (srcline->links) {
                    for (int32_t j = 0; j < chars_to_move; ++j) {
                        VtRune*  r      = &srcline->data.buf[j];
                        uint16_t srcidx = r->hyperlink_idx;
                        if (srcidx && srcidx <= srcline->links->size) {
                            const char* uri =
                              Vector_at_VtUri(srcline->links, srcidx - 1)->uri_string;
                            if (uri) {
                                r->hyperlink_idx = VtLine_add_link(tgtline, uri) + 1;
                            }
                        }
                    }
                }

                /* Move the actual data */
                Vector_pushv_VtRune(&tgtline->data, srcline->data.buf, chars_to_move);
                Vector_remove_at_VtRune(&srcline->data, 0, chars_to_move);

                if (self->selection.mode == SELECT_MODE_NORMAL) {
                    if (self->selection.begin_line == i + 1) {
                        if (self->selection.begin_char_idx <= chars_to_move) {
                            --self->selection.begin_line;
                            self->selection.begin_char_idx =
                              self->selection.begin_char_idx + tgtline->data.size - chars_to_move;
                        } else {
                            self->selection.begin_char_idx -= chars_to_move;
                        }
                    }
                    if (self->selection.end_line == i + 1) {
                        if (self->selection.end_char_idx < chars_to_move) {
                            --self->selection.end_line;
                            self->selection.end_char_idx =
                              self->selection.end_char_idx + tgtline->data.size - chars_to_move;
                        } else {
                            self->selection.end_char_idx -= chars_to_move;
                        }
                    }
                }

                Vt_mark_proxy_fully_damaged(self, i);
                Vt_mark_proxy_fully_damaged(self, i + 1);

                if (!srcline->data.size) {
                    tgtline->was_reflown = false;
                    size_t remove_index  = i + 1;

                    if (srcline->mark_command_output_start)
                        tgtline->mark_command_output_start = true;
                    if (srcline->mark_command_output_end)
                        tgtline->mark_command_output_end = true;
                    if (srcline->mark_command_invoke)
                        tgtline->mark_command_invoke = true;
                    if (srcline->mark_explicit)
                        tgtline->mark_explicit = true;

                    Vector_remove_at_VtLine(&self->lines, remove_index, 1);
                    srcline = &self->lines.buf[i + 1];
                    tgtline = &self->lines.buf[i];

                    Vt_shift_global_line_index_refs(self, remove_index + 1, -1, false);

                    if (self->lines.size - 1 < Vt_row(self)) {
                        Vector_push_VtLine(&self->lines, VtLine_new());
                    }

                    --bottom_bound;
                    ++removals;
                }
            }
        } // end should reflow
    }

    int underflow = -((int64_t)self->lines.size - Vt_row(self));

    if (underflow > 0) {
        for (int i = 0; i < (int)MIN(underflow, removals); ++i) {
            Vector_push_VtLine(&self->lines, VtLine_new());
        }
    }

    /* do not scroll past end of screen (self->ws was not updated yet, so Vt_scroll_down does not
     * prevent this) */
    if (Vt_visual_top_line(self) > Vt_top_line(self)) {
        Vt_visual_scroll_reset(self);
    }
}

static void Vt_reflow_shrink(Vt* self, uint32_t x)
{
    size_t insertions_made = 0;
    size_t bottom_bound    = self->cursor.row;

    while (bottom_bound > 0 && self->lines.buf[bottom_bound].rejoinable) {
        --bottom_bound;
    }

    for (size_t i = 0; i < bottom_bound; ++i) {
        VtLine* srcline = &self->lines.buf[i];
        VtLine* tgtline = &self->lines.buf[i + 1];

        if (!srcline->was_reflown)
            VtLine_strip_blanks(srcline);

        if (srcline->data.size > x && srcline->reflowable) {
            size_t chars_to_move = srcline->data.size - x;

            /* move select to next line */
            bool end_just_moved = false, begin_just_moved = false;
            if (self->selection.mode == SELECT_MODE_NORMAL) {
                if (self->selection.begin_char_idx > (int32_t)x &&
                    self->selection.begin_line == i) {
                    ++self->selection.begin_line;
                    self->selection.begin_char_idx = self->selection.begin_char_idx - x;
                    begin_just_moved               = true;
                }
                if (self->selection.end_char_idx > (int32_t)x && self->selection.end_line == i) {
                    ++self->selection.end_line;
                    self->selection.end_char_idx = self->selection.end_char_idx - x;
                    end_just_moved               = true;
                }
            }

            /* line below is a reflow already */
            if (i + 1 < bottom_bound && tgtline->rejoinable) {
                for (size_t ii = 0; ii < chars_to_move; ++ii) {

                    /* shift selection points right */
                    if (self->selection.mode == SELECT_MODE_NORMAL) {
                        if (self->selection.begin_line == i + 1 && !begin_just_moved)
                            ++self->selection.begin_char_idx;
                        if (self->selection.end_line == i + 1 && !end_just_moved)
                            ++self->selection.end_char_idx;
                    }

                    VtRune* r = srcline->data.buf + x + chars_to_move - ii - 1;

                    /* update link idx-es */
                    if (r->hyperlink_idx && srcline->links &&
                        r->hyperlink_idx <= (uint16_t)srcline->links->size) {
                        const char* uri = srcline->links->buf[r->hyperlink_idx - 1].uri_string;
                        if (uri) {
                            r->hyperlink_idx = VtLine_add_link(tgtline, uri) + 1;
                        }
                    }

                    Vector_insert_VtRune(&tgtline->data, tgtline->data.buf, *r);
                }
                Vt_mark_proxy_fully_damaged(self, i + 1);
            } else if (i < bottom_bound) {
                ++insertions_made;
                size_t insert_index = i + 1;
                Vector_insert_VtLine(&self->lines, self->lines.buf + insert_index, VtLine_new());
                srcline = &self->lines.buf[i];
                tgtline = &self->lines.buf[i + 1];

                Vt_shift_global_line_index_refs(self, insert_index, 1, false);

                ++bottom_bound;

                /* update link idx-es */
                for (size_t j = 0; j < chars_to_move; ++j) {
                    VtRune* r = Vt_at(self, x + j, i);
                    if (r && r->hyperlink_idx && srcline->links &&
                        r->hyperlink_idx <= (uint16_t)srcline->links->size) {
                        const char* uri = srcline->links->buf[r->hyperlink_idx - 1].uri_string;
                        if (uri) {
                            r->hyperlink_idx = VtLine_add_link(tgtline, uri) + 1;
                        }
                    }
                }

                Vector_pushv_VtRune(&tgtline->data, srcline->data.buf + x, chars_to_move);

                if (srcline->mark_command_output_end) {
                    srcline->mark_command_output_end = false;
                    tgtline->mark_command_output_end = true;
                }

                srcline->was_reflown = true;
                tgtline->rejoinable  = true;
            }
        }
    }

    if (self->lines.size - 1 != self->cursor.row) {
        size_t overflow = self->lines.size > Vt_row(self) ? self->lines.size - Vt_row(self) : 0;
        size_t whitespace_below = self->lines.size - 1 - self->cursor.row;
        size_t to_pop           = MIN(overflow, MIN(whitespace_below, insertions_made));
        Vector_pop_n_VtLine(&self->lines, to_pop);
    }
}

/**
 * Remove extra columns from all lines */
static void Vt_trim_columns(Vt* self)
{
    for (uint16_t i = 0; i < self->lines.size; ++i) {
        if (self->lines.buf[i].data.size > Vt_col(self)) {
            Vt_mark_proxy_fully_damaged(self, i);

            CALL(self->callbacks.destroy_proxy,
                 self->callbacks.user_data,
                 &self->lines.buf[i].proxy);

            size_t blanks = 0;

            size_t s = self->lines.buf[i].data.size;
            Vector_pop_n_VtRune(&self->lines.buf[i].data, s - Vt_col(self));

            if (self->lines.buf[i].was_reflown) {
                continue;
            }

            s = self->lines.buf[i].data.size;

            for (blanks = 0; blanks < s; ++blanks) {
                if (!(self->lines.buf[i].data.buf[s - 1 - blanks].rune.code == ' ' &&
                      ColorRGBA_eq(
                        self->colors.bg,
                        Vt_rune_bg(self, &self->lines.buf[i].data.buf[s - 1 - blanks])))) {
                    break;
                }
            }
            Vector_pop_n_VtRune(&self->lines.buf[i].data, blanks);
        }
    }
}

void Vt_resize(Vt* self, uint32_t x, uint32_t y)
{
    if (x < 2 || y < 2) {
        return;
    }

    if (!self->alt_lines.buf) {
        Vt_trim_columns(self);
    }

    self->saved_cursor_pos  = MIN(self->saved_cursor_pos, x);
    self->saved_active_line = MIN(self->saved_active_line, self->lines.size);

    static uint16_t ox = 0, oy = 0;
    if (x != ox || y != oy) {
        if (!self->alt_lines.buf && !Vt_scroll_region_not_default(self)) {
            if (self->selection.mode == SELECT_MODE_BOX) {
                Vt_select_end(self);
            }
            if (x < ox) {
                Vt_reflow_shrink(self, x);
            } else if (x > ox) {
                Vt_reflow_expand(self, x);
            }
        } else {
            Vt_select_end(self);
        }
        if (Vt_row(self) > y) {
            uint16_t to_pop = Vt_row(self) - y;
            if (self->cursor.row + to_pop > Vt_bottom_line(self)) {
                to_pop -= self->cursor.row + to_pop - Vt_bottom_line(self);
            }
            Vector_pop_n_VtLine(&self->lines, to_pop);
            if (self->alt_lines.buf) {
                uint16_t to_pop_alt = Vt_row(self) - y;
                if (self->alt_active_line + to_pop_alt > Vt_bottom_line_alt(self)) {
                    to_pop_alt -= self->alt_active_line + to_pop_alt - Vt_bottom_line_alt(self);
                }
                Vector_pop_n_VtLine(&self->alt_lines, to_pop_alt);
            }
        } else {
            for (uint16_t i = 0; i < y - Vt_row(self); ++i) {
                Vector_push_VtLine(&self->lines, VtLine_new());
            }
            if (self->alt_lines.buf) {
                for (uint16_t i = 0; i < y - Vt_row(self); ++i) {
                    Vector_push_VtLine(&self->alt_lines, VtLine_new());
                }
            }
        }
        ox = x;
        oy = y;
    }

    Pair_uint32_t px =
      CALL(self->callbacks.on_window_size_from_cells_requested, self->callbacks.user_data, x, y);

    Vt_clear_all_image_proxies(self);

    self->ws =
      (struct winsize){ .ws_col = x, .ws_row = y, .ws_xpixel = px.first, .ws_ypixel = px.second };

    LOG("resized to: %d %d [%d %d]\n",
        self->ws.ws_col,
        self->ws.ws_row,
        self->ws.ws_xpixel,
        self->ws.ws_ypixel);

    self->pixels_per_cell_x = (double)self->ws.ws_xpixel / Vt_col(self);
    self->pixels_per_cell_y = (double)self->ws.ws_ypixel / Vt_row(self);

    if (self->master_fd > 1) {
        if (ioctl(self->master_fd, TIOCSWINSZ, &self->ws) < 0) {
            WRN("ioctl(%d, TIOCSWINSZ, winsize { %d, %d, %d, %d }) failed:  %s\n",
                self->master_fd,
                self->ws.ws_col,
                self->ws.ws_row,
                self->ws.ws_xpixel,
                self->ws.ws_ypixel,
                strerror(errno));
        }
    }

    self->scroll_region_top    = 0;
    self->scroll_region_bottom = Vt_row(self) - 1;
    self->scroll_region_left   = 0;
    self->scroll_region_right  = Vt_col(self) - 1;

    Vt_init_tab_ruler(self);
}

static inline int32_t short_sequence_get_int_argument(const char* seq)
{
    return *seq == 0 || seq[1] == 0 ? 1 : atoi(seq);
}

/*
 * value:
 * 0 => not recognized
 * 1 => enabled
 * 2 => disabled
 * 3 => permanently enabled
 * 4 => permanently disabled
 */
static inline void Vt_report_dec_mode(Vt* self, int code)
{
    bool value;
    switch (code) {
        case 1:
            value = self->modes.application_keypad_cursor;
            break;
        case 7:
            value = self->modes.no_wraparound;
            break;
        case 8:
            value = self->modes.auto_repeat;
            break;
        case 12:
        case 13:
            value = self->cursor.blinking;
            break;
        case 25:
            value = self->cursor.hidden;
            break;
        case 80:
            value = self->modes.sixel_scrolling;
            break;
        case 1000:
            value = self->modes.mouse_btn_report;
            break;
        case 1002:
            value = self->modes.mouse_motion_on_btn_report;
            break;
        case 1003:
            value = self->modes.mouse_motion_report;
            break;
        case 1004:
            value = self->modes.window_focus_events_report;
            break;
        case 1006:
            value = self->modes.extended_report;
            break;
        case 1037:
            value = self->modes.del_sends_del;
            break;
        case 1039:
            value = self->modes.no_alt_sends_esc;
            break;
        case 1042:
            value = self->modes.urgency_on_bell;
            break;
        case 1043:
            value = self->modes.pop_on_bell;
            break;
        case 47:
        case 1047:
        case 1049:
            value = Vt_alt_buffer_enabled(self);
            break;
        case 1070:
            value = self->modes.sixel_private_color_registers;
            break;
        case 8452:
            value = self->modes.sixel_scrolling_move_cursor_right;
            break;
        default: {
            WRN("Unknown DECRQM mode: %d\n", code);
            Vt_output_formated(self, "\e[?%d;0$y", code);
            return;
        }
    }
    Vt_output_formated(self, "\e[?%d;%c$y", code, value ? '1' : '2');
}

static inline void Vt_handle_regular_mode(Vt* self, int code, bool on)
{
    switch (code) {
        case 2:
            STUB("KAM");
            break;
        case 4:
            self->modes.no_insert_replace_mode = on;
            break;
        case 12:
            self->modes.send_receive_mode = on;
            break;
        case 20:
            STUB("LNM");
            break;
        default:
            WRN("unknown SM mode: %d\n", code);
    }
}

static inline void Vt_handle_dec_mode(Vt* self, int code, bool on)
{
    switch (code) {

        /* Cursor Keys Mode (DEC Private) (DECCKM)
         *
         * This is a private parameter applicable to set mode (SM) and reset mode (RM) control
         * sequences. This mode is only effective when the terminal is in keypad application mode
         * (see DECKPAM) and the ANSI/VT52 mode (DECANM) is set (see DECANM). Under these
         * conditions, if the cursor key mode is reset, the four cursor function keys will send ANSI
         * cursor control commands. If cursor key mode is set, the four cursor function keys will
         * send application functions.
         */
        case 1:
            self->modes.application_keypad_cursor = on;
            break;
            /* Column mode 132/80 (DECCOLM)
             *
             * The reset state causes a maximum of 80 columns on the screen. The set state causes a
             * maximum of 132 columns on the screen.
             */
        case 3: {
            if (self->modes.allow_column_size_switching && settings.windowops_manip) {
                Pair_uint32_t target_text_area_dims =
                  CALL(self->callbacks.on_window_size_from_cells_requested,
                       self->callbacks.user_data,
                       on ? 132 : 80,
                       on ? 26 : 24);
                CALL(self->callbacks.on_text_area_dimensions_set,
                     self->callbacks.user_data,
                     target_text_area_dims.first,
                     target_text_area_dims.second);
            }
            Vt_move_cursor(self, 0, 0);
            Vt_clear_display_and_scrollback(self);
        } break;

        /* Smooth (Slow) Scroll (DECSCLM), VT100. */
        case 4:
            STUB("DECSCLM");
            break;

        /* Reverse video (DECSCNM) */
        case 5:
            STUB("DECSCNM");
            break;

        /* Origin mode (DECCOM)
         * makes cursor movement relative to the scroll region */
        case 6:
            self->modes.origin = on;
            Vt_move_cursor(self, 0, 0);
            break;

        /* DECAWM */
        case 7:
            self->modes.no_wraparound = !on;
            break;

        /* DECARM */
        case 8:
            self->modes.auto_repeat = on;
            break;

        /* Show toolbar (rxvt) */
        case 10:
            break;

        /* Start Blinking Cursor (AT&T 610) */
        case 12:
        /* Start blinking cursor (xterm non-standard) */
        case 13:
            self->cursor.blinking = !on;
            break;

        /* Printer status request (DSR) */
        case 15:
            /* Printer not connected. */
            Vt_output(self, "\e[?13n", 6);
            break;

        /* hide/show cursor (DECTCEM) */
        case 25:
            if (likely(!settings.debug_vt)) {
                self->cursor.hidden = !on;
            }
            break;

        /* Allow 80 => 132 Mode, xterm */
        case 40:
            self->modes.allow_column_size_switching = on;
            break;

        /* Reverse-wraparound Mode, xterm. */
        case 45:
            self->modes.reverse_wraparound = on;
            break;

        /* Page cursor-coupling mode (DECPCCM) */
        case 64:
        /* Vertical cursor-coupling mode (DECVCCM) */
        case 61:
            STUB("DECPCCM/DECVCCM");
            break;

        /* Numeric keypad (DECNKM) */
        case 66:
            STUB("DECNKM");
            break;

        /* Set Backarrow key to backspace/delete (DECBKM) */
        case 67:
            STUB("DECBKM");
            break;

        /* Keyboard usage (DECKBUM) */
        case 68:
            STUB("DECKBUM");
            break;

        /* Enable left and right margin mode (DECVSSM) */
        case 69:
            self->modes.vertical_split_screen_mode = on;
            break;

        /* Enable Sixel Scrolling (DECSDM) */
        case 80:
            self->modes.sixel_scrolling = on;
            break;

        /* X11 xterm mouse protocol. */
        case 1000:
            self->modes.mouse_btn_report = on;
            break;

        /* Highlight mouse tracking, xterm */
        case 1001:
            STUB("Highlight mouse tracking");
            break;

        /* xterm cell motion mouse tracking */
        case 1002:
            self->modes.mouse_motion_on_btn_report = on;
            break;

        /* xterm all motion tracking */
        case 1003:
            self->modes.mouse_motion_report = on;
            break;

        case 1004:
            self->modes.window_focus_events_report = on;
            break;

        /* utf8 Mouse Mode */
        case 1005:
            STUB("utf8 mouse mode");
            break;

        /* SGR mouse mode */
        case 1006:
            self->modes.extended_report = on;
            break;

        /* urxvt mouse mode */
        case 1015:
            STUB("urxvt mouse mode");
            break;

        case 1034:
            STUB("xterm eightBitInput");
            break;

        case 1035:
            STUB("xterm numLock");
            break;

            /* xterm metaSendsEscape */
        case 1036:
            STUB("xterm metaSendsEscape")
            break;

        case 1037:
            self->modes.del_sends_del = on;
            break;

        /* xterm altSendsEscape */
        case 1039:
            self->modes.no_alt_sends_esc = !on;
            break;

        /* Bell sets urgent WM hint (xterm) */
        case 1042:
            self->modes.urgency_on_bell = on;
            break;

        /* Bell raises window popOnBell (xterm) */
        case 1043:
            self->modes.pop_on_bell = on;
            break;

        /* Use alternate screen buffer, (xterm) */
        case 47:
        /* Also use alternate screen buffer (xterm) */
        case 1047:
        /* After saving the cursor, switch to the Alternate Screen Buffer,
         * clearing it first. */
        case 1049:
            if (on) {
                Vt_alt_buffer_on(self, code == 1049);
            } else {
                Vt_alt_buffer_off(self, code == 1049);
            }
            break;

        case 2004:
            self->modes.bracketed_paste = on;
            break;

        case 1051: /* Sun function-key mode, xterm. */
            STUB("Sun function-key mode");
            break;

        case 1052: /* HP function-key mode, xterm. */
            STUB("HP function-key mode");
            break;

        case 1053: /* SCO function-key mode, xterm. */
            STUB("SCO function-key mode");
            break;

        case 1060: /* legacy keyboard emulation, i.e, X11R6, */
            STUB("legacy keyboard emulation");
            break;

        case 1061: /* VT220 keyboard emulation, xterm. */
            STUB("VT220 keyboard emulation");
            break;

        case 1070: /* use private color registers for each graphic */
            self->modes.sixel_private_color_registers = on;
            break;

        case 8452: /* Sixel scrolling leaves cursor to right of graphic */
            self->modes.sixel_scrolling_move_cursor_right = on;
            break;

        default:
            WRN("Unknown DECSET/DECRST code: %d\n", code);
    }
}

__attribute__((hot)) static inline void Vt_handle_CSI(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_csi_sequence_terminated(self->parser.active_sequence.buf,
                                   self->parser.active_sequence.size)) {
        self->defered_events.repaint = true;
        Vector_push_char(&self->parser.active_sequence, '\0');
        char* seq        = self->parser.active_sequence.buf;
        char  first_char = *seq;
        char  last_char  = self->parser.active_sequence.buf[self->parser.active_sequence.size - 2];
        char  second_last_char;
        if (self->parser.active_sequence.size < 3) {
            second_last_char = '\0';
        } else {
            second_last_char =
              self->parser.active_sequence.buf[self->parser.active_sequence.size - 3];
        }
        bool is_single_arg = !strchr(seq, ';') && !strchr(seq, ':');

#define MULTI_ARG_IS_ERROR                                                                         \
    if (!is_single_arg) {                                                                          \
        WRN("Unexpected additional arguments for CSI sequence \'%s\'\n", seq);                     \
        break;                                                                                     \
    }

        switch (first_char) {

            /* <ESC>[! ... */
            case '!': {
                switch (last_char) {
                    /* <ESC>[!p - Soft terminal reset (DECSTR), VT220 and up. */
                    case 'p': {
                        Vt_soft_reset(self);
                    } break;

                    default:
                        WRN("Unknown CSI sequence: %s\n", seq);
                }
            } break;

            /* <ESC>[? ... */
            case '?': {
                switch (second_last_char) {

                    /* <ESC>[? ... $ ... */
                    case '$': {
                        switch (last_char) {

                            /* <ESC>[? Ps $p - Request DEC private mode (DECRQM). VT300 and up
                             *
                             * Ps - mode id as per DECSET/DECSET
                             *
                             * reply:
                             *   CSI? <mode id>;<value> $y
                             */
                            case 'p': {
                                /* Not recognized */
                                int code;
                                if (sscanf(seq, "?%d$p", &code) == 1) {
                                    Vt_report_dec_mode(self, code);
                                }
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                    } break;

                    default:
                        switch (last_char) {
                            /* <ESC>[? Pm h - DEC Private Mode Set (DECSET) */
                            case 'h':
                            /* <ESC>[? Pm l - DEC Private Mode Reset (DECRST) */
                            case 'l': {
                                bool               is_enable = last_char == 'h';
                                Vector_Vector_char tokens =
                                  string_split_on(seq + 1, ";:", NULL, NULL);
                                for (Vector_char* token = NULL;
                                     (token = Vector_iter_Vector_char(&tokens, token));) {
                                    errno         = 0;
                                    uint32_t code = atoi(token->buf + 1);
                                    if (code && !errno) {
                                        Vt_handle_dec_mode(self, code, is_enable);
                                    } else {
                                        WRN("Invalid %s argument: \'%s\'\n",
                                            is_enable ? "DECSET" : "DECRST",
                                            token->buf + 1);
                                    }
                                }
                                Vector_destroy_Vector_char(&tokens);
                            } break;

                            /* <ESC>[? Ps i -  Media Copy (MC), DEC-specific */
                            case 'i':
                                break;

                                /* <ESC>[? Ps S - Set or request graphics attribute (XTSMGRAPHICS),
                                 * xterm/VT340+ */
                            case 'S': {
                                int32_t args[3];
                                char *  s = seq + 1, *arg = NULL;

                                for (int i = 0; i < 3; ++i) {
                                    arg     = strsep(&s, ";");
                                    args[i] = atoi(arg);
                                }

                                int32_t status = 0, value = 0, value2 = 0;

                                switch (args[0]) {
                                    case 1: /* number of color registers */
                                        switch (args[1]) {
                                            case 1: /* read */
                                            case 2: /* reset */
                                            case 4: /* get max value */
                                                value = 256;
                                                break;
                                            case 3: /* set to args[2] */
                                                value = 256;
                                                break;
                                            default:
                                                status = 2;
                                        }
                                        break;
                                    case 2: /* sixel pixels */
                                        switch (args[1]) {
                                            case 1: /* read */
                                            case 2: /* reset */
                                            case 4: /* get max value */
                                                value  = self->ws.ws_xpixel;
                                                value2 = self->ws.ws_ypixel;
                                                break;
                                            case 3: /* set to args[2] */
                                                break;
                                            default:
                                                status = 2;
                                        }
                                        break;
                                    case 3: /* regis pixels */
                                        status = 3;
                                        break;
                                    default:
                                        status = 1;
                                }

                                if (value2) {
                                    Vt_output_formated(self,
                                                       "\e[?%d;%d;%d;%dS",
                                                       args[0],
                                                       status,
                                                       value,
                                                       value2);
                                } else {
                                    Vt_output_formated(self,
                                                       "\e[?%d;%d;%dS",
                                                       args[0],
                                                       status,
                                                       value);
                                }
                            } break;

                                /* <ESC>[? Ps n Device Status Report (DSR, DEC-specific) */
                            case 'n': {
                                int arg = short_sequence_get_int_argument(seq);
                                switch (arg) {
                                    case 6: { /* report cursor position */
                                        Vt_output_formated(self,
                                                           "\e[?%u;%uR",
                                                           Vt_cursor_row(self) + 1,
                                                           self->cursor.col + 1);
                                    } break;

                                    case 15: /* Report Printer status */
                                             /* not ready */
                                        Vt_output(self, "\e[?11n", 6);
                                        break;

                                    case 26: /* Report keyboard status */
                                             /* always report US */
                                        Vt_output(self, "\e[?27;1;0;0n", 12);
                                        break;

                                    case 53: /* Report locator status */
                                             /* No locator (xterm not compiled-in) */
                                        Vt_output(self, "\e[?50n", 6);
                                        break;

                                    case 56: /* Report locator type */
                                             /* Cannot identify (xterm not compiled-in) */
                                        Vt_output(self, "\e[?57;0n", 8);
                                        break;

                                    case 85: /* Report multi-session configuration */
                                        /* Device not configured for multi-session operation */
                                        Vt_output(self, "\e[?83n", 6);
                                        break;

                                    default:
                                        WRN("Unimplemented DSR sequence: %d\n", arg);
                                }
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                }
            } break;

            /* <ESC>[> ... */
            case '>': {
                switch (last_char) {
                        /* <ESC>[> Pp m / <ESC>[> Pp ; Pv m - Set/reset key modifier options
                         * (XTMODKEYS)
                         *
                         * 0 => modifyKeyboard
                         * 1 => modifyCursorKeys
                         * 2 => modifyFunctionKeys
                         * 4 => modifyOtherKeys */
                    case 'm': {
                        int resource, value;
                        int nargs = sscanf(seq, "%d;%d", &resource, &value);
                        if (nargs == EOF) {
                            goto invalid;
                        }
                        if (!nargs) {
                            self->xterm_modify_keyboard      = VT_XT_MODIFY_KEYBOARD_DFT;
                            self->xterm_modify_cursor_keys   = VT_XT_MODIFY_CURSOR_KEYS_DFT;
                            self->xterm_modify_function_keys = VT_XT_MODIFY_FUNCTION_KEYS_DFT;
                            self->xterm_modify_other_keys    = VT_XT_MODIFY_OTHER_KEYS_DFT;
                            break;
                        }

                        switch (resource) {
                            case 0:
                                self->xterm_modify_keyboard =
                                  nargs == 2 ? value : VT_XT_MODIFY_KEYBOARD_DFT;
                                break;
                            case 1:
                                self->xterm_modify_cursor_keys =
                                  nargs == 2 ? value : VT_XT_MODIFY_CURSOR_KEYS_DFT;
                                break;
                            case 2:
                                self->xterm_modify_function_keys =
                                  nargs == 2 ? value : VT_XT_MODIFY_FUNCTION_KEYS_DFT;
                                break;
                            case 4:
                                self->xterm_modify_other_keys =
                                  nargs == 2 ? value : VT_XT_MODIFY_OTHER_KEYS_DFT;
                                break;
                            default:
                                goto invalid;
                        }

                        break;
                    invalid:
                        WRN("Invalid XTMODKEYS command \'%s\'\n", seq);
                    } break;

                        /* <ESC>[> Ps n - Disable key modifier options, xterm
                         *
                         * This control sequence corresponds to a resource value of "-1", which
                         * cannot be set with the other sequence
                         *
                         * If the parameter is omitted, modifyFunctionKeys is disabled
                         *
                         * 0 => modifyKeyboard
                         * 1 => modifyCursorKeys
                         * 2 => modifyFunctionKeys
                         * 4 => modifyOtherKeys */
                    case 'n': {
                        MULTI_ARG_IS_ERROR
                        int arg = seq[1] == 'n' ? 2 : short_sequence_get_int_argument(seq);
                        switch (arg) {
                            case 0:
                                self->xterm_modify_keyboard = -1;
                                break;
                            case 1:
                                self->xterm_modify_cursor_keys = -1;
                                break;
                            case 2:
                                self->xterm_modify_function_keys = -1;
                                break;
                            case 4:
                                self->xterm_modify_other_keys = -1;
                                break;
                            default:
                                WRN("Invalid XTMODKEYS command \'%s\'\n", seq);
                        }
                    } break;

                    /* <ESC>[ > Ps c - Send Device Attributes (Secondary DA) */
                    case 'c': {
                        MULTI_ARG_IS_ERROR
                        int arg = short_sequence_get_int_argument(seq);
                        if (arg == 0) {
                            /* report VT100, firmware ver. 0, ROM number 0 */
                            Vt_output_formated(self, "%s", "\e[>0;0;0c");
                        }
                    } break;

                    /* <ESC>[ > Ps p - Set resource value pointerMode XTSMPOINTER (xterm)
                     * 0 - never hide the pointer.
                     * 1 - hide if the mouse tracking mode is not enabled.
                     * 2 - always hide the pointer, except when leaving the window.
                     * 3 - always hide the pointer, even if leaving/entering the window.
                     */
                    case 'p': {
                        MULTI_ARG_IS_ERROR
                        int arg = short_sequence_get_int_argument(seq);
                        if (self->gui_pointer_mode != VT_GUI_POINTER_MODE_FORCE_HIDE &&
                            self->gui_pointer_mode != VT_GUI_POINTER_MODE_FORCE_SHOW) {
                            switch (arg) {
                                case 0:
                                    self->gui_pointer_mode = VT_GUI_POINTER_MODE_SHOW;
                                    break;
                                case 1:
                                    self->gui_pointer_mode = VT_GUI_POINTER_MODE_SHOW_IF_REPORTING;
                                    break;
                                case 2:
                                /* We don't control the pointer outside of the window anyway */
                                case 3:
                                    self->gui_pointer_mode = VT_GUI_POINTER_MODE_HIDE;
                                    break;
                                default:
                                    WRN("unknown XTSMPOINTER parameter \'%d\'\n", arg);
                            }
                        } else {
                            WRN("XTSMPOINTER ignored because of user setting\n");
                        }
                    } break;

                    default:
                        WRN("Unknown CSI sequence: %s\n", seq);
                }
            } break;

            /* <ESC>[= ... */
            case '=': {
                switch (last_char) {
                    /* <ESC>[ = Ps c - Send Device Attributes (Tertiary DA). */
                    case 'c': {
                        MULTI_ARG_IS_ERROR
                        int arg = short_sequence_get_int_argument(seq);
                        if (arg == 0) {
                            Vt_output(self, "\e[?6c", 5);
                        }
                    } break;

                    default:
                        WRN("Unknown CSI sequence: %s\n", seq);
                }
            } break;

            /* <ESC>[... */
            default: {
                switch (second_last_char) {
                    /* <ESC>[ .. ; .. SP ?  */
                    case ' ':
                        switch (last_char) {
                            /* <ECS>[ Ps SP @ - Shift left Ps columns(s) (default = 1) (SL), ECMA-48
                             */
                            case '@': {
                                STUB("SL");
                            } break;

                            /* <ESC>[ Ps SP A - Shift right Ps columns(s) (default = 1) (SR),
                             * ECMA-48 */
                            case 'A': {
                                STUB("SR");
                            } break;

                            /* <ESC>[ Ps SP q - Set cursor style (DECSCUSR) */
                            case 'q': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                switch (arg) {
                                    case 0:
                                    case 1:
                                        self->cursor.type     = CURSOR_BLOCK;
                                        self->cursor.blinking = false;
                                        break;
                                    case 2:
                                        self->cursor.type     = CURSOR_BLOCK;
                                        self->cursor.blinking = true;
                                        break;
                                    case 3:
                                        self->cursor.type     = CURSOR_UNDERLINE;
                                        self->cursor.blinking = true;
                                        break;
                                    case 4:
                                        self->cursor.type     = CURSOR_UNDERLINE;
                                        self->cursor.blinking = false;
                                        break;
                                    case 5:
                                        self->cursor.type     = CURSOR_BEAM;
                                        self->cursor.blinking = true;
                                        break;
                                    case 6:
                                        self->cursor.type     = CURSOR_BEAM;
                                        self->cursor.blinking = false;
                                        break;

                                    default:
                                        WRN("Unknown DECSCUR code: %d\n", arg);
                                }
                            } break;
                        }
                        break;

                    /* <ESC>[ .. ; .. "?  */
                    case '\"':
                        switch (last_char) {
                            /* <ESC>[ Ps "q - Select character protection attribute (DECSCA), VT220.
                             *
                             * 0 => DECSED and DECSEL can erase (default).
                             * 1 => DECSED and DECSEL cannot erase.
                             * 2 => DECSED and DECSEL can erase.
                             */
                            case 'q': {
                                STUB("DECSCA");
                            } break;

                            /* <ESC>[ Pl ; Pc "p - Set conformance level (DECSCL), VT220 and up.
                             * (Pl)
                             *   61 => level 1, e.g., VT100.
                             *   62 => level 2, e.g., VT200.
                             *   63 => level 3, e.g., VT300.
                             *   64 => level 4, e.g., VT400.
                             *   65 => level 5, e.g., VT500.
                             * (Pc)
                             *   0 => 8-bit controls.
                             *   1 => 7-bit controls (DEC factory default).
                             *   2 => 8-bit controls.
                             */
                            case 'p': {
                                STUB("DECSCL");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. #?  */
                    case '#':
                        switch (last_char) {
                            /* <ESC>[ Pm #{ - Push video attributes onto stack (XTPUSHSGR), xterm.
                             *
                             * The optional parameters correspond to the SGR encoding for video
                             * attributes, except for colors (which do not have a unique SGR code):
                             * 1  => Bold
                             * 2  => Faint
                             * 3  => Italicized
                             * 4  => Underlined
                             * 5  => Blink
                             * 7  => Inverse
                             * 8  => Invisible
                             * 9  => Crossed-out characters
                             * 21 => Doubly-underlined
                             * 30 => Foreground color
                             * 31 => Background color
                             */
                            case '{': {
                                MULTI_ARG_IS_ERROR
                                STUB("XTPUSHSGR");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr #|
                             *
                             * Report selected graphic rendition (XTREPORTSGR), xterm. The
                             * response is an SGR sequence which contains the attributes which
                             * are common to all cells in a rectangle. Pt ; Pl ; Pb ; Pr denotes
                             * the rectangle.
                             */
                            case '|': {
                                MULTI_ARG_IS_ERROR
                                STUB("XTREPORTSGR");
                            } break;

                            /* <ESC>[#} - Pop video attributes from stack (XTPOPSGR), xterm.
                             *
                             * Popping restores the video-attributes which were saved using
                             * XTPUSHSGR to their previous state.
                             */
                            case '}':

                            /* <ESC>[#q - Alias for <ESC>[#} */
                            case 'q': {
                                MULTI_ARG_IS_ERROR
                                STUB("XTPOPSGR");
                            } break;

                            /* <ESC>[ Pm #P - Push current dynamic and ANSI-palette colors onto
                             * stack (XTPUSHCOLORS), xterm.
                             *
                             * Parameters (integers in the range 1 through 10, since the default 0
                             * will push) may be used to store the palette into the stack without
                             * pushing.
                             */
                            case 'P': {
                                MULTI_ARG_IS_ERROR
                                STUB("XTPUSHCOLORS");
                            } break;

                            /* <ESC>[ Pm #Q -  Pop stack to set dynamic- and ANSI-palette
                             * colors (XTPOPCOLORS), xterm.
                             *
                             * Parameters (integers in the range 1 through 10, since the default
                             * 0 will pop) may be used to restore the palette from the stack
                             * without popping.
                             */
                            case 'Q': {
                                MULTI_ARG_IS_ERROR
                                STUB("XTPOPCOLORS");
                            } break;

                            /* <ESC> #R
                             * Report the current entry on the palette stack, and the number of
                             * palettes stored on the stack, using the same form as XTPOPCOLOR
                             * (default = 0) (XTREPORTCOLORS), xterm.
                             */
                            case 'R': {
                                MULTI_ARG_IS_ERROR
                                STUB("XTREPORTCOLORS");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. '?  */
                    case '\'':
                        switch (last_char) {

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr 'w - Enable Filter Rectangle (DECEFR),
                             * VT420 and up
                             *
                             * Parameters are [top;left;bottom;right]. Defines the coordinates
                             * of a filter rectangle and activates it. Anytime the locator is
                             * detected outside of the filter rectangle, an outside rectangle
                             * event is generated and the rectangle is disabled. Filter
                             * rectangles are always treated as "one-shot" events. Any
                             * parameters that are omitted default to the current locator
                             * position. If all parameters are omitted, any locator motion
                             * will be reported. DECELR always cancels any previous rectangle
                             * definition.
                             */
                            case 'w': {
                                STUB("DECEFR");
                            } break;

                            /* <ESC>[ Ps ; Pu 'z - Enable Locator Reporting (DECELR)
                             * (Ps)
                             *   0 => Locator disabled (default)
                             *   1 => Locator enabled
                             *   2 => Locator enabled for one report
                             * (Pu) <coordinate unit>
                             *   0, 2 => Cells (default)
                             *   1    => Pixels
                             */
                            case 'z': {
                                STUB("DECELR");
                            } break;

                            /* <ESC>[ Pm '{ - Select Locator Events (DECSLE)
                             *
                             * 0 => Explicit host request only (DECRQLP) (default)
                             * 1 => on button down ON
                             * 2 => on button down OFF
                             * 3 => on button up ON
                             * 4 => on button up OFF
                             */
                            case '{': {
                                STUB("DECSLE");
                            } break;

                            /* <ESC>[ Ps '| - Request Locator Position (DECRQLP)
                             *
                             * Valid values for the parameter are 0, 1 or omitted => transmit a
                             * single DECLRP locator report.
                             *
                             * If Locator Reporting has been enabled by a DECELR, xterm will respond
                             * with a DECLRP Locator Report.  This report is also generated on
                             * button up and down events if they have been enabled with a DECSLE, or
                             * when the locator is detected outside of a filter rectangle, if filter
                             * rectangles have been enabled with a DECEFR.
                             *
                             * CSI Pe ; Pb ; Pr ; Pc ; Pp &w
                             * Parameters are [event;button;row;column;page].
                             * Valid values for the event:
                             * (Pe)
                             *   0  =>  locator unavailable - no other parameters sent.
                             *   1  =>  request - xterm received a DECRQLP.
                             *   2  =>  left button down.
                             *   3  =>  left button up.
                             *   4  =>  middle button down.
                             *   5  =>  middle button up.
                             *   6  =>  right button down.
                             *   7  =>  right button up.
                             *   8  =>  M4 button down.
                             *   9  =>  M4 button up.
                             *   10 =>  locator outside filter rectangle.
                             *
                             * The "button" parameter is a bitmask indicating which buttons are
                             * pressed: Pb = 0  =>  no buttons down. Pb & 1  =>  right button down.
                             * Pb & 2  =>  middle button down.
                             * Pb & 4  =>  left button down.
                             * Pb & 8  =>  M4 button down.
                             *
                             * The "row" and "column" parameters are the coordinates of the locator
                             * position in the xterm window, encoded as ASCII decimal. The "page"
                             * parameter is not used by xterm.
                             */
                            case '|': {
                                /* locator unavailable */
                                Vt_output(self, "\e[0&w", 5);
                            } break;

                            /* <ESC>['} - Insert Ps Column(s) (default = 1) (DECIC), VT420 and up */
                            case '}': {
                                // TODO: vmargins
                                STUB("DECIC");
                            } break;

                            /* <ESC>['~ - Delete Ps Column(s) (default = 1) (DECDC), VT420 and up */
                            case '~': {
                                // TODO: vmargins
                                STUB("DECDC");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. * ..  */
                    case '*':
                        switch (last_char) {
                            /* <ESC>[ Ps *x - Select Attribute Change Extent (DECSACE), VT420 and up
                             * (Ps)
                             *   0, 1 => from start to end position, wrapped
                             *   2    => rectangle (exact)
                             */
                            case 'x': {
                                MULTI_ARG_IS_ERROR
                                STUB("DECSACE");
                            } break;

                            /* <ESC>[ Pi ; Pg ; Pt ; Pl ; Pb ; Pr *y
                             * Request Checksum of Rectangular Area (DECRQCRA), VT420 and up
                             *
                             * Response is DCS Pi ! ~ x x x x ST Pi is the request id. Pg is the
                             * page number. Pt ; Pl ; Pb ; Pr denotes the rectangle. The x's are
                             * hexadecimal digits 0-9 and A-F.
                             */
                            case 'y': {
                                STUB("DECRQCRA");
                            } break;

                            /* <ESC>[*| - Select number of lines per screen (DECSNLS), VT420 and up
                             */
                            case '|': {
                                STUB("DECSNLS");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                    /* <ESC>[ .. ; .. $ ..  */
                    case '$':
                        switch (last_char) {

                            /* <ESC>[ Ps $p - Request ANSI mode (DECRQM). VT300 and up
                             *
                             * reply: CSI ? <DECSET/DECRT code>; <value> $ y
                             *
                             * value:
                             * 0 => not recognized
                             * 1 => enabled
                             * 2 => disabled
                             * 3 => permanently enabled
                             * 4 => permanently disabled
                             */
                            case 'p': {
                                int arg = short_sequence_get_int_argument(seq);
                                Vt_report_dec_mode(self, arg);
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr ; Ps $r - Change Attributes in Rectangular
                             * Area (DECCARA), VT400 and up
                             *
                             * Pt ; Pl ; Pb ; Pr denotes the rectangle.
                             * Ps denotes the SGR attributes to change: 0, 1, 4, 5, 7
                             */
                            case 'r': {
                                STUB("DECCARA");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr ; Ps $t - Reverse Attributes in Rectangular
                             * Area (DECRARA), VT400 and up
                             *
                             * Pt ; Pl ; Pb ; Pr denotes the rectangle. Ps denotes the attributes to
                             * reverse, i.e.,  1, 4, 5, 7.
                             */
                            case 't': {
                                STUB("DECRARA");
                            } break;

                            /* <ESC>[ Ps $w - Request presentation state report (DECRQPSR), VT320
                             * and up
                             *
                             * (Ps)
                             *   0 => error
                             *   1 => cursor information report (DECCIR)
                             *     Response is DCS 1 $ u Pt ST Refer to the VT420 programming
                             *     manual, which requires six pages to document the data string Pt,
                             *   2 => tab stop report (DECTABSR). Response is DCS 2 $ u Pt ST The
                             *     data string Pt is a list of the tab-stops, separated by "/"
                             *     characters.
                             */
                            case 'w': {
                                STUB("DECRQPSR");
                            } break;

                            /* <ESC>[ Pc ; Pt ; Pl ; Pb ; Pr $x - Fill Rectangular Area (DECFRA),
                             * VT420 and up
                             *
                             * Pc is the character to use
                             * Pt ; Pl ; Pb ; Pr denotes the rectangle
                             */
                            case 'x': {
                                STUB("DECFRA");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr $z
                             * Erase Rectangular Area (DECERA), VT400 and up
                             */
                            case 'z': {
                                STUB("DECERA");
                            } break;

                            /* <ESC>[ Pt ; Pl ; Pb ; Pr ${
                             * Selective Erase Rectangular Area (DECSERA), VT400 and up
                             */
                            case '{': {
                                STUB("DECSERA");
                            } break;

                            /* <ESC>[ Ps $| - Select columns per page (DECSCPP), VT340 */
                            case '|': {
                                STUB("DECSCPP");
                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);
                        }
                        break;

                        /* <ESC>[ .. ; .. ?  */
                    default: {
                        switch (last_char) {
                            /* <ESC>[ Ps ; ... m - change one or more text attributes (SGR) */
                            case 'm': {
                                Vector_pop_n_char(&self->parser.active_sequence, 2); // 'm', '\0'
                                Vector_push_char(&self->parser.active_sequence, '\0');
                                Vt_handle_multi_argument_SGR(self,
                                                             self->parser.active_sequence,
                                                             NULL);
                            } break;

                            /* <ESC>[ Ps K - clear(erase) line right of cursor (EL)
                             * none/0 - right 1 - left 2 - all */
                            case 'K': {
                                MULTI_ARG_IS_ERROR
                                int arg = *seq == 'K' ? 0 : short_sequence_get_int_argument(seq);

                                switch (arg) {
                                    case 0:
                                        Vt_clear_right(self);
                                        break;
                                    case 1:
                                        Vt_clear_left(self);
                                        break;
                                    case 2:
                                        Vt_clear_left(self);
                                        Vt_clear_right(self);
                                        break;

                                    default:
                                        WRN("Unknown CSI(EL) sequence: %s\n", seq);
                                }
                            } break;

                            /* <ECS>[ Ps @ - Insert Ps Chars (ICH) */
                            case '@': {
                                // the normal character
                                // attribute. The cursor remains at the beginning of the blank
                                // characters. Text between the cursor and right margin moves to the
                                // right.
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                for (int i = 0; i < arg; ++i) {
                                    Vt_insert_char_at_cursor_with_shift(self, self->blank_space);
                                }
                            } break;

                            /* <ESC>[ Ps a - move cursor right (forward) Ps lines (HPR) */
                            case 'a':
                            /* <ESC>[ Ps C - move cursor right (forward) Ps lines (CUF) */
                            case 'C': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, self->cursor.col + arg, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps L - Insert line at cursor shift rest down (IL) */
                            case 'L': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_insert_line(self);
                            } break;

                            /* <ESC>[ Ps D - move cursor left (back) Ps lines (CUB) */
                            case 'D': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                uint16_t new_col =
                                  arg >= self->cursor.col ? 0 : (self->cursor.col - arg);
                                Vt_move_cursor(self, new_col, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps A - move cursor up Ps lines (CUU) */
                            case 'A': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                size_t new_row = Vt_cursor_row(self) - arg;
                                Vt_move_cursor(self, self->cursor.col, new_row);
                            } break;

                            /* <ESC>[ Ps e - move cursor down Ps lines (VPR) */
                            case 'e':
                            /* <ESC>[ Ps B - move cursor down Ps lines (CUD) */
                            case 'B': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, self->cursor.col, Vt_cursor_row(self) + arg);
                            } break;

                            case 'E': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, 0, Vt_cursor_row(self) + arg);
                            } break;

                            case 'F': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, 0, Vt_cursor_row(self) - arg);
                            } break;

                            /* <ESC>[ Ps ` - move cursor to column Ps (CBT)*/
                            case '`':
                            /* <ESC>[ Ps G - move cursor to column Ps (CHA)*/
                            case 'G': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_move_cursor(self, arg - 1, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps J - Erase display (ED) - clear... */
                            case 'J': {
                                MULTI_ARG_IS_ERROR
                                if (*seq == 'J') { /* ...from cursor to end of screen */
                                    Vt_erase_to_end(self);
                                } else {
                                    int arg = short_sequence_get_int_argument(seq);

                                    switch (arg) {
                                        case 0:
                                            Vt_erase_to_end(self);
                                            break;

                                        case 1: /* ...from start to cursor */
                                            if (Vt_scroll_region_not_default(self) ||
                                                Vt_alt_buffer_enabled(self)) {
                                                Vt_clear_above(self);
                                            } else {
                                                Vt_clear_above(self);
                                                // Vt_scroll_out_above(self);
                                            }
                                            break;

                                        case 3: /* ...whole display + scrollback buffer */
                                            /* if (settings.allow_scrollback_clear) { */
                                            /*     Vt_clear_display_and_scrollback(self); */
                                            /* } */
                                            /* break; */

                                        case 2: /* ...whole display. Contents should not
                                                 * actually be removed, but saved to scroll
                                                 * history if no scroll region is set */
                                            if (self->alt_lines.buf) {
                                                Vt_clear_display_and_scrollback(self);
                                            } else {
                                                if (Vt_scroll_region_not_default(self)) {
                                                    Vt_clear_above(self);
                                                    Vt_erase_to_end(self);
                                                } else {
                                                    Vt_scroll_out_all_content(self);
                                                }
                                            }
                                            break;
                                    }
                                }
                            } break;

                            /* <ESC>[ Ps d - move cursor to row Ps (VPA) */
                            case 'd': {
                                MULTI_ARG_IS_ERROR
                                /* origin is 1:1 */
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                --arg;
                                Vt_move_cursor(self, self->cursor.col, arg);
                            } break;

                            /* <ESC>[ Ps ; Ps r - Set scroll region (top;bottom) (DECSTBM)
                             * default: full window */
                            case 'r': {
                                int32_t top = Vt_top_line(self), bottom = Vt_bottom_line(self);

                                if (*seq != 'r') {
                                    if (sscanf(seq, "%d;%d", &top, &bottom) == EOF) {
                                        WRN("invalid CSI(DECSTBM) sequence %s\n", seq);
                                        break;
                                    }
                                    if (top <= 0)
                                        top = 1;
                                    if (bottom <= 0)
                                        bottom = 1;
                                    --top;
                                    --bottom;
                                } else {
                                    top    = 0;
                                    bottom = CALL(self->callbacks.on_number_of_cells_requested,
                                                  self->callbacks.user_data)
                                               .second -
                                             1;
                                }

                                self->scroll_region_top    = top;
                                self->scroll_region_bottom = bottom;
                            } break;

                            /* <ESC>[ Ps I - cursor forward ps tabulations (CHT) */
                            case 'I': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                uint16_t rt;
                                for (rt = 0; self->cursor.col + rt < Vt_col(self) && arg; ++rt) {
                                    if (self->tab_ruler[self->cursor.col + rt])
                                        --arg;
                                }
                                Vt_move_cursor(self, self->cursor.col + rt, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Ps Z - cursor backward ps tabulations (CBT) */
                            case 'Z': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                uint16_t lt;
                                for (lt = 0; self->cursor.col - lt && arg; ++lt) {
                                    if (self->tab_ruler[self->cursor.col - lt])
                                        --arg;
                                }
                                Vt_move_cursor(self, self->cursor.col - lt, Vt_cursor_row(self));
                            } break;

                            /* <ESC>[ Pm ... h - Set mode (SM) */
                            case 'h':
                            /* <ESC>[ Pm ... l - Reset Mode (RM) */
                            case 'l': {
                                bool               is_enable = last_char == 'h';
                                Vector_Vector_char tokens = string_split_on(seq, ";:", NULL, NULL);
                                for (Vector_char* token = NULL;
                                     (token = Vector_iter_Vector_char(&tokens, token));) {
                                    errno         = 0;
                                    uint32_t code = atoi(token->buf + 1);
                                    if (code && !errno) {
                                        Vt_handle_regular_mode(self, code, is_enable);
                                    } else {
                                        WRN("Invalid %s argument: \'%s\'\n",
                                            is_enable ? "SM" : "RM",
                                            token->buf + 1);
                                    }
                                }
                                Vector_destroy_Vector_char(&tokens);
                            } break;

                            /* <ESC>[ Pn g - tabulation clear (TBC) */
                            case 'g': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);

                                switch (arg) {
                                    case 0:
                                        self->tab_ruler[self->cursor.col] = false;
                                        break;
                                    case 3:
                                        Vt_clear_all_tabstops(self);
                                        break;
                                    default:;
                                }

                            } break;

                            /* no args: 1:1, one arg: x:1 */
                            /* <ESC>[ Py ; Px f - move cursor to Px-Py (HVP) (deprecated) */
                            case 'f':
                            /* <ESC>[ Py ; Px H - move cursor to Px-Py (CUP) */
                            case 'H': {
                                int32_t x = 1, y = 1;
                                if (*seq != 'H' && sscanf(seq, "%d;%d", &y, &x) == EOF) {
                                    WRN("invalid CUP/HVP sequence %s\n", seq);
                                    break;
                                }
                                if (x <= 0)
                                    x = 1;
                                if (y <= 0)
                                    y = 1;
                                --x;
                                --y;
                                Vt_move_cursor(self, x, y);
                            } break;

                            /* <ESC>[...c - Send device attributes (Primary DA)
                             *
                             * 1        132 columns
                             * 2        Printer port
                             * 4        Sixel
                             * 6        Selective erase
                             * 7        Soft character set (DRCS)
                             * 8        User-defined keys (UDKs)
                             * 9        National replacement character sets (NRCS) (International
                             *terminal only) 12       Yugoslavian (SCS) 15       Technical character
                             *set 18       Windowing capability 21       Horizontal scrolling 23
                             *Greek 24       Turkish 42       ISO Latin-2 character set 44 PCTerm 45
                             *Soft key map 46       ASCII emulation
                             *
                             **/
                            case 'c': {
                                /* report vt340 type device with sixel ,132column, and window system
                                 * support */
                                Vt_output(self, "\e[?63;1;4c", 10);
                                /* Vt_output(self, "\e[?64;1;4;18c", 11); */
                            } break;

                            /* <ESC>[...n - Device status report (DSR) */
                            case 'n': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg == 5) {
                                    /* 5 - is terminal ok
                                     *  ok - 0, not ok - 3 */
                                    Vt_output(self, "\e[0n", 4);
                                } else if (arg == 6) {
                                    /* 6 - report cursor position */
                                    Vt_output_formated(self,
                                                       "\e[%u;%uR",
                                                       Vt_cursor_row(self) + 1,
                                                       self->cursor.col + 1);
                                } else {
                                    WRN("Unimplemented DSR code: %d\n", arg);
                                }
                            } break;

                            /* <ESC>[ Ps M - Delete lines (default = 1) (DL) */
                            case 'M': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_delete_line(self);
                            } break;

                            /* <ESC>[ Ps S - Scroll up (default = 1) (SU) */
                            case 'S': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_scroll_up(self);
                            } break;

                            /* <ESC>[ Ps T - Scroll down (default = 1) (SD) */
                            case 'T': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                for (int i = 0; i < arg; ++i)
                                    Vt_scroll_down(self);
                            } break;

                                /* <ESC>[ Ps X - Erase Ps Character(s) (default = 1) (ECH) */
                            case 'X': {
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_erase_chars(self, arg);
                            } break;

                                /* <ESC>[ Ps P - Delete Ps Character(s) (default = 1) (DCH) */
                            case 'P': {
                                // As characters are deleted, the remaining
                                // characters between the cursor
                                // and right margin move to the left.
                                MULTI_ARG_IS_ERROR
                                int arg = short_sequence_get_int_argument(seq);
                                if (arg <= 0)
                                    arg = 1;
                                Vt_delete_chars(self, arg);
                            } break;

                            /* <ESC>[ Ps b -  Repeat the preceding graphic character Ps times (REP)
                             * in xterm any cursor movement or SGR sequences after inserting the
                             * character cause this to have no effect */
                            case 'b': {
                                MULTI_ARG_IS_ERROR
                                if (likely(self->has_last_inserted_rune)) {
                                    int arg = short_sequence_get_int_argument(seq);
                                    if (arg <= 0)
                                        arg = 1;
                                    VtRune repeated = self->last_inserted;
                                    for (int i = 0; i < arg; ++i) {
                                        Vt_insert_char_at_cursor(self, repeated);
                                    }
                                }
                            } break;

                            /* <ESC>[ Ps i -  Media Copy (MC) Local printing related commands */
                            case 'i':
                                break;

                            /* <ESC>[u - Restore cursor (SCORC, also ANSI.SYS) */
                            /* <ESC>[Ps SP u - Set margin-bell volume (DECSMBV), VT520 */
                            case 'u': {
                                if (*seq == 'u') {
                                    // TODO: cursor restore
                                } else {
                                    WRN("DECSMBV not implemented\n");
                                }
                            } break;

                            case 's': {
                                /* CSI Pl ; Pr s Set left and right margins (DECSLRM), VT420 and up.
                                 * This is available only when DECLRMM is enabled.
                                 * `If the left and right margins are set to columns other than
                                 * 1 and 80 (or 132), the terminal cannot scroll smoothly.' */
                                if (self->modes.vertical_split_screen_mode) {
                                    uint16_t lmargin = 1, rmargin = Vt_col(self);
                                    sscanf(seq, "%hu;%hus", &lmargin, &rmargin);
                                    rmargin = MIN(rmargin, Vt_col(self));
                                    if (rmargin > lmargin + 1) {
                                        self->scroll_region_left  = lmargin - 1;
                                        self->scroll_region_right = rmargin - 1;
                                    } else {
                                        WRN("invalid DECSLRM values\n");
                                    }

                                    /* DECSLRM moves the cursor to column 1, line 1 of the page */
                                    self->cursor.col = 0;
                                    self->cursor.row = Vt_top_line(self);

                                } else {
                                    /* <ESC>[s - Save cursor (SCOSC, also ANSI.SYS) available only
                                     * when DECLRMM is disabled */
                                    STUB("SCOSC");
                                }
                            } break;

                            /* <ESC>[ Ps q - Manipulate keyboard LEDs (DECLL), VT100 */
                            case 'q':
                                break;

                            /* <ESC>[ Ps ; Ps ; Ps t - xterm windowOps (XTWINOPS)*/
                            case 't': {
                                int32_t nargs;
                                int32_t args[4];

                                /* Set omitted args to -1 */
                                for (nargs = 0; seq && nargs < 4 && *seq != 't'; ++nargs) {
                                    *(args + nargs) = *seq == ';' ? -1 : atoi(seq);
                                    seq             = strstr(seq, ";");
                                    if (seq)
                                        ++seq;
                                }
                                if (!nargs) {
                                    break;
                                }

                                switch (args[0]) {

                                    /* de-iconyfy */
                                    case 1:
                                        // TODO:
                                        break;

                                    /* iconyfy */
                                    case 2:
                                        // TODO:
                                        break;

                                    /* move window to args[1]:args[2] */
                                    case 3:
                                        // TODO:
                                        break;

                                    /* Resize window in pixels
                                     *
                                     * Omitted parameters reuse the current height or width. Zero
                                     * parameters use the display's height or width.
                                     *
                                     * FIXME: This should accounts for window decorations
                                     * (_NET_FRAME_EXTENTS).
                                     */
                                    case 4:
                                        if (!settings.windowops_manip) {
                                            break;
                                        }

                                        if (nargs >= 2) {
                                            int32_t target_h = args[1];
                                            int32_t target_w = nargs >= 3 ? args[2] : -1;

                                            if (target_w == -1 || target_h == -1) {
                                                Pair_uint32_t current_dims =
                                                  CALL(self->callbacks.on_window_size_requested,
                                                       self->callbacks.user_data);

                                                if (target_w == -1) {
                                                    target_w = current_dims.first;
                                                }
                                                if (target_h == -1) {
                                                    target_h = current_dims.second;
                                                }
                                            }
                                            if (target_w == 0 || target_h == 0) {
                                                // TODO: get display size
                                                STUB("XTWINOPS display size reports");
                                                break;
                                            }

                                            CALL(self->callbacks.on_window_dimensions_set,
                                                 self->callbacks.user_data,
                                                 target_w,
                                                 target_h);
                                        } else {
                                            WRN("Invalid XTWINOPS sequence: %s\n", seq);
                                        }
                                        break;

                                    /* Raise window */
                                    case 5:
                                        // TODO:
                                        break;

                                    /* lower window */
                                    case 6:
                                        // TODO:
                                        break;

                                    /* Refresh window */
                                    case 7:
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        self->defered_events.action_performed = true;
                                        self->defered_events.repaint          = true;
                                        break;

                                    /* Resize in cells */
                                    case 8: {
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        if (nargs >= 2) {
                                            int32_t target_rows = args[1];
                                            int32_t target_cols = nargs >= 3 ? args[2] : -1;

                                            Pair_uint32_t target_text_area_dims = CALL(
                                              self->callbacks.on_window_size_from_cells_requested,
                                              self->callbacks.user_data,
                                              target_cols > 0 ? target_cols : 1,
                                              target_rows > 0 ? target_rows : 1);

                                            Pair_uint32_t currnet_text_area_dims =
                                              CALL(self->callbacks.on_text_area_size_requested,
                                                   self->callbacks.user_data);

                                            if (target_cols == -1) {
                                                target_text_area_dims.first =
                                                  currnet_text_area_dims.first;
                                            }
                                            if (target_rows == -1) {
                                                target_text_area_dims.second =
                                                  currnet_text_area_dims.second;
                                            }
                                            if (target_cols == 0 || target_rows == 0) {
                                                STUB("XTWINOPS display size reports");
                                                break;
                                            }

                                            CALL(self->callbacks.on_text_area_dimensions_set,
                                                 self->callbacks.user_data,
                                                 target_text_area_dims.first,
                                                 target_text_area_dims.second);
                                        } else {
                                            WRN("Invalid XTWINOPS sequence: %s\n", seq);
                                        }
                                    } break;

                                    /* Maximize */
                                    case 9: {
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        if (nargs >= 2) {
                                            switch (args[1]) {
                                                /* Unmaximize */
                                                case 0:
                                                    CALL(
                                                      self->callbacks.on_window_maximize_state_set,
                                                      self->callbacks.user_data,
                                                      false);
                                                    break;

                                                /* invisible-island.net:
                                                 * `xterm uses Extended Window Manager Hints (EWMH)
                                                 * to maximize the window.  Some window managers
                                                 * have incomplete support for EWMH.  For instance,
                                                 * fvwm, flwm and quartz-wm advertise support for
                                                 * maximizing windows horizontally or vertically,
                                                 * but in fact equate those to the maximize
                                                 * operation.`
                                                 *
                                                 * Waylands xdg_shell/wl_shell have no concept of
                                                 * 'vertical/horizontal window maximization' so we
                                                 * should also treat that as regular maximization.
                                                 */
                                                /* Maximize */
                                                case 1:
                                                /* Maximize vertically */
                                                case 2:
                                                /* Maximize horizontally */
                                                case 3:
                                                    CALL(
                                                      self->callbacks.on_window_maximize_state_set,
                                                      self->callbacks.user_data,
                                                      true);
                                                    break;

                                                default:
                                                    WRN("Invalid XTWINOPS: %s\n", seq);
                                            }
                                        } else {
                                            WRN("Invalid XTWINOPS: %s\n", seq);
                                        }
                                    } break;

                                        /* Fullscreen */
                                    case 10: {
                                        if (!settings.windowops_manip) {
                                            break;
                                        }
                                        if (nargs >= 2) {
                                            switch (args[1]) {
                                                /* Disable */
                                                case 0:
                                                    CALL(self->callbacks
                                                           .on_window_fullscreen_state_set,
                                                         self->callbacks.user_data,
                                                         false);
                                                    break;

                                                /* Enable */
                                                case 1:
                                                    CALL(self->callbacks
                                                           .on_window_fullscreen_state_set,
                                                         self->callbacks.user_data,
                                                         true);
                                                    break;

                                                    /* Toggle */
                                                case 2: {
                                                    bool current_state = CALL(
                                                      self->callbacks.on_fullscreen_state_requested,
                                                      self->callbacks.user_data);
                                                    CALL(self->callbacks
                                                           .on_window_fullscreen_state_set,
                                                         self->callbacks.user_data,
                                                         !current_state);
                                                } break;

                                                default:
                                                    WRN("Invalid XTWINOPS: %s\n", seq);
                                                    break;
                                            }
                                        } else {
                                            WRN("Invalid XTWINOPS: %s\n", seq);
                                        }
                                    } break;

                                    /* Report iconification state */
                                    case 11: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        bool is_minimized =
                                          CALL(self->callbacks.on_minimized_state_requested,
                                               self->callbacks.user_data);
                                        Vt_output_formated(self, "\e[%d", is_minimized ? 1 : 2);

                                    } break;

                                    /* Report window position */
                                    case 13: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Pair_uint32_t pos =
                                          CALL(self->callbacks.on_window_position_requested,
                                               self->callbacks.user_data);
                                        Vt_output_formated(self,
                                                           "\e[3;%d;%d;t",
                                                           pos.first,
                                                           pos.second);
                                    } break;

                                    /* Report window size in pixels */
                                    case 14: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self,
                                                           "\e[4;%d;%d;t",
                                                           self->ws.ws_xpixel,
                                                           self->ws.ws_ypixel);
                                    } break;

                                    /* Report text area size in chars */
                                    case 18: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self,
                                                           "\e[8;%d;%d;t",
                                                           Vt_col(self),
                                                           Vt_row(self));

                                    } break;

                                    /* Report window size in chars */
                                    case 19: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self,
                                                           "\e[9;%d;%d;t",
                                                           Vt_col(self),
                                                           Vt_row(self));

                                    } break;

                                    /* Report icon name */
                                    case 20:
                                        /* Report window title */
                                    case 21: {
                                        if (!settings.windowops_info) {
                                            break;
                                        }
                                        Vt_output_formated(self, "\e]L%s\e\\", self->title);
                                    } break;

                                    /* push title to stack */
                                    case 22:
                                        Vt_push_title(self);
                                        LOG("Title stack push\n");
                                        break;

                                    /* pop title from stack */
                                    case 23:
                                        Vt_pop_title(self);
                                        LOG("Title stack pop\n");
                                        break;

                                    /* Resize window to args[1] lines (DECSLPP) */
                                    default: {
                                        Pair_uint32_t target_dims =
                                          CALL(self->callbacks.on_window_size_from_cells_requested,
                                               self->callbacks.user_data,
                                               Vt_col(self),
                                               short_sequence_get_int_argument(seq));

                                        CALL(self->callbacks.on_window_dimensions_set,
                                             self->callbacks.user_data,
                                             target_dims.first,
                                             target_dims.second);
                                    }
                                }

                            } break;

                            default:
                                WRN("Unknown CSI sequence: %s\n", seq);

                        } // end switch (last_char)
                    }
                } // end switch (second_last_char)
            }
        } // end switch (first_char)

        Vector_clear_char(&self->parser.active_sequence);
        self->parser.state = PARSER_STATE_LITERAL;
    }
}

static inline void Vt_alt_buffer_on(Vt* self, bool save_mouse)
{
    if (self->alt_lines.buf) {
        return;
    }
    Vt_clear_all_proxies(self);
    Vt_visual_scroll_reset(self);
    Vt_select_end(self);
    self->has_last_inserted_rune = false;
    self->alt_lines              = self->lines;
    self->alt_image_views        = self->image_views;
    self->alt_scrolled_sixels    = self->scrolled_sixels;
    self->lines                  = Vector_new_VtLine(self);
    self->image_views            = Vector_new_RcPtr_VtImageSurfaceView();
    self->scrolled_sixels        = Vector_new_RcPtr_VtSixelSurface();
    for (uint16_t i = 0; i < Vt_row(self); ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
    }
    if (save_mouse) {
        self->alt_cursor_pos  = self->cursor.col;
        self->alt_active_line = self->cursor.row;
    }
    self->cursor.row = 0;
    Vt_command_output_interrupted(self);

    CALL(self->callbacks.on_buffer_changed, self->callbacks.user_data);
}

static inline void Vt_alt_buffer_off(Vt* self, bool save_mouse)
{
    if (self->alt_lines.buf) {
        self->has_last_inserted_rune = false;
        Vt_select_end(self);
        Vector_destroy_VtLine(&self->lines);
        Vector_destroy_RcPtr_VtImageSurfaceView(&self->image_views);
        Vector_destroy_RcPtr_VtSixelSurface(&self->scrolled_sixels);
        self->lines           = self->alt_lines;
        self->image_views     = self->alt_image_views;
        self->scrolled_sixels = self->alt_scrolled_sixels;
        self->alt_lines.buf   = NULL;
        self->alt_lines.size  = 0;
        if (save_mouse) {
            self->cursor.col = self->alt_cursor_pos;
            self->cursor.row = self->alt_active_line;
        }
        self->scroll_region_top    = 0;
        self->scroll_region_bottom = Vt_row(self) - 1;
        Vt_visual_scroll_reset(self);
        Vt_command_output_interrupted(self);

        CALL(self->callbacks.on_buffer_changed, self->callbacks.user_data);
    }
}

/**
 * Interpret a single argument SGR command */
__attribute__((hot)) static void Vt_handle_single_argument_SGR(Vt*     self,
                                                               char*   command,
                                                               VtRune* opt_target)
{
    int cmd = *command ? atoi(command) : 0;

    VtRune* r = OR(opt_target, &self->parser.char_state);

#define MAYBE_DISABLE_ALL_UNDERLINES                                                               \
    if (!settings.allow_multiple_underlines) {                                                     \
        r->underlined      = false;                                                                \
        r->doubleunderline = false;                                                                \
        r->curlyunderline  = false;                                                                \
    }

    switch (cmd) {
        /**'Enable' character attribs */

        /* Normal (default) */
        case 0:
            Vt_reset_text_attribs(self, opt_target);
            break;

        /* Bold, VT100 */
        case 1:
            if (r->rune.style == VT_RUNE_ITALIC) {
                r->rune.style = VT_RUNE_BOLD_ITALIC;
            } else {
                r->rune.style = VT_RUNE_BOLD;
            }
            break;

        /* Faint/dim/decreased intensity, ECMA-48 2nd */
        case 2:
            r->dim = true;
            break;

        /* Italicized, ECMA-48 2nd */
        case 3:
            if (r->rune.style == VT_RUNE_BOLD) {
                r->rune.style = VT_RUNE_BOLD_ITALIC;
            } else {
                r->rune.style = VT_RUNE_ITALIC;
            }
            break;

        /* Underlined */
        case 4:
            MAYBE_DISABLE_ALL_UNDERLINES
            r->underlined = true;
            break;

        /* Slow (less than 150 per minute) blink */
        case 5:
        /* Fast blink (MS-DOS) (most terminals that implement this use the same speed) */
        case 6:
            r->blinkng = true;
            break;

        /* Inverse
         * There is no clear definition of what this should actually do, but reversing all colors,
         * after bg and fg were determined, seems to be the widely accepted behavior. */
        case 7:
            r->invert = true;
            break;

        /* Invisible, i.e., hidden, ECMA-48 2nd, VT300 */
        case 8:
            r->hidden = true;
            break;

        /* Crossed-out characters, ECMA-48 3rd */
        case 9:
            r->strikethrough = true;
            break;

        /* Fraktur (not widely supported) */
        /* case 20: */
        /*     break; */

        /* Doubly-underlined, ECMA-48 3rd */
        case 21:
            MAYBE_DISABLE_ALL_UNDERLINES
            r->doubleunderline = true;
            break;

        /* Framed (not widely supported) */
        /* case 51: */
        /*     break; */

        /* Encircled (not widely supported) */
        /* case 52: */
        /*     break; */

        /* Overlined (widely supported extension) */
        case 53:
            r->overline = true;
            break;

            /* Superscript (non-standard extension) */
            /* case 73: */
            /*     break; */

            /* Subscript (non-standard extension) */
            /* case 74: */
            /*     break; */

            /* Ideogram */
            /* case 60 ... 64: */
            /*     break; */

            /** 'Disable' character attribs */

        /* Normal (neither bold nor faint), ECMA-48 3rd
         *
         * It works this way because 'Bold' used to be 'bright'/'high intensity', the oposite of
         * 'faint'. SGR 22 was supposed to reset the color intensity to default. At some point
         * terminals started representing 'bright' characters as bold instead of changing the color.
         */
        case 22:
            if (r->rune.style == VT_RUNE_BOLD_ITALIC) {
                r->rune.style = VT_RUNE_ITALIC;
            } else if (r->rune.style == VT_RUNE_BOLD) {
                r->rune.style = VT_RUNE_NORMAL;
            }
            r->dim = false;
            break;

        /* Not italicized, ECMA-48 3rd */
        case 23:
            if (r->rune.style == VT_RUNE_BOLD_ITALIC) {
                r->rune.style = VT_RUNE_BOLD;
            } else if (r->rune.style == VT_RUNE_ITALIC) {
                r->rune.style = VT_RUNE_NORMAL;
            }
            break;

        /*  Not underlined, ECMA-48 3rd */
        case 24:
            r->underlined = false;
            break;

        /* Steady (not blinking), ECMA-48 3rd */
        case 25:
            r->blinkng = false;
            break;

        /* Positive (not inverse), ECMA-48 3rd */
        case 27:
            r->invert = false;
            break;

        /* Visible (not hidden), ECMA-48 3rd, VT300 */
        case 28:
            r->hidden = false;
            break;

        /* Not crossed-out, ECMA-48 3rd */
        case 29:
            r->strikethrough = false;
            break;

        /* Set foreground color to default, ECMA-48 3rd */
        case 39:
            Vt_set_fg_color_default(self, opt_target);
            break;

        /* Set background color to default, ECMA-48 3rd */
        case 49:
            Vt_set_bg_color_default(self, opt_target);
            break;

        /* Set underline color to default (widely supported extension) */
        case 59:
            r->line_color_not_default = false;
            break;

            /* Disable all ideogram attributes */
            /* case 65: */
            /*     break; */

        case 30 ... 37:
            Vt_set_fg_color_palette(self, cmd - 30, opt_target);
            break;

        case 40 ... 47:
            Vt_set_bg_color_palette(self, cmd - 40, opt_target);
            break;

        case 90 ... 97:
            Vt_set_fg_color_palette(self, cmd - 82, opt_target);
            break;

        case 100 ... 107:
            Vt_set_bg_color_palette(self, cmd - 92, opt_target);
            break;

        default:
            WRN("Unknown SGR code: %d\n", cmd);
    }
}

/**
 * Interpret an SGR sequence
 *
 * SGR codes are separated by one ';' or ':', some values require a set number of following
 * 'arguments'. 'Commands' may be combined into a single sequence. A ';' without any text should be
 * interpreted as a 0 (CSI ; 3 m == CSI 0 ; 3 m), but ':' should not
 * (CSI 58:2::130:110:255 m == CSI 58:2:130:110:255 m)" */
static void Vt_handle_multi_argument_SGR(Vt* self, Vector_char seq, VtRune* opt_target)
{
    Vector_Vector_char tokens = string_split_on(seq.buf, ";", ":", NULL);
    for (Vector_char* token = NULL; (token = Vector_iter_Vector_char(&tokens, token));) {
        Vector_char* args[] = { token, NULL, NULL, NULL, NULL };

        /* color change 'commands' */
        if (!strcmp(token->buf + 1, "38") /* foreground */ ||
            !strcmp(token->buf + 1, "48") /* background */ ||
            !strcmp(token->buf + 1, "58") /* underline  */) {
            /* next argument determines how the color will be set and final number of args */

            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                (args[2] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                if (!strcmp(args[1]->buf + 1, "5")) {
                    /* from 256 palette (one argument) */
                    uint32_t idx = MIN(atoi(args[2]->buf + 1), 255);
                    if (args[0]->buf[1] == '3') {
                        Vt_set_fg_color_palette(self, idx, opt_target);
                    } else if (args[0]->buf[1] == '4') {
                        Vt_set_bg_color_palette(self, idx, opt_target);
                    } else if (args[0]->buf[1] == '5') {
                        Vt_set_line_color_palette(self, idx, opt_target);
                    }
                } else if (!strcmp(args[1]->buf + 1, "2")) {
                    /* sent as 24-bit rgb (three arguments) */
                    if ((args[3] = (token = Vector_iter_Vector_char(&tokens, token))) &&
                        (args[4] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                        uint32_t c[3] = { MIN(atoi(args[2]->buf + 1), 255),
                                          MIN(atoi(args[3]->buf + 1), 255),
                                          MIN(atoi(args[4]->buf + 1), 255) };

                        if (args[0]->buf[1] == '3') {
                            ColorRGB clr = { .r = c[0], .g = c[1], .b = c[2] };
                            Vt_set_fg_color_custom(self, clr, opt_target);
                        } else if (args[0]->buf[1] == '4') {
                            ColorRGBA clr = { .r = c[0], .g = c[1], .b = c[2], .a = 255 };
                            Vt_set_bg_color_custom(self, clr, opt_target);
                        } else if (args[0]->buf[1] == '5') {
                            ColorRGB clr = { .r = c[0], .g = c[1], .b = c[2] };
                            Vt_set_line_color_custom(self, clr, opt_target);
                        }
                    }
                }
            }
        } else if (!strcmp(token->buf + 1, "4")) {
            /* possible curly underline */
            if ((args[1] = (token = Vector_iter_Vector_char(&tokens, token)))) {
                /* enable this only on "4:3" not "4;3" */
                if (!strcmp(args[1]->buf, ":3")) {
                    VtRune* r = OR(opt_target, &self->parser.char_state);
                    if (!settings.allow_multiple_underlines) {
                        r->underlined      = false;
                        r->doubleunderline = false;
                    }
                    r->curlyunderline = true;
                } else {
                    Vt_handle_single_argument_SGR(self, args[0]->buf + 1, opt_target);
                    token = Vector_iter_back_Vector_char(&tokens, token);
                }
            } else {
                Vt_handle_single_argument_SGR(self, args[0]->buf + 1, opt_target);
                break; // end of sequence
            }
        } else {
            Vt_handle_single_argument_SGR(self, token->buf + 1, opt_target);
        }
    }
    Vector_destroy_Vector_char(&tokens);
}

static void Vt_handle_APC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);
    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {

        if (*Vector_last_char(&self->parser.active_sequence) == '\\') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        if (*Vector_last_char(&self->parser.active_sequence) == '\e') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        if (*Vector_last_char(&self->parser.active_sequence) == '\a') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        Vector_push_char(&self->parser.active_sequence, '\0');
        const char* seq = self->parser.active_sequence.buf;

        switch (seq[0]) {
            /* Terminal image protocol */
            case 'G': {
                // WRN("Terminal image protocol support is incomplete and unstable\n");

                if (!seq[1])
                    break;

                char *s      = (char*)seq + 1, *control_data, *payload, *arg;
                control_data = strsep(&s, ";");
                payload      = strsep(&s, ";");

                vt_image_proto_action_t       action       = VT_IMAGE_PROTO_ACTION_TRANSMIT;
                vt_image_proto_compression_t  compression  = VT_IMAGE_PROTO_COMPRESSION_NONE;
                vt_image_proto_transmission_t transmission = VT_IMAGE_PROTO_TRANSMISSION_DIRECT;
                uint8_t                       format       = 24;
                uint32_t                      id           = 0;
                size_t                        size         = 0;
                size_t                        offset       = 0;
                uint32_t                      image_width  = 0;
                uint32_t                      image_height = 0;
                bool                          complete     = true;
                char delete                                = 'a';
                vt_image_proto_display_args_t display_args = {
                    .z_layer         = 0,
                    .cell_width      = 0,
                    .cell_height     = 0,
                    .anchor_offset_x = 0,
                    .anchor_offset_y = 0,
                    .sample_offset_x = 0,
                    .sample_offset_y = 0,
                    .sample_width    = 0,
                    .sample_height   = 0,
                };

                for (char* cdata = control_data; (arg = strsep(&cdata, ","));) {

                    if (strstr(arg, "a=")) {
                        switch (arg[2]) {
                            case 't':
                                action = VT_IMAGE_PROTO_ACTION_TRANSMIT;
                                break;
                            case 'T':
                                action = VT_IMAGE_PROTO_ACTION_TRANSMIT_AND_DISPLAY;
                                break;
                            case 'q':
                                action = VT_IMAGE_PROTO_ACTION_QUERY;
                                break;
                            case 'p':
                                action = VT_IMAGE_PROTO_ACTION_DISPLAY;
                                break;
                            case 'd':
                                action = VT_IMAGE_PROTO_ACTION_DELETE;
                                break;
                        }
                    } else if (strstr(arg, "m=")) {
                        if (arg[2] == '1') {
                            complete = false;
                        }
                    } else if (strstr(arg, "o=")) {
                        switch (arg[2]) {
                            case 'z':
                                compression = VT_IMAGE_PROTO_COMPRESSION_ZLIB;
                                break;
                        }
                        break;
                    } else if (strstr(arg, "f=")) {
                        format = atoi(arg + 2);
                    } else if (strstr(arg, "i=")) {
                        long tmp = atol(arg + 2);
                        if (tmp > 0)
                            id = MIN(tmp, UINT32_MAX);
                    } else if (strstr(arg, "s=")) {
                        image_width = atoi(arg + 2);
                    } else if (strstr(arg, "v=")) {
                        image_height = atoi(arg + 2);
                    } else if (strstr(arg, "S=")) {
                        size = atoi(arg + 2);
                    } else if (strstr(arg, "t=")) {
                        switch (arg[2]) {
                            case 'd':
                                transmission = VT_IMAGE_PROTO_TRANSMISSION_DIRECT;
                                break;
                            case 'f':
                                transmission = VT_IMAGE_PROTO_TRANSMISSION_FILE;
                                break;
                            case 't':
                                transmission = VT_IMAGE_PROTO_TRANSMISSION_TEMP_FILE;
                                break;
                            case 's':
                                transmission = VT_IMAGE_PROTO_TRANSMISSION_SHARED_MEM;
                                break;
                        }
                    } else if (strstr(arg, "X=")) {
                        display_args.anchor_offset_x = atoi(arg + 2);
                    } else if (strstr(arg, "Y=")) {
                        display_args.anchor_offset_y = atoi(arg + 2);
                    } else if (strstr(arg, "x=")) {
                        display_args.sample_offset_x = atoi(arg + 2);
                    } else if (strstr(arg, "y=")) {
                        display_args.sample_offset_y = atoi(arg + 2);
                    } else if (strstr(arg, "w=")) {
                        display_args.sample_width = atoi(arg + 2);
                    } else if (strstr(arg, "h=")) {
                        display_args.sample_height = atoi(arg + 2);
                    } else if (strstr(arg, "c=")) {
                        display_args.cell_width = atoi(arg + 2);
                    } else if (strstr(arg, "r=")) {
                        display_args.cell_height = atoi(arg + 2);
                    } else if (strstr(arg, "d=")) {
                        delete = arg[2];
                    } else if (strnlen(arg, 1)) {
                        WRN("unknown image protocol argument\'%s\'\n", arg);
                    }
                }

                const char* error_string = NULL;
                switch (action) {
                    case VT_IMAGE_PROTO_ACTION_TRANSMIT:
                    case VT_IMAGE_PROTO_ACTION_TRANSMIT_AND_DISPLAY: {
                        error_string = Vt_img_proto_transmit(
                          self,
                          transmission,
                          compression,
                          format,
                          complete,
                          offset,
                          size,
                          display_args,
                          action == VT_IMAGE_PROTO_ACTION_TRANSMIT_AND_DISPLAY,
                          id,
                          image_width,
                          image_height,
                          payload);

                        if (id) {
                            Vt_output_formated(self, "\e_Gi=%u;%s\e\\", id, OR(error_string, "OK"));
                        }
                    } break;
                    case VT_IMAGE_PROTO_ACTION_DISPLAY: {
                        Vt_img_proto_display(self, id, display_args);
                        if (id) {
                            Vt_output_formated(self, "\e_Gi=%u;%s\e\\", id, OR(error_string, "OK"));
                        }
                    } break;
                    case VT_IMAGE_PROTO_ACTION_DELETE: {

#define L_DELETE_IMG_VIEWS_FILTERED(expr)                                                          \
    Vector_vt_image_surface_view_delete_action_t dels =                                            \
      Vector_new_vt_image_surface_view_delete_action_t();                                          \
                                                                                                   \
    for (RcPtr_VtImageSurfaceView* i = NULL;                                                       \
         (i = Vector_iter_RcPtr_VtImageSurfaceView(&self->image_views, i));) {                     \
        VtImageSurfaceView* view = RcPtr_get_VtImageSurfaceView(i);                                \
        if (view && expr) {                                                                        \
            Vector_push_vt_image_surface_view_delete_action_t(                                     \
              &dels,                                                                               \
              (vt_image_surface_view_delete_action_t){ .line = view->anchor_global_index,          \
                                                       .view = view });                            \
        }                                                                                          \
    }                                                                                              \
    for (vt_image_surface_view_delete_action_t* i = NULL;                                          \
         (i = Vector_iter_vt_image_surface_view_delete_action_t(&dels, i));) {                     \
        VtLine* ln = &self->lines.buf[i->line];                                                    \
        if (ln->graphic_attachments && ln->graphic_attachments->images) {                          \
            for (RcPtr_VtImageSurfaceView* p = NULL;                                               \
                 (p =                                                                              \
                    Vector_iter_RcPtr_VtImageSurfaceView(ln->graphic_attachments->images, p));) {  \
                VtImageSurfaceView* v = RcPtr_get_VtImageSurfaceView(p);                           \
                if (v == i->view) {                                                                \
                    Vector_remove_at_RcPtr_VtImageSurfaceView(                                     \
                      ln->graphic_attachments->images,                                             \
                      Vector_index_RcPtr_VtImageSurfaceView(ln->graphic_attachments->images, p),   \
                      1);                                                                          \
                }                                                                                  \
            }                                                                                      \
            if (!ln->graphic_attachments->images->size) {                                          \
                Vector_destroy_RcPtr_VtImageSurfaceView(ln->graphic_attachments->images);          \
                free(ln->graphic_attachments->images);                                             \
                ln->graphic_attachments->images = NULL;                                            \
            }                                                                                      \
            if (!ln->graphic_attachments->sixels) {                                                \
                free(ln->graphic_attachments);                                                     \
                ln->graphic_attachments = NULL;                                                    \
            }                                                                                      \
        }                                                                                          \
    }                                                                                              \
    Vector_destroy_vt_image_surface_view_delete_action_t(&dels);

                        switch (delete) {
                            /* Delete all images visible on screen */
                            case 'A':
                            case 'a': {
                                L_DELETE_IMG_VIEWS_FILTERED(
                                  Vt_ImageSurfaceView_is_visible(self, view))
                            } break;

                            /* Delete all images with the specified id */
                            case 'i':
                            case 'I': {
                                L_DELETE_IMG_VIEWS_FILTERED(
                                  view->source_image_surface.block &&
                                  view->source_image_surface.block->payload.id == id)
                            } break;

                            /* Delete all images that intersect with the current cursor position */
                            case 'c':
                            case 'C': {
                                L_DELETE_IMG_VIEWS_FILTERED(
                                  VtImageSurfaceView_intersects(view,
                                                                self->cursor.row,
                                                                self->cursor.col))
                            } break;

                            /* Delete all images that intersect a specific cell (x=, y=) */
                            case 'p':
                            case 'P': {
                                L_DELETE_IMG_VIEWS_FILTERED(VtImageSurfaceView_intersects(
                                  view,
                                  display_args.sample_offset_y + Vt_top_line(self) - 1,
                                  display_args.sample_offset_x))
                            } break;

                            /* Delete all images that intersect a specific cell on given z-layer
                             * (x=, y=, z=) */
                            case 'q':
                            case 'Q': {
                                L_DELETE_IMG_VIEWS_FILTERED(
                                  view->z_layer == display_args.z_layer &&
                                  VtImageSurfaceView_intersects(view,
                                                                display_args.sample_offset_y +
                                                                  Vt_top_line(self) - 1,
                                                                display_args.sample_offset_x))
                            } break;

                            /* Delete all images that intersect a specific column (x=) */
                            case 'x':
                            case 'X': {
                                L_DELETE_IMG_VIEWS_FILTERED(
                                  VtImageSurfaceView_spans_column(view,
                                                                  display_args.anchor_offset_x - 1))
                            } break;

                            /* Delete all images that intersect a specific row (y=) */
                            case 'y':
                            case 'Y': {
                                L_DELETE_IMG_VIEWS_FILTERED(VtImageSurfaceView_spans_line(
                                  view,
                                  display_args.anchor_offset_y + Vt_top_line(self) - 1))
                            } break;

                            /* Delete all  on given z-layer (z=) */
                            case 'z':
                            case 'Z': {
                                L_DELETE_IMG_VIEWS_FILTERED(view->z_layer == display_args.z_layer)
                            } break;
                        }
                    } break;
                    case VT_IMAGE_PROTO_ACTION_QUERY: {
                        error_string =
                          Vt_img_proto_validate(self, transmission, compression, format);
                        Vt_output_formated(self, "\e_Gi=%u;%s\e\\", id, OR(error_string, "OK"));
                    } break;
                }

            } break;

            default: {
                char* str = pty_string_prettyfy(seq, strlen(seq));
                WRN("Unknown APC: %s\n", str);
                free(str);
            }
        }

        Vector_clear_char(&self->parser.active_sequence);
        self->parser.state = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_DCS(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {

        if (*Vector_last_char(&self->parser.active_sequence) == '\\') {
            Vector_pop_char(&self->parser.active_sequence);
            if (*Vector_last_char(&self->parser.active_sequence) == '\e') {
                Vector_pop_char(&self->parser.active_sequence);
            }
        }

        if (*Vector_last_char(&self->parser.active_sequence) == '\a') {
            Vector_pop_char(&self->parser.active_sequence);
        }

        Vector_push_char(&self->parser.active_sequence, '\0');

        const char*  seq     = self->parser.active_sequence.buf;
        const size_t seq_len = self->parser.active_sequence.size;

        switch (*seq) {
            /* Synchronized update */
            case '=':
                if ((seq[1] == '1' || seq[1] == '2') && seq[2] == 's') {
                    if (seq[1] == '1') {
                        /* Begin synchronized update (BSU) (iTerm2)
                         *
                         * Meant to reduce flicker when redrawing large portions of the screen.
                         * The terminal should create a snapshot of the screen content and display
                         * that until ESU is sent (or some kind of timeout in case the program
                         * is killed/crashes).
                         */

                        // TODO: some way to deal with proxy object updates
                    } else {
                        /* End synchronized update (ESU) (iTerm2) */
                    }
                }
                return;
            default: {
                char *graphic_data, *fst_non_arg = (char*)seq;

                while ((isdigit(*fst_non_arg) || *fst_non_arg == ';')) {
                    ++fst_non_arg;
                }

                if ((graphic_data = strstr(seq, "q")) == fst_non_arg && seq_len > 4) {
                    WRN("sixel graphics support is incomplete and unstable!\n");
                    int32_t pixel_aspect_ratio = 0, p2_param = 0, horizontal_grid_size = 0;
                    bool    zero_pos_retains_color = false;
                    sscanf(seq, "%d;%d;%d", &pixel_aspect_ratio, &p2_param, &horizontal_grid_size);
                    if (pixel_aspect_ratio) {
                        WRN("sixel pixel aspect ratio set via DCS instead of raster attributes "
                            "command\n");
                    }

                    switch (pixel_aspect_ratio) {
                        case 0:
                        case 1:
                        case 5:
                        case 6:
                            pixel_aspect_ratio = 2;
                            break;
                        case 2:
                            pixel_aspect_ratio = 5;
                            break;
                        case 3:
                        case 4:
                            pixel_aspect_ratio = 3;
                            break;
                        case 7:
                        case 8:
                        case 9:
                            pixel_aspect_ratio = 1;
                            break;
                        default:
                            WRN("incorect sixel pixel aspect ratio parameter \'%d\'\n",
                                pixel_aspect_ratio);
                    }

                    if (p2_param == 1) {
                        zero_pos_retains_color = true;
                    }

                    if (horizontal_grid_size) {
                        WRN("sixel horizontal grid size parameter ignored\n");
                    }

                    graphic_color_registers_t private_color_regs;
                    if (self->modes.sixel_private_color_registers) {
                        memset(&private_color_regs, 0, sizeof(private_color_regs));
                    }

                    VtSixelSurface surf = VtSixelSurface_new_from_data(
                      pixel_aspect_ratio,
                      !zero_pos_retains_color,
                      (uint8_t*)graphic_data + 1,
                      self->modes.sixel_private_color_registers
                        ? &private_color_regs
                        : &self->colors.global_graphic_color_registers);

                    if (surf.width && surf.height) {
                        surf.anchor_cell_idx     = self->cursor.col;
                        surf.anchor_global_index = self->cursor.row;

                        Pair_uint32_t cellsize =
                          CALL(self->callbacks.on_window_size_from_cells_requested,
                               self->callbacks.user_data,
                               1,
                               1);

                        for (uint32_t i = 0; i <= (surf.height - 1) / cellsize.second; ++i) {
                            Vt_insert_new_line(self);
                        }

                        if (self->modes.sixel_scrolling) {
                            VtLine* ln = Vt_cursor_line(self);
                            if (!ln->graphic_attachments) {
                                ln->graphic_attachments =
                                  calloc(1, sizeof(VtGraphicLineAttachments));
                            }
                            if (!ln->graphic_attachments->sixels) {
                                ln->graphic_attachments->sixels =
                                  malloc(sizeof(Vector_RcPtr_VtSixelSurface));
                                *ln->graphic_attachments->sixels =
                                  Vector_new_RcPtr_VtSixelSurface();
                            }

                            if (self->modes.sixel_scrolling_move_cursor_right) {
                                self->cursor.col =
                                  MIN((surf.width - 1 / cellsize.first) + 1, Vt_col(self));
                            }

                            RcPtr_VtSixelSurface sp        = RcPtr_new_VtSixelSurface(self);
                            *RcPtr_get_VtSixelSurface(&sp) = surf;
                            RcPtr_VtSixelSurface sp2       = RcPtr_new_shared_VtSixelSurface(&sp);

                            Vector_push_RcPtr_VtSixelSurface(ln->graphic_attachments->sixels, sp);
                            Vector_push_RcPtr_VtSixelSurface(&self->scrolled_sixels, sp2);
                        } else {
                            VtSixelSurface_destroy(self, &surf);
                            // Vector_push_VtSixelSurface(&self->static_sixels, surf);
                        }
                    } else {
                        VtSixelSurface_destroy(self, &surf);
                    }

                } else if (strstr(seq, "p") == fst_non_arg && seq_len > 4) {
                    // TODO: Primary DA Ps = 5 - regis support
                    STUB("ReGIS graphics");
                } else {
                    char* str = pty_string_prettyfy(self->parser.active_sequence.buf,
                                                    self->parser.active_sequence.size);
                    WRN("Unknown DCS: %s\n", str);
                    free(str);
                }
            }
        }

        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_PM(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);
    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {
        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_handle_OSC(Vt* self, char c)
{
    Vector_push_char(&self->parser.active_sequence, c);

    if (is_string_sequence_terminated(self->parser.active_sequence.buf,
                                      self->parser.active_sequence.size)) {

        if (*Vector_last_char(&self->parser.active_sequence) == '\\') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        if (*Vector_last_char(&self->parser.active_sequence) == '\e') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        if (*Vector_last_char(&self->parser.active_sequence) == '\a') {
            Vector_pop_char(&self->parser.active_sequence);
        }
        Vector_push_char(&self->parser.active_sequence, '\0');
        char*    seq  = self->parser.active_sequence.buf;
        uint32_t arg  = 0;
        char*    text = seq;

        if (isdigit(*seq)) {
            arg = strtoul(seq, &text, 10);
            if (text && !(*text == ';' || *text == ':')) {
                text = seq;
            } else if (text && *text) {
                ++text;
            }
        } else {
            WRN("no numerical argument in OSC \'%s\'\n", seq);
        }

        switch (arg) {
            /* Change Icon Name and Window Title */
            case 0:
            /* Change Icon Name */
            case 1:
            /* Change Window Title */
            case 2:
                Vt_set_title(self, text);
                break;

            /* Set X property on top-level window (prop=val) */
            case 3:
                // TODO: CALL(self->callbacs.on_xproperty_set, self->callbacks.user_data, seq +2,
                // strstr(seq+2,"=") + 1);
                WRN("OSC 3 not implemented\n");
                break;

            /* Modify regular color palette
             *
             * Any number of c/spec pairs may be given
             *
             * If a "?" is given rather than a name or RGB specification, xterm replies with a
             * control sequence of the same form which can be used to set the corresponding color.
             */
            case 4: {
                seq += 2;
                char *arg_idx, *arg_clr;
                while ((arg_idx = strsep(&seq, ";")) && (arg_clr = strsep(&seq, ";"))) {
                    uint32_t index = atoi(arg_idx);
                    if (index >= ARRAY_SIZE(self->colors.palette_256)) {
                        continue;
                    }
                    if (*arg_clr == '?') {
                        ColorRGB color = self->colors.palette_256[index];
                        Vt_output_formated(self,
                                           "\e]4;%u;rgb:%x/%x/%x\a",
                                           index,
                                           color.r,
                                           color.g,
                                           color.b);
                    } else {
                        set_rgb_color_from_xterm_string(&self->colors.palette_256[index], arg_clr);
                    }
                }
                Vt_clear_all_proxies(self);
                self->defered_events.repaint = true;
            } break;

            /* 104 ; c Reset Color Number
             * It is reset to the color specified by the corresponding X resource. Any number of
             * c parameters may be given.  These parameters correspond to the ANSI colors 0-7,
             * their bright versions 8-15, and if supported, the remainder of the 88-color or
             * 256-color table. If no parameters are given, the entire table will be reset.
             */
            case 104: {
                seq += 3;
                if (!*seq) {
                    Vt_init_color_palette(self);
                } else {
                    ++seq;
                    for (char* index; (index = strsep(&seq, ";"));) {
                        uint32_t idx = atoi(index);
                        if (idx >= ARRAY_SIZE(self->colors.palette_256)) {
                            continue;
                        }
                        Vt_reset_color_palette_entry(self, idx);
                    }
                }
                Vt_clear_all_proxies(self);
                self->defered_events.repaint = true;
            } break;

            /* Modify special color palette */
            case 5:
            /* Enable/disable special color */
            case 6:

            /* 105 ; c Reset Special Color Number c.
             * It is reset to the color specified by the corresponding X resource. Any number
             * of c parameters may be given.  These parameters correspond to the special
             * colors which can be set using an OSC 5 control (or by adding the maximum number
             * of colors using an OSC 4  control).
             */
            case 105:

            /* 1 0 6 ; c ; f  Enable/disable Special Color Number c.
             * The second parameter tells xterm to enable the corresponding color mode if
             * nonzero, disable it if zero
             */
            case 106:
                // TODO:
                WRN("Special colors not implemented \'%s\'\n", seq);
                break;

            /* pwd info as URI */
            case 7: {
                free(self->work_dir);
                free(self->client_host);
                char* uri = seq + 2; // 7;
                if (streq_glob(uri, "file:*") && strnlen(uri, 8) == 8) {
                    uri += 6; // skip 'file://'
                    char* host = uri;
                    while (*uri && *uri != '/')
                        ++uri;
                    ptrdiff_t s                            = uri - host;
                    self->client_host                      = malloc(s + 1);
                    strncpy(self->client_host, host, s)[s] = '\0';
                    self->work_dir                         = strdup(uri + 1 /* skip second '/' */);
                    LOG("Vt::osc7{ host: %s, pwd: %s }\n", self->client_host, self->work_dir);
                } else {
                    self->work_dir = self->client_host = NULL;
                    WRN("Bad URI \'%s\', scheme is not \'file\'\n", uri);
                }
            } break;

            /* mark text as hyperlink with URL */
            case 8: {
                strsep(&seq, ";");
                strsep(&seq, ";");
                char* link = strsep(&seq, ";");

                if (strnlen(link, 1)) {
                    free(self->active_hyperlink);
                    self->active_hyperlink = strdup(link);
                } else {
                    free(self->active_hyperlink);
                    self->active_hyperlink = NULL;
                }
            } break;

            /* Send Growl(some kind of notification daemon for OSX) notification (iTerm2) */
            case 9:
                CALL(self->callbacks.on_desktop_notification_sent,
                     self->callbacks.user_data,
                     NULL,
                     seq + 2 /* 9; */);
                break;

            /* Set title for tab (konsole extension) */
            case 30:
                break;

            /* Set dynamic colors for xterm colorOps
             *
             * If a "?" is given rather than a name or RGB specification, xterm replies with a
             * control sequence of the same form which can be used to set the corresponding
             * dynamic color.
             */
            case 10 ... 19: {
                bool query = *(seq + 3) == '?';
                if (query) {
                    switch (arg) {
                        /* VT100 text foreground color */
                        case 10: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%x/%x/%x\e\\",
                                               arg,
                                               self->colors.fg.r,
                                               self->colors.fg.g,
                                               self->colors.fg.b);
                        } break;

                        /* VT100 text background color */
                        case 11: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%x/%x/%x\e\\",
                                               arg,
                                               self->colors.bg.r,
                                               self->colors.bg.g,
                                               self->colors.bg.b);
                        } break;

                        /* highlight background color */
                        case 17: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%3u/%3u/%3u\e\\",
                                               arg,
                                               self->colors.highlight.bg.r,
                                               self->colors.highlight.bg.g,
                                               self->colors.highlight.bg.b);
                        } break;

                        /* highlight foreground color */
                        case 19: {
                            Vt_output_formated(self,
                                               "\e]%u;rgb:%3u/%3u/%3u\e\\",
                                               arg,
                                               self->colors.highlight.fg.r,
                                               self->colors.highlight.fg.g,
                                               self->colors.highlight.fg.b);
                        } break;

                        /* Tektronix background color */
                        case 16:
                        /* pointer foreground color */
                        case 13:
                        /* pointer background color */
                        case 14:
                        /* text cursor color */
                        case 12:
                        /* Tektronix foreground color */
                        case 15:
                        /* Tektronix cursor color */
                        case 18:
                            WRN("Unimplemented color \'%d\'\n", arg);
                            break;

                        default:
                            ASSERT_UNREACHABLE;
                            break;
                    }
                } else {
                    /*
                     * At least one parameter is expected. Each successive parameter changes the
                     * next color in the list.
                     */

                    char* sequence_arg = seq + 3;
                    while (sequence_arg && *sequence_arg) {
                        switch (arg) {
                            /* VT100 text foreground color */
                            case 10: {
                                set_rgb_color_from_xterm_string(&self->colors.fg, sequence_arg);
                            } break;

                            /* VT100 text background color */
                            case 11: {
                                set_rgba_color_from_xterm_string(&self->colors.bg, sequence_arg);
                            } break;

                            /* highlight background color */
                            case 17: {
                                set_rgba_color_from_xterm_string(&self->colors.highlight.bg,
                                                                 sequence_arg);
                            } break;

                            /* highlight foreground color */
                            case 19: {
                                set_rgb_color_from_xterm_string(&self->colors.highlight.fg,
                                                                sequence_arg);
                            } break;

                            /* Tektronix background color */
                            case 16:
                            /* text cursor color */
                            case 12:
                            /* Tektronix foreground color */
                            case 15:
                            /* Tektronix cursor color */
                            case 18:
                            /* pointer foreground color */
                            case 13:
                            /* pointer background color */
                            case 14:
                            default:
                                break;
                        }
                        sequence_arg = strstr(sequence_arg, ";");
                        if (!sequence_arg || !*sequence_arg) {
                            break;
                        }
                        ++sequence_arg;
                        ++arg;
                    }
                    Vt_clear_all_proxies(self);
                    self->defered_events.repaint = true;
                }
            } break;

            /* Coresponding resets */
            case 110 ... 119: {
                switch (arg - 100) {
                    /* VT100 text foreground color */
                    case 10:
                    /* text cursor color */
                    case 12:
                    /* Tektronix foreground color */
                    case 15:
                    /* Tektronix cursor color */
                    case 18: {
                        self->colors.fg = settings.fg;
                    } break;

                    /* VT100 text background color */
                    case 11:
                    /* Tektronix background color */
                    case 16: {
                        self->colors.bg = settings.bg;
                    } break;

                    /* pointer foreground color */
                    case 13:
                        break;

                    /* pointer background color */
                    case 14:
                        break;

                    /* highlight background color */
                    case 17: {
                        self->colors.highlight.bg = settings.bghl;
                    } break;

                    /* highlight foreground color */
                    case 19: {
                        self->colors.highlight.fg = settings.fghl;
                    } break;

                    default:
                        ASSERT_UNREACHABLE;
                        break;
                }
            } break;

            case 50:
                WRN("xterm fontOps not implemented\n");
                break;

            /* Manipulate selection data */
            case 52:
                WRN("Selection manipulation not implemented\n");
                break;

            /* Shell integration mark (FinalTerm/iTerm2)
             * https://iterm2.com/documentation-shell-integration.html
             *
             * [PROMPT]prompt% [COMMAND_START] ls -l
             * [COMMAND_EXECUTED]
             * -rw-r--r-- 1 user group 127 May 1 2016 filename
             * [COMMAND_FINISHED]
             */
            case 133: {

                if (Vt_alt_buffer_enabled(self)) {
                    break;
                }

                switch (seq[4] /* 113;? */) {
                    /* PROMPT */
                    case 'A':
                        Vt_shell_integration_begin_prompt(self);
                        break;

                    /* COMMAND_START */
                    case 'B':
                        Vt_shell_integration_begin_command(self);
                        break;

                    /* COMMAND_EXECUTED */
                    case 'C':
                        Vt_shell_integration_begin_execution(self, false, false);
                        break;

                    /* COMMAND_FINISHED */
                    case 'D':
                        Vt_shell_integration_end_execution(
                          self,
                          strnlen(seq, 6) >= 6 ? seq + 6 : NULL /* 133;D; */);
                        break;

                    default:
                        WRN("Invalid shell integration command\n");
                }
            } break;

            /* Shell integration command (iTerm2)*/
            case 1337: {
                for (char* a; (a = strsep(&seq, ";"));) {
                    if (strstr(a, "ShellIntegrationVersion")) {
                        char* tmp = strstr(a, "=") + 1;
                        if (tmp) {
                            self->shell_integration_protocol_version = atoi(tmp);
                        }
                    } else if (strstr(a, "RemoteHost")) {
                        char* tmp = strstr(a, "=") + 1;
                        if (tmp) {
                            free(self->shell_integration_shell_host);
                            self->shell_integration_shell_host = strdup(tmp);
                        }
                    } else if (strstr(a, "shell")) {
                        char* tmp = strstr(a, "=") + 1;
                        if (tmp) {
                            free(self->shell_integration_shell_id);
                            self->shell_integration_shell_id = strdup(tmp);
                        }
                    } else if (strstr(a, "CurrentDir")) {
                        char* tmp = strstr(a, "=") + 1;
                        if (tmp) {
                            free(self->shell_integration_current_dir);
                            self->shell_integration_current_dir = strdup(tmp);
                        }
                    } else if (strstr(a, "ClearScrollback")) {
                        Vt_clear_scrollback(self);
                    } else if (strstr(a, "SetMark")) {
                        Vt_cursor_line(self)->mark_explicit = true;
                    } else if (strstr(a, "RequestAttention")) {
                        CALL(self->callbacks.on_urgency_set, self->callbacks.user_data);
                    } else if (strstr(a, "StealFocus")) {
                        CALL(self->callbacks.on_restack_to_front, self->callbacks.user_data);
                    }
                    // TODO: ReportCellSize
                    // TODO: Copy
                    // TODO: CopyToClipboard
                    // TODO: EndCopy
                }
            } break;

            /* Send desktop notification (rxvt)
             *     OSC 777;notify;title;body ST
             *
             * or command integration notification sequence (VTE)
             *     OSC 777;precmd ST [user@host:~] $ ls -l OSC 777;preexec ST
             *     total 1
             *     drwxr-xr-x  6 user user  4096 Dec 12 15:37 Stuff
             *     OSC 777;notify;Command completed;ls -l ST
             *
             *     Ending 'notify' should only send the notification when the terminal is minimized.
             *     We need to check if the actively running command was inited by VTE sequences, so
             *     we don't break normal OSC 777 behaviour.
             *     There is no distinction between the prompt and command invocation, so we don't
             *     know what we're running until it completes.
             * */
            case 777: {
                Vector_Vector_char tokens = string_split_on(seq + 4 /* 777; */, ";", NULL, NULL);
                if (tokens.size >= 2) {
                    if (!strcmp(tokens.buf[0].buf + 1, "notify")) {
                        if (tokens.size == 2) {
                            CALL(self->callbacks.on_desktop_notification_sent,
                                 self->callbacks.user_data,
                                 NULL,
                                 tokens.buf[1].buf + 1);
                        } else if (tokens.size == 3) {
                            VtCommand* cmd = Vt_shell_integration_get_active_command(self);
                            if (cmd && cmd->is_vte_protocol &&
                                !strcmp(tokens.buf[1].buf + 1, "Command completed")) {
                                Vt_shell_integration_active_command_name_changed(self,
                                                                                 tokens.buf[2].buf +
                                                                                   1);
                                Vt_shell_integration_end_execution(self, NULL);
                            } else {
                                CALL(self->callbacks.on_desktop_notification_sent,
                                     self->callbacks.user_data,
                                     tokens.buf[1].buf + 1,
                                     tokens.buf[2].buf + 1);
                            }
                        } else {
                            WRN("Unexpected argument in OSC 777 \'%s\'\n", seq);
                        }
                    } else {
                        WRN("Second argument to OSC 777 \'%s\' is not recognized\n", seq);
                    }
                } else {
                    if (tokens.size) {
                        if (!strcmp(tokens.buf[0].buf + 1, "precmd")) {
                            Vt_shell_integration_begin_prompt(self);
                            Vt_shell_integration_begin_command(self);
                        } else if (!strcmp(tokens.buf[0].buf + 1, "preexec")) {
                            Vt_shell_integration_begin_execution(self, true, true);
                        } else {
                            WRN("OSC 777 \'%s\' unknown argument\n", seq);
                        }
                    }
                    Vector_destroy_Vector_char(&tokens);
                }
            } break;

            default:
                WRN("Unknown OSC: %s\n", self->parser.active_sequence.buf);
        }
        Vector_destroy_char(&self->parser.active_sequence);
        self->parser.active_sequence = Vector_new_char();
        self->parser.state           = PARSER_STATE_LITERAL;
    }
}

static void Vt_push_title(Vt* self)
{
    if (self->title) {
        Vector_push_DynStr(&self->title_stack, (DynStr){ .s = strdup(self->title) });
    }
}

static void Vt_pop_title(Vt* self)
{
    if (self->title_stack.size) {
        Vt_set_title(self, Vector_last_DynStr(&self->title_stack)->s);
        Vector_pop_DynStr(&self->title_stack);
    } else {
        free(self->title);
        self->title = NULL;
    }
}

static void Vt_reset_text_attribs(Vt* self, VtRune* opt_target)
{
    VtRune* r       = OR(opt_target, &self->parser.char_state);
    Rune    oldrune = r->rune;
    memset(r, 0, sizeof(self->parser.char_state));
    r->rune       = oldrune;
    r->rune.style = VT_RUNE_NORMAL;
    Vt_set_bg_color_default(self, opt_target);
    Vt_set_fg_color_default(self, opt_target);
    Vt_set_line_color_default(self, opt_target);
}

/**
 * Move cursor to first column */
static void Vt_carriage_return(Vt* self)
{
    self->has_last_inserted_rune = false;
    Vt_move_cursor(self, 0, Vt_cursor_row(self));
}

/**
 * make a new empty line at cursor position, scroll down contents below */
static void Vt_insert_line(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    self->has_last_inserted_rune = false;
    Vector_insert_at_VtLine(&self->lines, self->cursor.row, VtLine_new());
    Vt_shift_global_line_index_refs(self, self->cursor.row, 1, true);

    Vt_empty_line_fill_bg(self, self->cursor.row);

    size_t rem_idx = MIN(Vt_get_scroll_region_bottom(self), Vt_bottom_line(self));
    Vt_about_to_delete_line_by_scroll_down(self, rem_idx);
    Vector_remove_at_VtLine(&self->lines, rem_idx, 1);
    Vt_shift_global_line_index_refs(self, rem_idx + 1, -1, true);

    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

/**
 * the same as insert line, but adds before cursor line */
static void Vt_reverse_line_feed(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    self->has_last_inserted_rune = false;
    if (self->cursor.row == Vt_get_scroll_region_top(self)) {
        Vt_about_to_delete_line_by_scroll_down(self, Vt_get_scroll_region_bottom(self));
        Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_bottom(self), 1);
        Vt_shift_global_line_index_refs(self, Vt_get_scroll_region_bottom(self) + 1, -1, true);

        Vector_insert_at_VtLine(&self->lines, self->cursor.row, VtLine_new());
        Vt_shift_global_line_index_refs(self, self->cursor.row, 1, true);

        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else if (Vt_cursor_row(self)) {
        Vt_move_cursor(self, self->cursor.col, Vt_cursor_row(self) - 1);
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * delete active line, content below scrolls up */
static void Vt_delete_line(Vt* self)
{
    self->has_last_inserted_rune = false;

    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    size_t insert_idx = MIN(Vt_get_scroll_region_bottom(self), Vt_bottom_line(self)) + 1;
    size_t remove_idx = self->cursor.row;

    Vector_insert_at_VtLine(&self->lines, insert_idx, VtLine_new());
    Vt_shift_global_line_index_refs(self, insert_idx, 1, true);
    Vt_empty_line_fill_bg(self, insert_idx);

    Vt_about_to_delete_line_by_scroll_up(self, remove_idx);
    Vector_remove_at_VtLine(&self->lines, remove_idx, 1);
    Vt_shift_global_line_index_refs(self, remove_idx + 1, -1, true);
}

static void Vt_scroll_up(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    self->has_last_inserted_rune = false;
    size_t insert_idx            = MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self)) + 1;
    Vector_insert_at_VtLine(&self->lines, insert_idx, VtLine_new());
    Vt_shift_global_line_index_refs(self, insert_idx, 1, true);

    size_t new_line_idx = MIN(Vt_bottom_line(self), Vt_get_scroll_region_bottom(self));
    Vt_empty_line_fill_bg(self, new_line_idx);

    Vt_about_to_delete_line_by_scroll_up(self, Vt_get_scroll_region_top(self) - 1);
    Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_top(self) - 1, 1);
    Vt_shift_global_line_index_refs(self, Vt_get_scroll_region_top(self) - 1 + 1, -1, true);
    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

static void Vt_scroll_down(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    self->has_last_inserted_rune = false;
    Vector_insert_at_VtLine(&self->lines, Vt_get_scroll_region_top(self), VtLine_new());
    Vt_shift_global_line_index_refs(self, Vt_get_scroll_region_top(self), 1, true);

    size_t rm_idx = MAX(Vt_top_line(self), Vt_get_scroll_region_bottom(self));
    Vt_about_to_delete_line_by_scroll_down(self, rm_idx);
    Vector_remove_at_VtLine(&self->lines, rm_idx, 1);
    Vt_shift_global_line_index_refs(self, rm_idx + 1, -1, true);

    Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
}

static inline void Vt_erase_to_end(Vt* self)
{
    for (size_t i = self->cursor.row + 1; i <= Vt_bottom_line(self); ++i) {
        Vector_clear_VtRune(&self->lines.buf[i].data);
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_right(self);
    Vt_clear_proxies_in_region(self, self->cursor.row, Vt_bottom_line(self));
}

static inline void Vt_handle_backspace(Vt* self)
{
    if (self->cursor.col)
        Vt_move_cursor(self, self->cursor.col - 1, Vt_cursor_row(self));
    else if (self->modes.reverse_wraparound) {
        uint16_t r = Vt_cursor_row(self);
        Vt_move_cursor(self, Vt_col(self) - 1, r ? (r - 1) : 0);
    }
}

/**
 * Overwrite characters with colored space */
static inline void Vt_erase_chars(Vt* self, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        size_t idx = self->cursor.col + i;
        if (idx >= Vt_cursor_line(self)->data.size) {
            Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
        } else {
            Vt_cursor_line(self)->data.buf[idx] = self->parser.char_state;
        }
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * remove characters at cursor, remaining content scrolls left */
static void Vt_delete_chars(Vt* self, size_t n)
{
    /* Trim if line is longer than screen area */
    if (Vt_cursor_line(self)->data.size > Vt_col(self)) {
        Vector_pop_n_VtRune(&Vt_cursor_line(self)->data,
                            Vt_cursor_line(self)->data.size - Vt_col(self));
    }

    size_t rm_size = Vt_cursor_line(self)->data.size == self->cursor.col
                       ? Vt_cursor_line(self)->data.size - self->cursor.col
                       : Vt_cursor_line(self)->data.size;
    Vector_remove_at_VtRune(&Vt_cursor_line(self)->data, self->cursor.col, MIN(rm_size, n));

    /* Fill line to the cursor position with spaces with original propreties
     * before scolling so we get the expected result, when we... */
    VtRune tmp = self->parser.char_state;

    Vt_reset_text_attribs(self, NULL);

    if (Vt_cursor_line(self)->data.size >= 2) {
        self->parser.char_state.bg_data =
          Vt_cursor_line(self)->data.buf[Vt_cursor_line(self)->data.size - 2].bg_data;

        self->parser.char_state.bg_is_palette_entry =
          Vt_cursor_line(self)->data.buf[Vt_cursor_line(self)->data.size - 2].bg_is_palette_entry;
    } else {
        Vt_set_bg_color_default(self, NULL);
    }

    uint16_t st = Vt_cursor_line(self)->data.size ? Vt_cursor_line(self)->data.size - 1 : 0;
    for (size_t i = st; i < Vt_col(self); ++i) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
    }

    self->parser.char_state = tmp;

    if (Vt_cursor_line(self)->data.size > Vt_col(self)) {
        Vector_pop_n_VtRune(&Vt_cursor_line(self)->data,
                            Vt_cursor_line(self)->data.size - Vt_col(self));
    }

    /* ...add n spaces with currently set attributes to the end (right margin) */
    for (uint16_t i = 0; i < n && self->cursor.col + i < self->scroll_region_right + 1; ++i) {
        if (i == Vt_cursor_line(self)->data.size) {
            Vector_push_VtRune(&Vt_cursor_line(self)->data, self->parser.char_state);
        } else {
            Vector_insert_at_VtRune(&Vt_cursor_line(self)->data,
                                    self->scroll_region_right,
                                    self->parser.char_state);
        }
    }

    /* Trim to screen size again */
    if (self->lines.buf[self->cursor.row].data.size > Vt_col(self)) {
        Vector_pop_n_VtRune(&Vt_cursor_line(self)->data,
                            Vt_cursor_line(self)->data.size - Vt_col(self));
    }
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Get the number of used lines counting from the top of the viewport and add blanks to scroll them
 * out. */
static inline void Vt_scroll_out_all_content(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    size_t to_add = Vt_top_line(self);
    for (size_t i = Vt_bottom_line(self); i >= Vt_top_line(self); --i) {
        if (self->lines.buf[i].data.size) {
            to_add = i + 1;
            break;
        }
    }
    to_add -= Vt_top_line(self);

    for (size_t i = 0; i < to_add; ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }
    if (to_add > 0) {
        self->cursor.row += to_add;
    }
}

static inline void Vt_scroll_out_above(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    size_t to_add = Vt_cursor_row(self);
    for (size_t i = 0; i <= to_add; ++i) {
        size_t insert_point = self->cursor.row;
        Vector_insert_at_VtLine(&self->lines, insert_point, VtLine_new());
        Vt_empty_line_fill_bg(self, insert_point);
        ++self->cursor.row;
    }
}

static inline void Vt_clear_above(Vt* self)
{
    for (size_t i = Vt_visual_top_line(self); i < self->cursor.row; ++i) {
        self->lines.buf[i].data.size = 0;
        Vt_empty_line_fill_bg(self, i);
    }
    Vt_clear_left(self);
}

static inline void Vt_clear_display_and_scrollback(Vt* self)
{
    Vt_visual_scroll_reset(self);
    Vector_destroy_VtLine(&self->lines);
    self->lines = Vector_new_VtLine(self);

    for (uint16_t i = 0; i < Vt_row(self); ++i) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }
    self->cursor.row = 0;

    Vector_clear_RcPtr_VtSixelSurface(&self->scrolled_sixels);
    Vector_clear_RcPtr_VtImageSurfaceView(&self->image_views);
    Vector_clear_RcPtr_VtImageSurface(&self->images);
}

/**
 * Clear active line left of cursor and fill it with whatever character
 * attributes are set */
static inline void Vt_clear_left(Vt* self)
{
    if (self->cursor.col >= Vt_cursor_line(self)->data.size) {
        Vector_reserve_VtRune(&Vt_cursor_line(self)->data, self->cursor.col + 1);
        Vt_cursor_line(self)->data.size = MAX(Vt_cursor_line(self)->data.size, self->cursor.col);
    }

    for (size_t i = 0; i <= self->cursor.col; ++i) {
        Vt_cursor_line(self)->data.buf[i] = self->parser.char_state;
    }

    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Clear active line right of cursor and fill it with whatever character
 * attributes are set */
static inline void Vt_clear_right(Vt* self)
{
    Vector_reserve_VtRune(&Vt_cursor_line(self)->data, Vt_col(self));
    Vt_cursor_line(self)->data.size = Vt_col(self);

    for (uint16_t i = self->cursor.col; i < Vt_col(self); ++i) {
        Vt_cursor_line(self)->data.buf[i] = self->parser.char_state;
    }

    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static void Vt_overwrite_char_at(Vt* self, size_t column, size_t row, VtRune c)
{
    VtLine* line = Vt_line_at(self, row);
    while (line->data.size <= column) {
        Vector_push_VtRune(&line->data, self->blank_space);
    }
    line->data.buf[column] = c;
}

/**
 * Insert character literal at cursor position, deal with reaching column limit */
__attribute__((hot)) static void Vt_insert_char_at_cursor(Vt* self, VtRune c)
{
    self->defered_events.repaint = true;

    if (self->wrap_next && !self->modes.no_wraparound) {
        self->cursor.col                  = 0;
        Vt_cursor_line(self)->was_reflown = true;
        Vt_insert_new_line(self);
        Vt_cursor_line(self)->rejoinable = true;
    }

    while (self->lines.size <= self->cursor.row) {
        Vector_push_VtLine(&self->lines, VtLine_new());
    }

    while (Vt_cursor_line(self)->data.size <= self->cursor.col) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->blank_space);
    }

    VtRune* insert_point = &self->lines.buf[self->cursor.row].data.buf[self->cursor.col];

    if (likely(memcmp(insert_point, &c, sizeof(VtRune)))) {

        if (self->modes.no_insert_replace_mode) {
            // TODO: Vt_mark_proxy_damaged_shift()
            Vt_mark_proxy_fully_damaged(self, self->cursor.row);
            Vector_insert_at_VtRune(&Vt_cursor_line(self)->data, self->cursor.col, c);
            if (Vt_cursor_line(self)->data.size >= Vt_col(self)) {
                Vector_pop_VtRune(&Vt_cursor_line(self)->data);
            }
        } else {
            Vt_mark_proxy_damaged_cell(self, self->cursor.row, self->cursor.col);
            *Vt_cursor_cell(self) = c;
        }
    }

    self->last_inserted = *Vt_cursor_cell(self);
    ++self->cursor.col;

    int width = C_WIDTH(c.rune.code);

    if (unlikely(width > 1)) {
        VtRune tmp    = c;
        tmp.rune.code = VT_RUNE_CODE_WIDE_TAIL;

        for (int i = 0; i < (width - 1); ++i) {
            if (Vt_cursor_line(self)->data.size <= self->cursor.col) {
                Vector_push_VtRune(&Vt_cursor_line(self)->data, tmp);
            } else {
                if (self->modes.no_insert_replace_mode) {
                    Vector_insert_at_VtRune(&Vt_cursor_line(self)->data, self->cursor.col, tmp);
                } else {
                    *Vt_cursor_cell(self) = tmp;
                }
            }

            ++self->cursor.col;

            if (self->modes.no_insert_replace_mode) {
                Vt_mark_proxy_fully_damaged(self, self->cursor.row);
            } else {
                Vt_mark_proxy_damaged_cell(self, self->cursor.row, self->cursor.col);
            }
        }
    } else if (unlikely(unicode_is_ambiguous_width(c.rune.code))) {
        Vector_push_VtRune(&Vt_cursor_line(self)->data, self->blank_space);
        Vt_mark_proxy_damaged_cell(self, self->cursor.row, self->cursor.col + 1);
    }

    self->wrap_next  = self->cursor.col >= (size_t)Vt_col(self);
    self->cursor.col = MIN(self->cursor.col, (Vt_col(self) - 1));
}

static void Vt_insert_char_at_cursor_with_shift(Vt* self, VtRune c)
{
    if (unlikely(self->cursor.col >= (size_t)Vt_col(self))) {
        if (unlikely(self->modes.no_wraparound)) {
            --self->cursor.col;
        } else {
            self->cursor.col = 0;
            Vt_insert_new_line(self);
            Vt_cursor_line(self)->rejoinable = true;
        }
    }

    if (self->scroll_region_right != Vt_col(self) - 1 &&
        self->scroll_region_right > self->cursor.col) {
        Vector_remove_at_VtRune(&Vt_cursor_line(self)->data, self->scroll_region_right, 1);
    }

    Vector_insert_at_VtRune(&Vt_cursor_line(self)->data, self->cursor.col, c);

    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

static inline void Vt_empty_line_fill_bg(Vt* self, size_t idx)
{
    ASSERT(self->lines.buf[idx].data.size == 0, "line is empty");

    Vt_mark_proxy_fully_damaged(self, idx);
    if (!ColorRGBA_eq(Vt_active_bg_color(self), self->colors.bg)) {
        for (uint16_t i = 0; i < Vt_col(self); ++i) {
            Vector_push_VtRune(&self->lines.buf[idx].data, self->parser.char_state);
        }
    }
}

/**
 * Move one line down or insert a new one, scrolls if region is set */
static inline void Vt_insert_new_line(Vt* self)
{
    if (unlikely(self->selection.mode)) {
        Vt_mark_proxies_damaged_in_selected_region_and_scroll_region(self);
    }

    if (self->cursor.row == Vt_get_scroll_region_bottom(self) &&
        Vt_scroll_region_not_default(self)) {
        Vt_about_to_delete_line_by_scroll_up(self, Vt_get_scroll_region_top(self));
        Vector_remove_at_VtLine(&self->lines, Vt_get_scroll_region_top(self), 1);
        Vt_shift_global_line_index_refs(self, Vt_get_scroll_region_top(self) + 1, -1, true);

        Vector_insert_at_VtLine(&self->lines, self->cursor.row, VtLine_new());
        Vt_shift_global_line_index_refs(self, self->cursor.row, 1, true);

        Vt_empty_line_fill_bg(self, self->cursor.row);
    } else if (Vt_bottom_line(self) == self->cursor.row) {
        Vector_push_VtLine(&self->lines, VtLine_new());
        Vt_empty_line_fill_bg(self, self->lines.size - 1);
    }

    Vt_move_cursor(self, self->cursor.col, Vt_cursor_row(self) + 1);
    Vt_mark_proxy_fully_damaged(self, self->cursor.row);
}

/**
 * Move cursor to given location (@param rows is relative to the screen!) */
static inline void Vt_move_cursor(Vt* self, uint16_t column, uint16_t rows)
{
    self->wrap_next = false;
    size_t max_row, min_row;
    if (self->modes.origin) {
        rows += (Vt_get_scroll_region_top(self) - Vt_top_line(self));
        min_row = Vt_get_scroll_region_top(self);
        max_row = Vt_get_scroll_region_bottom(self);
    } else {
        max_row = Vt_bottom_line(self);
        min_row = Vt_top_line(self);
    }

    self->cursor.row = CLAMP(rows + Vt_top_line(self), min_row, max_row);
    self->cursor.col = MIN(column, (uint32_t)Vt_col(self) - 1);

    VtCommand*       cmd;
    RcPtr_VtCommand* cmd_ptr;
    if (self->shell_integration_state >= VT_SHELL_INTEG_STATE_COMMAND &&
        (cmd_ptr = Vector_last_RcPtr_VtCommand(&self->shell_commands)) &&
        (cmd = RcPtr_get_VtCommand(cmd_ptr))) {

        if (self->shell_integration_state == VT_SHELL_INTEG_STATE_OUTPUT &&
            self->cursor.row < cmd->output_rows.first) {
            Vt_command_output_interrupted(self);
        } else if (self->cursor.row < cmd->command_start_row) {
            Vt_command_output_interrupted(self);
        }
    }

    self->defered_events.repaint = true;
}

/**
 * Add a character as a combining character for that rune */
static void VtRune_push_combining(VtRune* self, char32_t codepoint)
{
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(self->rune.combine); ++i) {
        if (!self->rune.combine[i]) {
            self->rune.combine[i] = codepoint;
            return;
        }
    }
    WRN("Combining character limit (%zu) exceeded\n", ARRAY_SIZE(self->rune.combine));
}

/**
 * Try to interpret a combining character as an SGR property
 * @return was succesfull  */
static bool VtRune_try_normalize_as_property(VtRune* self, char32_t codepoint)
{
    if (!self->line_color_not_default) {
        switch (codepoint) {
            case 0x00001AB6: /* COMBINING WIGGLY LINE BELOW */
                self->curlyunderline = true;
                return true;
            case 0x00000332: /* COMBINING UNDERLINE */
                self->underlined = true;
                return true;
            case 0x00000333: /* COMBINING DOUBLE LOW LINE */
                self->doubleunderline = true;
                return true;
            case 0x00000305: /* COMBINING OVERLINE */
                self->overline = true;
                return true;
            case 0x00000336: /* COMBINING LONG STROKE OVERLAY */
                self->strikethrough = true;
                return true;
        }
    }
    return false;
}

/**
 * Try to do something about combinable characters */
static void Vt_handle_combinable(Vt* self, char32_t c)
{
    if (self->has_last_inserted_rune) {
        if (VtRune_try_normalize_as_property(&self->last_inserted, c)) {
            return;
        }
#ifndef NOUTF8PROC
        if (self->last_inserted.rune.combine[0]) {
#endif
            /* Already contains a combining char that failed to normalize */
            VtRune_push_combining(&self->last_inserted, c);
#ifndef NOUTF8PROC
        } else {
            mbstate_t mbs;
            memset(&mbs, 0, sizeof(mbs));
            char   buff[MB_CUR_MAX * 2 + 1];
            size_t oft = c32rtomb(buff, self->last_inserted.rune.code, &mbs);
            buff[c32rtomb(buff + oft, c, &mbs) + oft] = 0;
            size_t   old_len                          = strlen(buff);
            char*    res     = (char*)utf8proc_NFC((const utf8proc_uint8_t*)buff);
            char32_t conv[2] = { 0, 0 };

            if (res && old_len == strnlen(res, old_len + 1)) {
                VtRune_push_combining(&self->last_inserted, c);
            } else if (mbrtoc32(conv, res, ARRAY_SIZE(buff) - 1, &mbs) < 1) {
                /* conversion failed */
                WRN("Unicode normalization failed %s\n", strerror(errno));
                Vt_grapheme_break(self);
            } else {
                LOG("Vt::unicode{ u+%x + u+%x -> u+%x }\n",
                    self->last_inserted.rune.code,
                    c,
                    conv[0]);

                self->last_inserted.rune.code = conv[0];
                self->last_codepoint          = conv[0];
            }
            free(res);
        }
#endif
    } else {
        WRN("Got combining character, but no previous character is recorded\n");
    }
}

__attribute__((hot, flatten)) void Vt_handle_literal(Vt* self, char c)
{
    if (self->parser.in_mb_seq) {
        char32_t res;
        size_t   rd = mbrtoc32(&res, &c, 1, &self->parser.input_mbstate);

        if (unlikely(rd == (size_t)-1)) {
            /* encoding error */
            WRN("%s\n", strerror(errno));
            errno = 0;

            /* zero-initialized mbstate_t always represents the initial conversion state */
            memset(&self->parser.input_mbstate, 0, sizeof(mbstate_t));
        } else if (rd != (size_t)-2) {
            /* sequence is complete */
            self->parser.in_mb_seq = false;

            bool is_combining = false;
#ifndef NOUTF8PROC
            if (self->last_codepoint) {
                is_combining = !utf8proc_grapheme_break_stateful(self->last_codepoint,
                                                                 res,
                                                                 &self->utf8proc_state) &&
                               utf8proc_charwidth(res) == 0;
            } else {
                is_combining = unicode_is_combining(res);
            }
#else
            is_combining = unicode_is_combining(res);
#endif

            Vt_uri_next_char(self, res);

            if (unlikely(is_combining)) {
                Vt_handle_combinable(self, res);
                self->last_codepoint = res;
            } else {
                VtRune new_rune      = self->parser.char_state;
                self->last_codepoint = res;
                new_rune.rune.code   = res;
                if (self->active_hyperlink) {
                    new_rune.hyperlink_idx =
                      VtLine_add_link(Vt_cursor_line(self), self->active_hyperlink) + 1;
                }
                Vt_insert_char_at_cursor(self, new_rune);
            }
        }
    } else {
        switch (c) {
            case '\a':
                Vt_grapheme_break(self);
                Vt_bell(self);
                break;

            case '\b':
                Vt_grapheme_break(self);
                Vt_uri_break_match(self);
                Vt_handle_backspace(self);
                break;

            case '\r':
                Vt_grapheme_break(self);
                Vt_uri_break_match(self);
                Vt_carriage_return(self);
                break;

            case '\f':
            case '\v':
            case '\n':
                Vt_grapheme_break(self);
                Vt_uri_break_match(self);
                if (self->modes.new_line_mode) {
                    Vt_carriage_return(self);
                }
                Vt_insert_new_line(self);
                break;

            case '\e':
                Vt_uri_break_match(self);
                Vt_grapheme_break(self);
                self->parser.state = PARSER_STATE_ESCAPED;
                break;

            /* Invoke the G1 character set as GL */
            case 14 /* SO */:
                Vt_grapheme_break(self);
                self->charset_gl = &self->charset_g1;
                break;

            /* Invoke the G0 character set (the default) as GL */
            case 15 /* SI */:
                Vt_grapheme_break(self);
                self->charset_gl = &self->charset_g0;
                break;

            case '\t': {
                Vt_grapheme_break(self);
                Vt_uri_break_match(self);
                uint16_t rt;
                for (rt = 0; self->cursor.col + rt + 1 < Vt_col(self);) {
                    if (self->tab_ruler[self->cursor.col + ++rt])
                        break;
                }
                Vt_move_cursor(self, self->cursor.col + rt, Vt_cursor_row(self));
            } break;

            default: {
                // TODO: ISO 8859-1 charset (not UTF-8 mode)
                if (c & (1 << 7)) {
                    mbrtoc32(NULL, &c, 1, &self->parser.input_mbstate);
                    self->parser.in_mb_seq = true;
                    break;
                }

                VtRune new_rune    = self->parser.char_state;
                new_rune.rune.code = c;

                if (self->active_hyperlink) {
                    new_rune.hyperlink_idx =
                      VtLine_add_link(Vt_cursor_line(self), self->active_hyperlink) + 1;
                }

                if (unlikely(self->charset_single_shift && *self->charset_single_shift)) {
                    new_rune.rune.code         = (*(self->charset_single_shift))(c);
                    self->charset_single_shift = NULL;
                } else if (unlikely(self->charset_gl && (*self->charset_gl))) {
                    new_rune.rune.code = (*(self->charset_gl))(c);
                }

                self->last_codepoint = new_rune.rune.code;
                Vt_uri_next_char(self, new_rune.rune.code);
                Vt_insert_char_at_cursor(self, new_rune);
            }
        }
    }
}

__attribute__((always_inline, hot)) static inline void Vt_handle_char(Vt* self, char c)
{
    switch (expect(self->parser.state, PARSER_STATE_LITERAL)) {

        case PARSER_STATE_LITERAL:
            Vt_handle_literal(self, c);
            break;

        case PARSER_STATE_CSI:
            switch (c) {
                case '\a':
                    Vt_bell(self);
                    break;
                case '\b':
                    Vt_handle_backspace(self);
                    break;
                case '\r':
                    Vt_carriage_return(self);
                    break;
                case '\f':
                case '\v':
                case '\n':
                    Vt_insert_new_line(self);
                    if (self->modes.new_line_mode) {
                        Vt_carriage_return(self);
                    }
                    break;
                default:
                    Vt_handle_CSI(self, c);
            }
            break;

        case PARSER_STATE_ESCAPED:
            switch (expect(c, '[')) {

                /* Control sequence introduce (CSI) */
                case '[':
                    self->parser.state = PARSER_STATE_CSI;
                    return;

                /* Operating system command (OSC) */
                case ']':
                    self->parser.state = PARSER_STATE_OSC;
                    return;

                /* Device control */
                case 'P':
                    self->parser.state = PARSER_STATE_DCS;
                    return;

                /* Application Programming Command (APC) */
                case '_':
                    self->parser.state = PARSER_STATE_APC;
                    return;

                /* Reverse line feed (RI) */
                case 'M':
                    Vt_reverse_line_feed(self);
                    self->parser.state = PARSER_STATE_LITERAL;
                    return;

                /* New line (NEL) */
                case 'E':
                    Vt_carriage_return(self);
                    /* fallthrough */

                /* Line feed (IND) */
                case 'D':
                    Vt_insert_new_line(self);
                    self->parser.state = PARSER_STATE_LITERAL;
                    return;

                case '#':
                    self->parser.state = PARSER_STATE_DEC_SPECIAL;
                    return;

                /* Set tab stop at current column (HTS) */
                case 'H':
                    self->tab_ruler[self->cursor.col] = true;
                    self->parser.state                = PARSER_STATE_LITERAL;
                    return;

                /* set primary charset G0 */
                case '(':
                    self->parser.state = PARSER_STATE_CHARSET_G0;
                    return;

                /* set secondary charset G1 */
                case ')':
                    self->parser.state = PARSER_STATE_CHARSET_G1;
                    return;

                /* set tertiary charset G2 */
                case '*':
                    self->parser.state = PARSER_STATE_CHARSET_G2;
                    return;

                /* set quaternary charset G3 */
                case '+':
                    self->parser.state = PARSER_STATE_CHARSET_G3;
                    return;

                /* Select default character set(<ESC>%@) or Select UTF-8 character set(<ESC>%G)
                 */
                case '%':
                    self->parser.state = PARSER_STATE_CHARSET;
                    return;

                case 'g':
                    Vt_bell(self);
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Application Keypad (DECKPAM) */
                case '=':
                    self->modes.application_keypad = true;
                    self->parser.state             = PARSER_STATE_LITERAL;
                    return;

                /* Normal Keypad (DECKPNM) */
                case '>':
                    self->modes.application_keypad = false;
                    self->parser.state             = PARSER_STATE_LITERAL;
                    return;

                /* Disable Manual Input (DMI) */
                case '`':
                /* Enable Manual Input (EMI) */
                case 'b':
                    STUB("EMI/DMI");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Reset initial state (RIS) */
                case 'c':
                    Vt_hard_reset(self);
                    return;

                /* Save cursor (DECSC) */
                case '7':
                    self->saved_active_line = self->cursor.row;
                    self->saved_cursor_pos  = self->cursor.col;
                    self->parser.state      = PARSER_STATE_LITERAL;
                    return;

                /* Restore cursor (DECRC) */
                case '8':
                    self->cursor.row   = self->saved_active_line;
                    self->cursor.col   = self->saved_cursor_pos;
                    self->parser.state = PARSER_STATE_LITERAL;
                    return;

                /* Back Index (DECBI) (VT400)
                 *
                 * This control function moves the cursor backward one column. If the cursor is
                 * at the left margin, all screen data within the margins moves onecolumn to the
                 * right. The column shifted past the right margin is lost
                 */
                case '6':

                /* Forward Index (DECFI) (VT400)
                 *
                 * This control function moves the cursor forward one column. If the cursor is
                 *at the right margin, all screen data within the margins moves onecolumn to the
                 *left. The column shifted past the left margin is lost.
                 **/
                case '9':
                    STUB("DECBI/DECFI");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Coding Method Delimiter (CMD) */
                case 'd':
                    STUB("CMD");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G2 Character Set into GL (VT200 mode only) (LS2) */
                case 'n':
                    self->charset_gl   = &self->charset_g2;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G3 Character Set into GL (VT200 mode only) (LS3) */
                case 'o':
                    self->charset_gl   = &self->charset_g3;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /*  Invoke the G3 Character Set into GR (VT200 mode only) (LS3R) */
                case '|':
                    self->charset_gr   = &self->charset_g3;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G2 Character Set into GR (VT200 mode only) (LS2R) */
                case '}':
                    self->charset_gr   = &self->charset_g2;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Invoke the G1 Character Set into GR (VT200 mode only) (LS1R) */
                case '~':
                    self->charset_gr   = &self->charset_g1;
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Single Shift Select of G2 Character Set (SS2), VT220 */
                case 'N':
                    self->charset_single_shift = &self->charset_g2;
                    self->parser.state         = PARSER_STATE_LITERAL;
                    break;

                /* Single Shift Select of G3 Character Set (SS3), VT220 */
                case 'O':
                    self->charset_single_shift = &self->charset_g3;
                    self->parser.state         = PARSER_STATE_LITERAL;
                    break;

                /* Old (tab)title set sequence */
                case 'k':
                    self->parser.state = PARSER_STATE_TITLE;
                    break;

                /* Start of string */
                case 'X':
                    STUB("SOS");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* Start of guarded area */
                case 'V':
                /* End of guarded area */
                case 'W':
                    STUB("SGA/EGA");
                    self->parser.state = PARSER_STATE_LITERAL;
                    break;

                /* ST */
                case '\\':
                    return;

                case '\e':
                    return;

                default: {
                    const char* cs    = control_char_get_pretty_string(c);
                    char        cb[2] = { c, 0 };
                    WRN("Unknown escape sequence: %s (%d)\n", cs ? cs : cb, c);

                    self->parser.state = PARSER_STATE_LITERAL;
                    return;
                }
            }
            break;

        case PARSER_STATE_CHARSET_G0:
            self->parser.state = PARSER_STATE_LITERAL;
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            switch (c) {
                case '0':
                    self->charset_g0 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g0 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g0 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G1:
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            self->parser.state = PARSER_STATE_LITERAL;
            switch (c) {
                case '0':
                    self->charset_g1 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g1 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g1 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G2:
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            self->parser.state = PARSER_STATE_LITERAL;
            switch (c) {
                case '0':
                    self->charset_g2 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g2 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g2 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET_G3:
            if (!self->charset_gl) {
                self->charset_gl = &self->charset_g0;
            }
            if (!self->charset_gr) {
                self->charset_gr = &self->charset_g1;
            }
            self->parser.state = PARSER_STATE_LITERAL;
            switch (c) {
                case '0':
                    self->charset_g3 = &char_sub_gfx;
                    return;
                case 'A':
                    self->charset_g3 = &char_sub_uk;
                    return;
                case 'B':
                    self->charset_g3 = NULL;
                    return;
                default:
                    WRN("Unknown character set code %c\n", c);
                    return;
            }
            break;

        case PARSER_STATE_CHARSET:
            STUB("character set select command");
            self->parser.state = PARSER_STATE_LITERAL;
            break;

        case PARSER_STATE_OSC:
            Vt_handle_OSC(self, c);
            break;

        case PARSER_STATE_PM:
            Vt_handle_PM(self, c);
            break;

        case PARSER_STATE_DCS:
            Vt_handle_DCS(self, c);
            break;

        case PARSER_STATE_APC:
            Vt_handle_APC(self, c);
            break;

        case PARSER_STATE_DEC_SPECIAL:
            switch (c) {
                /* DEC Screen Alignment Test (DECALN), VT100.
                 *
                 * note:
                 * DECALN sets the margins to the extremes of the page, and moves
                 * the cursor to the home position.
                 */
                case '8': {
                    self->scroll_region_left   = 0;
                    self->scroll_region_right  = Vt_col(self) - 1;
                    self->scroll_region_top    = 0;
                    self->scroll_region_bottom = Vt_row(self) - 1;
                    Vt_move_cursor(self, 0, 0);
                    VtRune blank_E    = self->blank_space;
                    blank_E.rune.code = 'E';
                    for (size_t cl = 0; cl < Vt_col(self); ++cl) {
                        for (size_t r = Vt_top_line(self); r <= Vt_bottom_line(self); ++r) {
                            Vt_overwrite_char_at(self, cl, r, blank_E);
                        }
                    }
                } break;

                default:
                    WRN("Unknown DEC escape\n");
            }
            self->parser.state = PARSER_STATE_LITERAL;
            break;

        case PARSER_STATE_TITLE: {
            Vector_push_char(&self->parser.active_sequence, c);
            size_t len = self->parser.active_sequence.size;
            if ((len >= 2 && self->parser.active_sequence.buf[len - 2] == '\e' &&
                 self->parser.active_sequence.buf[len - 1] == '\\') ||
                c == '\a') {
                Vector_pop_n_char(&self->parser.active_sequence, 2);
                Vector_push_char(&self->parser.active_sequence, '\0');
                Vt_set_title(self, self->parser.active_sequence.buf);
                self->parser.state = PARSER_STATE_LITERAL;
                Vector_clear_char(&self->parser.active_sequence);
            }
        } break;

        default:
            ASSERT_UNREACHABLE;
    }
}

static void Vt_shift_global_line_index_refs(Vt* self, size_t point, int64_t change, bool refs_only)
{
    LOG("Vt::shift_idx{ pt: %zu, delta: %ld }\n", point, change);

    if (!refs_only) {
        if (self->cursor.row >= point) {
            self->cursor.row += change;
        }

        if (self->visual_scroll_top >= point) {
            self->visual_scroll_top += change;
        }

        if (self->scroll_region_top >= point) {
            self->scroll_region_top += change;
        }

        if (self->scroll_region_bottom >= point) {
            self->scroll_region_bottom += change;
        }
    }

    for (RcPtr_VtImageSurfaceView* rp = NULL;
         (rp = Vector_iter_RcPtr_VtImageSurfaceView(&self->image_views, rp));) {
        VtImageSurfaceView* sv = RcPtr_get_VtImageSurfaceView(rp);

        if (sv && sv->anchor_global_index >= point) {
            sv->anchor_global_index += change;
        }
    }

    for (RcPtr_VtSixelSurface* rp = NULL;
         (rp = Vector_iter_RcPtr_VtSixelSurface(&self->scrolled_sixels, rp));) {
        VtSixelSurface* ss = RcPtr_get_VtSixelSurface(rp);

        if (ss && ss->anchor_global_index >= point) {
            ss->anchor_global_index += change;
        }
    }

    for (RcPtr_VtCommand* rp = NULL;
         (rp = Vector_iter_RcPtr_VtCommand(&self->shell_commands, rp));) {
        VtCommand* cmd = RcPtr_get_VtCommand(rp);

        if (cmd->command_start_row >= point) {
            cmd->command_start_row += change;
        }

        if (cmd->output_rows.first >= point) {
            cmd->output_rows.first += change;
        }

        if (cmd->output_rows.second >= point) {
            cmd->output_rows.second += change;
        }
    }

    if (self->selection.mode == SELECT_MODE_NORMAL) {
        if (self->selection.begin_line >= point) {
            self->selection.begin_line += change;
        }

        if (self->selection.end_line >= point) {
            self->selection.end_line += change;
        }
    }
}

static void Vt_remove_scrollback(Vt* self, size_t lines)
{
    if (Vt_alt_buffer_enabled(self))
        return;

    lines = MIN(lines, (self->lines.size - Vt_row(self)));
    Vector_remove_at_VtLine(&self->lines, 0, lines);
    Vt_shift_global_line_index_refs(self, lines, -(int64_t)lines, false);

    for (RcPtr_VtCommand* rpp = NULL;
         (rpp = Vector_iter_RcPtr_VtCommand(&self->shell_commands, rpp));) {
        VtCommand* cmd = RcPtr_get_VtCommand(rpp);

        if (cmd->command_start_row < (size_t)lines)
            RcPtr_destroy_VtCommand(rpp);
    }

    while (self->shell_commands.size && (!RcPtr_get_VtCommand(self->shell_commands.buf) ||
                                         RcPtr_is_unique_VtCommand(self->shell_commands.buf))) {
        Vector_remove_at_RcPtr_VtCommand(&self->shell_commands, 0, 1);
    }

    while (self->image_views.size) {
        bool removed = false;
        for (RcPtr_VtImageSurfaceView* i = NULL;
             (i = Vector_iter_RcPtr_VtImageSurfaceView(&self->image_views, i));) {
            if (RcPtr_is_unique_VtImageSurfaceView(i)) {
                Vector_remove_at_RcPtr_VtImageSurfaceView(
                  &self->image_views,
                  Vector_index_RcPtr_VtImageSurfaceView(&self->image_views, i),
                  1);
                RcPtr_destroy_VtImageSurfaceView(i);
                removed = true;
                break;
            }

            if (removed) {
                continue;
            } else {
                break;
            }
        }
    }

    while (self->scrolled_sixels.size) {
        bool removed = false;
        for (RcPtr_VtSixelSurface* i = NULL;
             (i = Vector_iter_RcPtr_VtSixelSurface(&self->scrolled_sixels, i));) {
            if (RcPtr_is_unique_VtSixelSurface(i)) {
                Vector_remove_at_RcPtr_VtSixelSurface(
                  &self->scrolled_sixels,
                  Vector_index_RcPtr_VtSixelSurface(&self->scrolled_sixels, i),
                  1);
                RcPtr_destroy_VtSixelSurface(i);
                removed = true;
                break;
            }
        }

        if (removed) {
            continue;
        } else {
            break;
        }
    }
}

void Vt_clear_scrollback(Vt* self)
{
    if (Vt_alt_buffer_enabled(self)) {
        return;
    }

    Vt_remove_scrollback(self, self->lines.size);
}

static void Vt_shrink_scrollback(Vt* self)
{
    if (Vt_alt_buffer_enabled(self)) {
        return;
    }

    int64_t ln_cnt = self->lines.size;
    if (unlikely(ln_cnt > MAX(settings.scrollback * 1.1, Vt_row(self)))) {
        int64_t to_remove = ln_cnt - settings.scrollback - Vt_row(self);
        Vt_remove_scrollback(self, to_remove);
    }
}

static inline void Vt_clear_proxies(Vt* self)
{
    if (self->scrolling_visual) {
        if (self->visual_scroll_top > Vt_row(self) * 5) {
            size_t begin = Vt_visual_bottom_line(self) + 4 * Vt_row(self);
            Vt_clear_proxies_in_region(self, begin, self->lines.size - 1);
        }
    } else if (self->lines.size > Vt_row(self)) {
        size_t end = Vt_visual_top_line(self) ? Vt_visual_top_line(self) - 1 : 0;
        Vt_clear_proxies_in_region(self, 0, end);
    }
}

inline void Vt_interpret(Vt* self, char* buf, size_t bytes)
{
    if (unlikely(settings.debug_pty)) {
        char* str = pty_string_prettyfy(buf, bytes);
        fprintf(stderr, "pty.read (%3zu) ~> { %s }\n\n", bytes, str);
        free(str);
    }

    memset(&self->defered_events, 0, sizeof(self->defered_events));

    for (size_t i = 0; i < bytes; ++i) {
        Vt_handle_char(self, buf[i]);
    }

    Vt_shrink_scrollback(self);

    if (self->defered_events.action_performed) {
        CALL(self->callbacks.on_action_performed, self->callbacks.user_data);
    }

    if (self->defered_events.repaint) {
        CALL(self->callbacks.on_repaint_required, self->callbacks.user_data);
    }
}

void Vt_get_visible_lines(const Vt* self, VtLine** out_begin, VtLine** out_end)
{
    if (out_begin) {
        *out_begin = self->lines.buf + Vt_visual_top_line(self);
    }

    if (out_end) {
        *out_end = self->lines.buf + Vt_visual_bottom_line(self) + 1;
    }
}

/**
 * Start entering unicode codepoint as hex */
void Vt_start_unicode_input(Vt* self)
{
    self->unicode_input.active   = true;
    self->defered_events.repaint = true;
}

void Vt_handle_button(void*    _self,
                      uint32_t button,
                      bool     state,
                      int32_t  x,
                      int32_t  y,
                      int32_t  ammount,
                      uint32_t mods)
{
    Vt*  self        = _self;
    bool in_window   = x >= 0 && x <= self->ws.ws_xpixel && y >= 0 && y <= self->ws.ws_ypixel;
    bool btn_reports = Vt_reports_mouse(self);

    if (btn_reports && in_window) {
        if (!self->scrolling_visual) {
            self->last_click_x = (double)x / self->pixels_per_cell_x;
            self->last_click_y = (double)y / self->pixels_per_cell_y;

            if (self->modes.x10_mouse_compat) {
                button += (FLAG_IS_SET(mods, MODIFIER_SHIFT) ? 4 : 0) +
                          (FLAG_IS_SET(mods, MODIFIER_ALT) ? 8 : 0) +
                          (FLAG_IS_SET(mods, MODIFIER_CONTROL) ? 16 : 0);
            }
            if (self->modes.extended_report) {
                Vt_output_formated(self,
                                   "\e[<%u;%d;%d%c",
                                   button - 1,
                                   self->last_click_x + 1,
                                   self->last_click_y + 1,
                                   state ? 'M' : 'm');
            } else if (self->modes.mouse_btn_report) {
                Vt_output_formated(self,
                                   "\e[M%c%c%c",
                                   32 + button - 1 + !state * 3,
                                   (char)(32 + self->last_click_x + 1),
                                   (char)(32 + self->last_click_y + 1));
            }
        }
    }
}

void Vt_handle_motion(void* _self, uint32_t button, int32_t x, int32_t y)
{
    Vt* self = _self;

    if (self->modes.extended_report) {
        if (!self->scrolling_visual) {
            x              = CLAMP(x, 0, self->ws.ws_xpixel);
            y              = CLAMP(y, 0, self->ws.ws_ypixel);
            size_t click_x = (double)x / self->pixels_per_cell_x;
            size_t click_y = (double)y / self->pixels_per_cell_y;
            if (click_x != self->last_click_x || click_y != self->last_click_y) {
                self->last_click_x = click_x;
                self->last_click_y = click_y;
                Vt_output_formated(self,
                                   "\e[<%d;%zu;%zuM",
                                   (int)button - 1 + 32,
                                   click_x + 1,
                                   click_y + 1);
            }
        }
    }
}

void Vt_handle_clipboard(void* self, const char* text)
{
    Vt* vt = self;

    size_t len = strlen(text);

    if (!text || !len) {
        return;
    }

    if (vt->modes.bracketed_paste) {
        Vt_output(vt, "\e[200~", 6);
    }

    char last = '\0';
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '\n') {
            if (last != '\r') {
                Vector_push_char(&vt->output, '\r');
            }
        } else {
            Vector_push_char(&vt->output, c);
        }
        last = c;
    }

    if (vt->modes.bracketed_paste) {
        Vt_output(vt, "\e[201~", 6);
    }
}

static void Vt_set_title(Vt* self, const char* title)
{
    free(self->title);
    self->title = strdup(title);
    CALL(self->callbacks.on_title_changed, self->callbacks.user_data, self->title);
}

void Vt_peek_output(Vt* self, size_t len, char** out_buf, size_t* out_size)
{
    ASSERT(out_buf && out_size, "has output ptrs");
    *out_buf  = self->output.buf;
    *out_size = MIN(self->output.size, len);
}

void Vt_consumed_output(Vt* self, size_t len)
{
    if (likely(self->output.size < len)) {
        Vector_clear_char(&self->output);
    } else {
        Vector_remove_at_char(&self->output, 0, len);
    }
}

void Vt_destroy(Vt* self)
{
    Vector_destroy_VtLine(&self->lines);

    if (Vt_alt_buffer_enabled(self)) {
        Vector_destroy_VtLine(&self->alt_lines);
        Vector_destroy_RcPtr_VtImageSurfaceView(&self->alt_image_views);
        Vector_destroy_RcPtr_VtSixelSurface(&self->alt_scrolled_sixels);
    }

    Vector_destroy_char(&self->parser.active_sequence);
    Vector_destroy_DynStr(&self->title_stack);
    Vector_destroy_char(&self->unicode_input.buffer);
    Vector_destroy_char(&self->output);
    Vector_destroy_char(&self->staged_output);
    Vector_destroy_char(&self->uri_matcher.match);
    Vector_destroy_RcPtr_VtImageSurface(&self->images);
    Vector_destroy_RcPtr_VtImageSurfaceView(&self->image_views);
    Vector_destroy_RcPtr_VtCommand(&self->shell_commands);
    Vector_destroy_RcPtr_VtSixelSurface(&self->scrolled_sixels);
    RcPtr_destroy_VtImageSurface(&self->manipulated_image);
    free(self->title);
    free(self->active_hyperlink);
    free(self->work_dir);
    free(self->tab_ruler);
    free(self->client_host);
    free(self->shell_integration_current_dir);
    free(self->shell_integration_shell_host);
    free(self->shell_integration_shell_id);
}
