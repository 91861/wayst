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

typedef struct Wayst
{
    Window_* win;
    Gfx*     gfx;
    Vt       vt;

    Pair_uint32_t resolution;
} App;

static App instance = { NULL };

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
        ERR("Failed to create window");
    }

    App_set_callbacks(self);
    Window_set_swap_interval(self->win, 0);

    gl_load_ext = App_load_gl_ext;

    Gfx_init_with_context_activated(self->gfx);

    Pair_uint32_t size = Window_size(self->win);
    Gfx_resize(self->gfx, size.first, size.second);

    self->resolution = (Pair_uint32_t){ 0, 0 };
}

void App_run(App* self)
{

    while (!Window_closed(self->win) && !self->vt.is_done) {
        Window_events(self->win);

        while (Vt_wait(&self->vt) || Vt_read(&self->vt))
            if (self->vt.is_done)
                break;

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

        if (!!Gfx_update_timers(self->gfx, &self->vt) +
            !!Gfx_set_focus(self->gfx, FLAG_IS_SET(self->win->state_flags,
                                                   WINDOW_IN_FOCUS))) {
            Window_notify_content_change(self->win);
        }

        if (Window_needs_repaint(self->win)) {
            Gfx_draw_vt(self->gfx, &self->vt);
        }

        Window_maybe_swap(self->win);
    }

    Vt_destroy(&self->vt);
    Gfx_destroy(self->gfx);
    Window_destroy(self->win);
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

void App_key_handler(void* self, uint32_t key, uint32_t mods)
{
    Vt_handle_key(&((App*)self)->vt, key, mods);
}

void App_button_handler(void* self, uint32_t button, bool state, int32_t x, int32_t y, int32_t ammount, uint32_t mods)
{
    Vt_handle_button(&((App*)self)->vt, button, state, x, y, ammount, mods);
}

void App_motion_handler(void *self, uint32_t button, int32_t x, int32_t y)
{
    Vt_handle_motion(&((App*)self)->vt, button, x, y);
}

void App_clipboard_handler(void *self, const char *text)
{
    Vt_handle_clipboard(&((App*)self)->vt, text);
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
    self->vt.callbacks.on_bell_flash       = App_flash;
    self->vt.callbacks.on_action_performed = App_action;

    self->win->callbacks.key_handler             = App_key_handler;
    self->win->callbacks.button_handler          = App_button_handler;
    self->win->callbacks.motion_handler          = App_motion_handler;
    self->win->callbacks.clipboard_handler       = App_clipboard_handler;
    self->win->callbacks.activity_notify_handler = App_action;
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
