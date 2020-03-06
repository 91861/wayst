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

#include "vt.h"
#include "settings.h"



struct WindowBase* win = NULL;
Vt terminal;


__attribute__((constructor))
void
on_load()
{
    terminal.pid = 0;
}


__attribute__((destructor))
void
on_terminate()
{
    Vt_kill_program(&terminal);
}


static inline void
assign_vt_to_window(Vt* self, const struct WindowBase* const win)
{
    self->window_data = (void*) win;
    self->window_itable = win->subclass_interface;
    self->on_title_update = Window_update_title;
    self->repaint_required_notify = Window_notify_content_change2;

    self->get_position = Window_position2;
    self->get_pixels = Window_size2;
}

void*
load_gl_ext_app(const char* name)
{
    void* retval = Window_get_proc_adress(win, name);

    if (!retval) {
        ERR("Failed to load extension proc adress for: %s", name);
    }

    LOG("extension proc adress %s : %p\n", name , retval);

    return retval;
}

int
main(int argc, char** argv)
{
    settings_init(argc, argv);
    Vt_destroy_line_proxy = gfx_destroy_line_proxy;
    terminal = Vt_new(settings.cols, settings.rows);
    gl_init_font();
    if (!settings.x11_is_default)
        #ifndef NOWL
        win = Window_new_wayland(gl_pixels(settings.cols, settings.rows),
                                 &terminal,
                                 &Vt_handle_key,
                                 &Vt_handle_button,
                                 &Vt_handle_motion,
                                 &Vt_handle_clipboard);
        #endif
    if (!win) {
        #ifndef NOX
        win = Window_new_x11(gl_pixels(settings.cols, settings.rows),
                             &terminal,
                             &Vt_handle_key,
                             &Vt_handle_button,
                             &Vt_handle_motion,
                             &Vt_handle_clipboard);
        #endif
    }

    if (!win) {
        ERR("Failed to create window");
    }

    Window_set_swap_interval(win, 0);
    assign_vt_to_window(&terminal, win);
    gl_load_ext = load_gl_ext_app;
    gl_init_renderer_with_size(Window_size(win));
    Pair_uint32_t res = (Pair_uint32_t) { 0, 0 };
    while (!Window_closed(win) && !terminal.is_done) {
        Window_events(win);

        if (Vt_wait(&terminal) || Vt_read(&terminal))
            continue;

        Pair_uint32_t newres = Window_size(win);
        if (newres.first != res.first || newres.second != res.second) {
            res = newres;
            gl_set_size(res.first, res.second);
            Pair_uint32_t chars = gl_get_char_size();
            Window_notify_content_change(win);
            Vt_resize(&terminal, chars.first, chars.second);
        }

        if (!!gl_check_timers(&terminal) +
            !!gl_set_focus(FLAG_IS_SET(win->state_flags, WINDOW_IN_FOCUS)))
        {
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

