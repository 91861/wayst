/* See LICENSE for license information. */

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>

#include "gfx_gl21.h"

#ifndef NOWL
#include "wl.h"
#endif

#ifndef NOX
#include "x.h"
#endif

#include "settings.h"
#include "vt.h"

#define SCROLLBAR_HIDE_DELAY_MS 1500
#define DOUBLE_CLICK_DELAY_MS   300
#define AUTOSCROLL_DELAY_MS     50

typedef struct
{
    Window_* win;
    Gfx*     gfx;
    Vt       vt;

    Pair_uint32_t resolution;
    Scrollbar     scrollbar;
} App;

static App instance = { NULL };

static void App_update_scrollbar_dims(App* self);

static void App_update_scrollbar_vis(App* self);

void App_do_autoscroll(App* self);

void* App_load_gl_ext(const char* name)
{
    void* addr = Window_get_proc_adress(instance.win, name);

    if (!addr) {
        ERR("Failed to load extension proc adress for: %s", name);
    }

    return addr;
}

static void App_set_callbacks(App* self);

void App_init(App* self)
{
    self->vt = Vt_new(settings.cols, settings.rows);

    self->gfx = Gfx_new_OpenGL21();

    if (!settings.x11_is_default)
#ifndef NOWL
        self->win = Window_new_wayland(
          Gfx_pixels(self->gfx, settings.cols, settings.rows));
#endif
    if (!self->win) {
#ifndef NOX
        self->win =
          Window_new_x11(Gfx_pixels(self->gfx, settings.cols, settings.rows));
#endif
    }

    if (!self->win) {
        ERR("Failed to create window"
#ifdef NOX
            ", note: compiled without X11 support"
#endif
        );
    }

    App_set_callbacks(self);

    settings_after_window_system_connected();

    Window_set_swap_interval(self->win, 0);

    gl_load_ext = App_load_gl_ext;

    Gfx_init_with_context_activated(self->gfx);

    Pair_uint32_t size = Window_size(self->win);
    Gfx_resize(self->gfx, size.first, size.second);

    Pair_uint32_t chars = Gfx_get_char_size(self->gfx);
    Vt_resize(&self->vt, chars.first, chars.second);

    self->scrollbar.width = 10;

    self->resolution = size;
}

void App_run(App* self)
{
    while (!Window_closed(self->win) && !self->vt.is_done) {
        Window_events(self->win);

        Vt_wait(&self->vt);
        while ((Vt_read(&self->vt)) && !self->vt.is_done)
            ;

        Pair_uint32_t newres = Window_size(self->win);

        if (newres.first != self->resolution.first ||
            newres.second != self->resolution.second) {
            self->resolution = newres;

            Gfx_resize(self->gfx, self->resolution.first,
                       self->resolution.second);
            Pair_uint32_t chars = Gfx_get_char_size(self->gfx);

            Window_notify_content_change(self->win);
            Vt_resize(&self->vt, chars.first, chars.second);
        }

        App_do_autoscroll(self);
        App_update_scrollbar_vis(self);
        App_update_scrollbar_dims(self);

        if (!!Gfx_update_timers(self->gfx, &self->vt, &self->scrollbar) +
            !!Gfx_set_focus(self->gfx, FLAG_IS_SET(self->win->state_flags,
                                                   WINDOW_IN_FOCUS))) {
            Window_notify_content_change(self->win);
        }

        if (Window_needs_repaint(self->win)) {
            Gfx_draw(self->gfx, &self->vt, &self->scrollbar);
        }

        Window_maybe_swap(self->win);
    }

    Vt_destroy(&self->vt);
    Gfx_destroy(self->gfx);
    Window_destroy(self->win);
}

void App_clipboard_handler(void* self, const char* text)
{
    Vt_handle_clipboard(&((App*)self)->vt, text);
}

void App_reload_font(void* self)
{
    Gfx_reload_font(((App*)self)->gfx);
    Gfx_draw(((App*)self)->gfx, &((App*)self)->vt, &((App*)self)->scrollbar);
    Window_notify_content_change(((App*)self)->win);
    Window_maybe_swap(((App*)self)->win);
}

uint32_t App_get_key_code(void* self, char* name)
{
    return Window_get_keysym_from_name(((App*)self)->win, name);
}

void App_destroy_proxy(int32_t proxy[static 4])
{
    Gfx_destroy_proxy(instance.gfx, proxy);
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

/**
 * key commands used by the terminal itself
 * @return keypress was consumed */
static bool App_maybe_handle_application_key(App*     self,
                                             uint32_t key,
                                             uint32_t rawkey,
                                             uint32_t mods)
{
    Vt* vt = &((App*)self)->vt;

    if (KeyCommand_is_active(&settings.key_commands[KCMD_COPY], key, rawkey,
                             mods)) {
        Vector_char txt = Vt_select_region_to_string(vt);
        App_clipboard_handler(self, txt.buf);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_PASTE], key,
                                    rawkey, mods)) {
        App_clipboard_get(self);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_FONT_SHRINK],
                                    key, rawkey, mods)) {
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
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_FONT_ENLARGE],
                                    key, rawkey, mods)) {
        ++settings.font_size;
        Vt_clear_all_proxies(vt);
        App_reload_font(self);
        Pair_uint32_t cells = App_get_char_size(self);
        Vt_resize(vt, cells.first, cells.second);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_DEBUG], key,
                                    rawkey, mods)) {
        Vt_dump_info(vt);
        return true;
    } else if (KeyCommand_is_active(&settings.key_commands[KCMD_UNICODE_ENTRY],
                                    key, rawkey, mods)) {
        Vt_start_unicode_input(vt);
        return true;
    }

    return false;
}

void App_key_handler(void* self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    if (!App_maybe_handle_application_key(self, key, rawkey, mods))
        Vt_handle_key(&((App*)self)->vt, key, rawkey, mods);
}

/**
 * Update gui scrollbar dimensions */
static void App_update_scrollbar_dims(App* self)
{
    Vt* vt                 = &self->vt;
    self->scrollbar.length = 2.0 / vt->lines.size * vt->ws.ws_row;
    self->scrollbar.top =
      2.0 * (double)Vt_visual_top_line(vt) / (vt->lines.size - 1);
}

static bool App_scrollbar_consume_drag(App*     self,
                                       uint32_t button,
                                       int32_t  x,
                                       int32_t  y)
{
    if (!self->scrollbar.dragging)
        return false;

    Vt* vt   = &self->vt;
    y        = CLAMP(y, 0, vt->ws.ws_ypixel);
    float dp = 2.0f * ((float)y / (float)vt->ws.ws_ypixel) -
               self->scrollbar.drag_position;
    float  range       = 2.0f - self->scrollbar.length;
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
    Vt* vt = &self->vt;

    self->scrollbar.autoscroll = AUTOSCROLL_NONE;

    if (!self->scrollbar.visible || button > 3)
        return false;

    if (self->scrollbar.dragging && !state) {
        self->scrollbar.dragging = false;
        App_notify_content_change(self);
        return false;
    }

    float dp = 2.0f * ((float)y / (float)vt->ws.ws_ypixel);

    if (x > vt->ws.ws_xpixel - self->scrollbar.width) {
        // inside region
        if (self->scrollbar.top < dp &&
            self->scrollbar.top + self->scrollbar.length > dp) {
            // inside scrollbar
            if (state &&
                (button == MOUSE_BTN_LEFT || button == MOUSE_BTN_RIGHT ||
                 button == MOUSE_BTN_MIDDLE)) {
                self->scrollbar.dragging      = true;
                self->scrollbar.drag_position = dp - self->scrollbar.top;
            }
        } else {
            // outside of scrollbar
            if (state && button == MOUSE_BTN_LEFT) {
                /* jump to that position and start dragging in the middle */
                self->scrollbar.dragging      = true;
                self->scrollbar.drag_position = self->scrollbar.length / 2;
                dp = 2.0f * ((float)y / (float)vt->ws.ws_ypixel) -
                     self->scrollbar.drag_position;
                float  range = 2.0f - self->scrollbar.length;
                size_t target_line =
                  Vt_top_line(vt) * CLAMP(dp, 0.0, range) / range;
                if (target_line != Vt_visual_top_line(vt)) {
                    Vt_visual_scroll_to(vt, target_line);
                }
            } else if (state && button == MOUSE_BTN_RIGHT) {
                self->scrollbar.autoscroll_next_step =
                  TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);

                if (dp > self->scrollbar.top + self->scrollbar.length / 2) {
                    self->scrollbar.autoscroll = AUTOSCROLL_DN;
                } else {
                    self->scrollbar.autoscroll = AUTOSCROLL_UP;
                }

            } else if (state && button == MOUSE_BTN_MIDDLE) {
                /* jump one screen in that direction */
                if (dp > self->scrollbar.top + self->scrollbar.length / 2) {
                    Vt_visual_scroll_to(vt,
                                        vt->visual_scroll_top + vt->ws.ws_row);
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
    Vt*         vt             = &self->vt;
    static bool last_scrolling = false;
    if (!vt->scrolling) {
        if (last_scrolling) {
            self->scrollbar.hide_time =
              TimePoint_ms_from_now(SCROLLBAR_HIDE_DELAY_MS);
        } else if (self->scrollbar.dragging) {
            self->scrollbar.hide_time =
              TimePoint_ms_from_now(SCROLLBAR_HIDE_DELAY_MS);
        } else if (TimePoint_passed(self->scrollbar.hide_time)) {
            if (self->scrollbar.visible) {
                self->scrollbar.visible = false;
                App_notify_content_change(self);
            }
        }
    }
    last_scrolling = vt->scrolling;
}

void App_do_autoscroll(App* self)
{
    Vt* vt = &self->vt;
    App_update_scrollbar_vis(self);

    if (self->scrollbar.autoscroll == AUTOSCROLL_UP &&
        TimePoint_passed(self->scrollbar.autoscroll_next_step)) {
        self->scrollbar.visible = true;
        Vt_visual_scroll_up(vt);
        self->scrollbar.autoscroll_next_step =
          TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (self->scrollbar.autoscroll == AUTOSCROLL_DN &&
               TimePoint_passed(self->scrollbar.autoscroll_next_step)) {
        Vt_visual_scroll_down(vt);
        self->scrollbar.autoscroll_next_step =
          TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    }
}

void App_button_handler(void*    self,
                        uint32_t button,
                        bool     state,
                        int32_t  x,
                        int32_t  y,
                        int32_t  ammount,
                        uint32_t mods)
{
    Vt* vt = &((App*)self)->vt;
    if (button == MOUSE_BTN_WHEEL_DOWN && state) {
        uint8_t lines = ammount ? ammount : settings.scroll_discrete_lines;
        ((App*)self)->scrollbar.visible = true;
        for (uint8_t i = 0; i < lines; ++i)
            Vt_visual_scroll_down(vt);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (button == MOUSE_BTN_WHEEL_UP && state) {
        uint8_t lines = ammount ? ammount : settings.scroll_discrete_lines;
        ((App*)self)->scrollbar.visible = true;
        for (uint8_t i = 0; i < lines; ++i)
            Vt_visual_scroll_up(vt);
        App_update_scrollbar_vis(self);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (!App_scrollbar_consume_click(self, button, state, x, y)) {
        Vt_handle_button(&((App*)self)->vt, button, state, x, y, ammount, mods);
    }
}

void App_motion_handler(void* self, uint32_t button, int32_t x, int32_t y)
{
    if (!App_scrollbar_consume_drag(self, button, x, y))
        Vt_handle_motion(&((App*)self)->vt, button, x, y);
}

static void App_set_callbacks(App* self)
{
    Vt_destroy_line_proxy = App_destroy_proxy;

    self->vt.callbacks.user_data   = self;
    self->win->callbacks.user_data = self;

    self->vt.callbacks.on_repaint_required          = App_notify_content_change;
    self->vt.callbacks.on_clipboard_sent            = App_clipboard_send;
    self->vt.callbacks.on_clipboard_requested       = App_clipboard_get;
    self->vt.callbacks.on_window_size_requested     = App_window_size;
    self->vt.callbacks.on_window_position_requested = App_window_position;
    self->vt.callbacks.on_window_size_from_cells_requested = App_pixels;
    self->vt.callbacks.on_number_of_cells_requested        = App_get_char_size;
    self->vt.callbacks.on_title_changed                    = App_update_title;
    self->vt.callbacks.on_bell_flash                       = App_flash;
    self->vt.callbacks.on_action_performed                 = App_action;
    self->vt.callbacks.on_font_reload_requseted            = App_reload_font;

    self->win->callbacks.key_handler             = App_key_handler;
    self->win->callbacks.button_handler          = App_button_handler;
    self->win->callbacks.motion_handler          = App_motion_handler;
    self->win->callbacks.clipboard_handler       = App_clipboard_handler;
    self->win->callbacks.activity_notify_handler = App_action;

    settings.callbacks.user_data           = self;
    settings.callbacks.keycode_from_string = App_get_key_code;
}

__attribute__((destructor)) void destructor()
{
    Vt_kill_program(&instance.vt);
}

int main(int argc, char** argv)
{
    settings_init(argc, argv);

    App_init(&instance);
    App_run(&instance);

    settings_cleanup();
}
