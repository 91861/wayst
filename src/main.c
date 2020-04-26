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

struct WindowBase* win = NULL;
Vt                 terminal;

__attribute__((constructor)) void on_load()
{
    terminal.pid = 0;
}

__attribute__((destructor)) void on_terminate()
{
    Vt_kill_program(&terminal);
}

static inline void assign_vt_to_window(Vt* vt, struct WindowBase* win)
{
    vt->callbacks.user_data                    = win;
    vt->callbacks.on_repaint_required          = Window_notify_content_change;
    vt->callbacks.on_clipboard_sent            = Window_clipboard_send;
    vt->callbacks.on_clipboard_requested       = Window_clipboard_get;
    vt->callbacks.on_window_size_requested     = Window_size;
    vt->callbacks.on_window_position_requested = Window_position;
    vt->callbacks.on_window_size_from_cells_requested = gfx_pixels;
    vt->callbacks.on_number_of_cells_requested        = gfx_get_char_size;
    vt->callbacks.on_title_changed                    = Window_update_title;
    vt->callbacks.on_bell_flash                       = gfx_flash;
    vt->callbacks.on_action_performed                 = gfx_notify_action;

    win->callbacks.user_data               = vt;
    win->callbacks.key_handler             = Vt_handle_key;
    win->callbacks.button_handler          = Vt_handle_button;
    win->callbacks.motion_handler          = Vt_handle_motion;
    win->callbacks.clipboard_handler       = Vt_handle_clipboard;
    win->callbacks.activity_notify_handler = gfx_notify_action;
}

void* load_gl_ext_app(const char* name)
{
    void* retval = Window_get_proc_adress(win, name);

    if (!retval) {
        ERR("Failed to load extension proc adress for: %s", name);
    }

    LOG("extension proc adress %s : %p\n", name, retval);

    return retval;
}

int main(int argc, char** argv)
{
    settings_init(argc, argv);

    Vt_destroy_line_proxy = gfx_destroy_line_proxy;
    terminal              = Vt_new(settings.cols, settings.rows);

    gfx_load_font();

    if (!settings.x11_is_default)
#ifndef NOWL
        win =
          Window_new_wayland(gfx_pixels(NULL, settings.cols, settings.rows));
#endif
    if (!win) {
#ifndef NOX
        win = Window_new_x11(gfx_pixels(NULL, settings.cols, settings.rows));
#endif
    }

    if (!win) {
        ERR("Failed to create window");
    }

    assign_vt_to_window(&terminal, win);

    Window_set_swap_interval(win, 0);
    gl_load_ext = load_gl_ext_app;

    gfx_init_with_size(Window_size(win));
    Pair_uint32_t res = (Pair_uint32_t){ 0, 0 };

    while (!Window_closed(win) && !terminal.is_done) {
        Window_events(win);

        while (Vt_wait(&terminal) || Vt_read(&terminal))
            if (terminal.is_done)
                break;

        Pair_uint32_t newres = Window_size(win);
        if (newres.first != res.first || newres.second != res.second) {
            res = newres;
            gfx_resize(res.first, res.second);
            Pair_uint32_t chars = gfx_get_char_size(NULL);
            Window_notify_content_change(win);
            Vt_resize(&terminal, chars.first, chars.second);
        }

        if (!!gfx_update_timers(&terminal) +
            !!gfx_set_focus(FLAG_IS_SET(win->state_flags, WINDOW_IN_FOCUS))) {
            Window_notify_content_change(win);
        }

        if (Window_needs_repaint(win))
            gfx_draw_vt(&terminal);

        Window_maybe_swap(win);
    }

    Vt_destroy(&terminal);
    gfx_cleanup();
    Window_destroy(win);
    settings_cleanup();
}
