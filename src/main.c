/* See LICENSE for license information. */

/**
 * App links Vt, Monitor, Window, Gfx modules together and deals with updating ui
 *
 * TODO:
 * - It makes more sense for the unicode input prompt to be here
 * - Move bell animations here
 * - Some way of dealing with multiple timers that set poll timeout
 * - Multiple vt instacnes within the same application
 * - Move ui updates to ui.c
 * - vt.size != window.size, because tabbar/splits, maybe wrap Vt in a VtWidget:IWidget class? Then
 *   we can add a SpliterWidget:IWidget nesting other widgets and forwarding mouse/kbd events etc.
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

#include "fmt.h"
#include "freetype.h"
#include "html.h"
#include "key.h"
#include "settings.h"
#include "ui.h"
#include "vt.h"

#ifndef DOUBLE_CLICK_DELAY_MS
#define DOUBLE_CLICK_DELAY_MS 300
#endif

#ifndef AUTOSCROLL_DELAY_MS
#define AUTOSCROLL_DELAY_MS 50
#endif

#ifndef KSM_CLEAR_INPUT_BUFFER_DELAY_MS
#define KSM_CLEAR_INPUT_BUFFER_DELAY_MS 1000
#endif

#ifndef AUTOSCROLL_TRIGGER_MARGIN_PX
#define AUTOSCROLL_TRIGGER_MARGIN_PX 2
#endif

typedef struct
{
    Window_* win;
    Gfx*     gfx;
    Vt       vt;
    Freetype freetype;
    Monitor  monitor;

    /* Bytes written since reading from pty */
    int written_bytes;

    Pair_uint32_t resolution;

    bool       swap_performed;
    TimePoint* closest_pending_wakeup;
    TimePoint  interpreter_start_time;

    bool exit;

    char* hostname;

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
    TimePoint    scrollbar_hide_time;
    TimePoint    autoscroll_next_step;
    float        scrollbar_drag_position;
    bool         last_scrolling;
    bool         autoselect;
    Pair_int32_t autoscroll_autoselect;

    Cursor ksm_cursor;

    Vector_char ksm_input_buf;

    TimePoint ksm_last_input;

    bool      do_title_refresh;
    TimePoint next_title_refresh;

    Ui ui;

    char* vt_title;

    Vector_char queued_output_buffer;

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
static void App_do_autoscroll(App* self);
static void App_notify_content_change(void* self);
static void App_clamp_cursor(App* self, Pair_uint32_t chars);
static void App_set_monitor_callbacks(App* self);
static void App_set_callbacks(App* self);
static void App_maybe_resize(App* self, Pair_uint32_t newres);
static void App_handle_uri(App* self, const char* uri);
static void App_update_hover(App* self, int32_t x, int32_t y);
static void App_update_padding(App* self);
static void App_set_title(void* self);
static void App_focus_changed(void* self, bool current_state);

static void* App_load_extension_proc_address(void* self, const char* name)
{
    App*  app  = self;
    void* addr = Window_get_proc_adress(app->win, name);
    if (!addr) {
        ERR("Failed to load extension proc adress for: %s", name);
    }
    return addr;
}

static void App_create_window(App* self, Pair_uint32_t res)
{
#if !defined(NOX) && !defined(NOWL)
    if (!settings.x11_is_default)
        self->win = Window_new_wayland(res);
    if (!self->win) {
        self->win = Window_new_x11(res);
    }
#elif !defined(NOWL)
    self->win = Window_new_wayland(res);
#else
    self->win = Window_new_x11(res);
#endif

    if (!self->win) {
        ERR("Failed to create window"
#ifdef NOX
            ", note: compiled without X11 support"
#endif
        );
    }
}

static void App_init(App* self)
{
    memset(self, 0, sizeof(App));

    self->ksm_input_buf = Vector_new_char();

    self->monitor = Monitor_new();

    App_set_monitor_callbacks(self);

    if (settings.directory.str) {
        if (chdir(settings.directory.str)) {
            WRN("Failed to change directory: %s\n", strerror(errno));
        }
    }

    Monitor_fork_new_pty(&self->monitor, settings.cols, settings.rows);

    Vt_init(&self->vt, settings.cols, settings.rows);
    self->vt.master_fd = self->monitor.child_fd;
    self->freetype     = Freetype_new();
    self->gfx          = Gfx_new_OpenGL21(&self->freetype);

    App_create_window(self, Gfx_pixels(self->gfx, settings.cols, settings.rows));
    App_set_callbacks(self);

    /* We may have gotten events during initialization. We can ignore everything except for focus */
    App_focus_changed(self, FLAG_IS_SET(self->win->state_flags, WINDOW_IS_IN_FOCUS));

    settings_after_window_system_connected();
    Window_set_swap_interval(self->win, 0);
    Gfx_init_with_context_activated(self->gfx);

    Pair_uint32_t size = Window_size(self->win);
    Gfx_resize(self->gfx, size.first, size.second);

    Pair_uint32_t chars = Gfx_get_char_size(self->gfx);
    Vt_resize(&self->vt, chars.first, chars.second);

    Monitor_watch_window_system_fd(&self->monitor, Window_get_connection_fd(self->win));

    self->ui.scrollbar.width     = settings.scrollbar_width_px;
    self->ui.hovered_link.active = false;
    self->swap_performed         = false;
    self->resolution             = size;

    Window_events(self->win);

    /* kwin ignores the initial egl surface size and every window resize request before the first
     * event dispatch */
    char *xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP"),
         *xdg_session_type    = getenv("XDG_SESSION_TYPE");
    if (xdg_current_desktop && xdg_session_type && !strcmp(xdg_current_desktop, "KDE") &&
        !strcmp(xdg_session_type, "wayland")) {
        Window_resize(self->win, size.first, size.second);
    }

    App_update_padding(self);

    if (!settings.dynamic_title) {
        Window_update_title(self->win, settings.title.str);
    }
}

static void App_run(App* self)
{
    while (!(self->exit || Window_is_closed(self->win))) {
        int timeout_ms = 0;

        if (!self->swap_performed && self->vt.output.size == 0) {
            if (self->closest_pending_wakeup) {
                timeout_ms = TimePoint_is_ms_ahead(*(self->closest_pending_wakeup));
            } else {
                timeout_ms = -1;
            }
        }

        Monitor_wait(&self->monitor, timeout_ms);
        self->closest_pending_wakeup = NULL;

        if (Monitor_are_window_system_events_pending(&self->monitor)) {
            Window_events(self->win);
        }

        TimePoint* pending_window_timer = Window_process_timers(self->win);
        if (pending_window_timer) {
            self->closest_pending_wakeup = pending_window_timer;
        }

        ssize_t bytes                = 0;
        self->interpreter_start_time = TimePoint_now();
        do {
            if (unlikely(settings.debug_slow)) {
                usleep(5000);
                App_notify_content_change(self);
            } else if (unlikely(TimePoint_is_ms_ahead(TimePoint_now()) >
                                settings.pty_chunk_timeout_ms)) {
                break;
            }

            self->written_bytes = 0;
            bytes               = Monitor_read(&self->monitor);

            if (bytes > 0) {
                Vt_interpret(&self->vt, self->monitor.input_buffer, bytes);
                Gfx_notify_action(self->gfx);
            } else {
                break;
            }

            if (settings.pty_chunk_wait_delay_ns) {
                usleep(settings.pty_chunk_wait_delay_ns);
            }
        } while (bytes && likely(!settings.debug_slow));

        char*        buf;
        size_t       len;
        Vector_char* out = Vt_get_output(&self->vt, PIPE_BUF, &buf, &len);
        if (out) {
            if (out->size) {
                Monitor_write(&self->monitor, out->buf, self->written_bytes = out->size);
            }
        } else if (len) {
            Monitor_write(&self->monitor, buf, self->written_bytes = len);
        }

        App_maybe_resize(self, Window_size(self->win));

        if (self->ui.scrollbar.visible || self->vt.scrolling_visual ||
            self->autoscroll != AUTOSCROLL_NONE) {
            App_do_autoscroll(self);
            App_update_scrollbar_vis(self);
            App_update_scrollbar_dims(self);
        }

        App_update_cursor(self);

        if (self->do_title_refresh && TimePoint_passed(self->next_title_refresh)) {
            App_set_title(self);
        }

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

        if ((self->autoscroll && !self->closest_pending_wakeup) ||
            (self->autoscroll && self->closest_pending_wakeup &&
             TimePoint_is_earlier(self->autoscroll_next_step, *self->closest_pending_wakeup))) {
            self->closest_pending_wakeup = &self->autoscroll_next_step;
        }

        if ((self->do_title_refresh && !self->closest_pending_wakeup) ||
            (self->do_title_refresh && self->closest_pending_wakeup &&
             TimePoint_is_earlier(self->next_title_refresh, *self->closest_pending_wakeup))) {
            self->closest_pending_wakeup = &self->next_title_refresh;
        }

        self->swap_performed = Window_maybe_swap(self->win);
    }

    Monitor_kill(&self->monitor);
    Vt_destroy(&self->vt);
    Gfx_destroy(self->gfx);
    Freetype_destroy(&self->freetype);
    Window_destroy(self->win);
    Vector_destroy_char(&self->ksm_input_buf);
    free(self->hostname);
    free(self->vt_title);
}

static void App_immediate_write_pty(void* self, char* buf, size_t size)
{
    App* app = self;
    app->written_bytes += size;
    Monitor_write(&app->monitor, buf, size);
}

/* Whenever some other application sets primary selection while we are out of focus, upon focus gain
 * we should end our selection */
static void App_primary_claimed_by_other_client(void* self)
{
    App* app = self;

    if (app->vt.selection.mode) {
        LOG("App::selection_claimed\n");
        Vt_select_end(&app->vt);
    }
}

static void App_selection_end_handler(void* self)
{
    // App* app = self;
    LOG("App::selection_end\n");
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

        if (settings.dynamic_title) {
            App_set_title(self);
        }
    }
}

static void App_clipboard_handler(void* self, const char* text)
{
    App* app = self;
    Vt_handle_clipboard(&app->vt, text);
}

static void App_reload_font(void* self)
{
    App* app = self;
    Freetype_reload_fonts(&app->freetype);
    Gfx_reload_font(app->gfx);
    Gfx_draw(app->gfx, &app->vt, &app->ui);
    App_update_padding(self);
    Window_notify_content_change(app->win);
    Window_maybe_swap(app->win);
}

static uint32_t App_get_key_code(void* self, char* name)
{
    return Window_get_keysym_from_name(((App*)self)->win, name);
}

static void App_notify_content_change(void* self)
{
    Window_notify_content_change(((App*)self)->win);
}

static void App_clipboard_send(void* self, const char* text)
{
    Window_clipboard_send(((App*)self)->win, text);
}

static void App_primary_send(void* self, const char* text)
{
    Window_primary_send(((App*)self)->win, text);
}

static void App_clipboard_get(void* self)
{
    Window_clipboard_get(((App*)self)->win);
}

static Pair_uint32_t App_window_size(void* self)
{
    return Window_size(((App*)self)->win);
}

static Pair_uint32_t App_window_position(void* self)
{
    return Window_position(((App*)self)->win);
}

static Pair_uint32_t App_pixels(void* self, uint32_t rows, uint32_t columns)
{
    return Gfx_pixels(((App*)self)->gfx, rows, columns);
}

static Pair_uint32_t App_get_char_size(void* self)
{
    return Gfx_get_char_size(((App*)self)->gfx);
}

static void App_set_title(void* self)
{
    App* app = self;

    if (!settings.dynamic_title) {
        return;
    }

    const VtCommand* c                   = Vt_shell_integration_get_active_command(&app->vt);
    char*            sAppTitle           = settings.title.str;
    char*            sVtTitle            = app->vt_title;
    bool             bIsReportingMouse   = Vt_is_reporting_mouse(&app->vt);
    bool             bIsAltBufferEnabled = Vt_alt_buffer_enabled(&app->vt);
    /* we do not know what command is running if its using the VTE prompts */
    bool      bCommandIsRunning = c && c->command;
    char*     sRunningCommand   = c ? c->command : "";
    TimePoint now               = TimePoint_now();
    if (c) {
        TimePoint_subtract(&now, c->execution_time.start);
        app->next_title_refresh = TimePoint_s_from_now(1);
        app->do_title_refresh   = true;
    } else {
        app->do_title_refresh = false;
    }
    int32_t i32CommandTimeSec = !c ? 0.0 : TimePoint_get_secs(&now);
    int32_t i32Rows           = Vt_row(&app->vt);
    int32_t i32Cols           = Vt_col(&app->vt);
    int32_t i32Width          = app->win->w;
    int32_t i32Height         = app->win->h;

    char* err        = NULL;
    char* fmtd_title = fmt_new_interpolated(settings.title_format.str,
                                            &err,
                                            &FMT_ARG_STR(sAppTitle),
                                            &FMT_ARG_STR(sVtTitle),
                                            &FMT_ARG_BOOL(bCommandIsRunning),
                                            &FMT_ARG_BOOL(bIsReportingMouse),
                                            &FMT_ARG_BOOL(bIsAltBufferEnabled),
                                            &FMT_ARG_I32(i32CommandTimeSec),
                                            &FMT_ARG_STR(sRunningCommand),
                                            &FMT_ARG_I32(i32Rows),
                                            &FMT_ARG_I32(i32Cols),
                                            &FMT_ARG_I32(i32Width),
                                            &FMT_ARG_I32(i32Height),
                                            NULL);

    static bool threw_warn = false;
    if (err && !threw_warn) {
        threw_warn = true;
        WRN("error in title format string: %s\n", err);
    }

    Window_update_title(app->win, fmtd_title);
    free(fmtd_title);
}

static void App_update_title(void* self, const char* title)
{
    App* app = self;
    free(app->vt_title);
    app->vt_title = strdup(title);
    App_set_title(self);
}

static void App_command_changed(void* self)
{
    App_set_title(self);
}

static void App_buffer_changed(void* self)
{
    App_set_title(self);
}

static void App_mouse_report_changed(void* self)
{
    App_set_title(self);
}

static void App_flash(void* self)
{
    Gfx_flash(((App*)self)->gfx);
}

static void App_action(void* self)
{
    App* app = self;
    Gfx_notify_action(app->gfx);

    if (!Window_is_pointer_hidden(app->win)) {
        App_update_hover(
          app,
          CLAMP(app->win->pointer_x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first),
          CLAMP(app->win->pointer_y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second));
    }
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

static int App_get_ksm_number(App* self)
{
    if (!self->ksm_input_buf.size) {
        return 0;
    }
    Vector_push_char(&self->ksm_input_buf, '\0');
    int res = atoi(self->ksm_input_buf.buf);
    Vector_clear_char(&self->ksm_input_buf);
    return res;
}

/**
 * key commands used in keyboard select mode
 * @return exit ksm mode */
static bool App_handle_keyboard_select_mode_key(App*     self,
                                                uint32_t key,
                                                uint32_t rawkey,
                                                uint32_t mods)
{
    Vt* vt = &self->vt;

#define L_UPDATE_SELECT_END                                                                        \
    if (self->vt.selection.mode) {                                                                 \
        size_t col = self->ksm_cursor.col;                                                         \
        size_t row = self->ksm_cursor.row - Vt_visual_top_line(vt);                                \
        Vt_select_set_end_cell(vt, col, row);                                                      \
    }

    TimePoint now = TimePoint_now();
    TimePoint_subtract(&now, self->ksm_last_input);
    if (TimePoint_get_ms(now) > KSM_CLEAR_INPUT_BUFFER_DELAY_MS) {
        Vector_clear_char(&self->ksm_input_buf);
    }
    self->ksm_last_input = TimePoint_now();

    switch (rawkey) {
        case KEY(Escape):
            Vt_select_end(vt);
            App_notify_content_change(self);
            self->keyboard_select_mode = false;
            Vt_visual_scroll_reset(vt);
            Gfx_notify_action(self->gfx);
            return true;

        case KEY(m):
            App_notify_content_change(self);
            self->ksm_cursor.row = Vt_visual_top_line(vt) + Vt_row(vt) / 2;
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(Left):
        case KEY(h):
            App_notify_content_change(self);
            if (rawkey == KEY(h) && FLAG_IS_SET(mods, MODIFIER_SHIFT)) {
                self->ksm_cursor.row = Vt_visual_top_line(vt);
            } else {
                int p = App_get_ksm_number(self);
                for (int i = MAX(1, p); i > 0; --i) {
                    if (self->ksm_cursor.col) {
                        self->ksm_cursor.col--;
                    }
                }
            }
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(Down):
        case KEY(j):
            App_notify_content_change(self);
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                if (self->ksm_cursor.row < Vt_max_line(vt)) {
                    self->ksm_cursor.row++;
                }
                if (Vt_visual_bottom_line(vt) == Vt_max_line(vt)) {
                    Vt_visual_scroll_reset(vt);
                } else {
                    while (Vt_visual_bottom_line(vt) < self->ksm_cursor.row) {
                        if (Vt_visual_scroll_down(vt)) {
                            break;
                        }
                    }
                }
            }
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(Up):
        case KEY(k): {
            App_notify_content_change(self);
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                if (self->ksm_cursor.row) {
                    self->ksm_cursor.row--;
                }
                while (Vt_visual_top_line(vt) > self->ksm_cursor.row) {
                    if (Vt_visual_scroll_up(vt)) {
                        break;
                    }
                }
            }
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
        } break;

        case KEY(Right):
        case KEY(l):
            App_notify_content_change(self);
            if (rawkey == KEY(l) && FLAG_IS_SET(mods, MODIFIER_SHIFT)) {
                self->ksm_cursor.row = Vt_visual_bottom_line(vt);
            } else {
                int p = App_get_ksm_number(self);
                for (int i = MAX(1, p); i > 0; --i) {
                    if (self->ksm_cursor.col + 1 < Vt_col(vt)) {
                        self->ksm_cursor.col++;
                    }
                }
            }
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(y):
        case KEY(c): {
            if (self->vt.selection.mode) {
                Vector_char txt = Vt_select_region_to_string(vt);
                if (txt.size) {
                    App_clipboard_send(self, txt.buf);
                    Vt_select_end(vt);
                } else {
                    Vector_destroy_char(&txt);
                }
            }
        }
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(Return): {
            if (FLAG_IS_SET(mods, MODIFIER_CONTROL)) {
                const char* uri = Vt_uri_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                if (uri) {
                    App_handle_uri(self, uri);
                    break;
                }
            }
        }
            /* fallthrough */
        case KEY(v):
            if (self->vt.selection.mode) {
                Vt_select_end(vt);
                break;
            }
            size_t          col = self->ksm_cursor.col;
            size_t          row = (self->ksm_cursor.row) - Vt_visual_top_line(vt);
            enum SelectMode mode =
              FLAG_IS_SET(mods, MODIFIER_CONTROL) ? SELECT_MODE_BOX : SELECT_MODE_NORMAL;
            Vt_select_init_cell(vt, mode, col, row);
            Vt_select_commit(vt);
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(b): {
            // jump back by word
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                for (bool initial = true;; initial = false) {
                    if (!self->ksm_cursor.col) {
                        break;
                    }
                    VtRune* rune      = Vt_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                    VtRune* prev_rune = Vt_at(vt, self->ksm_cursor.col - 1, self->ksm_cursor.row);
                    if (!rune || !prev_rune) {
                        break;
                    }
                    char32_t code = rune->rune.code, prev_code = prev_rune->rune.code;

                    if (isblank(prev_code) && !isblank(code) && !initial) {
                        break;
                    }
                    --self->ksm_cursor.col;
                    App_notify_content_change(self);
                }
            }
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
        } break;

        case KEY(w): {
            // jump forward to next word
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                for (bool initial = true;; initial = false) {
                    VtLine* row_line = Vt_line_at(vt, self->ksm_cursor.row);
                    if (self->ksm_cursor.col + 1 >= Vt_col(vt) || !row_line ||
                        self->ksm_cursor.col + 1 >= (uint16_t)row_line->data.size) {
                        break;
                    }
                    VtRune* rune      = Vt_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                    VtRune* next_rune = Vt_at(vt, self->ksm_cursor.col + 1, self->ksm_cursor.row);
                    if (!rune || !next_rune) {
                        break;
                    }
                    char32_t code = rune->rune.code, next_code = next_rune->rune.code;
                    ++self->ksm_cursor.col;
                    App_notify_content_change(self);
                    if ((isblank(code) && !isblank(next_code)) && !initial) {
                        break;
                    }
                }
            }
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
        } break;

        case KEY(e): {
            // jump to end of word
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                for (bool initial = true;; initial = false) {
                    VtLine* row_line = Vt_line_at(vt, self->ksm_cursor.row);
                    if (self->ksm_cursor.col + 1 >= Vt_col(vt) || !row_line ||
                        self->ksm_cursor.col + 1 >= (uint16_t)row_line->data.size) {
                        break;
                    }
                    VtRune*  rune      = Vt_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                    VtRune*  next_rune = Vt_at(vt, self->ksm_cursor.col + 1, self->ksm_cursor.row);
                    char32_t code = rune->rune.code, next_code = next_rune->rune.code;
                    if ((isblank(next_code) && !isblank(code)) && !initial) {
                        break;
                    }
                    ++self->ksm_cursor.col;
                }
            }
            App_notify_content_change(self);
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
        } break;

        case KEY(g):
            if (FLAG_IS_SET(mods, MODIFIER_SHIFT)) {
                Vt_visual_scroll_to(vt, Vt_top_line(vt));
                self->ksm_cursor.row = Vt_max_line(vt);
            } else if (self->ksm_input_buf.size) {
                size_t tgt           = App_get_ksm_number(self);
                tgt                  = MIN(tgt, Vt_max_line(vt));
                self->ksm_cursor.row = tgt;
                if (tgt > Vt_visual_top_line(vt) && tgt < Vt_visual_bottom_line(vt)) {
                } else {
                    if (tgt < Vt_visual_top_line(vt)) {
                        Vt_visual_scroll_to(vt, tgt);
                    } else {
                        Vt_visual_scroll_to(vt, tgt - Vt_row(vt) + 1);
                    }
                }
            } else {
                Vt_visual_scroll_to(vt, 0);
                self->ksm_cursor.row = 0;
            }
            App_notify_content_change(self);
            L_UPDATE_SELECT_END
            break;

        case KEY(0):
            if (self->ksm_input_buf.size) {
                if (self->ksm_input_buf.size < 3) {
                    Vector_push_char(&self->ksm_input_buf, '0');
                }
            } else {
                App_notify_content_change(self);
                self->ksm_cursor.col = 0;
                L_UPDATE_SELECT_END
            }
            break;

        case KEY(4):
            // jump to last non-blank in line
            if (FLAG_IS_SET(mods, MODIFIER_SHIFT)) {
                App_notify_content_change(self);
                size_t last          = Vt_line_at(vt, self->ksm_cursor.row)->data.size - 1;
                self->ksm_cursor.col = 0;
                for (size_t i = last; i > 0; --i) {
                    VtRune* rune = Vt_at(vt, i, self->ksm_cursor.row);
                    if (!rune) {
                        break;
                    }
                    char32_t code = rune->rune.code;
                    if (code != VT_RUNE_CODE_WIDE_TAIL && !isblank(code)) {
                        self->ksm_cursor.col = i;
                        break;
                    }
                }
                L_UPDATE_SELECT_END
            } else {
                if (self->ksm_input_buf.size < 3) {
                    Vector_push_char(&self->ksm_input_buf, '4');
                }
            }
            break;

            // TODO: case KEY(5):

        case KEY(6):
            // jump to first non-blank in line
            if (FLAG_IS_SET(mods, MODIFIER_SHIFT)) {
                App_notify_content_change(self);
                self->ksm_cursor.col = 0;
                for (size_t i = 0; i < Vt_line_at(vt, self->ksm_cursor.row)->data.size; ++i) {
                    VtRune* rune = Vt_at(vt, i, self->ksm_cursor.row);
                    if (!rune) {
                        break;
                    }
                    char32_t code = rune->rune.code;
                    if (code != VT_RUNE_CODE_WIDE_TAIL && !isblank(code)) {
                        self->ksm_cursor.col = i;
                        break;
                    }
                }
                L_UPDATE_SELECT_END
            } else {
                if (self->ksm_input_buf.size < 3) {
                    Vector_push_char(&self->ksm_input_buf, '6');
                }
            }
            break;

        case KEY(1)... KEY(3):
        case KEY(5):
        case KEY(7)... KEY(9):
            if (self->ksm_input_buf.size < 3) {
                Vector_push_char(&self->ksm_input_buf, rawkey - KEY(1) + '1');
            }
            break;

        default:
            LOG("KSM key: %d(%d)\n", key, rawkey);
    }
    return false;
}

static void App_do_extern_pipe(App* self)
{
    if (!settings.extern_pipe_handler.str) {
        WRN("external pipe not configured\n");
        App_flash(self);
        return;
    }
    enum extern_pipe_source_e source  = EXTERN_PIPE_SOURCE_BUFFER;
    const VtCommand*          command = NULL;
    Vector_char               content;
    switch (settings.extern_pipe_source) {
        case EXTERN_PIPE_SOURCE_COMMAND: {
            command = Vt_get_last_completed_command(&self->vt);
            if (command) {
                source  = EXTERN_PIPE_SOURCE_COMMAND;
                content = Vt_command_to_string(&self->vt, command, 0);
                break;
            }
        }
        /* fallthrough */
        case EXTERN_PIPE_SOURCE_VIEWPORT:
            source  = EXTERN_PIPE_SOURCE_VIEWPORT;
            content = Vt_region_to_string(&self->vt,
                                          Vt_visual_top_line(&self->vt),
                                          Vt_visual_bottom_line(&self->vt));
            break;
        case EXTERN_PIPE_SOURCE_BUFFER:
            content = Vt_region_to_string(&self->vt, 0, Vt_bottom_line(&self->vt));
            break;
        default:
            ASSERT_UNREACHABLE;
    }

    if (content.size > 1) {
        int   ti = 0;
        char  tmp[16][128];
        int   argc = 0;
        char* argv[16];
        argv[argc++] = settings.extern_pipe_handler.str;

        snprintf(tmp[ti], sizeof(tmp[ti]), "--rows=%u", Vt_row(&self->vt));
        argv[argc++] = tmp[ti++];
        snprintf(tmp[ti], sizeof(tmp[ti]), "--columns=%u", Vt_col(&self->vt));
        argv[argc++] = tmp[ti++];

        snprintf(tmp[ti],
                 sizeof(tmp[ti]),
                 "--app-id=%s",
                 OR(settings.user_app_id, APPLICATION_NAME));
        argv[argc++] = tmp[ti++];

        snprintf(tmp[ti], sizeof(tmp[ti]), "--pid=%u", getpid());
        argv[argc++] = tmp[ti++];

        snprintf(tmp[ti], sizeof(tmp[ti]), "--title=%s", self->vt.title);
        argv[argc++] = tmp[ti++];

        int64_t window_id = Window_get_window_id(self->win);
        if (window_id >= 0) {
            snprintf(tmp[ti], sizeof(tmp[ti]), "--x-window-id=%ld", window_id);
            argv[argc++] = tmp[ti++];
        }

        switch (source) {
            case EXTERN_PIPE_SOURCE_COMMAND:
                snprintf(tmp[ti], sizeof(tmp[ti]), "--command=%s", command->command);
                argv[argc++] = tmp[ti++];
                snprintf(tmp[ti], sizeof(tmp[ti]), "--command-exit-code=%d", command->exit_status);
                argv[argc++] = tmp[ti++];
                break;
            default:;
        }
        argv[argc++] = NULL;

        int pipe_input_fd = spawn_process(NULL, argv[0], argv, true, true);
        if (pipe_input_fd) {
            if (write(pipe_input_fd, content.buf, content.size - 1) == -1) {
                WRN("extern pipe write failed: %s\n", strerror(errno));
            }
            close(pipe_input_fd);
        }
    } else
        App_flash(self);

    Vector_destroy_char(&content);
}

/**
 * key commands used by the terminal itself
 * @return keypress was consumed */
static bool App_maybe_handle_application_key(App*     self,
                                             uint32_t key,
                                             uint32_t rawkey,
                                             uint32_t mods)
{
    Vt*         vt  = &self->vt;
    KeyCommand* cmd = settings.key_commands;

    if (self->keyboard_select_mode) {
        if (!(self->keyboard_select_mode =
                !App_handle_keyboard_select_mode_key(self, key, rawkey, mods))) {
            App_notify_content_change(self);
        }
        return true;
    }

    if (KeyCommand_is_active(&cmd[KCMD_COPY], key, rawkey, mods)) {
        if (vt->selection.mode) {
            Vector_char txt = Vt_select_region_to_string(vt);
            if (txt.size)
                App_clipboard_send(self, txt.buf);
            else
                Vector_destroy_char(&txt);
        }
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_PASTE], key, rawkey, mods)) {
        App_clipboard_get(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_FONT_SHRINK], key, rawkey, mods)) {
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
    } else if (KeyCommand_is_active(&cmd[KCMD_FONT_ENLARGE], key, rawkey, mods)) {
        ++settings.font_size;
        Vt_clear_all_proxies(vt);
        App_reload_font(self);
        Pair_uint32_t cells = App_get_char_size(self);
        Vt_resize(vt, cells.first, cells.second);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_HTML_DUMP], key, rawkey, mods)) {
        time_t     current_time;
        struct tm* calendar_time;
        char *     path = NULL, *prefix = NULL, time_string[128];
        FILE*      file;
        if ((time(&current_time) == -1) || (!(calendar_time = localtime(&current_time))) ||
            (!strftime(time_string, sizeof(time_string), "%Y.%m.%d.%H.%M.%S", calendar_time)) ||
            (!(prefix = getcwd(NULL, 0))) ||
            (!(path = asprintf("%s/" APPLICATION_NAME ".%s.html", prefix, time_string))) ||
            (!(file = fopen(path, "w+x")))) {
            WRN("Failed to create screen dump: %s\n", strerror(errno));
        } else {
            write_html_screen_dump(vt, file);
            fclose(file);
        }
        free(prefix);
        free(path);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_DEBUG], key, rawkey, mods)) {
        Vt_dump_info(vt);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_UNICODE_ENTRY], key, rawkey, mods)) {
        Vt_start_unicode_input(vt);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_KEYBOARD_SELECT], key, rawkey, mods)) {
        Vector_clear_char(&self->ksm_input_buf);
        self->ksm_cursor           = self->vt.cursor;
        self->ksm_cursor.blinking  = false;
        self->ui.cursor            = &self->ksm_cursor;
        self->keyboard_select_mode = true;
        self->ksm_cursor.row =
          CLAMP(self->ksm_cursor.row, Vt_visual_top_line(vt), Vt_visual_bottom_line(vt));
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_LINE_SCROLL_UP], key, rawkey, mods)) {
        Vt_visual_scroll_up(vt);
        self->ui.scrollbar.visible = true;
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_LINE_SCROLL_DN], key, rawkey, mods)) {
        Vt_visual_scroll_down(vt);
        self->ui.scrollbar.visible = true;
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_PAGE_SCROLL_UP], key, rawkey, mods)) {
        Vt_visual_scroll_page_up(vt);
        self->ui.scrollbar.visible = true;
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_PAGE_SCROLL_DN], key, rawkey, mods)) {
        Vt_visual_scroll_page_down(vt);
        self->ui.scrollbar.visible = true;
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_MARK_SCROLL_UP], key, rawkey, mods)) {
        if (Vt_visual_scroll_mark_up(vt)) {
            self->ui.scrollbar.visible = true;
            App_notify_content_change(self);
        } else
            App_flash(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_MARK_SCROLL_DN], key, rawkey, mods)) {
        if (Vt_visual_scroll_mark_down(vt)) {
            self->ui.scrollbar.visible = true;
            App_notify_content_change(self);
        } else
            App_flash(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_DUPLICATE], key, rawkey, mods)) {
        /* use the full path to the running file (it may have been started with a relative path) */
        char* this_file  = get_running_binary_path();
        char* old_argv_0 = settings.argv[0];
        settings.argv[0] = this_file;
        spawn_process(Vt_get_work_directory(vt), settings.argv[0], settings.argv, true, false);
        settings.argv[0] = old_argv_0;
        free(this_file);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_COPY_OUTPUT], key, rawkey, mods)) {
        const VtCommand* command = Vt_get_last_completed_command(vt);
        if (command && command->output_rows.first != command->output_rows.second) {
            Vector_char txt = Vt_command_to_string(vt, command, 0);
            if (txt.size > 1) {
                App_clipboard_send(self, txt.buf);
            } else {
                Vector_destroy_char(&txt);
                App_flash(self);
            }
            return true;
        }
        App_flash(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_EXTERN_PIPE], key, rawkey, mods)) {
        App_do_extern_pipe(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_OPEN_PWD], key, rawkey, mods)) {
        const char* wd = Vt_get_work_directory(&self->vt);
        if (wd)
            App_handle_uri(self, wd);
        else
            App_flash(self);
        return true;
    }
    return false;
}

void App_gui_pointer_mode_change_handler(void* self)
{
#define L_SET_POINTER_STYLE                                                                        \
    if (app->selection_dragging_left || app->selection_dragging_right) {                           \
        Window_set_pointer_style(app->win, MOUSE_POINTER_I_BEAM);                                  \
    } else {                                                                                       \
        Window_set_pointer_style(app->win, MOUSE_POINTER_ARROW);                                   \
    }

    App* app = self;
    switch (app->vt.gui_pointer_mode) {
        case VT_GUI_POINTER_MODE_FORCE_HIDE:
        case VT_GUI_POINTER_MODE_HIDE:
            Window_set_pointer_style(app->win, MOUSE_POINTER_HIDDEN);
            break;
        case VT_GUI_POINTER_MODE_FORCE_SHOW:
        case VT_GUI_POINTER_MODE_SHOW:
            L_SET_POINTER_STYLE;
            break;
        case VT_GUI_POINTER_MODE_SHOW_IF_REPORTING:
            if (Vt_is_reporting_mouse(&app->vt)) {
                L_SET_POINTER_STYLE;
            } else {
                Window_set_pointer_style(app->win, MOUSE_POINTER_HIDDEN);
            }
            break;
    }
}

void App_key_handler(void* self, uint32_t key, uint32_t rawkey, uint32_t mods)
{
    if (!App_maybe_handle_application_key(self, key, rawkey, mods)) {
        App* app = self;
        switch (app->vt.gui_pointer_mode) {
            case VT_GUI_POINTER_MODE_FORCE_HIDE:
            case VT_GUI_POINTER_MODE_HIDE:
                Window_set_pointer_style(app->win, MOUSE_POINTER_HIDDEN);
                break;

            case VT_GUI_POINTER_MODE_SHOW_IF_REPORTING:
                if (!Vt_reports_mouse(&app->vt)) {
                    Window_set_pointer_style(app->win, MOUSE_POINTER_HIDDEN);
                }
                break;
            default:;
        }
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
    double  minimum_length    = (2.0 / self->resolution.second) * settings.scrollbar_length_px;
    double  length            = 2.0 / vt->lines.size * vt->ws.ws_row;
    double  extra_length      = length > minimum_length ? 0.0 : (minimum_length - length);
    int32_t scrollable_lines  = Vt_top_line(&self->vt);
    int32_t lines_scrolled    = Vt_visual_top_line(&self->vt);
    double  fraction_scrolled = (double)lines_scrolled / scrollable_lines;
    self->ui.scrollbar.length = length + extra_length;
    self->ui.scrollbar.top    = (2.0 - length - extra_length) * fraction_scrolled;
    int64_t ms                = TimePoint_is_ms_ahead(self->scrollbar_hide_time);

    if (ms > 0 && ms < settings.scrollbar_fade_time_ms && !self->vt.scrolling_visual) {
        self->ui.scrollbar.opacity = ((float)ms / settings.scrollbar_fade_time_ms);
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
    Vt* vt = &self->vt;

    if (!self->selection_dragging_left) {
        self->autoscroll = AUTOSCROLL_NONE;
    }

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
                    Vt_visual_scroll_page_down(vt);
                } else {
                    Vt_visual_scroll_page_up(vt);
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
            self->scrollbar_hide_time = TimePoint_ms_from_now(settings.scrollbar_hide_delay_ms);
        } else if (self->ui.scrollbar.dragging) {
            self->scrollbar_hide_time = TimePoint_ms_from_now(settings.scrollbar_hide_delay_ms);
        } else if (TimePoint_passed(self->scrollbar_hide_time)) {
            if (self->ui.scrollbar.visible) {
                self->ui.scrollbar.visible = false;
                App_notify_content_change(self);
            }
        }
    }
    self->last_scrolling = vt->scrolling_visual;
}

static void App_do_autoscroll(App* self)
{
    Vt* vt = &self->vt;
    App_update_scrollbar_vis(self);

    if (self->autoscroll == AUTOSCROLL_UP && TimePoint_passed(self->autoscroll_next_step)) {
        LOG("App::do_autoscroll{ direction: up }\n");
        self->ui.scrollbar.visible = true;
        Vt_visual_scroll_up(vt);
        self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (self->autoscroll == AUTOSCROLL_DN && TimePoint_passed(self->autoscroll_next_step)) {
        LOG("App::do_autoscroll{ direction: down }\n");
        Vt_visual_scroll_down(vt);
        self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    }

    if (self->autoselect) {
        Vt_select_set_end(&self->vt,
                          self->autoscroll_autoselect.first,
                          self->autoscroll_autoselect.second);
    }
}

/**
 * @return event was consumed */
static bool App_consume_drag(App* self, uint32_t button, int32_t x, int32_t y)
{
    Vt* vt = &self->vt;

    self->click_count = 0;
    if (button == MOUSE_BTN_LEFT && self->selection_dragging_left) {
        int32_t high_bound = self->ui.pixel_offset_y + AUTOSCROLL_TRIGGER_MARGIN_PX;
        int32_t low_bound  = self->win->h - self->ui.pixel_offset_y - AUTOSCROLL_TRIGGER_MARGIN_PX;

        LOG("App::select_drag{ %d x %d ", x, y);

        if (y <= high_bound) {
            self->autoselect = true;
            if (!self->autoscroll) {
                self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
            }
            self->autoscroll = AUTOSCROLL_UP;
            Vt_select_set_end(
              vt,
              (self->autoscroll_autoselect.first = x),
              (self->autoscroll_autoselect.second = high_bound - AUTOSCROLL_TRIGGER_MARGIN_PX));
            App_update_scrollbar_dims(self);
            App_notify_content_change(self);
            LOG("(start autoscroll up) }\n");
        } else if (y >= low_bound) {
            self->autoselect = true;
            if (!self->autoscroll) {
                self->autoscroll_next_step = TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS);
            }
            self->autoscroll = AUTOSCROLL_DN;
            Vt_select_set_end(
              vt,
              (self->autoscroll_autoselect.first = x),
              (self->autoscroll_autoselect.second = low_bound + AUTOSCROLL_TRIGGER_MARGIN_PX));
            App_update_scrollbar_dims(self);
            App_notify_content_change(self);
            LOG("(start autoscroll down) }\n");
        } else {
            LOG("}\n");

            self->autoselect = false;
            self->autoscroll = AUTOSCROLL_NONE;
            if (vt->selection.next_mode) {
                Vt_select_commit(vt);
            }
            Vt_select_set_end(vt, x, y);
        }

        return true;
    } else if (button == MOUSE_BTN_RIGHT && self->selection_dragging_right) {
        self->autoselect = false;
        self->autoscroll = AUTOSCROLL_NONE;
        LOG("App::select_modify_drag{ %d x %d set selection point: ", x, y);
        if (self->selection_dragging_right == SELECT_DRAG_RIGHT_BACK) {
            LOG("end }\n");
            Vt_select_set_end(vt, x, y);
        } else {
            LOG("front }\n");
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
                enum SelectMode mode =
                  FLAG_IS_SET(mods, MODIFIER_CONTROL) ? SELECT_MODE_BOX : SELECT_MODE_NORMAL;
                Vt_select_init(vt, mode, x, y);
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
            /* we don't own primary, get it from the window system */
            Window_primary_get(self->win);
        }
    } else if (vt->selection.mode != SELECT_MODE_NONE && state) {
        Window_set_pointer_style(self->win, MOUSE_POINTER_ARROW);
        Vt_select_end(vt);
        return true;
    }
    return false;
}

static void App_button_handler(void*    self,
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
    app->autoselect      = false;

    if (!vt_wants_scroll && (button == MOUSE_BTN_WHEEL_DOWN && state)) {
        uint8_t lines             = ammount ? ammount : settings.scroll_discrete_lines;
        app->ui.scrollbar.visible = true;

        for (uint8_t i = 0; i < lines; ++i) {
            if (Vt_visual_scroll_down(vt)) {
                break;
            }
        }

        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (!vt_wants_scroll && (button == MOUSE_BTN_WHEEL_UP && state)) {
        uint8_t lines             = ammount ? ammount : settings.scroll_discrete_lines;
        app->ui.scrollbar.visible = true;

        for (uint8_t i = 0; i < lines; ++i) {
            if (Vt_visual_scroll_up(vt)) {
                break;
            }
        }

        App_update_scrollbar_vis(self);
        App_update_scrollbar_dims(self);
        App_notify_content_change(self);
    } else if (!App_scrollbar_consume_click(self, button, state, x, y) &&
               !App_maybe_consume_click(self, button, state, x, y, mods)) {
        /* Maybe theres a link there */
        if (FLAG_IS_SET(mods, MODIFIER_CONTROL)) {
            Pair_uint16_t cells = Vt_pixels_to_cells(vt, x, y);
            cells.second += Vt_visual_top_line(vt);
            const char* uri = Vt_uri_at(vt, cells.first, cells.second);

            if (uri) {
                App_handle_uri(self, uri);
                return;
            }
        }
        Vt_handle_button(vt, button, state, x, y, ammount, mods);
    }
}

static void App_update_hover(App* self, int32_t x, int32_t y)
{
    if (self->selection_dragging_left || self->selection_dragging_right)
        return;

    self->autoscroll = AUTOSCROLL_NONE;

    Pair_uint16_t cells = Vt_pixels_to_cells(&self->vt, x, y);
    cells.second += Vt_visual_top_line(&self->vt);

    if (Vt_uri_at(&self->vt, cells.first, cells.second)) {
        if (self->win->current_pointer_style != MOUSE_POINTER_HAND) {
            Window_set_pointer_style(self->win, MOUSE_POINTER_HAND);
        }

        Pair_size_t   rows;
        Pair_uint16_t cols;
        const char*   uri;

        if ((uri = Vt_uri_range_at(&self->vt, cells.first, cells.second, &rows, &cols))) {
            hovered_link_t new_hovered_link = {
                .active         = true,
                .start_line_idx = rows.first,
                .end_line_idx   = rows.second,
                .start_cell_idx = cols.first,
                .end_cell_idx   = cols.second,
            };

            if (memcmp(&new_hovered_link, &self->ui.hovered_link, sizeof(new_hovered_link))) {
                self->ui.hovered_link = new_hovered_link;
                App_notify_content_change(self);

                LOG("App::link hover{ [%dx%d]:\'%s\': %ux%zu - %ux%zu}\n",
                    x,
                    y,
                    uri,
                    cols.first,
                    rows.first,
                    cols.second,
                    rows.second);
            }
        }
    } else {
        if (self->ui.hovered_link.active) {
            self->ui.hovered_link.active = false;
            App_notify_content_change(self);
        }

        if (self->win->current_pointer_style != MOUSE_POINTER_ARROW) {
            Window_set_pointer_style(self->win, MOUSE_POINTER_ARROW);
        }
    }
}

static void App_motion_handler(void* self, uint32_t button, int32_t x, int32_t y)
{
    App* app = self;
    x        = CLAMP(x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first);
    y        = CLAMP(y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second);

    if (button) {
        if (!App_scrollbar_consume_drag(self, button, x, y) &&
            !App_consume_drag(self, button, x, y)) {
            Vt_handle_motion(&app->vt, button, x, y);
        }
    } else {
        App_update_hover(app, x, y);
    }
}

static void App_destroy_proxy_handler(void* self, VtLineProxy* proxy)
{
    App* app = self;
    Gfx_destroy_proxy(app->gfx, proxy->data);
}

static void App_destroy_image_proxy_handler(void* self, VtImageSurfaceProxy* proxy)
{
    App* app = self;
    Gfx_destroy_image_proxy(app->gfx, proxy->data);
}

static void App_destroy_sixel_proxy_handler(void* self, VtSixelSurfaceProxy* proxy)
{
    App* app = self;
    Gfx_destroy_sixel_proxy(app->gfx, proxy->data);
}

static void App_destroy_image_view_proxy_handler(void* self, VtImageSurfaceViewProxy* proxy)
{
    App* app = self;
    Gfx_destroy_image_view_proxy(app->gfx, proxy->data);
}

static void App_set_monitor_callbacks(App* self)
{
    self->monitor.callbacks.on_exit   = App_exit_handler;
    self->monitor.callbacks.user_data = self;
}

static void App_handle_uri(App* self, const char* uri)
{
    LOG("App::handle_uri{ \'%s\' }\n", uri);
    const char* argv[] = { settings.uri_handler.str, uri, NULL };
    spawn_process(Vt_get_work_directory(&self->vt), argv[0], (char**)argv, true, false);
}

static void App_send_desktop_notification(void* self, const char* opt_title, const char* text)
{
    LOG("App::send_desktop_notification{ title: %s text: %s }\n", opt_title, text);
    const char* argv[] = { "notify-send", OR(opt_title, text), opt_title ? text : NULL, NULL };
    spawn_process(NULL, argv[0], (char**)argv, true, false);
}

static void App_focus_changed(void* self, bool current_state)
{
    App* app = self;
    Window_notify_content_change(app->win);

    app->ui.draw_out_of_focus_tint = !current_state;

    if (!current_state && app->vt.selection.mode) {
        Vector_char txt = Vt_select_region_to_string(&app->vt);
        if (txt.size) {
            App_primary_send(self, txt.buf);
        } else {
            Vector_destroy_char(&txt);
        }
    }
}

static bool App_minimized(void* self)
{
    App* app = self;
    return Window_is_minimized(app->win);
}

static bool App_fullscreen(void* self)
{
    App* app = self;
    return Window_is_fullscreen(app->win);
}

static void App_set_maximized_state(void* self, bool maximize)
{
    App* app = self;
    Window_set_maximized(app->win, maximize);
}

static void App_set_fullscreen_state(void* self, bool fullscreen)
{
    App* app = self;
    Window_set_fullscreen(app->win, fullscreen);
}

static void App_set_window_size(void* self, int32_t width, int32_t height)
{
    App* app = self;
    Window_resize(app->win, width, height);
}

static void App_set_text_area_size(void* self, int32_t width, int32_t height)
{
    App* app = self;
    Window_resize(app->win, width + 2 * settings.padding, height + 2 * settings.padding);
}

static Pair_uint32_t App_text_area_size(void* self)
{
    App* app = self;

    Pair_uint32_t win_size = Window_size(app->win);
    win_size.first -= settings.padding;
    win_size.second -= settings.padding;

    return win_size;
}

static void App_set_urgent(void* self)
{
    if (!Window_is_focused(((App*)self)->win))
        Window_set_urgent(((App*)self)->win);
}

static void App_restack_to_front(void* self)
{
    Window_set_stack_order(((App*)self)->win, true);
}

static const char* App_get_hostname(void* self)
{
    App* app = self;

    if (!app->hostname) {
        app->hostname = get_hostname();
    }

    return app->hostname;
}

static void App_set_callbacks(App* self)
{
    self->vt.callbacks.user_data                           = self;
    self->vt.callbacks.on_repaint_required                 = App_notify_content_change;
    self->vt.callbacks.on_clipboard_sent                   = App_clipboard_send;
    self->vt.callbacks.on_clipboard_requested              = App_clipboard_get;
    self->vt.callbacks.on_window_size_requested            = App_window_size;
    self->vt.callbacks.on_text_area_size_requested         = App_text_area_size;
    self->vt.callbacks.on_window_position_requested        = App_window_position;
    self->vt.callbacks.on_window_size_from_cells_requested = App_pixels;
    self->vt.callbacks.on_number_of_cells_requested        = App_get_char_size;
    self->vt.callbacks.on_minimized_state_requested        = App_minimized;
    self->vt.callbacks.on_fullscreen_state_requested       = App_fullscreen;
    self->vt.callbacks.on_title_changed                    = App_update_title;
    self->vt.callbacks.on_visual_bell                      = App_flash;
    self->vt.callbacks.on_window_maximize_state_set        = App_set_maximized_state;
    self->vt.callbacks.on_window_fullscreen_state_set      = App_set_fullscreen_state;
    self->vt.callbacks.on_window_dimensions_set            = App_set_window_size;
    self->vt.callbacks.on_text_area_dimensions_set         = App_set_text_area_size;
    self->vt.callbacks.on_action_performed                 = App_action;
    self->vt.callbacks.on_font_reload_requseted            = App_reload_font;
    self->vt.callbacks.on_desktop_notification_sent        = App_send_desktop_notification;
    self->vt.callbacks.on_urgency_set                      = App_set_urgent,
    self->vt.callbacks.on_restack_to_front                 = App_restack_to_front,
    self->vt.callbacks.on_application_hostname_requested   = App_get_hostname;
    self->vt.callbacks.destroy_proxy                       = App_destroy_proxy_handler;
    self->vt.callbacks.destroy_image_proxy                 = App_destroy_image_proxy_handler;
    self->vt.callbacks.destroy_image_view_proxy            = App_destroy_image_view_proxy_handler;
    self->vt.callbacks.destroy_sixel_proxy                 = App_destroy_sixel_proxy_handler;
    self->vt.callbacks.on_select_end                       = App_selection_end_handler;
    self->vt.callbacks.immediate_pty_write                 = App_immediate_write_pty;
    self->vt.callbacks.on_command_state_changed            = App_command_changed;
    self->vt.callbacks.on_mouse_report_state_changed       = App_mouse_report_changed;
    self->vt.callbacks.on_buffer_changed                   = App_buffer_changed;
    self->vt.callbacks.on_gui_pointer_mode_changed         = App_gui_pointer_mode_change_handler;

    self->win->callbacks.user_data               = self;
    self->win->callbacks.key_handler             = App_key_handler;
    self->win->callbacks.button_handler          = App_button_handler;
    self->win->callbacks.motion_handler          = App_motion_handler;
    self->win->callbacks.clipboard_handler       = App_clipboard_handler;
    self->win->callbacks.activity_notify_handler = App_action;
    self->win->callbacks.on_redraw_requested     = App_redraw;
    self->win->callbacks.on_focus_changed        = App_focus_changed;
    self->win->callbacks.on_primary_changed      = App_primary_claimed_by_other_client;

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
