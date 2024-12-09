/* See LICENSE for license information. */

/**
 * App links Vt, Monitor, Window, Gfx modules together and deals with updating ui
 *
 * TODO:
 * - It makes more sense for the unicode input prompt to be here
 * - Multiple vt instacnes within the same application
 * - Move ui updates to ui.c
 * - vt.size != window.size, because tabbar/splits, maybe wrap Vt in a VtWidget:IWidget class? Then
 *   we can add a SpliterWidget:IWidget nesting other widgets and forwarding mouse/kbd events etc.
 */

#define _GNU_SOURCE
#include "gfx.h"
#include "timing.h"
#include "util.h"
#include "window.h"
#include <stdint.h>

#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "gfx_gl2.h"

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
#include "monitor.h"
#include "settings.h"
#include "ui.h"
#include "vt.h"

#ifndef DOUBLE_CLICK_DELAY_MS
#define DOUBLE_CLICK_DELAY_MS 300
#endif

#ifndef VISUAL_BELL_FLASH_DURATION_MS
#define VISUAL_BELL_FLASH_DURATION_MS 300
#endif

#ifndef AUTOSCROLL_DELAY_MS
#define AUTOSCROLL_DELAY_MS 30
#endif

#ifndef KSM_CLEAR_INPUT_BUFFER_DELAY_MS
#define KSM_CLEAR_INPUT_BUFFER_DELAY_MS 1000
#endif

#ifndef AUTOSCROLL_TRIGGER_MARGIN_PX
#define AUTOSCROLL_TRIGGER_MARGIN_PX 2
#endif

typedef struct
{
    WindowBase*  win;
    Gfx*         gfx;
    Vt           vt;
    Freetype     freetype;
    Monitor      monitor;
    Ui           ui;
    TimerManager timer_manager;

    ssize_t       written_bytes; /* Bytes written since reading from pty */
    Pair_int32_t  autoscroll_autoselect;
    Pair_uint32_t resolution;

    TimePoint* closest_pending_wakeup;
    TimePoint  interpreter_start_time;
    TimePoint  next_click_limit;

    Timer autoscroll_timer, scrollbar_hide_timer, visual_bell_timer, cursor_blink_end_timer,
      cursor_blink_switch_timer, cursor_blink_anim_delay_timer, cursor_blink_suspend_timer,
      text_blink_switch_timer, title_update_timer, cursor_movement_timer, cursor_fade_timer;

    char* hostname;
    char* vt_title;

    enum AutoscrollDir
    {
        AUTOSCROLL_NONE = 0,
        AUTOSCROLL_UP   = 1,
        AUTOSCROLL_DN   = -1,
    } autoscroll;

    enum SelectionDragRight
    {
        SELECT_DRAG_RIGHT_NONE = 0,
        SELECT_DRAG_RIGHT_FORNT,
        SELECT_DRAG_RIGHT_BACK
    } selection_dragging_right;

    float   scrollbar_drag_position;
    uint8_t click_count;
    bool    selection_dragging_left;
    bool    keyboard_select_mode;
    bool    last_scrolling;
    bool    autoselect;
    bool    exit;

    /* we are in a time period after kbd input when the cursor should blink */
    bool cursor_blink_animation_should_play;

    /* do vt modes allow cursor blinking */
    bool cursor_blink_animation_should_play_vt;

    bool tex_blink_animation_should_play;
    bool csd_mode_changed_before_resize;
    bool did_paint_in_sync_update_mode;

    VtCursor    ksm_cursor;
    Vector_char ksm_input_buf;
    TimePoint   ksm_last_input;
} App;

static void          App_update_scrollbar_dims(App* self);
static void          App_update_cursor(App* self);
static void          App_do_autoscroll(App* self);
static void          App_notify_content_change(void* self);
static void          App_maybe_clamp_ksm_cursor(App* self, Pair_uint32_t chars);
static void          App_set_monitor_callbacks(App* self);
static void          App_set_callbacks(App* self);
static void          App_set_up_timers(App* self);
static void          App_maybe_resize(App* self, Pair_uint32_t newres);
static void          App_handle_uri(App* self, const char* uri);
static void          App_update_hover(App* self, int32_t x, int32_t y);
static void          App_update_padding(App* self);
static void          App_set_title(void* self);
static void          App_focus_changed(void* self, bool current_state);
static void          App_action(void* self);
static void          App_framebuffer_damage(void* self);
static Pair_uint32_t App_get_char_size(void* self);
static bool          App_fullscreen(void* self);
static void          App_maybe_swap_window(App* self);
static void          App_csd_changed(void* self, ui_csd_mode_e csd_mode);
static void          App_primary_output_changed(void*         self,
                                                const int32_t display_index,
                                                const char*   display_name,
                                                lcd_filter_e  new_filter,
                                                uint16_t      physical_dpi);

static void* App_load_extension_proc_address(void* self, const char* name)
{
    App*  app  = self;
    void* addr = Window_get_proc_adress(app->win, name);
    if (!addr) {
        ERR("Failed to load extension proc adress for: %s", name);
    }
    return addr;
}

static void App_create_window(App* self, Pair_uint32_t res, Pair_uint32_t cell_dims)
{
#ifndef GFX_GLES
    gfx_api_t gfx = (gfx_api_t){
        .type          = GFX_API_GL,
        .version_major = 2,
        .version_minor = 1,
    };

#else
    gfx_api_t gfx = (gfx_api_t){
        .type          = GFX_API_GLES,
        .version_major = 2,
        .version_minor = 0,
    };

#endif

#if !defined(NOX) && !defined(NOWL)
    if (!settings.x11_is_default)
        self->win = Window_new_wayland(res, cell_dims, gfx, &self->ui);
    if (!self->win) {
        self->win = Window_new_x11(res, cell_dims, gfx, &self->ui);
    }
#elif !defined(NOWL)
    self->win = Window_new_wayland(res, cell_dims, gfx, &self->ui);
#else
    self->win = Window_new_x11(res, cell_dims, gfx, &self->ui);
#endif

    if (!self->win) {
        ERR("Failed to create window"
#ifdef NOX
            ", note: compiled without X11 support"
#endif
        );
    }
}

static void App_show_scrollbar(App* self)
{
    TimerManager_schedule_tween(
      &self->timer_manager,
      self->scrollbar_hide_timer,
      TimePoint_ms_from_now(settings.scrollbar_hide_delay_ms),
      TimePoint_ms_from_now(settings.scrollbar_hide_delay_ms + settings.scrollbar_fade_time_ms));
    self->ui.scrollbar.visible = true;
    self->ui.scrollbar.opacity = 1.0f;
    App_notify_content_change(self);
}

static void App_autoscroll_timer_handler(void* self)
{
    App_do_autoscroll(self);
    App_update_scrollbar_dims(self);
    App_notify_content_change(self);
}

static void App_scrollbar_hide_timer_handler(void* self, double fraction, bool completed)
{
    App* app = self;

    if (Vt_is_scrolling_visual(&((App*)self)->vt)) {
        App_show_scrollbar(self);
        return;
    }

    if (completed) {
        app->ui.scrollbar.visible = false;
    } else {
        App_notify_content_change(self);
        app->ui.scrollbar.visible = true;
        app->ui.scrollbar.opacity = 1.0 - fraction;
    }

    App_update_scrollbar_dims(app);
}

/* Suspend cursor blinking for a short time afer kbd input */
static void App_cursor_blink_suspend_timer_handler(void* self)
{
    App* app                                = self;
    app->cursor_blink_animation_should_play = true;
    app->ui.draw_cursor_blinking            = true;
    app->ui.cursor_fade_fraction            = 1.0;

    if (settings.animate_cursor_blink) {
        TimerManager_schedule_tween_from_now(
          &app->timer_manager,
          app->cursor_blink_switch_timer,
          TimePoint_ms_from_now((float)settings.cursor_blink_interval_ms *
                                settings.animate_cursor_blink_fade_fraction));
        app->ui.draw_cursor_blinking = true;
    } else {
        TimerManager_schedule_point(&app->timer_manager,
                                    app->cursor_blink_switch_timer,
                                    TimePoint_ms_from_now(settings.cursor_blink_interval_ms));
    }

    App_notify_content_change(self);
}

/* Stop cursor from blinking alfer a long time of inactivity */
static void App_cursor_blink_end_timer_handler(void* self)
{
    App* app                                = self;
    app->cursor_blink_animation_should_play = false;
}

/* Switch between cursor shown/hidden in non-animated mode */
static void App_cursor_blink_switch_timer_handler(void* self)
{
    App* app = self;
    bool ui_condition =
      ((app->cursor_blink_animation_should_play) || !app->ui.draw_cursor_blinking);

    if (ui_condition && Window_is_focused(app->win)) {
        app->ui.draw_cursor_blinking = !app->ui.draw_cursor_blinking;
        TimerManager_schedule_point(&app->timer_manager,
                                    app->cursor_blink_switch_timer,
                                    TimePoint_ms_from_now(settings.cursor_blink_interval_ms));
    } else {
        app->ui.draw_cursor_blinking = true;
    }

    app->ui.cursor_fade_fraction = app->ui.draw_cursor_blinking;
    App_notify_content_change(self);
}

/* Delay before animating cursor blink */
static void App_cursor_fade_switch_delay_timer_handler(void* self)
{
    App* app = self;

    if (app->cursor_blink_animation_should_play || app->ui.draw_cursor_blinking) {
        app->ui.draw_cursor_blinking = !app->ui.draw_cursor_blinking;
        TimerManager_schedule_tween_from_now(
          &app->timer_manager,
          app->cursor_blink_switch_timer,
          TimePoint_ms_from_now((float)settings.cursor_blink_interval_ms *
                                settings.animate_cursor_blink_fade_fraction));
    }
}

/* Switch between cursor shown/hidden in animated mode */
static void App_cursor_fade_switch_timer_handler(void* self, double fraction, bool completed)
{
    App* app                     = self;
    app->ui.cursor_fade_fraction = app->ui.draw_cursor_blinking ? (1.0 - fraction) : fraction;

    if (completed) {
        if (app->cursor_blink_animation_should_play || app->ui.draw_cursor_blinking) {
            TimerManager_schedule_point(
              &app->timer_manager,
              app->cursor_blink_anim_delay_timer,
              TimePoint_ms_from_now((float)settings.cursor_blink_interval_ms *
                                    (1.0f - settings.animate_cursor_blink_fade_fraction)));
        }
    }

    if (likely(Window_is_focused(app->win))) {
        App_notify_content_change(self);
    } else {
        app->ui.cursor_fade_fraction = 1.0;
    }
}

/* Switch between blinking text shown/hidden */
static void App_text_blink_switch_timer_handler(void* self)
{
    App* app = self;

    if (app->gfx->has_blinking_text) {
        Window_notify_content_change(app->win);
        app->ui.draw_text_blinking = !app->ui.draw_text_blinking;
        TimerManager_schedule_point(&app->timer_manager,
                                    app->text_blink_switch_timer,
                                    TimePoint_ms_from_now(settings.cursor_blink_interval_ms));
    }
}

static void App_title_update_timer_handler(void* self)
{
    App_set_title(self);
}

static void App_cursor_movement_timer_handler(void* self, double fraction, bool completed)
{
    App*   app                   = self;
    double start                 = app->ui.cursor_cell_anim_start_point_cell_fraction;
    double end                   = app->ui.cursor->col;
    double diff                  = end - start;
    app->ui.cursor_cell_fraction = completed ? end : start + diff * fraction;
    App_notify_content_change(self);
}

static void App_bell_timer_handler(void* self, double fraction, bool completed)
{
    ((App*)self)->ui.flash_fraction = completed ? 0.0 : (1.0 - fraction);
    App_notify_content_change(self);
}

static void App_init(App* self)
{
    memset(self, 0, sizeof(App));

    WindowSystemLaunchEnv launch_env = WindowSystemLaunchEnv_capture();
    self->ksm_input_buf              = Vector_new_char();
    self->monitor                    = Monitor_new();

    App_set_monitor_callbacks(self);
    App_set_up_timers(self);

    if (settings.directory.str) {
        if (chdir(settings.directory.str)) {
            WRN("Failed to change directory: %s\n", strerror(errno));
        }
    }

    Monitor_fork_new_pty(&self->monitor, settings.cols, settings.rows);

    Vt_init(&self->vt, settings.cols, settings.rows);
    self->vt.master_fd = self->monitor.child_fd;
    self->freetype     = Freetype_new();
    self->gfx          = Gfx_new_OpenGL2(&self->freetype);

    Pair_uint32_t pixels    = Gfx_pixels(self->gfx, settings.cols, settings.rows);
    Pair_uint32_t cell_dims = { .first  = pixels.first / settings.cols,
                                .second = pixels.second / settings.rows };

    pixels.first += settings.padding * 2;
    pixels.second += settings.padding * 2;

    App_create_window(self, pixels, cell_dims);

    App_set_callbacks(self);

    App_primary_output_changed(self,
                               self->win->output_index,
                               self->win->output_name,
                               self->win->lcd_filter,
                               self->win->dpi);

    settings_after_window_system_connected();
    Window_set_swap_interval(self->win, 1);
    Gfx_init_with_context_activated(self->gfx);

    self->resolution    = Window_size(self->win);
    Pair_uint32_t chars = App_get_char_size(self);
    Vt_resize(&self->vt, chars.first, chars.second);
    Gfx_resize(self->gfx, self->resolution.first, self->resolution.second, chars);

    Monitor_watch_window_system_fd(&self->monitor, Window_get_connection_fd(self->win));

    self->ui.scrollbar.width                    = settings.scrollbar_width_px;
    self->ui.hovered_link.active                = false;
    self->ui.draw_cursor_blinking               = true;
    self->cursor_blink_animation_should_play    = false;
    self->cursor_blink_animation_should_play_vt = true;
    self->tex_blink_animation_should_play       = true;
    self->ui.cursor_fade_fraction               = 1.0;

    /* We may have gotten some events before callbacks were connected. We can ignore everything
     * except for window focus and ... */
    App_focus_changed(self, FLAG_IS_SET(self->win->state_flags, WINDOW_IS_IN_FOCUS));

    /* ... CSD state. */
    if (self->ui.csd.mode != UI_CSD_MODE_NONE) {
        App_csd_changed(self, self->ui.csd.mode);
    }

    Window_events(self->win);

    /* kwin ignores the initial egl surface size and every window resize request before the first
     * event dispatch */
    char *xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP"),
         *xdg_session_type    = getenv("XDG_SESSION_TYPE");
    if (xdg_current_desktop && xdg_session_type && !strcmp(xdg_current_desktop, "KDE") &&
        !strcmp(xdg_session_type, "wayland")) {
        Window_resize(self->win, self->resolution.first, self->resolution.second);
    }

    if (!settings.dynamic_title) {
        Window_update_title(self->win, settings.title.str);
    }

    App_update_padding(self);
    App_action(self);
    App_framebuffer_damage(self);

    Window_notify_initialization_complete(self->win, &launch_env);
    WindowSystemLaunchEnv_destroy(&launch_env);
}

static void App_run(App* self)
{
    while (!(self->exit || Window_is_closed(self->win))) {
        int timeout_ms = -1;
        if (Vt_get_output_size(&self->vt)) {
            timeout_ms = 0;
        } else {
            int64_t next_pending_action_ms =
              TimerManager_get_next_action_ms(&self->timer_manager,
                                              Window_get_target_frame_time_ms(self->win),
                                              TIME_POINT_PTR(self->closest_pending_wakeup),
                                              NULL);

            if (next_pending_action_ms == TIMER_MANAGER_NO_ACTION_PENDING) {
                timeout_ms = -1;
            } else {
                timeout_ms = CLAMP(next_pending_action_ms, 0, INT_MAX);
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
            if (unlikely(settings.debug_vt)) {
                usleep(settings.vt_debug_delay_usec);
                App_notify_content_change(self);
            } else if (unlikely(TimePoint_ms_in_the_past(self->interpreter_start_time) >
                                settings.pty_chunk_timeout_ms)) {
                // if we take more than a user-defined ammount of time to deal with all the data
                // continue on and draw the display.
                break;
            }

            bytes = Monitor_read(&self->monitor);

            if (bytes > 0) {
                self->written_bytes = 0;
                Vt_interpret(&self->vt, self->monitor.input_buffer, bytes);
                App_action(self);
            } else {
                break;
            }

            // Wait for any following data chunks. If the client is using multiple write()-s we may
            // have completed interpreting the initial chunk before everything intended as an atomic
            // update was sent. This may add latency but reduces flicker and 'screen tearing'.
            if (settings.pty_chunk_wait_delay_ns) {
                usleep(settings.pty_chunk_wait_delay_ns);
            }
        } while (bytes && likely(!settings.debug_vt));

        char*  buf;
        size_t len;
        Vt_peek_output(&self->vt, MONITOR_INPUT_BUFFER_SZ, &buf, &len);
        if (Monitor_write(&self->monitor, buf, len) > 0) {
            Vt_consumed_output(&self->vt, len);
        }

        App_maybe_resize(self, Window_size(self->win));
        TimerManager_update(&self->timer_manager);
        App_update_cursor(self);

        if (self->ui.hovered_link.active) {
            App_update_hover(self,
                             CLAMP(self->win->pointer_x - self->ui.pixel_offset_x,
                                   0,
                                   (int32_t)self->resolution.first),
                             CLAMP(self->win->pointer_y - self->ui.pixel_offset_y,
                                   0,
                                   (int32_t)self->resolution.second));
        }

        if (self->gfx->has_blinking_text) {
            if (!self->tex_blink_animation_should_play) {
                self->tex_blink_animation_should_play = true;
                TimerManager_schedule_point(
                  &self->timer_manager,
                  self->text_blink_switch_timer,
                  TimePoint_ms_from_now(settings.cursor_blink_interval_ms));
            }
        } else if (self->tex_blink_animation_should_play) {
            self->tex_blink_animation_should_play = false;
            TimerManager_cancel(&self->timer_manager, self->text_blink_switch_timer);
        }

        App_maybe_swap_window(self);
    }

    Monitor_kill(&self->monitor);
    self->vt.callbacks.destroy_proxy(self->vt.callbacks.user_data, &self->ui.cursor_proxy);
    Vt_destroy(&self->vt);
    Gfx_destroy(self->gfx);
    Ui_destroy(&self->ui);
    Freetype_destroy(&self->freetype);
    Window_destroy(self->win);
    TimerManager_destroy(&self->timer_manager);
    Vector_destroy_char(&self->ksm_input_buf);
    free(self->hostname);
    free(self->vt_title);
}

static void App_maybe_swap_window(App* self)
{
    if (unlikely(Vt_synchronized_update_is_active(&self->vt))) {
        bool do_swap = self->win->paint && !self->did_paint_in_sync_update_mode;
        Window_maybe_swap(self->win, do_swap);
        self->did_paint_in_sync_update_mode = true;
        if (do_swap) {
            TimerManaget_update_last_swap(&self->timer_manager, TimePoint_now());
        }
    } else {
        self->did_paint_in_sync_update_mode = false;
        Window_maybe_swap(self->win, true);
        TimerManaget_update_last_swap(&self->timer_manager, TimePoint_now());
    }
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

static window_partial_swap_request_t* App_redraw(void* self, uint8_t buffer_age)
{
    App* app = self;
    return Gfx_draw(app->gfx, &app->vt, &app->ui, buffer_age);
}

static void App_update_padding(App* self)
{
    Pair_uint32_t chars       = App_get_char_size(self);
    Pair_uint32_t used_pixels = Gfx_pixels(self->gfx, chars.first, chars.second);

    if (Ui_csd_titlebar_visible(&self->ui) && !App_fullscreen(self)) {
        used_pixels.second += (UI_CSD_TITLEBAR_HEIGHT_PX);
    }

    if (settings.padding_center) {
        self->ui.pixel_offset_x = (self->resolution.first - used_pixels.first) / 2;
        self->ui.pixel_offset_y = (self->resolution.second - used_pixels.second) / 2;
    } else {
        self->ui.pixel_offset_x = 0;
        self->ui.pixel_offset_y = 0;
    }

    self->ui.pixel_offset_x += settings.padding;
    self->ui.pixel_offset_y += settings.padding;

    if (Ui_csd_titlebar_visible(&self->ui) && !Window_is_fullscreen(self->win)) {
        self->ui.pixel_offset_y += UI_CSD_TITLEBAR_HEIGHT_PX;
    }
}

static void App_resize(App* self, Pair_uint32_t newres)
{
    self->csd_mode_changed_before_resize = false;
    self->resolution                     = newres;

    if (Ui_csd_titlebar_visible(&self->ui) && !App_fullscreen(self)) {
        self->resolution.second = MAX(self->resolution.second, UI_CSD_TITLEBAR_HEIGHT_PX);
    }

    Pair_uint32_t chars = App_get_char_size(self);
    Vt_clear_all_proxies(&self->vt);
    Gfx_destroy_proxy(self->gfx, self->ui.cursor_proxy.data);
    Gfx_resize(self->gfx, self->resolution.first, self->resolution.second, chars);
    App_update_padding(self);
    App_maybe_clamp_ksm_cursor(self, chars);
    chars = App_get_char_size(self);
    Vt_resize(&self->vt, chars.first, chars.second);
    Window_notify_content_change(self->win);
    App_framebuffer_damage(self);
    App_update_cursor(self);

    if (settings.dynamic_title) {
        App_set_title(self);
    }

    Ui_update_CSD_button_layout(&self->ui, self->resolution);
}

static void App_maybe_resize(App* self, Pair_uint32_t newres)
{
    if (unlikely(newres.first != self->resolution.first ||
                 newres.second != self->resolution.second) ||
        self->csd_mode_changed_before_resize) {
        App_resize(self, newres);
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
    Gfx_draw(app->gfx, &app->vt, &app->ui, 0);
    App_update_padding(self);
    Window_notify_content_change(app->win);
    App_framebuffer_damage(self);
    Window_maybe_swap(app->win, true);
}

static Pair_uint32_t App_get_cell_dims(App* self)
{
    Pair_uint32_t cells  = App_get_char_size(self);
    Pair_uint32_t pixels = Gfx_pixels(self->gfx, cells.first, cells.second);
    return (Pair_uint32_t){ .first  = pixels.first / cells.first,
                            .second = pixels.second / cells.second };
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
    App*          app = self;
    Pair_uint32_t res = app->resolution;

    if (Ui_csd_titlebar_visible(&app->ui) && !App_fullscreen(app)) {
        res.second -= (UI_CSD_TITLEBAR_HEIGHT_PX);
    }

    res.first -= settings.padding * 2;
    res.second -= settings.padding * 2;

    return Gfx_get_char_size(app->gfx, res);
}

static void App_set_title(void* self)
{
    App* app = self;

    if (!settings.dynamic_title) {
        return;
    }

    const VtCommand* c                   = Vt_shell_integration_get_active_command(&app->vt);
    char*            sAppTitle           = settings.title.str;
    char*            sVtTitle            = app->vt_title ? app->vt_title : "";
    bool             bIsReportingMouse   = Vt_is_reporting_mouse(&app->vt);
    bool             bIsAltBufferEnabled = Vt_alt_buffer_enabled(&app->vt);
    /* we do not know what command is running if its using the VTE prompts */
    bool      bCommandIsRunning = c && c->command;
    char*     sRunningCommand   = c ? c->command : "";
    TimePoint now               = TimePoint_now();
    if (c) {
        TimePoint_subtract(&now, c->execution_time.start);
        TimePoint next_title_refresh = TimePoint_s_from_now(1);
        TimerManager_schedule_point(&app->timer_manager,
                                    app->title_update_timer,
                                    next_title_refresh);
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
    app->vt_title = NULL;
    if (title) {
        app->vt_title = strdup(title);
    }
    App_set_title(self);
}

static void App_command_changed(void* self)
{
    App_set_title(self);
}

static void App_buffer_changed(void* self)
{
    App* app = self;
    App_set_title(app);
    Vt_clear_all_proxies(&app->vt);
    Gfx_external_framebuffer_damage(app->gfx);
}

static void App_mouse_report_changed(void* self)
{
    App_set_title(self);
}

static void App_flash(void* self)
{
    App* app = self;
    TimerManager_schedule_tween_to_ms(&app->timer_manager,
                                      app->visual_bell_timer,
                                      VISUAL_BELL_FLASH_DURATION_MS);
}

static void App_restart_cursor_blink(App* app)
{
    app->cursor_blink_animation_should_play = false;
    app->ui.draw_cursor_blinking            = true;
    app->ui.cursor_fade_fraction            = 1.0;
    App_framebuffer_damage(app);

    if (app->cursor_blink_animation_should_play_vt) {
        TimerManager_schedule_point(&app->timer_manager,
                                    app->cursor_blink_suspend_timer,
                                    TimePoint_ms_from_now(settings.cursor_blink_suspend_ms));
    }

    TimerManager_schedule_point(&app->timer_manager,
                                app->cursor_blink_end_timer,
                                TimePoint_s_from_now(settings.cursor_blink_end_s));

    TimerManager_cancel(&app->timer_manager, app->cursor_blink_switch_timer);

    if (settings.animate_cursor_blink) {
        TimerManager_cancel(&app->timer_manager, app->cursor_blink_anim_delay_timer);
    }
}

static void App_soft_restart_cursor_blink(App* app)
{
    if (!TimerManager_is_pending(&app->timer_manager, app->cursor_blink_end_timer)) {
        return;
    }

    app->cursor_blink_animation_should_play = true;
    app->ui.draw_cursor_blinking            = true;
    app->ui.cursor_fade_fraction            = 1.0;

    if (settings.animate_cursor_blink) {
        TimerManager_cancel(&app->timer_manager, app->cursor_blink_anim_delay_timer);
    }

    TimerManager_cancel(&app->timer_manager, app->cursor_blink_switch_timer);

    if (settings.animate_cursor_blink) {
        TimerManager_schedule_tween_from_now(
          &app->timer_manager,
          app->cursor_blink_switch_timer,
          TimePoint_ms_from_now((float)settings.cursor_blink_interval_ms *
                                settings.animate_cursor_blink_fade_fraction));
        app->ui.draw_cursor_blinking = true;
    } else {
        TimerManager_schedule_point(&app->timer_manager,
                                    app->cursor_blink_switch_timer,
                                    TimePoint_ms_from_now(settings.cursor_blink_interval_ms));
    }

    App_notify_content_change(app);
}

static void App_action(void* self)
{
    App* app = self;

    if (settings.enable_cursor_blink) {
        App_restart_cursor_blink(app);
    }

    if (!Window_is_pointer_hidden(app->win)) {
        App_update_hover(
          app,
          CLAMP(app->win->pointer_x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first),
          CLAMP(app->win->pointer_y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second));
    }
}

static void App_maybe_clamp_ksm_cursor(App* self, Pair_uint32_t chars)
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
    App* app = self;
    if (!settings.hold_after_child_process_exit) {
        app->exit = true;
    }
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

    const uint16_t VIM_PAGE_MARGIN = 2;

#define L_UPDATE_SELECT_END                                                                        \
    if (self->vt.selection.mode) {                                                                 \
        size_t col = self->ksm_cursor.col;                                                         \
        size_t row = self->ksm_cursor.row - Vt_visual_top_line(vt);                                \
        Vt_select_set_end_cell(vt, col, row);                                                      \
        App_notify_content_change(self);                                                           \
    }

    TimePoint now = TimePoint_now();
    TimePoint_subtract(&now, self->ksm_last_input);
    if (TimePoint_get_ms(now) > KSM_CLEAR_INPUT_BUFFER_DELAY_MS) {
        Vector_clear_char(&self->ksm_input_buf);
    }
    self->ksm_last_input = TimePoint_now();

    switch (rawkey) {
        case KEY(z):
            if (mods != MODIFIER_SHIFT) {
                break;
            } /* fallthrough */

        case KEY(Escape):
            Vt_select_end(vt);
            App_notify_content_change(self);
            App_show_scrollbar(self);
            self->keyboard_select_mode = false;
            Vt_visual_scroll_reset(vt);
            App_update_scrollbar_dims(self);
            self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
            App_action(self);
            return true;

        case KEY(m):
            if (!mods) {
                App_notify_content_change(self);
                App_show_scrollbar(self);
                self->ksm_cursor.row = Vt_visual_top_line(vt) + Vt_row(vt) / 2;
                L_UPDATE_SELECT_END
                App_update_scrollbar_dims(self);
                Vector_clear_char(&self->ksm_input_buf);
            }
            break;

        case KEY(Left):
        case KEY(h):
            App_notify_content_change(self);
            App_show_scrollbar(self);
            if (rawkey == KEY(h) && mods == MODIFIER_SHIFT) {
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
            self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
            App_action(self);
            break;

        case KEY(Down):
        case KEY(j):
            App_notify_content_change(self);
            App_show_scrollbar(self);
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                if (self->ksm_cursor.row < Vt_max_line(vt)) {
                    self->ksm_cursor.row++;
                }
                if (Vt_visual_bottom_line(vt) == Vt_max_line(vt)) {
                    Vt_visual_scroll_reset(vt);
                } else {
                    while (Vt_visual_bottom_line(vt) < self->ksm_cursor.row) {
                        if (Vt_visual_scroll_down(vt, !self->keyboard_select_mode)) {
                            break;
                        }
                    }
                }
            }
            App_update_scrollbar_dims(self);
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
            App_action(self);
            break;

        case KEY(Up):
        case KEY(k): {
            App_notify_content_change(self);
            App_show_scrollbar(self);
            int p = App_get_ksm_number(self);
            for (int i = MAX(1, p); i > 0; --i) {
                if (self->ksm_cursor.row) {
                    self->ksm_cursor.row--;
                }
                while (Vt_visual_top_line(vt) > self->ksm_cursor.row) {
                    if (Vt_visual_scroll_up(vt, !self->keyboard_select_mode)) {
                        break;
                    }
                }
            }
            App_update_scrollbar_dims(self);
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
            App_action(self);
        } break;

        case KEY(Right):
        case KEY(l):
            App_notify_content_change(self);
            App_show_scrollbar(self);
            if (rawkey == KEY(l) && mods == MODIFIER_SHIFT) {
                self->ksm_cursor.row = Vt_visual_bottom_line(vt);
            } else {
                int p = App_get_ksm_number(self);
                for (int i = MAX(1, p); i > 0; --i) {
                    if (self->ksm_cursor.col + 1 < Vt_col(vt)) {
                        self->ksm_cursor.col++;
                    }
                }
            }
            App_update_scrollbar_dims(self);
            L_UPDATE_SELECT_END
            Vector_clear_char(&self->ksm_input_buf);
            self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
            App_action(self);
            break;

        case KEY(y):
            if (mods == MODIFIER_CONTROL) /* line up */ {
                int n = App_get_ksm_number(self);
                for (int i = MAX(1, n); i > 0; --i) {
                    Vt_visual_scroll_up(&self->vt, !self->keyboard_select_mode);
                }
                App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
                L_UPDATE_SELECT_END
                App_action(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            } /* fallthrough */

        case KEY(c):
            if (rawkey != KEY(y) || !mods) {
                if (self->vt.selection.mode) {
                    Vector_char txt = Vt_select_region_to_string(vt);
                    if (txt.size) {
                        App_clipboard_send(self, txt.buf);
                        Vt_select_end(vt);
                        App_action(self);
                        App_notify_content_change(self);
                    } else {
                        Vector_destroy_char(&txt);
                    }
                }
            }
            Vector_clear_char(&self->ksm_input_buf);
            break;

        case KEY(Return): {
            if (mods == MODIFIER_CONTROL) {
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
            App_action(self);
            App_notify_content_change(self);
            break;

        case KEY(b): {
            if (mods == MODIFIER_CONTROL) /* page up */ {
                int n = App_get_ksm_number(self);
                for (int i = MAX(1, n); i > 0; --i) {
                    Vt_visual_scroll_page_up(&self->vt,
                                             VIM_PAGE_MARGIN,
                                             !self->keyboard_select_mode);
                }
                App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
                L_UPDATE_SELECT_END
                App_action(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            } else if (!mods) /* jump back by word */ {
                int p = App_get_ksm_number(self);
                for (int i = MAX(1, p); i > 0; --i) {
                    for (bool initial = true;; initial = false) {
                        if (!self->ksm_cursor.col) {
                            break;
                        }
                        VtRune* rune = Vt_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                        VtRune* prev_rune =
                          Vt_at(vt, self->ksm_cursor.col - 1, self->ksm_cursor.row);
                        if (!rune || !prev_rune) {
                            break;
                        }
                        char32_t code = rune->rune.code, prev_code = prev_rune->rune.code;

                        if (isblank(prev_code) && !isblank(code) && !initial) {
                            break;
                        }
                        --self->ksm_cursor.col;
                        App_notify_content_change(self);
                        App_show_scrollbar(self);
                    }
                }
                App_update_scrollbar_dims(self);
                L_UPDATE_SELECT_END
                Vector_clear_char(&self->ksm_input_buf);
                self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
                App_action(self);
            }
        } break;

        case KEY(w): /* jump forward to next word */ {
            if (!mods) {
                int p = App_get_ksm_number(self);
                for (int i = MAX(1, p); i > 0; --i) {
                    for (bool initial = true;; initial = false) {
                        VtLine* row_line = Vt_line_at(vt, self->ksm_cursor.row);
                        if (self->ksm_cursor.col + 1 >= Vt_col(vt) || !row_line ||
                            self->ksm_cursor.col + 1 >= (uint16_t)row_line->data.size) {
                            break;
                        }
                        VtRune* rune = Vt_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                        VtRune* next_rune =
                          Vt_at(vt, self->ksm_cursor.col + 1, self->ksm_cursor.row);
                        if (!rune || !next_rune) {
                            break;
                        }
                        char32_t code = rune->rune.code, next_code = next_rune->rune.code;
                        ++self->ksm_cursor.col;
                        App_notify_content_change(self);
                        App_show_scrollbar(self);
                        if ((isblank(code) && !isblank(next_code)) && !initial) {
                            break;
                        }
                    }
                }
                App_update_scrollbar_dims(self);
                L_UPDATE_SELECT_END
                Vector_clear_char(&self->ksm_input_buf);
                self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
                App_action(self);
            }
        } break;

        case KEY(d):
            if (mods == MODIFIER_CONTROL) /* half page down (keep cursor position) */ {
                int      n                   = App_get_ksm_number(self);
                uint16_t relative_cursor_pos = Vt_visual_top_line(&self->vt) - self->ui.cursor->row;
                for (int i = MAX(1, n); i > 0; --i) {
                    Vt_visual_scroll_to(&self->vt,
                                        self->vt.visual_scroll_top + Vt_row(&self->vt) / 2,
                                        !self->keyboard_select_mode);
                }
                self->ui.cursor->row = Vt_visual_top_line(&self->vt) + relative_cursor_pos;
                App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
                L_UPDATE_SELECT_END
                App_action(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            }
            break;

        case KEY(e): {
            if (mods == MODIFIER_CONTROL) /* scroll line down */ {
                int n = App_get_ksm_number(self);
                for (int i = MAX(1, n); i > 0; --i) {
                    Vt_visual_scroll_down(&self->vt, !self->keyboard_select_mode);
                }
                App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
                L_UPDATE_SELECT_END
                App_action(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            } else if (!mods) /* jump to end of word */ {
                int p = App_get_ksm_number(self);
                for (int i = MAX(1, p); i > 0; --i) {
                    for (bool initial = true;; initial = false) {
                        VtLine* row_line = Vt_line_at(vt, self->ksm_cursor.row);
                        if (self->ksm_cursor.col + 1 >= Vt_col(vt) || !row_line ||
                            self->ksm_cursor.col + 1 >= (uint16_t)row_line->data.size) {
                            break;
                        }
                        VtRune* rune = Vt_at(vt, self->ksm_cursor.col, self->ksm_cursor.row);
                        VtRune* next_rune =
                          Vt_at(vt, self->ksm_cursor.col + 1, self->ksm_cursor.row);
                        char32_t code = rune->rune.code, next_code = next_rune->rune.code;
                        if ((isblank(next_code) && !isblank(code)) && !initial) {
                            break;
                        }
                        ++self->ksm_cursor.col;
                    }
                }
                App_notify_content_change(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                L_UPDATE_SELECT_END
                Vector_clear_char(&self->ksm_input_buf);
                self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
                App_action(self);
            }
        } break;

        case KEY(f): {
            if (mods == MODIFIER_CONTROL) /* page down */ {
                int n = App_get_ksm_number(self);
                for (int i = MAX(1, n); i > 0; --i) {
                    Vt_visual_scroll_page_down(&self->vt,
                                               VIM_PAGE_MARGIN,
                                               !self->keyboard_select_mode);
                }
                App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
                L_UPDATE_SELECT_END
                App_action(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            }
        } break;

        case KEY(g):
            if (mods == MODIFIER_SHIFT) {
                Vt_visual_scroll_to(vt, Vt_top_line(vt), !self->keyboard_select_mode);
                self->ksm_cursor.row = Vt_max_line(vt);
            } else if (!mods && self->ksm_input_buf.size) {
                size_t tgt           = App_get_ksm_number(self);
                tgt                  = MIN(tgt, Vt_max_line(vt));
                self->ksm_cursor.row = tgt;
                if (tgt > Vt_visual_top_line(vt) && tgt < Vt_visual_bottom_line(vt)) {
                } else {
                    if (tgt < Vt_visual_top_line(vt)) {
                        Vt_visual_scroll_to(vt, tgt, !self->keyboard_select_mode);
                    } else {
                        Vt_visual_scroll_to(vt, tgt - Vt_row(vt) + 1, self->keyboard_select_mode);
                    }
                }
            } else if (!mods) {
                Vt_visual_scroll_to(vt, 0, self->keyboard_select_mode);
                self->ksm_cursor.row = 0;
            } else {
                break;
            }

            App_notify_content_change(self);
            App_show_scrollbar(self);
            App_update_scrollbar_dims(self);
            L_UPDATE_SELECT_END
            self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
            App_action(self);
            break;

        case KEY(u): {
            if (mods == MODIFIER_CONTROL) /* half page up (keep cursor position) */ {
                int      n                   = App_get_ksm_number(self);
                uint16_t relative_cursor_pos = Vt_visual_top_line(&self->vt) - self->ui.cursor->row;
                for (int i = MAX(1, n); i > 0; --i) {
                    size_t tgt_pos = (Vt_visual_top_line(&self->vt) > Vt_row(&self->vt) / 2)
                                       ? Vt_visual_top_line(&self->vt) - Vt_row(&self->vt) / 2
                                       : 0;
                    Vt_visual_scroll_to(&self->vt, tgt_pos, !self->keyboard_select_mode);
                }
                self->ui.cursor->row = Vt_visual_top_line(&self->vt) + relative_cursor_pos;
                App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
                L_UPDATE_SELECT_END
                App_action(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            }
        } break;

        case KEY(0):
            if (self->ksm_input_buf.size) {
                if (self->ksm_input_buf.size < 3) {
                    Vector_push_char(&self->ksm_input_buf, '0');
                }
            } else if (!mods) {
                App_notify_content_change(self);
                App_show_scrollbar(self);
                App_update_scrollbar_dims(self);
                self->ksm_cursor.col = 0;
                L_UPDATE_SELECT_END
                App_action(self);
            }
            break;

        case KEY(4):
            // jump to last non-blank in line
            if (mods == MODIFIER_SHIFT) {
                App_notify_content_change(self);
                App_show_scrollbar(self);
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
                self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
                App_action(self);
                App_update_scrollbar_dims(self);
                App_notify_content_change(self);
            } else if (!mods) {
                if (self->ksm_input_buf.size < 3) {
                    Vector_push_char(&self->ksm_input_buf, '4');
                }
            }
            break;

            // TODO: case KEY(5):

        case KEY(6):
            // jump to first non-blank in line
            if (mods == MODIFIER_SHIFT) {
                App_notify_content_change(self);
                App_show_scrollbar(self);
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
                self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
                App_action(self);
            } else if (!mods) {
                if (self->ksm_input_buf.size < 3) {
                    Vector_push_char(&self->ksm_input_buf, '6');
                }
            }
            App_update_scrollbar_dims(self);
            App_notify_content_change(self);
            break;

        case KEY(1)... KEY(3):
        case KEY(5):
        case KEY(7)... KEY(9):
            if (!mods && self->ksm_input_buf.size < 3) {
                Vector_push_char(&self->ksm_input_buf, rawkey - KEY(1) + '1');
            }
            break;

        case KEY(Page_Up): {
            int n = App_get_ksm_number(self);
            for (int i = MAX(1, n); i > 0; --i) {
                Vt_visual_scroll_page_up(&self->vt, VIM_PAGE_MARGIN, !self->keyboard_select_mode);
            }
            App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
            L_UPDATE_SELECT_END
            App_action(self);
            App_update_scrollbar_dims(self);
            App_notify_content_change(self);
        } break;

        case KEY(Page_Down): {
            int n = App_get_ksm_number(self);
            for (int i = MAX(1, n); i > 0; --i) {
                Vt_visual_scroll_page_down(&self->vt, VIM_PAGE_MARGIN, !self->keyboard_select_mode);
            }
            App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
            L_UPDATE_SELECT_END
            App_action(self);
            App_update_scrollbar_dims(self);
            App_notify_content_change(self);
        } break;

        default:
            if (KeyCommand_is_active(&settings.key_commands[KCMD_COPY], key, rawkey, mods) &&
                self->vt.selection.mode) {
                Vector_char txt = Vt_select_region_to_string(vt);
                if (txt.size) {
                    App_clipboard_send(self, txt.buf);
                    Vt_select_end(vt);
                    App_action(self);
                    App_notify_content_change(self);
                } else {
                    Vector_destroy_char(&txt);
                }
            }
            break;
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
            Gfx_destroy_proxy(self->gfx, self->ui.cursor_proxy.data);
            App_reload_font(self);
            App_resize(self, self->resolution);
            Pair_uint32_t cell = App_get_cell_dims(self);
            Window_set_incremental_resize(self->win, cell.first, cell.second);
        } else {
            App_flash(self);
        }
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_FONT_ENLARGE], key, rawkey, mods)) {
        ++settings.font_size;
        Vt_clear_all_proxies(vt);
        Gfx_destroy_proxy(self->gfx, self->ui.cursor_proxy.data);
        App_reload_font(self);
        App_resize(self, self->resolution);
        Pair_uint32_t cell = App_get_cell_dims(self);
        Window_set_incremental_resize(self->win, cell.first, cell.second);
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
        self->ksm_cursor            = self->vt.cursor;
        self->ksm_cursor.blinking   = false;
        self->ui.cursor             = &self->ksm_cursor;
        self->keyboard_select_mode  = true;
        self->ui.cursor_damage.type = VT_LINE_DAMAGE_FULL;
        Vt_start_visual_scroll(&self->vt);
        App_show_scrollbar(self);
        self->ksm_cursor.row =
          CLAMP(self->ksm_cursor.row, Vt_visual_top_line(vt), Vt_visual_bottom_line(vt));
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_LINE_SCROLL_UP], key, rawkey, mods)) {
        Vt_visual_scroll_up(vt, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_LINE_SCROLL_DN], key, rawkey, mods)) {
        Vt_visual_scroll_down(vt, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_PAGE_SCROLL_UP], key, rawkey, mods)) {
        Vt_visual_scroll_page_up(vt, 0, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_PAGE_SCROLL_DN], key, rawkey, mods)) {
        Vt_visual_scroll_page_down(vt, 0, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_notify_content_change(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_MARK_SCROLL_UP], key, rawkey, mods)) {
        if (Vt_visual_scroll_mark_up(vt, !self->keyboard_select_mode)) {
            App_show_scrollbar(self);
            App_notify_content_change(self);
        } else
            App_flash(self);
        return true;
    } else if (KeyCommand_is_active(&cmd[KCMD_MARK_SCROLL_DN], key, rawkey, mods)) {
        if (Vt_visual_scroll_mark_down(vt, !self->keyboard_select_mode)) {
            App_show_scrollbar(self);
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

void App_cursor_blink_change_handler(void* self, bool blink_state)
{
    App* app = self;

    if (!settings.enable_cursor_blink) {
        return;
    }

    app->cursor_blink_animation_should_play_vt = blink_state;

    if (blink_state) {
        App_soft_restart_cursor_blink(app);
    } else {
        app->cursor_blink_animation_should_play = false;
        app->ui.draw_cursor_blinking            = true;
        app->ui.cursor_fade_fraction            = 1.0;
        TimerManager_cancel(&app->timer_manager, app->cursor_blink_end_timer);
        TimerManager_cancel(&app->timer_manager, app->cursor_blink_switch_timer);
        TimerManager_cancel(&app->timer_manager, app->cursor_blink_suspend_timer);

        App_framebuffer_damage(app);

        if (settings.animate_cursor_blink) {
            TimerManager_cancel(&app->timer_manager, app->cursor_blink_anim_delay_timer);
        }
    }
}

void App_gui_pointer_mode_change_handler(void* self)
{

#define L_SET_POINTER_STYLE                                                                        \
    if (app->selection_dragging_left || app->selection_dragging_right ||                           \
        (app->vt.selection.mode != SELECT_MODE_NONE && !app->keyboard_select_mode)) {              \
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

    if (settings.smooth_cursor) {
        uint16_t vis_row = Vt_visual_cursor_row(&self->vt);
        if (self->ui.cursor->col != self->ui.last_cursor_cell_position) {
            if (self->ui.last_cursor_row_position == vis_row) {
                self->ui.last_cursor_cell_position                  = self->ui.cursor->col;
                self->ui.cursor_cell_anim_start_point_cell_fraction = self->ui.cursor_cell_fraction;

                if (likely(self->win->key_repeat_interval_ms)) {
                    TimerManager_schedule_tween_to_ms(&self->timer_manager,
                                                      self->cursor_movement_timer,
                                                      self->win->key_repeat_interval_ms);
                } else {
                    self->ui.cursor_cell_fraction = self->ui.last_cursor_cell_position;
                }
            } else {
                self->ui.cursor_cell_fraction = self->ui.cursor->col;
            }
        }
        self->ui.last_cursor_row_position = vis_row;
    } else {
        self->ui.cursor_cell_fraction = self->ui.cursor->col;
    }
}

/**
 * Update gui scrollbar dimensions */
static void App_update_scrollbar_dims(App* self)
{
    Vt* vt = &self->vt;

    uint32_t y_pixels = self->resolution.second;
    uint32_t y_offset = 0;
    double   area_y   = 2.0;

    if (Ui_csd_titlebar_visible(&self->ui) && !Window_is_fullscreen(self->win)) {
        y_offset = UI_CSD_TITLEBAR_HEIGHT_PX;
        area_y *= ((double)(self->resolution.second - y_offset) / self->resolution.second);
    }

    y_pixels -= y_offset;
    double  minimum_length    = (area_y / y_pixels) * settings.scrollbar_length_px;
    double  length            = area_y / vt->lines.size * vt->ws.ws_row;
    double  extra_length      = length > minimum_length ? 0.0 : (minimum_length - length);
    int32_t scrollable_lines  = Vt_top_line(&self->vt);
    int32_t lines_scrolled    = Vt_visual_top_line(&self->vt);
    double  fraction_scrolled = (double)lines_scrolled / scrollable_lines;
    self->ui.scrollbar.length = length + extra_length;
    self->ui.scrollbar.top = (area_y - length - extra_length) * fraction_scrolled + (2.0 - area_y);
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
        Vt_visual_scroll_to(vt, target_line, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
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
                App_show_scrollbar(self);
                self->ui.scrollbar.dragging   = true;
                self->scrollbar_drag_position = dp - self->ui.scrollbar.top;
            }
        } else {
            // outside of scrollbar
            if (state && button == MOUSE_BTN_LEFT) {
                /* jump to that position and start dragging in the middle */
                App_show_scrollbar(self);
                self->scrollbar_drag_position = self->ui.scrollbar.length / 2;
                dp = 2.0f * ((float)y / (float)vt->ws.ws_ypixel) - self->scrollbar_drag_position;
                float  range       = 2.0f - self->ui.scrollbar.length;
                size_t target_line = Vt_top_line(vt) * CLAMP(dp, 0.0, range) / range;
                if (target_line != Vt_visual_top_line(vt)) {
                    Vt_visual_scroll_to(vt, target_line, !self->keyboard_select_mode);
                }
            } else if (state && button == MOUSE_BTN_RIGHT) {
                App_show_scrollbar(self);
                if (dp > self->ui.scrollbar.top + self->ui.scrollbar.length / 2) {
                    self->autoscroll = AUTOSCROLL_DN;
                } else {
                    self->autoscroll = AUTOSCROLL_UP;
                }
                TimerManager_schedule_point(&self->timer_manager,
                                            self->autoscroll_timer,
                                            TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS));
            } else if (state && button == MOUSE_BTN_MIDDLE) {
                /* jump one screen in that direction */
                App_show_scrollbar(self);
                if (dp > self->ui.scrollbar.top + self->ui.scrollbar.length / 2) {
                    Vt_visual_scroll_page_down(vt, 0, !self->keyboard_select_mode);
                } else {
                    Vt_visual_scroll_page_up(vt, 0, !self->keyboard_select_mode);
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

static void App_do_autoscroll(App* self)
{
    Vt* vt = &self->vt;
    if (self->autoscroll == AUTOSCROLL_UP) {
        Vt_visual_scroll_up(vt, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
    } else if (self->autoscroll == AUTOSCROLL_DN) {
        Vt_visual_scroll_down(vt, !self->keyboard_select_mode);
        App_show_scrollbar(self);
        App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
    }

    if (self->autoselect) {
        Vt_select_set_end(&self->vt,
                          self->autoscroll_autoselect.first,
                          self->autoscroll_autoselect.second);
        App_notify_content_change(self);
    }

    if (self->autoscroll) {
        TimerManager_schedule_point(&self->timer_manager,
                                    self->autoscroll_timer,
                                    TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS));
    }
}

/**
 * @return event was consumed */
static bool App_consume_drag(App* self, uint32_t button, int32_t x, int32_t y)
{
    Vt* vt = &self->vt;

    self->click_count = 0;
    if (button == MOUSE_BTN_LEFT && self->selection_dragging_left) {
        int32_t high_bound = AUTOSCROLL_TRIGGER_MARGIN_PX;
        int32_t low_bound  = self->win->h - AUTOSCROLL_TRIGGER_MARGIN_PX;

        LOG("App::select_drag{ %d x %d ", x, y);

        if (y <= high_bound) {
            self->autoselect = true;
            if (!self->autoscroll) {
                App_show_scrollbar(self);
            }
            self->autoscroll = AUTOSCROLL_UP;
            TimerManager_schedule_point(&self->timer_manager,
                                        self->autoscroll_timer,
                                        TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS));
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
                App_show_scrollbar(self);
            }
            self->autoscroll = AUTOSCROLL_DN;
            TimerManager_schedule_point(&self->timer_manager,
                                        self->autoscroll_timer,
                                        TimePoint_ms_from_now(AUTOSCROLL_DELAY_MS));

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
            App_notify_content_change(self);
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

        if (self->vt.selection.mode == SELECT_MODE_NONE) {
            Window_set_pointer_style(self->win, MOUSE_POINTER_ARROW);
        } else {
            Window_set_pointer_style(self->win, MOUSE_POINTER_I_BEAM);
        }
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
                if (vt->selection.mode != SELECT_MODE_NONE) {
                    Window_set_pointer_style(self->win, MOUSE_POINTER_I_BEAM);
                }
                Vt_select_end(vt);
                enum SelectMode mode =
                  FLAG_IS_SET(mods, MODIFIER_CONTROL) ? SELECT_MODE_BOX : SELECT_MODE_NORMAL;

                App_notify_content_change(self);
                Vt_select_init(vt, mode, x, y);
                self->selection_dragging_left = true;
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
                App_notify_content_change(self);
                self->selection_dragging_right = SELECT_DRAG_RIGHT_BACK;
            } else {
                size_t clicked_cell = x / vt->pixels_per_cell_x,
                       center_cell =
                         (vt->selection.begin_char_idx + vt->selection.end_char_idx) / 2;
                if (clicked_cell > center_cell) {
                    Vt_select_set_end(vt, x, y);
                    App_notify_content_change(self);
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
                App_notify_content_change(self);
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
        Window_set_pointer_style(self->win, MOUSE_POINTER_I_BEAM);
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
    App* app = self;
    Vt*  vt  = &app->vt;

    if (Ui_csd_titlebar_visible(&app->ui)) {
        if (y < UI_CSD_TITLEBAR_HEIGHT_PX && !state && button == MOUSE_BTN_MIDDLE) {
            Window_set_minimized(app->win);
            return;
        }

        if (y < UI_CSD_TITLEBAR_HEIGHT_PX && !state && button == MOUSE_BTN_LEFT) {
            ui_csd_titlebar_button_info_t* btn = Ui_csd_get_hovered_button(&app->ui, x, y);
            if (btn) {
                switch (btn->type) {
                    case UI_CSD_TITLEBAR_BUTTON_CLOSE:
                        app->exit = true;
                        break;
                    case UI_CSD_TITLEBAR_BUTTON_MINIMIZE:
                        Window_set_minimized(app->win);
                        break;
                    case UI_CSD_TITLEBAR_BUTTON_MAXIMIZE:
                        Window_set_maximized(app->win, !Window_is_maximized(app->win));
                        break;
                    default:;
                }
                return;
            }
        }

        if (y <= UI_CSD_TITLEBAR_HEIGHT_PX) {
            return;
        }
    }

    x = CLAMP(x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first);
    y = CLAMP(y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second);

    bool vt_wants_scroll = Vt_reports_mouse(&app->vt);
    app->autoselect      = false;

    if (!vt_wants_scroll && (button == MOUSE_BTN_WHEEL_DOWN && state)) {
        uint8_t lines = ammount ? ammount : settings.scroll_discrete_lines;
        App_show_scrollbar(self);
        for (uint8_t i = 0; i < lines; ++i)
            if (Vt_visual_scroll_down(vt, !app->keyboard_select_mode))
                break;
        App_update_scrollbar_dims(self);
        App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
    } else if (!vt_wants_scroll && (button == MOUSE_BTN_WHEEL_UP && state)) {
        uint8_t lines = ammount ? ammount : settings.scroll_discrete_lines;
        App_show_scrollbar(self);
        for (uint8_t i = 0; i < lines; ++i)
            if (Vt_visual_scroll_up(vt, !app->keyboard_select_mode))
                break;
        App_update_scrollbar_dims(self);
        App_maybe_clamp_ksm_cursor(self, App_get_char_size(self));
    } else if (!App_scrollbar_consume_click(self, button, state, x, y) &&
               !App_maybe_consume_click(self, button, state, x, y, mods)) {

        /* Maybe theres a link there */
        if (FLAG_IS_SET(mods, MODIFIER_CONTROL) && !state) {
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
            }
        }
    } else {
        if (self->ui.hovered_link.active) {
            self->ui.hovered_link.active = false;
            App_notify_content_change(self);
        }

        if (self->vt.selection.mode == SELECT_MODE_NONE) {
            if (self->win->current_pointer_style != MOUSE_POINTER_ARROW) {
                Window_set_pointer_style(self->win, MOUSE_POINTER_ARROW);
            }
        } else {
            if (self->win->current_pointer_style != MOUSE_POINTER_I_BEAM) {
                Window_set_pointer_style(self->win, MOUSE_POINTER_I_BEAM);
            }
        }
    }
}

static void App_motion_handler(void* self, uint32_t button, int32_t x, int32_t y)
{
    App* app = self;

    if (Ui_csd_titlebar_visible(&app->ui)) {
        bool csd_changed = false;
        for (ui_csd_titlebar_button_info_t* i = NULL;
             (i = Vector_iter_ui_csd_titlebar_button_info_t(&app->ui.csd.buttons, i));) {
            if (i->highlight_fraction != 0.0f) {
                csd_changed = true;
            }
            i->highlight_fraction = 0.0f;
        }

        if (y < UI_CSD_TITLEBAR_HEIGHT_PX) {
            ui_csd_titlebar_button_info_t* btn = Ui_csd_get_hovered_button(&app->ui, x, y);
            if (btn) {
                if (btn->highlight_fraction != 1.0f) {
                    csd_changed = true;
                }
                btn->highlight_fraction = 1.0f;
            }
        }
        if (csd_changed) {
            app->ui.csd.damage = true;
            App_notify_content_change(self);
        }
    }

    x = CLAMP(x - app->ui.pixel_offset_x, 0, (int32_t)app->resolution.first);
    y = CLAMP(y - app->ui.pixel_offset_y, 0, (int32_t)app->resolution.second);

    if (button) {
        if (!App_scrollbar_consume_drag(self, button, x, y) &&
            !App_consume_drag(self, button, x, y)) {
            Vt_handle_motion(&app->vt, button, x, y);
        }
        if (app->selection_dragging_left) {
            Window_set_pointer_style(app->win, MOUSE_POINTER_I_BEAM);
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
    app->ui.window_in_focus        = current_state;
    app->ui.csd.requires_attention = false;

    if (app->ui.csd.mode) {
        App_framebuffer_damage(self);
    }

    if (!current_state && app->vt.selection.mode) {
        Vector_char txt = Vt_select_region_to_string(&app->vt);
        if (txt.size) {
            App_primary_send(self, txt.buf);
        } else {
            Vector_destroy_char(&txt);
        }
    }
}

static void App_csd_changed(void* self, ui_csd_mode_e csd_mode)
{
    App* app = self;

    if (!app->ui.csd.buttons.buf) {
        app->ui.csd.buttons = Vector_new_with_capacity_ui_csd_titlebar_button_info_t(1);
        Vector_pushv_ui_csd_titlebar_button_info_t(
          &app->ui.csd.buttons,
          (ui_csd_titlebar_button_info_t[]){
            { UI_CSD_TITLEBAR_BUTTON_CLOSE, 0.0f, { 0, 0 } },
            { UI_CSD_TITLEBAR_BUTTON_MAXIMIZE, 0.0f, { 0, 0 } },
            { UI_CSD_TITLEBAR_BUTTON_MINIMIZE, 0.0f, { 0, 0 } },
          },
          3);

        Ui_update_CSD_button_layout(&app->ui, app->resolution);
    }

    if (app->ui.csd.mode == csd_mode) {
        return;
    }

    switch (csd_mode) {
        case UI_CSD_MODE_FLOATING:
            app->ui.csd.mode = UI_CSD_MODE_FLOATING;
            // TODO: update titlebar text
            break;

        case UI_CSD_MODE_TILED:
        case UI_CSD_MODE_NONE:
            app->ui.csd.mode = csd_mode;
            break;
    }

    app->ui.csd.mode                    = csd_mode;
    app->ui.csd.damage                  = true;
    app->csd_mode_changed_before_resize = true;
    App_framebuffer_damage(self);
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
    App_resize(app, (Pair_uint32_t){ app->win->w, app->win->h });
}

static void App_set_window_size(void* self, uint32_t width, uint32_t height)
{
    if (width > UINT16_MAX || height > UINT16_MAX) {
        WRN("requested window size too large");
        return;
    }

    App* app = self;
    Window_resize(app->win, width, height);
    App_resize(self, (Pair_uint32_t){ app->win->w, app->win->h });
    Ui_update_CSD_button_layout(&app->ui, app->resolution);
    App_framebuffer_damage(self);
}

static void App_set_text_area_size(void* self, int32_t width, int32_t height)
{
    App* app = self;
    Window_resize(app->win, width + 2 * settings.padding, height + 2 * settings.padding);
}

static Pair_uint32_t App_text_area_size(void* self)
{
    App*          app      = self;
    Pair_uint32_t win_size = Window_size(app->win);
    win_size.first -= settings.padding * 2;
    win_size.second -= settings.padding * 2;
    return win_size;
}

static void App_visual_scroll_reset_handler(void* self)
{
    App* app = self;
    if (app->ui.scrollbar.visible) {
        App_update_scrollbar_dims(self);
        App_show_scrollbar(self);
    }
}

static void App_set_urgent(void* self)
{
    App* app = self;
    if (!Window_is_focused(app->win)) {
        Window_set_urgent(app->win);

        if (app->ui.csd.mode) {
            app->ui.csd.requires_attention = true;
            App_framebuffer_damage(self);
        }
    }
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

static void App_framebuffer_damage(void* self)
{
    App* app = self;
    Gfx_external_framebuffer_damage(app->gfx);
}

static void App_primary_output_changed(void*         self,
                                       const int32_t display_index,
                                       const char*   display_name,
                                       lcd_filter_e  new_filter,
                                       uint16_t      physical_dpi)
{
    App* app = self;

    uint16_t new_dpi         = settings.font_dpi;
    bool     get_lcd         = new_filter == LCD_FILTER_UNDEFINED || new_filter == LCD_FILTER_NONE;
    bool     no_rule_applies = true;

    if (settings.font_dpi_calculate_from_phisical && physical_dpi) {
        new_dpi = physical_dpi;
    }

    new_filter = LCD_FILTER_UNDEFINED;
    for (output_prefs_t* i = NULL;
         (i = Vector_iter_output_prefs_t(&settings.output_preferences, i));) {
        if (streq_glob(display_name, i->output_name) ||
            (display_index && (uint32_t)display_index == i->output_index)) {
            no_rule_applies = false;

            if (i->lcd_filter != LCD_FILTER_UNDEFINED && get_lcd) {
                new_filter = i->lcd_filter;
            }
            if (i->dpi) {
                new_dpi = i->dpi;
            }
            break;
        }
    }

    if (no_rule_applies) {
        new_filter = settings.general_lcd_filter;
        new_dpi    = settings.general_font_dpi;
    }

    if ((new_filter != LCD_FILTER_UNDEFINED && new_filter != settings.lcd_filter) ||
        new_dpi != settings.font_dpi) {
        settings.lcd_filter = new_filter;
        settings.font_dpi   = new_dpi;

        Vt_clear_all_proxies(&app->vt);
        Gfx_destroy_proxy(app->gfx, app->ui.cursor_proxy.data);
        Freetype_reload_fonts_with_output_type(
          &app->freetype,
          output_texture_type_from_lcd_filter(settings.lcd_filter));
        App_reload_font(app);

        INFO("Window moved to output %u '%s' " LCD_FILTER_FMT ", %u(%u) dpi.",
             display_index,
             display_name,
             LCD_FILTER_AP(new_filter),
             new_dpi,
             physical_dpi);
    }
}

static void App_visual_scroll_params_changed_handler(void* self)
{
    App* app = self;
    App_update_scrollbar_dims(app);
    App_maybe_clamp_ksm_cursor(self, App_get_char_size(app));
    App_notify_content_change(self);
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
    self->vt.callbacks.on_command_state_changed            = App_command_changed;
    self->vt.callbacks.on_mouse_report_state_changed       = App_mouse_report_changed;
    self->vt.callbacks.on_buffer_changed                   = App_buffer_changed;
    self->vt.callbacks.on_gui_pointer_mode_changed         = App_gui_pointer_mode_change_handler;
    self->vt.callbacks.on_cursor_blink_state_changed       = App_cursor_blink_change_handler;
    self->vt.callbacks.on_visual_scroll_reset              = App_visual_scroll_reset_handler;
    self->vt.callbacks.on_visual_scroll_params_changed = App_visual_scroll_params_changed_handler;

    self->win->callbacks.user_data               = self;
    self->win->callbacks.key_handler             = App_key_handler;
    self->win->callbacks.button_handler          = App_button_handler;
    self->win->callbacks.motion_handler          = App_motion_handler;
    self->win->callbacks.clipboard_handler       = App_clipboard_handler;
    self->win->callbacks.activity_notify_handler = App_action;
    self->win->callbacks.on_redraw_requested     = App_redraw;
    self->win->callbacks.on_focus_changed        = App_focus_changed;
    self->win->callbacks.on_primary_changed      = App_primary_claimed_by_other_client;
    self->win->callbacks.on_output_changed       = App_primary_output_changed;
    self->win->callbacks.on_framebuffer_damaged  = App_framebuffer_damage;
    self->win->callbacks.on_csd_style_changed    = App_csd_changed;

    self->gfx->callbacks.user_data                   = self;
    self->gfx->callbacks.load_extension_proc_address = App_load_extension_proc_address;

    settings.callbacks.user_data           = self;
    settings.callbacks.keycode_from_string = App_get_key_code;
}

static void App_set_up_timers(App* self)
{
    self->timer_manager = TimerManager_new(self);

    self->autoscroll_timer = TimerManager_create_timer(&self->timer_manager,
                                                       TIMER_TYPE_POINT,
                                                       App_autoscroll_timer_handler);

    self->scrollbar_hide_timer = TimerManager_create_timer(&self->timer_manager,
                                                           TIMER_TYPE_TWEEN,
                                                           App_scrollbar_hide_timer_handler);
    TimerManager_set_interpolation_func(&self->timer_manager,
                                        self->scrollbar_hide_timer,
                                        TWEEN_INTERPOLATION_TYPE_POW2);

    self->visual_bell_timer =
      TimerManager_create_timer(&self->timer_manager, TIMER_TYPE_TWEEN, App_bell_timer_handler);
    TimerManager_set_interpolation_func(&self->timer_manager,
                                        self->visual_bell_timer,
                                        TWEEN_INTERPOLATION_TYPE_SIN_IN);

    self->cursor_blink_suspend_timer =
      TimerManager_create_timer(&self->timer_manager,
                                TIMER_TYPE_POINT,
                                App_cursor_blink_suspend_timer_handler);

    self->cursor_blink_end_timer = TimerManager_create_timer(&self->timer_manager,
                                                             TIMER_TYPE_POINT,
                                                             App_cursor_blink_end_timer_handler);

    if (settings.enable_cursor_blink) {
        if (settings.animate_cursor_blink) {
            self->cursor_blink_anim_delay_timer =
              TimerManager_create_timer(&self->timer_manager,
                                        TIMER_TYPE_POINT,
                                        App_cursor_fade_switch_delay_timer_handler);
            self->cursor_blink_switch_timer =
              TimerManager_create_timer(&self->timer_manager,
                                        TIMER_TYPE_TWEEN,
                                        App_cursor_fade_switch_timer_handler);
        } else {
            self->cursor_blink_switch_timer =
              TimerManager_create_timer(&self->timer_manager,
                                        TIMER_TYPE_POINT,
                                        App_cursor_blink_switch_timer_handler);
        }
    }

    self->text_blink_switch_timer = TimerManager_create_timer(&self->timer_manager,
                                                              TIMER_TYPE_POINT,
                                                              App_text_blink_switch_timer_handler);

    self->title_update_timer = TimerManager_create_timer(&self->timer_manager,
                                                         TIMER_TYPE_POINT,
                                                         App_title_update_timer_handler);

    self->cursor_movement_timer = TimerManager_create_timer(&self->timer_manager,
                                                            TIMER_TYPE_TWEEN,
                                                            App_cursor_movement_timer_handler);
}

int main(int argc, char** argv)
{
    settings_init(argc, argv);
    App application;
    App_init(&application);
    App_run(&application);
    settings_cleanup();
}
