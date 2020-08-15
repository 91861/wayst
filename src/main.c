/* See LICENSE for license information. */

/**
 * App links Vt, Monitor, Window, Gfx modules together and deals with ui
 *
 * TODO:
 * - It makes more sense for the unicode input prompt to be here
 * - Move flash animations here
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "gfx_gl21.h"

#ifndef NOWL
#include "wl.h"
#endif

#ifndef NOX
#include "x.h"
#endif

#include "freetype.h"
#include "settings.h"
#include "ui.h"
#include "vt.h"

#ifndef SCROLLBAR_HIDE_DELAY_MS
#define SCROLLBAR_HIDE_DELAY_MS 1500
#endif

#ifndef SCROLLBAR_FADE_TIME_MS
#define SCROLLBAR_FADE_TIME_MS 150
#endif

#ifndef DOUBLE_CLICK_DELAY_MS
#define DOUBLE_CLICK_DELAY_MS 300
#endif

#ifndef AUTOSCROLL_DELAY_MS
#define AUTOSCROLL_DELAY_MS 50
#endif

#ifndef SCROLLBAR_MIN_LEN_PX
#define SCROLLBAR_MIN_LEN_PX 20
#endif

#ifndef SCROLLBAR_WIDTH_PX
#define SCROLLBAR_WIDTH_PX 10
#endif

typedef struct
{
    Window_* win;
    Gfx*     gfx;
    Vt       vt;
    Freetype freetype;
    Monitor  monitor;

    Pair_uint32_t resolution;

    bool       swap_performed;
    TimePoint* closest_pending_wakeup;

    bool exit;

    // selection
    uint8_t   click_count;
    TimePoint next_click_limit;
    bool      selection_dragging_left;
    enum SelectionDragRight
    {
        SELECT_DRAG_RIGHT_NONE = 0,
        SELECT_DRAG_RIGHT_FORNT,
        SELECT_DRAG_RIGHT_BACK
    } selection_dragging_right;
    bool keyboard_select_mode;

    // scrollbar
    TimePoint scrollbar_hide_time;
    TimePoint autoscroll_next_step;
    float     scrollbar_drag_position;
    bool      last_scrolling;

    Cursor ksm_cursor;

    Ui ui;

    enum AutoscrollDir
    {
        AUTOSCROLL_NONE = 0,
        AUTOSCROLL_UP   = 1,
        AUTOSCROLL_DN   = -1,

    } autoscroll;

} App;

static void App_update_scrollbar_dims(App* self);
static void App_update_scrollbar_vis(App* self);
static void App_update_cursor(App* self);
void        App_do_autoscroll(App* self);
void        App_notify_content_change(void* self);
static void App_clamp_cursor(App* self, Pair_uint32_t chars);
static void App_set_monitor_callbacks(App* self);
static void App_set_callbacks(App* self);
static void App_maybe_resize(App* self, Pair_uint32_t newres);

void* App_load_extension_proc_address(void* self, const char* name)
{
    App*  app  = self;
    void* addr = Window_get_proc_adress(app->win, name);
    if (!addr) {
        ERR("Failed to load extension proc adress for: %s", name);
    }
    return addr;
}

void App_create_window(App* self, Pair_uint32_t res)
{
    if (!settings.x11_is_default)
#ifndef NOWL
        self->win = Window_new_wayland(res);
#endif
    if (!self->win) {
#ifndef NOX
        self->win = Window_new_x11(res);
#endif
    }
    if (!self->win) {
        ERR("Failed to create window"
#ifdef NOX
            ", note: compiled without X11 support"
#endif
        );
    }
}

void App_init(App* self)
{
    memset(self, 0, sizeof(App));
    self->monitor = Monitor_new();

    App_set_monitor_callbacks(self);
    
    Monitor_fork_new_pty(&self->monitor, settings.cols, settings.rows);

    Vt_init(&self->vt, settings.cols, settings.rows);
    self->vt.master_fd = self->monitor.child_fd;
    self->freetype     = Freetype_new();
    self->gfx          = Gfx_new_OpenGL21(&self->freetype);

    App_create_window(self, Gfx_pixels(self->gfx, settings.cols, settings.rows));
    App_set_callbacks(self);

    settings_after_window_system_connected();
    Window_set_swap_interval(self->win, 0);
    Gfx_init_with_context_activated(self->gfx);

    Pair_uint32_t size = Window_size(self->win);
    Gfx_resize(self->gfx, size.first, size.second);

    Pair_uint32_t chars = Gfx_get_char_size(self->gfx);
    Vt_resize(&self->vt, chars.first, chars.second);

    Monitor_watch_window_system_fd(&self->monitor, Window_get_connection_fd(self->win));

    self->ui.scrollbar.width = SCROLLBAR_WIDTH_PX;
    self->ui.pixel_offset_x  = 0;
    self->ui.pixel_offset_y  = 0;
    self->swap_performed     = false;
    self->resolution         = size;
}

void App_run(App* self)
{
    while (!(self->exit || Window_is_closed(self->win))) {
        int timeout_ms = self->swap_performed
                           ? 0
                           : self->closest_pending_wakeup
                               ? TimePoint_is_ms_ahead(*(self->closest_pending_wakeup))
                               : -1;

        Monitor_wait(&self->monitor, timeout_ms);
        self->closest_pending_wakeup = NULL;
        if (Monitor_are_window_system_events_pending(&self->monitor)) {
            Window_events(self->win);
        }

        TimePoint* pending_window_timer = Window_process_timers(self->win);
        if (pending_window_timer) {
            self->closest_pending_wakeup = pending_window_timer;
        }

        char*  buf;
        size_t len;
        Vt_get_output(&self->vt, &buf, &len);
        if (len) {
            Monitor_write(&self->monitor, buf, len);
        }
        ssize_t bytes = 0;
        do {
            bytes = Monitor_read(&self->monitor);
            if (bytes > 0) {
                Vt_interpret(&self->vt, self->monitor.input_buffer, bytes);
                Gfx_notify_action(self->gfx);
            } else if (bytes < 0) {
                break;
            }
        } while (bytes);

        Vt_get_output(&self->vt, &buf, &len);
        if (len) {
            Monitor_write(&self->monitor, buf, len);
        }

        App_maybe_resize(self, Window_size(self->win));
        if (self->ui.scrollbar.visible || self->vt.scrolling_visual) {
            App_do_autoscroll(self);
            App_update_scrollbar_vis(self);
            App_update_scrollbar_dims(self);
        }
        App_update_cursor(self);

        TimePoint* closest_gfx_timer;
        if (!!Gfx_update_timers(self->gfx, &self->vt, &self->ui, &closest_gfx_timer) +
            !!Gfx_set_focus(self->gfx, FLAG_IS_SET(self->win->state_flags, WINDOW_IS_IN_FOCUS))) {
            Window_notify_content_change(self->win);
        }
        if ((closest_gfx_timer && !self->closest_pending_wakeup) ||
            (closest_gfx_timer && self->closest_pending_wakeup &&
             TimePoint_is_earlier(*closest_gfx_timer, *self->closest_pending_wakeup))) {
            self->closest_pending_wakeup = closest_gfx_timer;
        }

        self->swap_performed = Window_maybe_swap(self->win);
    }

    Monitor_kill(&self->monitor);
    Vt_destroy(&self->vt);
    Gfx_destroy(self->gfx);
    Freetype_destroy(&self->freetype);
    Window_destroy(self->win);
}

static void App_redraw(void* self)
{
    App* app = self;
    Gfx_draw(app->gfx, &app->vt, &app->ui);
}

static void App_update_padding(App* self)
{
    Pair_uint32_t chars       = Gfx_get_char_size(self->gfx);
    Pair_uint32_t used_pixels = Gfx_pixels(self->gfx, chars.first, chars.second);
    if (settings.padding_center) {
        self->ui.pixel_offset_x = (self->resolution.first - used_pixels.first) / 2;
        self->ui.pixel_offset_y = (self->resolution.second - used_pixels.second) / 2;
    } else {
        self->ui.pixel_offset_x = 0;
        self->ui.pixel_offset_y = 0;
    }
    self->ui.pixel_offset_x += settings.padding;
    self->ui.pixel_offset_y += settings.padding;
}

static void App_maybe_resize(App* self, Pair_uint32_t newres)
{
    if (newres.first != self->resolution.first || newres.second != self->resolution.second) {
        self->resolution = newres;
        Gfx_resize(self->gfx, self->resolution.first, self->resolution.second);
        Pair_uint32_t chars = Gfx_get_char_size(self->gfx);
        App_update_padding(self);
        App_clamp_cursor(self, chars);
        Window_notify_content_change(self->win);
        Vt_resize(&self->vt, chars.first, chars.second);
    }
}

void App_clipboard_handler(void* self, const char* text)
{
    Vt_handle_clipboard(&((App*)self)->vt, text);
}

void App_reload_font(void* self)
{
    App* app = self;
    Freetype_reload_fonts(&app->freetype);
    Gfx_reload_font(app->gfx);
    Gfx_draw(app->gfx, &app->vt, &app->ui);
    App_update_padding(self);
    Window_notify_content_change(app->win);
    Window_maybe_swap(app->win);
}

uint32_t App_get_key_code(void* self, char* name)
{
    return Window_get_keysym_from_name(((App*)self)->win, name);
}

void App_notify_content_change(void* self)
{
    Window_notify_content_change(((App*)self)->win);
}

void App_clipboard_send(void* self, const char* text)
{
    Window_clipboard_send(((App*)self)->win, text);
}

void App_clipboard_get(void* self)
{
    Window_clipboard_get(((App*)self)->win);
}

Pair_uint32_t App_window_size(void* self)
{
    return Window_size(((App*)self)->win);
}

Pair_uint32_t App_window_position(void* self)
{
    return Window_position(((App*)self)->win);
}

Pair_uint32_t App_pixels(void* self, uint32_t rows, uint32_t columns)
{
    return Gfx_pixels(((App*)self)->gfx, rows, columns);
}

Pair_uint32_t App_get_char_size(void* self)
{
    return Gfx_get_char_size(((App*)self)->gfx);
}

void App_update_title(void* self, const char* title)
{
    Window_update_title(((App*)self)->win, title);
}

void App_flash(void* self)
{
    Gfx_flash(((App*)self)->gfx);
}

void App_action(void* self)
{
    Gfx_notify_action(((App*)self)->gfx);
}

static void App_clamp_cursor(App* self, Pair_uint32_t chars)
{
    if (self->keyboard_select_mode) {
        self->ksm_cursor.col = MIN(self->ksm_cursor.col, chars.first - 1);
        self->ksm_cursor.row = CLAMP(self->ksm_cursor.row,
                                     Vt_visual_top_line(&self->vt),
                                     Vt_visual_bottom_line(&self->vt));
    }
}

static void App_exit_handler(void* self)
{
    App* app  = self;
    app->exit = true;
}

/**
 * key commands used in keyboard select mode
 * @return exit ksm mode */
static bool App_handle_keyboard_select_mode_key(App*     self,
                                                uint32_t key,
                                                uint32_t rawkey,
                                                uint32_t mods)
{
    switch (key) {
        case 27: // Escape
            Vt_select_end(&self->vt);
            App_notify_content_change(self);
            self->keyboard_select_mode = false;
            Vt_visual_scroll_reset(&self->vt);
            Gfx_notify_action(self->gfx);
            return true;

        case 65361: // left
        case 104:   // h
            App_notify_content_change(self);
            if (self->ksm_cursor.col) {
                self->ksm_cursor.col--;
            }
            if (self->vt.selection.mode) {
                Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
            }
            break;

        case 65364: // down
        case 106:   // j
            App_notify_content_change(self);
            if (self->ksm_cursor.row < self->vt.lines.size - 1) {
                self->ksm_cursor.row++;
            }
            if (Vt_visual_bottom_line(&self->vt) == self->vt.lines.size - 1) {
                Vt_visual_scroll_reset(&self->vt);
            } else {
                while (Vt_visual_bottom_line(&self->vt) < self->ksm_cursor.row) {
                    Vt_visual_scroll_down(&self->vt);
                }
            }
            if (self->vt.selection.mode) {
                Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
            }
            break;

        case 65362: // up
        case 107:   // k
            App_notify_content_change(self);
            if (self->ksm_cursor.row) {
                self->ksm_cursor.row--;
            }
            while (Vt_visual_top_line(&self->vt) > self->ksm_cursor.row) {
                Vt_visual_scroll_up(&self->vt);
            }
            if (self->vt.selection.mode) {
                Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
            }
            break;

        case 65363: // right
        case 108:   // l
            App_notify_content_change(self);
            if (self->ksm_cursor.col + 1 < self->vt.ws.ws_col) {
                self->ksm_cursor.col++;
            }
            if (self->vt.selection.mode) {
                Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
            }
            break;

        case 121: // y
        case 99:  // c
        {
            if (self->vt.selection.mode) {
                Vector_char txt = Vt_select_region_to_string(&self->vt);
                if (txt.size) {
                    App_clipboard_send(self, txt.buf);
                    Vt_select_end(&self->vt);
                } else {
                    Vector_destroy_char(&txt);
                }
            }
        } break;

        case 13: // Return
            if (self->vt.selection.mode) {
                Vt_select_end(&self->vt);
                break;
            }
            Vt_select_init_cell(&self->vt,
                                FLAG_IS_SET(mods, MODIFIER_CONTROL) ? SELECT_MODE_BOX
                                                                    : SELECT_MODE_NORMAL,
                                self->ksm_cursor.col,
                                self->ksm_cursor.row);
            Vt_select_commit(&self->vt);
            break;

        case 98: { // b
            // jump back by word
            for (bool initial = true;; initial = false) {
                if (!self->ksm_cursor.col) {
                    break;
                }
                char32_t code =
                  self->vt.lines.buf[self->ksm_cursor.row].data.buf[self->ksm_cursor.col].rune.code;
                char32_t prev_code = self->vt.lines.buf[self->ksm_cursor.row]
                                       .data.buf[self->ksm_cursor.col - 1]
                                       .rune.code;
                if (isblank(prev_code) && !isblank(code) && !initial) {
                    break;
                }
                --self->ksm_cursor.col;
                App_notify_content_change(self);
            }
            Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
        } break;

        case 119: { // w
            // jump forward to next word
            for (bool initial = true;; initial = false) {
                if (self->ksm_cursor.col + 1 >= self->vt.ws.ws_col ||
                    self->ksm_cursor.col + 1 >=
                      self->vt.lines.buf[self->ksm_cursor.row].data.size) {
                    break;
                }
                char32_t code =
                  self->vt.lines.buf[self->ksm_cursor.row].data.buf[self->ksm_cursor.col].rune.code;
                char32_t next_code = self->vt.lines.buf[self->ksm_cursor.row]
                                       .data.buf[self->ksm_cursor.col + 1]
                                       .rune.code;
                ++self->ksm_cursor.col;
                App_notify_content_change(self);
                if ((isblank(code) && !isblank(next_code)) && !initial)
                    break;
            }
            Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
        } break;

        case 101: { // e
            // jump to end of word
            for (bool initial = true;; initial = false) {
                if (self->ksm_cursor.col + 1 >= self->vt.ws.ws_col ||
                    self->ksm_cursor.col + 1 >=
                      self->vt.lines.buf[self->ksm_cursor.row].data.size) {
                    break;
                }
                char32_t code = self->vt.lines.buf[self->ksm_cursor.row]
                                  .data.buf[self->ksm_cursor.col]
                                  .rune.code,
                         next_code = self->vt.lines.buf[self->ksm_cursor.row]
                                       .data.buf[self->ksm_cursor.col + 1]
                                       .rune.code;
                if ((isblank(next_code) && !isblank(code)) && !initial)
                    break;
                ++self->ksm_cursor.col;
                App_notify_content_change(self);
            }
            Vt_select_set_end_cell(&self->vt, self->ksm_cursor.col, self->ksm_cursor.row);
        } break;

        default:
            LOG("KSM key: %d(%d)\n", key, rawkey);
    }
    return false;
}

/**
 * key commands used by the terminal itself
 * @return keypress was consumed */
static bool App_maybe_handle_application_key(App*     self,
                                             uint32_t key,
                                             uint32_t rawkey,
                                             uint32_t mods)
{
    Vt* vt = &self->vt;
    if (self->keyboard_select_mode) {
        self->keyboard_select_mode = !App_handle_keyboard_select_mode_key(self, key, rawkey, mods);
        return true;
    }
    if (KeyCommand_is_active(&settings.key_commands[KCMD_COPY], key, rawkey, mods)) {
        if (vt->selection.mode) {
            Vector_char txt = Vt_select_region_to_string(vt);
            if (txt.size)
                App_clipboard_send(self, txt.buf);
            else
                Vector_destroy_char(&txt);
        }
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_PASTE], key, rawkey, mods)) {
        App_clipboard_get(self);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_FONT_SHRINK], key, rawkey, mods)) {
        if (settings.font_size > 1) {
            --settings.font_size;
            Vt_clear_all_proxies(vt);
            App_reload_font(self);
            Pair_uint32_t cells = App_get_char_size(self);
            Vt_resize(vt, cells.first, cells.second);
            App_notify_content_change(self);
        } else {
            App_flash(self);
        }
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_FONT_ENLARGE], key, rawkey, mods)) {
        ++settings.font_size;
        Vt_clear_all_proxies(vt);
        App_reload_font(self);
        Pair_uint32_t cells = App_get_char_size(self);
        Vt_resize(vt, cells.first, cells.second);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_DEBUG], key, rawkey, mods)) {
        Vt_dump_info(vt);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_UNICODE_ENTRY],
                                    key,
                                    rawkey,
                                    mods)) {
        Vt_start_unicode_input(vt);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_KEYBOARD_SELECT],
                                    key,
                                    rawkey,
                                    mods)) {
        self->ksm_cursor           = self->vt.cursor;
        self->ksm_cursor.blinking  = false;
        self->ui.cursor            = &self->ksm_cursor;
        self->keyboard_select_mode = true;
        return true;
    }
    return false;
}

void App_key_handler(void* self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    if (!App_maybe_handle_application_key(self, key, rawkey, mods)) {
        App* app = self;
        Window_set_pointer_style(app->win, MOUSE_POINTER_HIDDEN);
        Vt_handle_key(&app->vt, key, rawkey, mods);
    }
}

static void App_update_cursor(App* self)
{
    if (self->keyboard_select_mode) {
        self->ui.cursor = &self->ksm_cursor;
    } else {
        self->ui.cursor = &self->vt.cursor;
    }
}

/**
 * Update gui scrollbar dimensions */
static void App_update_scrollbar_dims(App* self)
{
    Vt*     vt                = &self->vt;
    double  minimum_length    = (2.0 / self->resolution.second) * SCROLLBAR_MIN_LEN_PX;
    double  length            = 2.0 / vt->lines.size * vt->ws.ws_row;
    double  extra_length      = length > minimum_length ? 0.0 : (minimum_length - length);
    int32_t scrollable_lines  = Vt_top_line(&self->vt);
    int32_t lines_scrolled    = Vt_visual_top_line(&self->vt);
    double  fraction_scrolled = (double)lines_scrolled / scrollable_lines;
    self->ui.scrollbar.length = length + extra_length;
    self->ui.scrollbar.top    = (2.0 - length - extra_length) * fraction_scrolled;
    int64_t ms                = TimePoint_is_ms_ahead(self->scrollbar_hide_time);
    if (ms > 0 && ms < SCROLLBAR_FADE_TIME_MS && !self->vt.scrolling_visual) {
        self->ui.scrollbar.opacity = ((float)ms / SCROLLBAR_FADE_TIME_MS);
        App_notify_content_change(self);
    } else {
        self->ui.scrollbar.opacity = 1.0f;
    }
}

/**
 * @return drag event was consumed by gui scrollbar */
static bool App_scrollbar_consume_drag(App* self, uint32_t button, int32_t x, int32_t y)
{
    if (!self->ui.scrollbar.dragging) {
        return false;
    }
    Vt* vt       = &self->vt;
    y            = CLAMP(y, 0, vt->ws.ws_ypixel);
    float  dp    = 2.0f * ((float)y / (float)vt->ws.ws_ypixel) - self->scrollbar_drag_position;
    float  range = 2.0f - self->ui.scrollbar.length;
    size_t target_line = Vt_top_line(vt) * CLAMP(dp, 0.0, range) / range;
    if (target_line != Vt_visual_top_line(vt)) {
        Vt_visual_scroll_to(vt, target_line);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    }
    return true;
}

/**
 * @return click event was consumed by gui scrollbar */
static bool App_scrollbar_consume_click(App*     self,
                                        uint32_t button,
                                        uint32_t state,
                                        int32_t  x,
                                        int32_t  y)
{
    Vt* vt           = &self->vt;
    self->autoscroll = AUTOSCROLL_NONE;
    if (!self->ui.scrollbar.visible || button > 3)
        return false;
    if (self->ui.scrollbar.dragging && !state) {
        self->ui.scrollbar.dragging = false;
        App_notify_content_change(self);
        return false;
    }
    float dp = 2.0f * ((float)y / (float)vt->ws.ws_ypixel);
    if (x > vt->ws.ws_xpixel - self->ui.scrollbar.width) {
        // inside region
        if (self->ui.scrollbar.top < dp &&
            self->ui.scrollbar.top + self->ui.scrollbar.length > dp) {
            // inside scrollbar
            if (state && (button == MOUSE_BTN_LEFT || button == MOUSE_BTN_RIGHT ||
                          button == MOUSE_BTN_MIDDLE)) {
                self->ui.scrollbar.dragging   = true;
                self->scrollbar_drag_position = dp - self->ui.scrollbar.top;
            }
        } else {
            // outside of scrollbar
            if (state && button == MOUSE_BTN_LEFT) {
                /* jump to that position and start dragging in the middle */
                self->ui.scrollbar.dragging   = true;
                self->scrollbar_drag_position = self->ui.scrollbar.length / 2;
                dp = 2.0f * ((float)y / (float)vt->ws.ws_ypixel) - self->scrollbar_drag_position;
                float  range       = 2.0f - self->ui.scrollbar.length;
                size_t target_line = Vt_top_line(vt) * CLAMP(dp, 0.0, range) / range;
                if (target_line != Vt_visual_top_line(vt)) {
                    Vt_visual_scroll_to(vt, target_line);
                }
            } else if (state && button == MOUSE_BTN_RIGHT) {
                self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
                if (dp > self->ui.scrollbar.top + self->ui.scrollbar.length / 2) {
                    self->autoscroll = AUTOSCROLL_DN;
                } else {
                    self->autoscroll = AUTOSCROLL_UP;
                }
            } else if (state && button == MOUSE_BTN_MIDDLE) {
                /* jump one screen in that direction */
                if (dp > self->ui.scrollbar.top + self->ui.scrollbar.length / 2) {
                    Vt_visual_scroll_to(vt, vt->visual_scroll_top + vt->ws.ws_row);
                } else {
                    size_t to = vt->visual_scroll_top > vt->ws.ws_row
                                  ? vt->visual_scroll_top - vt->ws.ws_row
                                  : 0;
                    Vt_visual_scroll_to(vt, to);
                }
            }
        }
    } else {
        return false;
    }
    App_update_scrollbar_dims(self);
    App_notify_content_change(self);

    return true;
}

/**
 * Update gui scrollbar visibility */
static void App_update_scrollbar_vis(App* self)
{
    Vt* vt = &self->vt;
    if (!vt->scrolling_visual) {
        if (self->last_scrolling) {
            self->scrollbar_hide_time = TimePoint_ms_from_now(SCROLLBAR_HIDE_DELAY_MS);
        } else if (self->ui.scrollbar.dragging) {
            self->scrollbar_hide_time = TimePoint_ms_from_now(SCROLLBAR_HIDE_DELAY_MS);
        } else if (TimePoint_passed(self->scrollbar_hide_time)) {
            if (self->ui.scrollbar.visible) {
                self->ui.scrollbar.visible = false;
                App_notify_content_change(self);
            }
        }
    }
    self->last_scrolling = vt->scrolling_visual;
}

void App_do_autoscroll(App* self)
{
    Vt* vt = &self->vt;
    App_update_scrollbar_vis(self);
    if (self->autoscroll == AUTOSCROLL_UP && TimePoint_passed(self->autoscroll_next_step)) {
        self->ui.scrollbar.visible = true;
        Vt_visual_scroll_up(vt);
        self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (self->autoscroll == AUTOSCROLL_DN && TimePoint_passed(self->autoscroll_next_step)) {
        Vt_visual_scroll_down(vt);
        self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    }
}

/**
 * @return event was consumed */
static bool App_consume_drag(App* self, uint32_t button, int32_t x, int32_t y)
{
    Vt* vt            = &self->vt;
    self->click_count = 0;
    if (button == MOUSE_BTN_LEFT && self->selection_dragging_left) {
        if (vt->selection.next_mode) {
            Vt_select_commit(vt);
        }
        Vt_select_set_end(vt, x, y);
        return true;
    } else if (button == MOUSE_BTN_RIGHT && self->selection_dragging_right) {
        if (self->selection_dragging_right == SELECT_DRAG_RIGHT_BACK) {
            Vt_select_set_end(vt, x, y);
        } else {
            Vt_select_set_front(vt, x, y);
        }
        return true;
    }
    return false;
}

/**
 * @return does text field consume click event */
static bool App_maybe_consume_click(App*     self,
                                    uint32_t button,
                                    uint32_t state,
                                    int32_t  x,
                                    int32_t  y,
                                    uint32_t mods)
{
    Vt* vt = &self->vt;
    if (!state) {
        self->selection_dragging_left  = false;
        self->selection_dragging_right = SELECT_DRAG_RIGHT_NONE;
        Window_set_pointer_style(self->win, MOUSE_POINTER_ARROW);
    }
    if (vt->modes.x10_mouse_compat) {
        return false;
    }
    bool no_left_report = (!(vt->modes.extended_report || vt->modes.mouse_btn_report ||
                             vt->modes.mouse_motion_on_btn_report) ||
                           FLAG_IS_SET(mods, MODIFIER_SHIFT));

    bool no_middle_report =
      (!(vt->modes.mouse_btn_report || vt->modes.mouse_motion_on_btn_report) ||
       (FLAG_IS_SET(mods, MODIFIER_CONTROL) || FLAG_IS_SET(mods, MODIFIER_SHIFT)));

    if (button == MOUSE_BTN_LEFT && no_left_report) {
        if (!state && vt->selection.mode == SELECT_MODE_NONE) {
            return false;
        }
        if (state) {
            if (!TimePoint_passed(self->next_click_limit)) {
                ++self->click_count;
            } else {
                self->click_count = 0;
            }
            self->next_click_limit = TimePoint_ms_from_now(DOUBLE_CLICK_DELAY_MS);
            if (self->click_count == 0) {
                Vt_select_end(vt);
                Vt_select_init(vt,
                               FLAG_IS_SET(mods, MODIFIER_CONTROL) ? SELECT_MODE_BOX
                                                                   : SELECT_MODE_NORMAL,
                               x,
                               y);
                self->selection_dragging_left = true;
                Window_set_pointer_style(self->win, MOUSE_POINTER_I_BEAM);
            } else if (self->click_count == 1) {
                App_notify_content_change(self);
                Vt_select_end(vt);
                Vt_select_init_word(vt, x, y);
            } else if (self->click_count == 2) {
                App_notify_content_change(self);
                Vt_select_end(vt);
                Vt_select_init_line(vt, y);
                self->click_count = 0;
            }
        }
        return true;

        /* extend selection */
    } else if (button == MOUSE_BTN_RIGHT && state && no_middle_report && vt->selection.mode) {
        size_t clicked_line = Vt_visual_top_line(vt) + y / vt->pixels_per_cell_y;
        if (vt->selection.begin_line == vt->selection.end_line) {
            if (clicked_line < vt->selection.begin_line) {
                Vt_select_set_front(vt, x, y);
                self->selection_dragging_right = SELECT_DRAG_RIGHT_FORNT;
            } else if (clicked_line > vt->selection.begin_line) {
                Vt_select_set_end(vt, x, y);
                self->selection_dragging_right = SELECT_DRAG_RIGHT_BACK;
            } else {
                size_t clicked_cell = x / vt->pixels_per_cell_x,
                       center_cell =
                         (vt->selection.begin_char_idx + vt->selection.end_char_idx) / 2;
                if (clicked_cell > center_cell) {
                    Vt_select_set_end(vt, x, y);
                    self->selection_dragging_right = SELECT_DRAG_RIGHT_BACK;
                } else {
                    Vt_select_set_front(vt, x, y);
                    self->selection_dragging_right = SELECT_DRAG_RIGHT_FORNT;
                }
            }
        } else {
            size_t center_line = (vt->selection.begin_line + vt->selection.end_line) / 2;
            if (clicked_line < center_line) {
                Vt_select_set_front(vt, x, y);
                self->selection_dragging_right = SELECT_DRAG_RIGHT_FORNT;
            } else {
                Vt_select_set_end(vt, x, y);
                self->selection_dragging_right = SELECT_DRAG_RIGHT_BACK;
            }
        }
        Window_set_pointer_style(self->win, MOUSE_POINTER_I_BEAM);

        return true;
        /* paste from primary selection */
    } else if (button == MOUSE_BTN_MIDDLE && state && no_middle_report) {
        if (vt->selection.mode != SELECT_MODE_NONE) {
            Vector_char text = Vt_select_region_to_string(vt);
            Vt_handle_clipboard(&self->vt, text.buf);
            Vector_destroy_char(&text);
        } else {
            ; // TODO: we don't own primary, get it from the window system
        }
    } else if (vt->selection.mode != SELECT_MODE_NONE && state) {
        Window_set_pointer_style(self->win, MOUSE_POINTER_ARROW);
        Vt_select_end(vt);
        return true;
    }
    return false;
}

void App_button_handler(void*    self,
                        uint32_t button,
                        bool     state,
                        int32_t  x,
                        int32_t  y,
                        int32_t  ammount,
                        uint32_t mods)
{
    App* app             = self;
    Vt*  vt              = &app->vt;
    x                    = CLAMP(x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first);
    y                    = CLAMP(y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second);
    bool vt_wants_scroll = Vt_reports_mouse(&app->vt);
    if (!vt_wants_scroll && (button == MOUSE_BTN_WHEEL_DOWN && state)) {
        uint8_t lines             = ammount ? ammount : settings.scroll_discrete_lines;
        app->ui.scrollbar.visible = true;
        for (uint8_t i = 0; i < lines; ++i) {
            Vt_visual_scroll_down(vt);
        }
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (!vt_wants_scroll && (button == MOUSE_BTN_WHEEL_UP && state)) {
        uint8_t lines             = ammount ? ammount : settings.scroll_discrete_lines;
        app->ui.scrollbar.visible = true;
        for (uint8_t i = 0; i < lines; ++i) {
            Vt_visual_scroll_up(vt);
        }
        App_update_scrollbar_vis(self);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (!App_scrollbar_consume_click(self, button, state, x, y) &&
               !App_maybe_consume_click(self, button, state, x, y, mods)) {
        Vt_handle_button(vt, button, state, x, y, ammount, mods);
    }
}

void App_motion_handler(void* self, uint32_t button, int32_t x, int32_t y)
{
    App* app = self;
    x        = CLAMP(x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first);
    y        = CLAMP(y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second);
    if (!App_scrollbar_consume_drag(self, button, x, y) && !App_consume_drag(self, button, x, y)) {
        Vt_handle_motion(&app->vt, button, x, y);
    }
}

void App_destroy_proxy_handler(void* self, VtLineProxy* proxy)
{
    App* app = self;
    Gfx_destroy_proxy(app->gfx, proxy->data);
}

static void App_set_monitor_callbacks(App* self)
{
    self->monitor.callbacks.on_exit   = App_exit_handler;
    self->monitor.callbacks.user_data = self;
}

static void App_set_callbacks(App* self)
{
    self->vt.callbacks.user_data                           = self;
    self->vt.callbacks.on_repaint_required                 = App_notify_content_change;
    self->vt.callbacks.on_clipboard_sent                   = App_clipboard_send;
    self->vt.callbacks.on_clipboard_requested              = App_clipboard_get;
    self->vt.callbacks.on_window_size_requested            = App_window_size;
    self->vt.callbacks.on_window_position_requested        = App_window_position;
    self->vt.callbacks.on_window_size_from_cells_requested = App_pixels;
    self->vt.callbacks.on_number_of_cells_requested        = App_get_char_size;
    self->vt.callbacks.on_title_changed                    = App_update_title;
    self->vt.callbacks.on_bell_flash                       = App_flash;
    self->vt.callbacks.on_action_performed                 = App_action;
    self->vt.callbacks.on_font_reload_requseted            = App_reload_font;
    self->vt.callbacks.destroy_proxy                       = App_destroy_proxy_handler;

    self->win->callbacks.user_data               = self;
    self->win->callbacks.key_handler             = App_key_handler;
    self->win->callbacks.button_handler          = App_button_handler;
    self->win->callbacks.motion_handler          = App_motion_handler;
    self->win->callbacks.clipboard_handler       = App_clipboard_handler;
    self->win->callbacks.activity_notify_handler = App_action;
    self->win->callbacks.on_redraw_requested     = App_redraw;

    self->gfx->callbacks.user_data                   = self;
    self->gfx->callbacks.load_extension_proc_address = App_load_extension_proc_address;

    settings.callbacks.user_data           = self;
    settings.callbacks.keycode_from_string = App_get_key_code;
}

int main(int argc, char** argv)
{
    settings_init(argc, argv);
    App application;
    App_init(&application);
    App_run(&application);
    settings_cleanup();
}
