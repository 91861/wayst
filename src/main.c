/* See LICENSE for license information. */

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>

#include "gfx.h"

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
    Vt       vt;
    /* Gfx* renderer; */

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
    gfx_load_font();

    if (!settings.x11_is_default)
#ifndef NOWL
        self->win =
          Window_new_wayland(gfx_pixels(NULL, settings.cols, settings.rows));
#endif
    if (!self->win) {
#ifndef NOX
        self->win =
          Window_new_x11(gfx_pixels(NULL, settings.cols, settings.rows));
#endif
    }

    if (!self->win) {
        ERR("Failed to create window");
    }

    App_set_callbacks(self);
    Window_set_swap_interval(self->win, 0);

    gl_load_ext = App_load_gl_ext;
    gfx_init_with_size(Window_size(self->win));

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
            gfx_resize(self->resolution.first, self->resolution.second);
            Pair_uint32_t chars = gfx_get_char_size(NULL);
            Window_notify_content_change(self->win);
            Vt_resize(&self->vt, chars.first, chars.second);
        }

        if (!!gfx_update_timers(&self->vt) +
            !!gfx_set_focus(
              FLAG_IS_SET(self->win->state_flags, WINDOW_IN_FOCUS))) {
            Window_notify_content_change(self->win);
        }

        if (Window_needs_repaint(self->win))
            gfx_draw_vt(&self->vt);

        Window_maybe_swap(self->win);
    }

    Vt_destroy(&self->vt);
    gfx_cleanup();
    Window_destroy(self->win);
}

static void App_set_callbacks(App* self)
{
    self->vt.callbacks.user_data                = self->win;
    self->vt.callbacks.on_repaint_required      = Window_notify_content_change;
    self->vt.callbacks.on_clipboard_sent        = Window_clipboard_send;
    self->vt.callbacks.on_clipboard_requested   = Window_clipboard_get;
    self->vt.callbacks.on_window_size_requested = Window_size;
    self->vt.callbacks.on_window_position_requested        = Window_position;
    self->vt.callbacks.on_window_size_from_cells_requested = gfx_pixels;
    self->vt.callbacks.on_number_of_cells_requested        = gfx_get_char_size;
    self->vt.callbacks.on_title_changed    = Window_update_title;
    self->vt.callbacks.on_bell_flash       = gfx_flash;
    self->vt.callbacks.on_action_performed = gfx_notify_action;

    Vt_destroy_line_proxy = gfx_destroy_line_proxy;

    self->win->callbacks.user_data               = &self->vt;
    self->win->callbacks.key_handler             = Vt_handle_key;
    self->win->callbacks.button_handler          = Vt_handle_button;
    self->win->callbacks.motion_handler          = Vt_handle_motion;
    self->win->callbacks.clipboard_handler       = Vt_handle_clipboard;
    self->win->callbacks.activity_notify_handler = gfx_notify_action;
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
