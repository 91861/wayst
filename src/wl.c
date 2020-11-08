/* See LICENSE for license information. */

#ifndef NOWL

#define _GNU_SOURCE

#include "wl.h"
#include "timing.h"
#include "util.h"
#include "vector.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#include "wl_exts/kde-decoration.h"
#include "wl_exts/xdg-decoration.h"
#include "wl_exts/xdg-shell.h"

#include "eglerrors.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static WindowStatic* global;

DEF_VECTOR(char, NULL);

#define globalWl       ((GlobalWl*)&global->extend_data)
#define windowWl(base) ((WindowWl*)&base->extend_data)

static inline bool keysym_is_mod(xkb_keysym_t sym)
{
    return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || // Regular modifier keys
           (sym >= XKB_KEY_ISO_Lock &&
            sym <= XKB_KEY_ISO_Last_Group_Lock) || // Extension modifier keys
           (sym >= XKB_KEY_Multi_key &&
            sym <= XKB_KEY_PreviousCandidate); // International & multi-key
                                               // character composition
}

static inline bool keysym_is_misc(xkb_keysym_t sym)
{
    return (sym >= XKB_KEY_Select && sym <= XKB_KEY_Num_Lock) || // Regular misc keys
           (sym >= XKB_KEY_XF86Standby && sym <= XKB_KEY_XF86RotationLockToggle) ||
           (sym >= XKB_KEY_XF86ModeLock &&
            sym <= XKB_KEY_XF86MonBrightnessCycle) ||        // XFree86 vendor specific
           (sym >= XKB_KEY_Pause && sym <= XKB_KEY_Sys_Req); // TTY function keys
}

static inline bool keysym_is_dead(xkb_keysym_t sym)
{
    return (sym >= XKB_KEY_dead_grave && sym <= XKB_KEY_dead_currency) || // Extension function keys

           (sym >= XKB_KEY_dead_lowline &&
            sym <= XKB_KEY_dead_longsolidusoverlay) || // Extra dead elements for German T3 layout
           (sym >= XKB_KEY_dead_a &&
            sym <= XKB_KEY_dead_greek); // Dead vowels for universal syllable entry
}

static inline bool keysym_is_consumed(xkb_keysym_t sym)
{
    return sym == XKB_KEY_NoSymbol || keysym_is_mod(sym) || keysym_is_dead(sym) ||
           keysym_is_misc(sym);
}

PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC eglSwapBuffersWithDamageKHR;

static struct wl_data_source_listener data_source_listener;
static void                           cursor_set(struct wl_cursor* what, uint32_t serial);
static void                           WindowWl_dont_swap_buffers(struct WindowBase* self);
struct WindowBase*                    WindowWl_new(uint32_t w, uint32_t h);
static void       WindowWl_set_maximized(struct WindowBase* self, bool maximized);
static void       WindowWl_set_fullscreen(struct WindowBase* self, bool fullscreen);
static void       WindowWl_resize(struct WindowBase* self, uint32_t w, uint32_t h);
static void       WindowWl_events(struct WindowBase* self);
static TimePoint* WindowWl_process_timers(struct WindowBase* self);
static void       WindowWl_set_swap_interval(struct WindowBase* self, int32_t ival);
static void       WindowWl_set_wm_name(struct WindowBase* self, const char* title);
static void       WindowWl_set_title(struct WindowBase* self, const char* title);
static bool       WindowWl_maybe_swap(struct WindowBase* self);
static void       WindowWl_destroy(struct WindowBase* self);
static int        WindowWl_get_connection_fd(struct WindowBase* self);
static void       WindowWl_clipboard_send(struct WindowBase* self, const char* text);
static void       WindowWl_clipboard_get(struct WindowBase* self);
static void*      WindowWl_get_gl_ext_proc_adress(struct WindowBase* self, const char* name);
static uint32_t   WindowWl_get_keycode_from_name(struct WindowBase* self, char* name);
static void       WindowWl_set_pointer_style(struct WindowBase* self, enum MousePointerStyle style);
static void       WindowWl_set_current_context(struct WindowBase* self, bool this);
static void       WindowWl_set_urgent(struct WindowBase* self);
static void       WindowWl_set_stack_order(struct WindowBase* self, bool front_or_back);
static int64_t    WindowWl_get_window_id(struct WindowBase* self)
{
    return -1;
}

static struct IWindow window_interface_wayland = {
    .set_fullscreen         = WindowWl_set_fullscreen,
    .set_maximized          = WindowWl_set_maximized,
    .resize                 = WindowWl_resize,
    .events                 = WindowWl_events,
    .process_timers         = WindowWl_process_timers,
    .set_title              = WindowWl_set_title,
    .maybe_swap             = WindowWl_maybe_swap,
    .destroy                = WindowWl_destroy,
    .get_connection_fd      = WindowWl_get_connection_fd,
    .clipboard_send         = WindowWl_clipboard_send,
    .clipboard_get          = WindowWl_clipboard_get,
    .set_swap_interval      = WindowWl_set_swap_interval,
    .get_gl_ext_proc_adress = WindowWl_get_gl_ext_proc_adress,
    .get_keycode_from_name  = WindowWl_get_keycode_from_name,
    .set_pointer_style      = WindowWl_set_pointer_style,
    .set_current_context    = WindowWl_set_current_context,
    .set_urgent             = WindowWl_set_urgent,
    .set_stack_order        = WindowWl_set_stack_order,
    .get_window_id          = WindowWl_get_window_id,
};

typedef struct
{
    struct xkb_context* ctx;
    struct xkb_keymap*  keymap;
    struct xkb_state*   state;
    struct xkb_state*   clean_state;

    struct xkb_compose_table* compose_table;
    struct xkb_compose_state* compose_state;

    xkb_mod_mask_t ctrl_mask;
    xkb_mod_mask_t alt_mask;
    xkb_mod_mask_t shift_mask;

} Xkb;

typedef struct
{
    EGLDisplay          egl_display;
    struct wl_display*  display;
    struct wl_registry* registry;

    struct wl_compositor* compositor;
    struct wl_output*     output;
    struct wl_shm*        shm;

    struct wl_data_device_manager* data_device_manager;
    struct wl_data_device*         data_device;

    struct wl_shell*                   wl_shell;
    struct xdg_wm_base*                xdg_shell;
    struct zxdg_decoration_manager_v1* decoration_manager;

    struct wl_seat*     seat;
    struct wl_pointer*  pointer;
    struct wl_keyboard* keyboard;

    struct wl_cursor *      cursor_arrow, *cursor_beam, *cursor_hand;
    struct wl_cursor_theme* cursor_theme;
    struct wl_surface*      cursor_surface;

    int32_t   kbd_repeat_dealy, kbd_repeat_rate;
    uint32_t  keycode_to_repeat;
    uint32_t  last_button_pressed;
    TimePoint repeat_point;

    uint32_t serial;

    Xkb xkb;

} GlobalWl;

typedef struct
{
    struct wl_output* output;
    bool              is_active;
    int32_t           target_frame_time_ms;
} WlOutputInfo;

DEF_VECTOR(WlOutputInfo, NULL);

typedef struct
{
    struct wl_surface*       surface;
    struct wl_shell_surface* shell_surface;

    struct wl_egl_window* egl_window;
    EGLSurface            egl_surface;
    EGLContext            egl_context;

    struct xdg_surface*                 xdg_surface;
    struct xdg_toplevel*                xdg_toplevel;
    struct zxdg_toplevel_decoration_v1* toplevel_decoration;

    struct wl_data_offer*  data_offer;
    struct wl_data_source* data_source;
    struct wl_data_offer*  dnd_data_offer;
    int8_t                 data_offer_mime_idx; // -1 for none
    const char*            data_source_text;

    bool got_discrete_axis_event;

    Vector_WlOutputInfo outputs;
    WlOutputInfo*       active_output;
    bool                draw_next_frame;

} WindowWl;

/* in order of preference */
static const char* ACCEPTED_DND_MIMES[] = {
    "text/uri-list", "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT",
};

static inline xkb_keysym_t keysym_filter_compose(xkb_keysym_t sym)
{
    if (!globalWl->xkb.compose_state || sym == XKB_KEY_NoSymbol)
        return sym;

    if (xkb_compose_state_feed(globalWl->xkb.compose_state, sym) != XKB_COMPOSE_FEED_ACCEPTED) {
        return sym;
    }

    switch (xkb_compose_state_get_status(globalWl->xkb.compose_state)) {
        default:
        case XKB_COMPOSE_NOTHING:
            return sym;
        case XKB_COMPOSE_COMPOSING:
        case XKB_COMPOSE_CANCELLED:
            return XKB_KEY_NoSymbol;
        case XKB_COMPOSE_COMPOSED:
            return xkb_compose_state_get_one_sym(globalWl->xkb.compose_state);
    }
}

static void WindowWl_swap_buffers(struct WindowBase* self);

static void WindowWl_drain_pipe_to_clipboard(struct WindowBase* self,
                                             int                pipe_fd,
                                             bool               convert_mime_list)
{
#define PIPE_READ_CHUNK_SIZE 1024
    char        buf[PIPE_READ_CHUNK_SIZE];
    Vector_char text = Vector_new_char();

    ssize_t rd;
    do {
        errno = 0;
        rd    = read(pipe_fd, buf, PIPE_READ_CHUNK_SIZE);

        if (rd <= 0) {
            if (errno == EAGAIN) {
                continue;
            } else if (errno == EWOULDBLOCK) {
                break;
            } else if (errno) {
                WRN("IO error: %s\n", strerror(errno));
            }
        }

        // MIN to supress overflow warning
        Vector_pushv_char(&text, buf, MIN(rd, PIPE_READ_CHUNK_SIZE));
    } while (rd > 0);

    Vector_push_char(&text, '\0');

    if (convert_mime_list) {
        Vector_char conv = Vector_new_char();
        char*       seq  = (char*)text.buf;
        for (char* a; (a = strsep(&seq, "\n"));) {
            char* start = strstr((char*)a, "://");
            if (start) {
                start += 3;
                Vector_pushv_char(&conv, start, strlen(start) - 1);
                Vector_push_char(&conv, ' ');
            } else {
                Vector_pop_char(&conv);
            }
        }
        Vector_push_char(&conv, '\0');
        Vector_destroy_char(&text);
        text = conv;
    }

    CALL_FP(self->callbacks.clipboard_handler, self->callbacks.user_data, text.buf);
    Vector_destroy_char(&text);
}

static void WindowWl_clipboard_send(struct WindowBase* self, const char* text)
{
    if (!text)
        return;

    WindowWl* w = windowWl(self);

    LOG("making a data source\n");

    free((void*)w->data_source_text);

    windowWl(self)->data_source_text = text;
    windowWl(self)->data_source =
      wl_data_device_manager_create_data_source(globalWl->data_device_manager);
    wl_data_source_add_listener(w->data_source, &data_source_listener, self);

    wl_data_source_offer(w->data_source, "text/plain;charset=utf-8");

    wl_data_device_set_selection(globalWl->data_device, w->data_source, globalWl->serial);
}

static void WindowWl_clipboard_get(struct WindowBase* self)
{
    if (windowWl(self)->data_offer_mime_idx > -1) {
        LOG("last recorded wl_data_offer mime: \"%s\" \n",
            ACCEPTED_DND_MIMES[windowWl(self)->data_offer_mime_idx]);
    }

    if (windowWl(self)->data_offer) {
        int fds[2];

        errno = 0;
        if (pipe(fds)) {
            WRN("IO error: %s\n", strerror(errno));
            return;
        }

        wl_data_offer_receive(windowWl(self)->data_offer,
                              ACCEPTED_DND_MIMES[windowWl(self)->data_offer_mime_idx],
                              fds[1]);

        close(fds[1]);
        wl_display_roundtrip(globalWl->display);
        WindowWl_drain_pipe_to_clipboard(self, fds[0], false);
        close(fds[0]);
    }
}

static void frame_handle_done(void* data, struct wl_callback* callback, uint32_t time)
{
    wl_callback_destroy(callback);
    windowWl(((struct WindowBase*)data))->draw_next_frame = true;
}

static struct wl_callback_listener frame_listener = {
    .done = &frame_handle_done,
};

static void wl_surface_handle_enter(void*              data,
                                    struct wl_surface* wl_surface,
                                    struct wl_output*  output)
{
    struct WindowBase* win_base = data;
    WindowWl*          win      = windowWl(win_base);

    for (WlOutputInfo* i = NULL; (i = Vector_iter_WlOutputInfo(&win->outputs, i));) {
        if (output == i->output) {
            i->is_active                 = true;
            global->target_frame_time_ms = i->target_frame_time_ms;
            win->active_output           = i;
            break;
        }
    }
    FLAG_UNSET(win_base->state_flags, WINDOW_IS_MINIMIZED);
}

static void wl_surface_handle_leave(void*              data,
                                    struct wl_surface* wl_surface,
                                    struct wl_output*  output)
{
    struct WindowBase* win_base = data;
    WindowWl*          win      = windowWl(win_base);

    win->active_output = NULL;
    for (WlOutputInfo* i = NULL; (i = Vector_iter_WlOutputInfo(&win->outputs, i));) {
        if (output == i->output) {
            i->is_active = false;
        } else if (i->is_active) {
            win->active_output = i;
        }
    }
    if (!win->active_output) {
        FLAG_SET(win_base->state_flags, WINDOW_IS_MINIMIZED);
    }
}

static struct wl_surface_listener wl_surface_listener = {
    .enter = wl_surface_handle_enter,
    .leave = wl_surface_handle_leave,
};

/* Pointer listener */
static void pointer_handle_enter(void*              data,
                                 struct wl_pointer* pointer,
                                 uint32_t           serial,
                                 struct wl_surface* surface,
                                 wl_fixed_t         x,
                                 wl_fixed_t         y)
{
    struct WindowBase* win = data;
    FLAG_UNSET(win->state_flags, WINDOW_IS_POINTER_HIDDEN);
    cursor_set(globalWl->cursor_arrow, serial);
    win->pointer_x = wl_fixed_to_int(x);
    win->pointer_y = wl_fixed_to_int(y);
    win->callbacks.activity_notify_handler(win->callbacks.user_data);
    globalWl->serial = serial;
    Window_notify_content_change(data);
}

static void pointer_handle_leave(void*              data,
                                 struct wl_pointer* pointer,
                                 uint32_t           serial,
                                 struct wl_surface* surface)
{
    globalWl->serial = serial;
}

static void pointer_handle_motion(void*              data,
                                  struct wl_pointer* pointer,
                                  uint32_t           serial,
                                  wl_fixed_t         x,
                                  wl_fixed_t         y)
{
    globalWl->serial       = serial;
    struct WindowBase* win = data;
    win->pointer_x         = wl_fixed_to_int(x);
    win->pointer_y         = wl_fixed_to_int(y);
    if (FLAG_IS_SET(win->state_flags, WINDOW_IS_POINTER_HIDDEN)) {
        cursor_set(globalWl->cursor_arrow, 0);
        FLAG_UNSET(win->state_flags, WINDOW_IS_POINTER_HIDDEN);
    }

    win->callbacks.motion_handler(win->callbacks.user_data,
                                  globalWl->last_button_pressed,
                                  win->pointer_x,
                                  win->pointer_y);
}

static void pointer_handle_button(void*              data,
                                  struct wl_pointer* wl_pointer,
                                  uint32_t           serial,
                                  uint32_t           time,
                                  uint32_t           button,
                                  uint32_t           state)
{
    struct WindowBase* win = data;

    globalWl->serial = serial;

    uint32_t final_mods = 0;
    uint32_t mods       = xkb_state_serialize_mods(globalWl->xkb.state, XKB_STATE_MODS_EFFECTIVE);

    if (FLAG_IS_SET(mods, globalWl->xkb.ctrl_mask)) {
        FLAG_SET(final_mods, MODIFIER_CONTROL);
    }
    if (FLAG_IS_SET(mods, globalWl->xkb.alt_mask)) {
        FLAG_SET(final_mods, MODIFIER_ALT);
    }
    if (FLAG_IS_SET(mods, globalWl->xkb.shift_mask)) {
        FLAG_SET(final_mods, MODIFIER_SHIFT);
    }
    /* in wl MMB code is 3 +271 and RMB 2 +271, but in X11 it's 2 and 3 */
    button                        = button == 2 + 271 ? 3 : button == 3 + 271 ? 2 : button - 271;
    globalWl->last_button_pressed = state ? button : 0;

    CALL_FP(win->callbacks.button_handler,
            win->callbacks.user_data,
            button,
            state,
            win->pointer_x,
            win->pointer_y,
            0,
            final_mods);
}

static void pointer_handle_axis(void*              data,
                                struct wl_pointer* wl_pointer,
                                uint32_t           time,
                                uint32_t           axis,
                                wl_fixed_t         value)
{
    struct WindowBase* win = data;
    int32_t            v   = wl_fixed_to_int(value);

    if (v && !windowWl(win)->got_discrete_axis_event) {
        CALL_FP(win->callbacks.button_handler,
                win->callbacks.user_data,
                v < 0 ? 65 : 66,
                1,
                win->pointer_x,
                win->pointer_y,
                v < 0 ? -v : v,
                0);
    }

    windowWl(win)->got_discrete_axis_event = false;
}

static void pointer_handle_frame(void* data, struct wl_pointer* pointer) {}

static void pointer_handle_axis_source(void*              data,
                                       struct wl_pointer* wl_pointer,
                                       uint32_t           axis_source)
{}

static void pointer_handle_axis_stop(void*              data,
                                     struct wl_pointer* wl_pointer,
                                     uint32_t           time,
                                     uint32_t           axis)
{}

static void pointer_handle_axis_discrete(void*              data,
                                         struct wl_pointer* wl_pointer,
                                         uint32_t           axis,
                                         int32_t            discrete)
{
    struct WindowBase* win = data;
    /* this is sent before a coresponding axis event, tell it to do nothing */
    windowWl(win)->got_discrete_axis_event = true;

    CALL_FP(win->callbacks.button_handler,
            win->callbacks.user_data,
            discrete < 0 ? 65 : 66,
            1,
            win->pointer_x,
            win->pointer_y,
            0,
            0);
}

static struct wl_pointer_listener pointer_listener = {
    .enter         = pointer_handle_enter,
    .leave         = pointer_handle_leave,
    .motion        = pointer_handle_motion,
    .button        = pointer_handle_button,
    .axis          = pointer_handle_axis,
    .frame         = pointer_handle_frame,
    .axis_source   = pointer_handle_axis_source,
    .axis_stop     = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
};

/* Keyboard listener */
static void keyboard_handle_keymap(void*               data,
                                   struct wl_keyboard* keyboard,
                                   uint32_t            format,
                                   int                 fd,
                                   uint32_t            size)
{
    // this is sent when the keyboard configuration changes

    ASSERT(globalWl->xkb.ctx, "xkb context not created in keyboard::handle_keymap");

    if (globalWl->xkb.keymap) {
        if (globalWl->xkb.compose_state)
            xkb_compose_state_unref(globalWl->xkb.compose_state);
        globalWl->xkb.compose_state = NULL;

        if (globalWl->xkb.compose_table)
            xkb_compose_table_unref(globalWl->xkb.compose_table);
        globalWl->xkb.compose_table = NULL;

        xkb_state_unref(globalWl->xkb.state);
        globalWl->xkb.state = NULL;

        xkb_state_unref(globalWl->xkb.clean_state);
        globalWl->xkb.clean_state = NULL;

        xkb_keymap_unref(globalWl->xkb.keymap);
        globalWl->xkb.keymap = NULL;
    }

    char* map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (map_str == MAP_FAILED)
        ERR("Reading keymap info failed");

    globalWl->xkb.keymap = xkb_keymap_new_from_string(globalWl->xkb.ctx,
                                                      map_str,
                                                      XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(map_str, size);
    close(fd);

    if (!globalWl->xkb.keymap)
        ERR("Failed to generate keymap");

    globalWl->xkb.state       = xkb_state_new(globalWl->xkb.keymap);
    globalWl->xkb.clean_state = xkb_state_new(globalWl->xkb.keymap);

    if (!globalWl->xkb.state || !globalWl->xkb.clean_state)
        ERR("Failed to create keyboard state");

    ASSERT(settings.locale.str, "locale string is NULL")
    const char* compose_file_name = getenv("XCOMPOSEFILE");
    FILE*       compose_file      = NULL;
    if (compose_file_name && *compose_file_name && (compose_file = fopen(compose_file_name, "r"))) {
        LOG("using XCOMPOSEFILE = %s\n", compose_file_name);
        globalWl->xkb.compose_table = xkb_compose_table_new_from_file(globalWl->xkb.ctx,
                                                                      compose_file,
                                                                      settings.locale.str,
                                                                      XKB_COMPOSE_FORMAT_TEXT_V1,
                                                                      XKB_COMPOSE_COMPILE_NO_FLAGS);
        fclose(compose_file);
    } else {
        globalWl->xkb.compose_table =
          xkb_compose_table_new_from_locale(globalWl->xkb.ctx,
                                            settings.locale.str,
                                            XKB_COMPOSE_COMPILE_NO_FLAGS);
    }

    if (!globalWl->xkb.compose_table)
        ERR("Failed to generate keyboard compose table, is locale \'%s\' "
            "correct?",
            settings.locale.str);

    globalWl->xkb.compose_state =
      xkb_compose_state_new(globalWl->xkb.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);

    if (!globalWl->xkb.compose_state)
        ERR("Failed to create compose state");

    globalWl->xkb.ctrl_mask  = 1 << xkb_keymap_mod_get_index(globalWl->xkb.keymap, "Control");
    globalWl->xkb.alt_mask   = 1 << xkb_keymap_mod_get_index(globalWl->xkb.keymap, "Mod1");
    globalWl->xkb.shift_mask = 1 << xkb_keymap_mod_get_index(globalWl->xkb.keymap, "Shift");

    return;
}

static void keyboard_handle_enter(void*               data,
                                  struct wl_keyboard* keyboard,
                                  uint32_t            serial,
                                  struct wl_surface*  surface,
                                  struct wl_array*    keys)
{
    ((struct WindowBase*)data)
      ->callbacks.on_focus_changed(((struct WindowBase*)data)->callbacks.user_data, true);
    FLAG_SET(((struct WindowBase*)data)->state_flags, WINDOW_IS_IN_FOCUS);
}

static void keyboard_handle_leave(void*               data,
                                  struct wl_keyboard* keyboard,
                                  uint32_t            serial,
                                  struct wl_surface*  surface)
{
    globalWl->serial = serial;
    ((struct WindowBase*)data)
      ->callbacks.on_focus_changed(((struct WindowBase*)data)->callbacks.user_data, false);
    FLAG_UNSET(((struct WindowBase*)data)->state_flags, WINDOW_IS_IN_FOCUS);
    globalWl->keycode_to_repeat = 0;
}

static void keyboard_handle_key(void*               data,
                                struct wl_keyboard* keyboard,
                                uint32_t            serial,
                                uint32_t            time,
                                uint32_t            key,
                                uint32_t            state)
{
    bool               is_repeat_event = !keyboard;
    struct WindowBase* win             = data;
    uint32_t           utf, code = key + 8;
    xkb_keysym_t       sym, rawsym, composed_sym;

    if (!is_repeat_event) {
        globalWl->serial = serial;
        FLAG_SET(win->state_flags, WINDOW_NEEDS_SWAP);
    }

    sym = composed_sym = xkb_state_key_get_one_sym(globalWl->xkb.state, code);

    if (keysym_is_mod(sym))
        return;

    rawsym = xkb_state_key_get_one_sym(globalWl->xkb.clean_state, code);

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        composed_sym = keysym_filter_compose(sym);
    }

    if (composed_sym != sym) {
        utf = xkb_keysym_to_utf32(composed_sym);
    } else {
        utf = xkb_state_key_get_utf32(globalWl->xkb.state, code);
    }

    uint32_t final_mods = 0;
    uint32_t mods       = xkb_state_serialize_mods(globalWl->xkb.state, XKB_STATE_MODS_EFFECTIVE);

    LOG("Wl::key{ key: %d code: %d state: %d repeat: %d sym: %d rawsym: %d utfcode: %d }\n",
        key,
        code,
        state,
        is_repeat_event,
        sym,
        rawsym,
        utf);

    // xkb will signal a failed conversion to utf32 by returning 0, but 0 is the expected result for
    // Ctrl + `
    bool utf_conversion_success =
      utf || (sym == XKB_KEY_grave && FLAG_IS_SET(mods, globalWl->xkb.ctrl_mask));

    bool is_not_consumed = utf_conversion_success ? true : !keysym_is_consumed(sym);

    if (FLAG_IS_SET(mods, globalWl->xkb.ctrl_mask)) {
        FLAG_SET(final_mods, MODIFIER_CONTROL);
    }
    if (FLAG_IS_SET(mods, globalWl->xkb.alt_mask)) {
        FLAG_SET(final_mods, MODIFIER_ALT);
    }
    if (FLAG_IS_SET(mods, globalWl->xkb.shift_mask)) {
        FLAG_SET(final_mods, MODIFIER_SHIFT);
    }

    uint32_t final = utf_conversion_success ? utf : sym;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED && is_not_consumed) {
        globalWl->keycode_to_repeat = key;
        if (!is_repeat_event) {
            globalWl->repeat_point = TimePoint_ms_from_now(globalWl->kbd_repeat_dealy);
        }
        if (win->callbacks.key_handler) {
            win->callbacks.key_handler(win->callbacks.user_data, final, rawsym, final_mods);
        }
    } else if (globalWl->keycode_to_repeat == key) {
        globalWl->keycode_to_repeat = 0;
    }
}

static void keyboard_handle_modifiers(void*               data,
                                      struct wl_keyboard* keyboard,
                                      uint32_t            serial,
                                      uint32_t            mods_depressed,
                                      uint32_t            mods_latched,
                                      uint32_t            mods_locked,
                                      uint32_t            group)
{
    globalWl->serial = serial;
    xkb_state_update_mask(globalWl->xkb.state,
                          mods_depressed,
                          mods_latched,
                          mods_locked,
                          0,
                          0,
                          group);
}

static void keyboard_handle_repeat_info(void*               data,
                                        struct wl_keyboard* wl_keyboard,
                                        int32_t             rate,
                                        int32_t             delay)
{
    globalWl->kbd_repeat_rate  = OR(rate, 33);
    globalWl->kbd_repeat_dealy = OR(delay, 500);
}

static const struct wl_keyboard_listener keyboard_listener = { .keymap = keyboard_handle_keymap,
                                                               .enter  = keyboard_handle_enter,
                                                               .leave  = keyboard_handle_leave,
                                                               .key    = keyboard_handle_key,
                                                               .modifiers =
                                                                 keyboard_handle_modifiers,
                                                               .repeat_info =
                                                                 keyboard_handle_repeat_info };

/* Seat listener */
static void seat_test_capabilities(void* data, struct wl_seat* seat, uint32_t caps)
{
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (globalWl->pointer) {
            wl_pointer_destroy(globalWl->pointer);
        }
        globalWl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(globalWl->pointer, &pointer_listener, data);
    }

    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        if (globalWl->keyboard) {
            wl_keyboard_destroy(globalWl->keyboard);
        }

        globalWl->keyboard = wl_seat_get_keyboard(globalWl->seat);
        wl_keyboard_add_listener(globalWl->keyboard, &keyboard_listener, data);
    } else if (!globalWl->keyboard) {
        WRN("No keyboard capability found for seat\n");
    }
}

static void seat_handle_name(void* data, struct wl_seat* seat, const char* name) {}

static struct wl_seat_listener seat_listener = { .capabilities = seat_test_capabilities,
                                                 .name         = seat_handle_name };

/* zxdg_toplevel_decoration_listener */
static void zxdg_toplevel_decoration_manager_handle_configure(
  void*                               data,
  struct zxdg_toplevel_decoration_v1* zxdg_toplevel_decoration_v1,
  uint32_t                            mode)
{}

static const struct zxdg_toplevel_decoration_v1_listener zxdg_toplevel_decoration_listener = {
    .configure = zxdg_toplevel_decoration_manager_handle_configure
};

/* xdg_wm_base_listener */
static void xdg_wm_base_ping(void* data, struct xdg_wm_base* shell, uint32_t serial)
{
    globalWl->serial = serial;
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

/* xdg_surface_listener */
static void xdg_surface_handle_configure(void*               data,
                                         struct xdg_surface* xdg_surface,
                                         uint32_t            serial)
{
    globalWl->serial = serial;
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = { .configure =
                                                                    xdg_surface_handle_configure };

/* xdg_toplevel_listener */
static void xdg_toplevel_handle_close(void* data, struct xdg_toplevel* xdg_surface)
{
    FLAG_SET(((struct WindowBase*)data)->state_flags, WINDOW_IS_CLOSED);
}

static void xdg_toplevel_handle_configure(void*                data,
                                          struct xdg_toplevel* xdg_toplevel,
                                          int32_t              width,
                                          int32_t              height,
                                          struct wl_array*     states)
{
    struct WindowBase* win = data;
    /* enum xdg_toplevel_state* s; */
    /* bool                     is_active = false; */
    /* wl_array_for_each(s, states) { */
    /*     if (*s == XDG_TOPLEVEL_STATE_ACTIVATED) { */
    /*         is_active = true; */
    /*     } */
    /* } */
    if (!width && !height) {
        wl_egl_window_resize(windowWl(win)->egl_window, win->w, win->h, 0, 0);
    } else {
        win->w = width;
        win->h = height;
    }

    Window_notify_content_change(win);
    wl_egl_window_resize(windowWl(win)->egl_window, win->w, win->h, 0, 0);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_handle_configure,
    .close     = xdg_toplevel_handle_close,
};

/* wl_shell listener */
static void shell_surface_ping(void* data, struct wl_shell_surface* shell_surface, uint32_t serial)
{
    globalWl->serial = serial;
    wl_shell_surface_pong(shell_surface, serial);
}

static void shell_surface_configure(void*                    data,
                                    struct wl_shell_surface* shell_surface,
                                    uint32_t                 edges,
                                    int32_t                  width,
                                    int32_t                  height)
{
    struct WindowBase* win = data;
    wl_egl_window_resize(windowWl(win)->egl_window, width, height, 0, 0);
    win->w = width;
    win->h = height;
}

static void shell_surface_popup_done(void* data, struct wl_shell_surface* shell_surface) {}

static const struct wl_shell_surface_listener shell_surface_listener = {
    .ping       = shell_surface_ping,
    .configure  = shell_surface_configure,
    .popup_done = shell_surface_popup_done,
};

/* Output listener */
static void output_handle_geometry(void*             data,
                                   struct wl_output* wl_output,
                                   int32_t           x,
                                   int32_t           y,
                                   int32_t           physical_width,
                                   int32_t           physical_height,
                                   int32_t           subpixel,
                                   const char*       make,
                                   const char*       model,
                                   int32_t           transform)
{
    enum LcdFilter settings_value = LCD_FILTER_UNDEFINED;

    switch (subpixel) {
        case WL_OUTPUT_SUBPIXEL_NONE:
            break;
        case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:
            settings_value = LCD_FILTER_V_BGR;
            break;
        case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:
            settings_value = LCD_FILTER_V_RGB;
            break;
        case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:
            settings_value = LCD_FILTER_H_BGR;
            break;
        case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:
            settings_value = LCD_FILTER_H_RGB;
            break;
    }

    if (settings.lcd_filter == LCD_FILTER_UNDEFINED)
        settings.lcd_filter = settings_value;
}

static void output_handle_mode(void*             data,
                               struct wl_output* wl_output,
                               uint32_t          flags,
                               int32_t           w,
                               int32_t           h,
                               int32_t           refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        WindowWl* win           = windowWl(((struct WindowBase*)data));
        int32_t   frame_time_ms = 1000000 / refresh;
        bool      make_active   = !win->outputs.size;

        Vector_push_WlOutputInfo(&win->outputs,
                                 (WlOutputInfo){
                                   .output               = wl_output,
                                   .is_active            = make_active,
                                   .target_frame_time_ms = frame_time_ms,
                                 });
        if (make_active) {
            win->active_output = Vector_last_WlOutputInfo(&win->outputs);
        }
    }
}

static void output_handle_done(void* data, struct wl_output* wl_output) {}

static void output_handle_scale(void* data, struct wl_output* wl_output, int32_t factor) {}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode     = output_handle_mode,
    .done     = output_handle_done,
    .scale    = output_handle_scale,
};

/* End output listener */

/* data device listener */

static void data_offer_handle_offer(void*                 data,
                                    struct wl_data_offer* wl_data_offer,
                                    const char*           mime_type)
{
    WindowWl* w = windowWl(((struct WindowBase*)data));

    LOG("wl.data_offer::offer{ mime_type: %s ", mime_type);

    for (uint_fast8_t i = 0; i < ARRAY_SIZE(ACCEPTED_DND_MIMES); ++i) {
        if (!strcmp(mime_type, ACCEPTED_DND_MIMES[i]) &&
            (w->data_offer_mime_idx == -1 || w->data_offer_mime_idx > i)) {
            LOG("- ACCEPTED }\n");
            w->data_offer          = wl_data_offer;
            w->data_offer_mime_idx = i;
            wl_data_offer_accept(wl_data_offer, 0, mime_type);
            return;
        }
    }

    if (w->data_offer_mime_idx == -1) {
        LOG(" - REJECTED(not supported) }\n");
    } else {
        LOG(" - REJECTED(\'%s\' is prefferable) }\n", ACCEPTED_DND_MIMES[w->data_offer_mime_idx]);
    }

    wl_data_offer_accept(wl_data_offer, 0, NULL);
}

static void data_offer_handle_source_actions(void*                 data,
                                             struct wl_data_offer* wl_data_offer,
                                             uint32_t              source_actions)
{
    LOG("wl.data_offer::source_actions{ supported actions: ");
    if (source_actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) {
        LOG("copy ");
    }
    if (source_actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE) {
        LOG("move ");
    }
    LOG("}\n");
}

static void data_offer_handle_action(void*                 data,
                                     struct wl_data_offer* wl_data_offer,
                                     uint32_t              dnd_action)
{
    WindowWl* w = windowWl(((struct WindowBase*)data));
    LOG("wl.data_offer::action{ current action: ");
    switch (dnd_action) {
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE:
            LOG("none");
            break;
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY:
            LOG("copy");
            if (w->data_offer_mime_idx > -1) {
                wl_data_offer_accept(wl_data_offer,
                                     globalWl->serial,
                                     ACCEPTED_DND_MIMES[w->data_offer_mime_idx]);
            }
            break;
        case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE:
            LOG("move");
            break;
    }
    LOG(" }\n");
}

static const struct wl_data_offer_listener data_offer_listener = {
    .offer          = data_offer_handle_offer,
    .source_actions = data_offer_handle_source_actions,
    .action         = data_offer_handle_action,
};

static void data_device_handle_data_offer(void*                  data,
                                          struct wl_data_device* wl_data_device,
                                          struct wl_data_offer*  offer)
{
    LOG("wl.data_device::offer\n");
    wl_data_offer_add_listener(offer, &data_offer_listener, data);
}

static void data_device_handle_enter(void*                  data,
                                     struct wl_data_device* wl_data_device,
                                     uint32_t               serial,
                                     struct wl_surface*     surface,
                                     wl_fixed_t             x,
                                     wl_fixed_t             y,
                                     struct wl_data_offer*  offer)
{
    /* attempt to initiate drag and drop */
    globalWl->serial = serial;

    LOG("wl.data_device::enter{ x: %f, y: %f }\n", wl_fixed_to_double(x), wl_fixed_to_double(y));

    WindowWl* self = windowWl(((struct WindowBase*)data));

    self->dnd_data_offer = offer;
    wl_data_offer_set_actions(self->dnd_data_offer,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
}

static void data_device_handle_leave(void* data, struct wl_data_device* wl_data_device)
{
    WindowWl* self            = windowWl(((struct WindowBase*)data));
    self->dnd_data_offer      = NULL;
    self->data_offer_mime_idx = -1;
    LOG("wl.data_device::leave\n");
}

static void data_device_handle_motion(void*                  data,
                                      struct wl_data_device* wl_data_device,
                                      uint32_t               time,
                                      wl_fixed_t             x,
                                      wl_fixed_t             y)
{
    WindowWl* self = windowWl(((struct WindowBase*)data));

    LOG("wl.data_device::motion{ x: %f, y: %f, t: %u }\n",
        wl_fixed_to_double(x),
        wl_fixed_to_double(y),
        time);

    if (!self->dnd_data_offer) {
        return;
    }

    wl_data_offer_set_actions(self->dnd_data_offer,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
}

static void data_device_handle_drop(void* data, struct wl_data_device* wl_data_device)
{
    LOG("wl.data_device::drop{ ");

    struct WindowBase* base = (struct WindowBase*)data;
    WindowWl*          self = windowWl(base);

    if (!self->dnd_data_offer) {
        LOG("<offer expired> }\n");
        return;
    }

    struct wl_data_offer* offer    = self->dnd_data_offer;
    int                   mime_idx = self->data_offer_mime_idx;

    LOG("mime: %s }\n", ACCEPTED_DND_MIMES[mime_idx]);

    int fds[2];
    errno = 0;
    if (pipe(fds)) {
        WRN("IO error: %s\n", strerror(errno));
        return;
    }
    wl_data_offer_receive(offer, ACCEPTED_DND_MIMES[mime_idx], fds[1]);
    close(fds[1]);

    wl_display_roundtrip(globalWl->display);
    WindowWl_drain_pipe_to_clipboard(base, fds[0], mime_idx == 0);
    close(fds[0]);

    wl_data_offer_finish(offer);
    wl_data_offer_destroy(offer);
    self->dnd_data_offer = NULL;
}

static void data_device_handle_selection(void*                  data,
                                         struct wl_data_device* wl_data_device,
                                         struct wl_data_offer*  offer)
{
    LOG("wl.data_device::selection { has_offer: %d }\n", offer != NULL);

    if (!offer) {
        return;
    }
}

static struct wl_data_device_listener data_device_listener = {
    .data_offer = data_device_handle_data_offer,
    .enter      = data_device_handle_enter,
    .leave      = data_device_handle_leave,
    .motion     = data_device_handle_motion,
    .drop       = data_device_handle_drop,
    .selection  = data_device_handle_selection,
};

static void data_source_handle_target(void*                  data,
                                      struct wl_data_source* wl_data_source,
                                      const char*            mime_type)
{
    LOG("wl.data_source::target\n");
}

static void data_source_handle_send(void*                  data,
                                    struct wl_data_source* wl_data_source,
                                    const char*            mime_type,
                                    int32_t                fd)
{
    LOG("wl.data_source::send{ mime_type: %s }\n", mime_type);

    if (windowWl(((struct WindowBase*)data))->data_source_text &&
        (!strcmp(mime_type, "text/plain") || !strcmp(mime_type, "text/plain;charset=utf-8") ||
         !strcmp(mime_type, "TEXT") || !strcmp(mime_type, "STRING") ||
         !strcmp(mime_type, "UTF8_STRING"))) {
        LOG("writing \'%s\' to fd\n", windowWl(((struct WindowBase*)data))->data_source_text);

        write(fd,
              windowWl(((struct WindowBase*)data))->data_source_text,
              strlen(windowWl(((struct WindowBase*)data))->data_source_text));
    }
    close(fd);
}

static void data_source_handle_cancelled(void* data, struct wl_data_source* wl_data_source)
{
    LOG("wl.data_source::canceled\n");
}

static void data_source_handle_dnd_drop_performed(void* data, struct wl_data_source* wl_data_source)
{
    LOG("wl.data_source::dnd_drop_performed\n");
}

static void data_source_handle_dnd_finished(void* data, struct wl_data_source* wl_data_source)
{
    LOG("wl.data_source::dnd_finished\n");
}

static void data_source_handle_action(void*                  data,
                                      struct wl_data_source* wl_data_source,
                                      uint32_t               dnd_action)
{
    LOG("wl.data_source::action\n");
}

static struct wl_data_source_listener data_source_listener = {
    .target             = data_source_handle_target,
    .send               = data_source_handle_send,
    .cancelled          = data_source_handle_cancelled,
    .dnd_drop_performed = data_source_handle_dnd_drop_performed,
    .dnd_finished       = data_source_handle_dnd_finished,
    .action             = data_source_handle_action,
};

/* End data device listener */

/* Registry listener */
static void registry_add(void*               data,
                         struct wl_registry* registry,
                         uint32_t            name,
                         const char*         interface,
                         uint32_t            version)
{
    LOG("wl_registry.name{ name: %-40s, ver: %2u", interface, version);

    if (!strcmp(interface, wl_compositor_interface.name)) {
        globalWl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version);
    } else if (!strcmp(interface, wl_shell_interface.name)) {
        globalWl->wl_shell = wl_registry_bind(registry, name, &wl_shell_interface, version);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        globalWl->xdg_shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, version);
        xdg_wm_base_add_listener(globalWl->xdg_shell, &wm_base_listener, data);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        globalWl->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
        wl_seat_add_listener(globalWl->seat, &seat_listener, data);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        globalWl->output = wl_registry_bind(registry, name, &wl_output_interface, version);
        wl_output_add_listener(globalWl->output, &output_listener, data);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        globalWl->decoration_manager =
          wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, version);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        globalWl->shm = wl_registry_bind(registry, name, &wl_shm_interface, version);
    } else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
        globalWl->data_device_manager =
          wl_registry_bind(registry, name, &wl_data_device_manager_interface, version);
    } else {
        LOG(" - unused");
    }
    LOG(" }\n");
}

static void registry_remove(void* data, struct wl_registry* registry, uint32_t name) {}

static struct wl_registry_listener registry_listener = {
    .global        = registry_add,
    .global_remove = registry_remove,
};

/* End registry listener */

/* cursor */
static void setup_cursor(struct WindowBase* self)
{
    if (!(globalWl->cursor_theme = wl_cursor_theme_load(NULL, 16, globalWl->shm))) {
        WRN("Failed to load cursor theme\n");
        return;
    }

    globalWl->cursor_arrow = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "left_ptr");
    globalWl->cursor_beam  = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "xterm");
    globalWl->cursor_hand  = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "hand1");

    if (!globalWl->cursor_arrow || !globalWl->cursor_beam) {
        WRN("Failed to load cursor image");
        return;
    }

    globalWl->cursor_surface = wl_compositor_create_surface(globalWl->compositor);
}

/**
 * set cursor type
 * @param what - NULL hides pointer
 */
static void cursor_set(struct wl_cursor* what, uint32_t serial)
{
    globalWl->serial = serial;
    struct wl_buffer*       b;
    struct wl_cursor_image* img = (what ? what : globalWl->cursor_arrow)->images[0];
    if (what) {
        b = wl_cursor_image_get_buffer(img);
    }
    wl_pointer_set_cursor(globalWl->pointer,
                          serial,
                          globalWl->cursor_surface,
                          img->hotspot_x,
                          img->hotspot_y);
    wl_surface_attach(globalWl->cursor_surface, what ? b : NULL, 0, 0);
    wl_surface_damage(globalWl->cursor_surface, 0, 0, img->width, img->height);
    wl_surface_commit(globalWl->cursor_surface);
}

/* Window */
struct WindowBase* WindowWl_new(uint32_t w, uint32_t h)
{
    global = calloc(1, sizeof(WindowStatic) + sizeof(GlobalWl) - sizeof(uint8_t));
    global->target_frame_time_ms = 16;

    /* passing NULL grabs WAYLAND_DISPLAY from env */
    globalWl->display = wl_display_connect(NULL);

    if (!globalWl->display) {
        free(global);
        LOG("No wayland displays found\n");
        return NULL;
    }

    globalWl->xkb.ctx = xkb_context_new(0);

    struct WindowBase* win =
      calloc(1, sizeof(struct WindowBase) + sizeof(WindowWl) + sizeof(uint8_t));

    win->w                 = w;
    win->h                 = h;
    windowWl(win)->outputs = Vector_new_WlOutputInfo();
    FLAG_SET(win->state_flags, WINDOW_IS_IN_FOCUS);
    FLAG_SET(win->state_flags, WINDOW_IS_MINIMIZED);

    win->interface = &window_interface_wayland;

    windowWl(win)->data_offer_mime_idx = -1;

    globalWl->registry = wl_display_get_registry(globalWl->display);
    wl_registry_add_listener(globalWl->registry, &registry_listener, win);
    wl_display_roundtrip(globalWl->display);

    if (globalWl->data_device_manager) {
        globalWl->data_device =
          wl_data_device_manager_get_data_device(globalWl->data_device_manager, globalWl->seat);

        wl_data_device_add_listener(globalWl->data_device, &data_device_listener, win);
    }

    setup_cursor(win);

    globalWl->egl_display = eglGetDisplay(globalWl->display);
    ASSERT(globalWl->egl_display, "failed to get EGL display");

    EGLint cfg_attribs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                             EGL_RED_SIZE,     8,
                             EGL_GREEN_SIZE,   8,
                             EGL_BLUE_SIZE,    8,
                             EGL_ALPHA_SIZE,   8,
                             EGL_NONE };

    EGLAttrib srf_attribs[] = { EGL_NONE };

    EGLConfig config;
    EGLint    num_config;

    EGLint major, minor;
    if (eglInitialize(globalWl->egl_display, &major, &minor) != EGL_TRUE) {
        ERR("EGL init error %s", egl_get_error_string(eglGetError()));
    }

    LOG("EGL Initialized %d.%d\n", major, minor);

    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
        ERR("EGL API binding error %s", egl_get_error_string(eglGetError()));
    }

    eglChooseConfig(globalWl->egl_display, cfg_attribs, &config, 1, &num_config);

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE,
    };

    windowWl(win)->egl_context =
      eglCreateContext(globalWl->egl_display, config, EGL_NO_CONTEXT, context_attribs);

    if (!windowWl(win)->egl_context) {
        ERR("failed to create EGL context %s", egl_get_error_string(eglGetError()));
    }

    windowWl(win)->surface = wl_compositor_create_surface(globalWl->compositor);

    windowWl(win)->egl_window = wl_egl_window_create(windowWl(win)->surface, win->w, win->h);

    windowWl(win)->egl_surface = eglCreatePlatformWindowSurface(globalWl->egl_display,
                                                                config,
                                                                windowWl(win)->egl_window,
                                                                srf_attribs);

    eglSurfaceAttrib(globalWl->egl_display,
                     windowWl(win)->egl_surface,
                     EGL_SWAP_BEHAVIOR,
                     EGL_BUFFER_DESTROYED);

    if (globalWl->xdg_shell) {
        windowWl(win)->xdg_surface =
          xdg_wm_base_get_xdg_surface(globalWl->xdg_shell, windowWl(win)->surface);

        xdg_surface_add_listener(windowWl(win)->xdg_surface, &xdg_surface_listener, win);

        windowWl(win)->xdg_toplevel = xdg_surface_get_toplevel(windowWl(win)->xdg_surface);

        xdg_toplevel_add_listener(windowWl(win)->xdg_toplevel, &xdg_toplevel_listener, win);

        if (globalWl->decoration_manager) {
            windowWl(win)->toplevel_decoration =
              zxdg_decoration_manager_v1_get_toplevel_decoration(globalWl->decoration_manager,
                                                                 windowWl(win)->xdg_toplevel);

            zxdg_toplevel_decoration_v1_add_listener(windowWl(win)->toplevel_decoration,
                                                     &zxdg_toplevel_decoration_listener,
                                                     win);

            zxdg_toplevel_decoration_v1_set_mode(windowWl(win)->toplevel_decoration,
                                                 ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        } else
            WRN("Wayland compositor does not provide window decorations\n");

        wl_surface_commit(windowWl(win)->surface);
        wl_surface_add_listener(windowWl(win)->surface, &wl_surface_listener, win);

        wl_display_roundtrip(globalWl->display);
    } else {
        WRN("xdg_shell_v1 not supported by compositor, falling back to wl_shell\n");

        windowWl(win)->shell_surface =
          wl_shell_get_shell_surface(globalWl->wl_shell, windowWl(win)->surface);

        wl_shell_surface_add_listener(windowWl(win)->shell_surface, &shell_surface_listener, win);

        wl_shell_surface_set_toplevel(windowWl(win)->shell_surface);
    }

    eglMakeCurrent(globalWl->egl_display,
                   windowWl(win)->egl_surface,
                   windowWl(win)->egl_surface,
                   windowWl(win)->egl_context);

    Window_notify_content_change(win);

    const char* exts = NULL;
    exts             = eglQueryString(globalWl->egl_display, EGL_EXTENSIONS);

    if (strstr(exts, "EGL_KHR_swap_buffers_with_damage")) {
        eglSwapBuffersWithDamageKHR =
          (PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC)eglGetProcAddress("eglSwapBuffersWithDamageKHR");
    } else {
        WRN("EGL_KHR_swap_buffers_with_damage is not supported\n");
    }

    EGLint eglerror = eglGetError();
    if (eglerror != EGL_SUCCESS) {
        WRN("EGL Error %s\n", egl_get_error_string(eglerror));
    }

    struct wl_callback* frame_callback = wl_surface_frame(windowWl(win)->surface);
    wl_callback_add_listener(frame_callback, &frame_listener, win);

    return win;
}

struct WindowBase* Window_new_wayland(Pair_uint32_t res)
{

    struct WindowBase* win = WindowWl_new(res.first, res.second);

    if (!win)
        return NULL;

    win->title = NULL;
    WindowWl_set_title(win, settings.title.str);

    WindowWl_set_wm_name(win, OR(settings.user_app_id, APPLICATION_NAME));

    WindowWl_swap_buffers(win);
    WindowWl_events(win);

    return win;
}

static void WindowWl_set_no_context()
{
    eglMakeCurrent(globalWl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void WindowWl_set_current_context(struct WindowBase* self, bool this)
{
    if (self && this) {
        eglMakeCurrent(globalWl->egl_display,
                       windowWl(self)->egl_surface,
                       windowWl(self)->egl_surface,
                       windowWl(self)->egl_context);
    } else {
        WindowWl_set_no_context();
    }
}

void WindowWl_set_fullscreen(struct WindowBase* self, bool fullscreen)
{
    FLAG_UNSET(self->state_flags, WINDOW_IS_MAXIMIZED);

    if (fullscreen) {
        self->previous_h = self->h;
        self->previous_w = self->w;

        if (globalWl->xdg_shell) {
            xdg_toplevel_set_fullscreen(windowWl(self)->xdg_toplevel,
                                        windowWl(self)->active_output->output);
        } else {
            wl_shell_surface_set_fullscreen(windowWl(self)->shell_surface,
                                            WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER,
                                            0, /* fps in mHz 0 - don't care */
                                            globalWl->output);
        }
        FLAG_SET(self->state_flags, WINDOW_IS_FULLSCREEN);
    } else {
        if (globalWl->xdg_shell) {
            xdg_toplevel_unset_fullscreen(windowWl(self)->xdg_toplevel);
        } else {
            wl_shell_surface_set_toplevel(windowWl(self)->shell_surface);
        }

        if (self->previous_h && self->previous_w) {
            WindowWl_resize(self, self->previous_w, self->previous_h);
        }

        FLAG_UNSET(self->state_flags, WINDOW_IS_FULLSCREEN);
    }
}

void WindowWl_resize(struct WindowBase* self, uint32_t w, uint32_t h)
{
    wl_egl_window_resize(windowWl(self)->egl_window, w, h, 0, 0);
    self->previous_w = 0;
    self->previous_h = 0;
    self->w          = w;
    self->h          = h;
    Window_notify_content_change(self);
}

TimePoint* WindowWl_process_timers(struct WindowBase* self)
{
    if (globalWl->keycode_to_repeat && TimePoint_passed(globalWl->repeat_point)) {
        int32_t time_offset    = (1000 / globalWl->kbd_repeat_rate);
        globalWl->repeat_point = TimePoint_ms_from_now(time_offset);

        keyboard_handle_key(self,
                            NULL,
                            0,
                            0,
                            globalWl->keycode_to_repeat,
                            WL_KEYBOARD_KEY_STATE_PRESSED);

        return &globalWl->repeat_point;
    } else if (globalWl->keycode_to_repeat) {
        return &globalWl->repeat_point;
    } else {
        return NULL;
    }
}

void WindowWl_events(struct WindowBase* self)
{
    /* Wayland docs:
     *
     * A real world example of event queue usage is Mesa's implementation of
     * eglSwapBuffers() for the Wayland platform. This function might need to
     * block until a frame callback is received, but dispatching the default
     * queue could cause an event handler on the client to start drawing again.
     * This problem is solved using another event queue, so that only the events
     * handled by the EGL code are dispatched during the block. This creates
     * a problem where a thread dispatches a non-default queue, reading all the
     * data from the display fd. If the application would call poll(2) after
     * that it would block, even though there might be events queued on the
     * default queue. Those events should be dispatched with
     * wl_display_dispatch_pending() or wl_display_dispatch_queue_pending()
     * before flushing and blocking.
     */

    wl_display_prepare_read(globalWl->display);
    wl_display_read_events(globalWl->display);
    if (wl_display_dispatch_pending(globalWl->display) < 0) {
        ERR("wl_display_dispatch_pending() failed");
    }

    wl_display_flush(globalWl->display);
}

static void WindowWl_dont_swap_buffers(struct WindowBase* self)
{
    if (wl_display_dispatch_pending(globalWl->display) < 0) {
        ERR("wl_display_dispatch_pending() failed");
    }
    wl_display_flush(globalWl->display);
}

static void WindowWl_swap_buffers(struct WindowBase* self)
{
    self->paint                     = false;
    windowWl(self)->draw_next_frame = false;
    if (self->callbacks.on_redraw_requested) {
        self->callbacks.on_redraw_requested(self->callbacks.user_data);
    }
    if (eglSwapBuffers(globalWl->egl_display, windowWl(self)->egl_surface) != EGL_TRUE) {
        ERR("buffer swap failed EGL Error %s\n", egl_get_error_string(eglGetError()));
    }
    struct wl_callback* frame_callback = wl_surface_frame(windowWl(self)->surface);
    wl_callback_add_listener(frame_callback, &frame_listener, self);
}

static bool WindowWl_maybe_swap(struct WindowBase* self)
{
    if (windowWl(self)->draw_next_frame && self->paint) {
        WindowWl_swap_buffers(self);
        return true;
    } else {
        WindowWl_dont_swap_buffers(self);
        return false;
    }
}

static void WindowWl_set_swap_interval(struct WindowBase* self, int32_t ival)
{
    ival = EGL_MIN_SWAP_INTERVAL + ival;

    if (ival > EGL_MAX_SWAP_INTERVAL || ival < EGL_MIN_SWAP_INTERVAL)
        WRN("Buffer swap interval clamped [%d, %d]\n",
            EGL_MIN_SWAP_INTERVAL,
            EGL_MAX_SWAP_INTERVAL);

    eglSwapInterval(globalWl->egl_display, ival);
}

static void WindowWl_set_wm_name(struct WindowBase* self, const char* title)
{
    if (globalWl->xdg_shell)
        xdg_toplevel_set_app_id(windowWl(self)->xdg_toplevel, title);
    else
        wl_shell_surface_set_class(windowWl(self)->shell_surface, title);
}

static void WindowWl_set_maximized(struct WindowBase* self, bool maximized)
{
    if (maximized) {
        if (Window_is_fullscreen(self)) {
            WindowWl_set_fullscreen(self, false);
        } else {
            self->previous_w = self->w;
            self->previous_h = self->h;
        }

        if (globalWl->xdg_shell) {
            xdg_toplevel_set_maximized(windowWl(self)->xdg_toplevel);
        } else {
            wl_shell_surface_set_maximized(windowWl(self)->shell_surface, globalWl->output);
        }
        FLAG_SET(self->state_flags, WINDOW_IS_MAXIMIZED);
    } else {
        if (globalWl->xdg_shell) {
            xdg_toplevel_unset_maximized(windowWl(self)->xdg_toplevel);
        } else {
            wl_shell_surface_set_toplevel(windowWl(self)->shell_surface);
        }

        if (self->previous_h && self->previous_w) {
            WindowWl_resize(self, self->previous_w, self->previous_h);
        }

        FLAG_UNSET(self->state_flags, WINDOW_IS_MAXIMIZED);
    }
}

static void WindowWl_set_title(struct WindowBase* self, const char* title)
{
    if (globalWl->xdg_shell) {
        xdg_toplevel_set_title(windowWl(self)->xdg_toplevel, title);
    } else {
        wl_shell_surface_set_title(windowWl(self)->shell_surface, title);
    }
}

static void WindowWl_destroy(struct WindowBase* self)
{
    WindowWl_set_no_context();

    if (globalWl->cursor_theme) {
        wl_surface_destroy(globalWl->cursor_surface);
        wl_cursor_theme_destroy(globalWl->cursor_theme);
    }

    wl_egl_window_destroy(windowWl(self)->egl_window);
    eglDestroySurface(globalWl->egl_display, windowWl(self)->egl_surface);
    eglDestroyContext(globalWl->egl_display, windowWl(self)->egl_context);

    if (globalWl->decoration_manager) {
        zxdg_toplevel_decoration_v1_destroy(windowWl(self)->toplevel_decoration);
    }

    if (globalWl->xdg_shell) {
        xdg_toplevel_destroy(windowWl(self)->xdg_toplevel);
        xdg_surface_destroy(windowWl(self)->xdg_surface);
    } else {
        wl_shell_surface_destroy(windowWl(self)->shell_surface);
    }

    wl_surface_destroy(windowWl(self)->surface);

    if (globalWl->data_device_manager) {
        wl_data_device_manager_destroy(globalWl->data_device_manager);
    }

    if (globalWl->data_device) {
        wl_data_device_destroy(globalWl->data_device);
    }

    eglTerminate(globalWl->egl_display);
    eglReleaseThread();

    wl_registry_destroy(globalWl->registry);
    wl_display_disconnect(globalWl->display);

    free((void*)windowWl(self)->data_source_text);

    Vector_destroy_WlOutputInfo(&windowWl(self)->outputs);

    free(self);
}

static void WindowWl_set_urgent(struct WindowBase* self)
{
    /* currently there is no protocol extension for this */
}

static void WindowWl_set_stack_order(struct WindowBase* self, bool front_or_back)
{
    /* currently there is no protocol extension for this(?) */
}

static int WindowWl_get_connection_fd(struct WindowBase* self)
{
    return wl_display_get_fd(globalWl->display);
}

static void* WindowWl_get_gl_ext_proc_adress(struct WindowBase* self, const char* name)
{
    return eglGetProcAddress(name);
}

static uint32_t WindowWl_get_keycode_from_name(struct WindowBase* self, char* name)
{
    xkb_keysym_t xkb_keysym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    return xkb_keysym == XKB_KEY_NoSymbol ? 0 : xkb_keysym_to_utf32(xkb_keysym);
}

static void WindowWl_set_pointer_style(struct WindowBase* self, enum MousePointerStyle style)
{
    switch (style) {
        case MOUSE_POINTER_HIDDEN:
            FLAG_SET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            cursor_set(0, 0);
            break;

        case MOUSE_POINTER_ARROW:
            FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            cursor_set(globalWl->cursor_arrow, 0);
            break;

        case MOUSE_POINTER_I_BEAM:
            FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            cursor_set(globalWl->cursor_beam, 0);
            break;

        case MOUSE_POINTER_HAND:
            FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
            cursor_set(globalWl->cursor_hand, 0);
            break;
    }
}

#endif
