/* See LICENSE for license information. */

#ifndef NOWL

#define _GNU_SOURCE

#include "wl.h"
#include "colors.h"
#include "map.h"
#include "timing.h"
#include "ui.h"
#include "util.h"
#include "vector.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>

#include "window.h"
#include "wl_exts/kwin-blur.h"
#include "wl_exts/wp-primary-selection.h"
#include "wl_exts/xdg-decoration.h"
#include "wl_exts/xdg-shell.h"

#include "eglerrors.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define WL_DEFAULT_CURSOR_SIZE        16
#define WL_FALLBACK_TGT_FRAME_TIME_MS 16

static WindowStatic* global;

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
struct WindowBase* WindowWl_new(uint32_t w, uint32_t h, gfx_api_t gfx_api_t, Ui* ui);
static void        WindowWl_set_maximized(struct WindowBase* self, bool maximized);
static void        WindowWl_set_minimized(struct WindowBase* self);
static void        WindowWl_set_fullscreen(struct WindowBase* self, bool fullscreen);
static void        WindowWl_resize(struct WindowBase* self, uint32_t w, uint32_t h);
static void        WindowWl_events(struct WindowBase* self);
static TimePoint*  WindowWl_process_timers(struct WindowBase* self);
static void        WindowWl_set_swap_interval(struct WindowBase* self, int32_t ival);
static void        WindowWl_set_wm_name(struct WindowBase* self, const char* title);
static void        WindowWl_set_title(struct WindowBase* self, const char* title);
static bool        WindowWl_maybe_swap(WindowBase* self, bool do_swap);
static void        WindowWl_destroy(struct WindowBase* self);
static int         WindowWl_get_connection_fd(struct WindowBase* self);
static void        WindowWl_clipboard_send(struct WindowBase* self, const char* text);
static void        WindowWl_clipboard_get(struct WindowBase* self);
static void        WindowWl_primary_send(struct WindowBase* self, const char* text);
static void        WindowWl_primary_get(struct WindowBase* self);
static void*       WindowWl_get_gl_ext_proc_adress(struct WindowBase* self, const char* name);
static uint32_t    WindowWl_get_keycode_from_name(struct WindowBase* self, char* name);
static void    WindowWl_set_pointer_style(struct WindowBase* self, enum MousePointerStyle style);
static void    WindowWl_set_current_context(struct WindowBase* self, bool this);
static void    WindowWl_set_urgent(struct WindowBase* self);
static void    WindowWl_set_stack_order(struct WindowBase* self, bool front_or_back);
static void    WindowWl_set_incremental_resize(struct WindowBase* self, uint32_t x, uint32_t y);
static int64_t WindowWl_get_window_id(struct WindowBase* self)
{
    return -1;
}

static WindowStatic* WindowWl_get_static_ptr(struct WindowBase* self)
{
    return global;
}
static void WindowWl_notify_initialization_complete(WindowBase*            win,
                                                    WindowSystemLaunchEnv* launch_env)
{
}

static struct IWindow window_interface_wayland = {
    .set_fullscreen                 = WindowWl_set_fullscreen,
    .set_maximized                  = WindowWl_set_maximized,
    .set_minimized                  = WindowWl_set_minimized,
    .resize                         = WindowWl_resize,
    .events                         = WindowWl_events,
    .process_timers                 = WindowWl_process_timers,
    .set_title                      = WindowWl_set_title,
    .maybe_swap                     = WindowWl_maybe_swap,
    .destroy                        = WindowWl_destroy,
    .get_connection_fd              = WindowWl_get_connection_fd,
    .clipboard_send                 = WindowWl_clipboard_send,
    .clipboard_get                  = WindowWl_clipboard_get,
    .primary_send                   = WindowWl_primary_send,
    .primary_get                    = WindowWl_primary_get,
    .set_swap_interval              = WindowWl_set_swap_interval,
    .get_gl_ext_proc_adress         = WindowWl_get_gl_ext_proc_adress,
    .get_keycode_from_name          = WindowWl_get_keycode_from_name,
    .set_pointer_style              = WindowWl_set_pointer_style,
    .set_current_context            = WindowWl_set_current_context,
    .set_urgent                     = WindowWl_set_urgent,
    .set_stack_order                = WindowWl_set_stack_order,
    .get_window_id                  = WindowWl_get_window_id,
    .set_incremental_resize         = WindowWl_set_incremental_resize,
    .get_static_ptr                 = WindowWl_get_static_ptr,
    .notify_initialization_complete = WindowWl_notify_initialization_complete,
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

    struct wl_compositor*    compositor;
    struct wl_subcompositor* subcompositor;
    struct wl_output*        output;
    struct wl_shm*           shm;

    struct wl_data_device_manager* data_device_manager;
    struct wl_data_device*         data_device;

    struct zwp_primary_selection_device_manager_v1* primary_manager;
    struct zwp_primary_selection_device_v1*         primary_device;

    struct wl_shell*                   wl_shell;
    struct xdg_wm_base*                xdg_shell;
    struct zxdg_decoration_manager_v1* decoration_manager;

    struct wl_seat*     seat;
    struct wl_pointer*  pointer;
    struct wl_keyboard* keyboard;
    struct wl_surface*  moused_over_surface;

    struct wl_cursor *cursor_arrow, *cursor_beam, *cursor_hand, *cursor_bottom_left_corner,
      *cursor_bottom_right_corner, *cursor_top_left_corner, *cursor_top_right_corner,
      *cursor_top_side, *cursor_bottom_side, *cursor_left_side, *cursor_right_side, *cursor_move;
    struct wl_cursor_theme* cursor_theme;
    struct wl_surface*      cursor_surface;

    struct org_kde_kwin_blur_manager* kde_kwin_blur_manager;

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

    /* is the window within this output */
    bool is_active;

    /* lcd geometry the compositor told us */
    lcd_filter_e lcd_filter;

    /* based on display refresh rate */
    double target_frame_time_ms;

    /* dots per inch calculated from physical dimensions and resolution */
    uint16_t dpi;

    /* for calculating dpi. we need to store this because any of those values may be updated (and
     * they come from different events) */
    int32_t width_px;
    double  width_inch;

    /* output index, we can use this to distinguish displays with the same name */
    uint8_t global_index;

    /* output name */
    char* name;

} WlOutputInfo;

static void WlOutputInfo_destroy(WlOutputInfo* self)
{
    free(self->name);
    self->name = NULL;
}

DEF_VECTOR(WlOutputInfo, WlOutputInfo_destroy);

typedef size_t wl_output_ptr;

static size_t wl_output_ptr_hash(const wl_output_ptr* k)
{
    return ((size_t)*k) / 16;
}
static bool wl_output_ptr_eq(const wl_output_ptr* k, const wl_output_ptr* o)
{
    return *k == *o;
}

DEF_MAP(wl_output_ptr, WlOutputInfo, wl_output_ptr_hash, wl_output_ptr_eq, WlOutputInfo_destroy);

typedef enum
{
    /* Not using CSDs at all */
    CSD_MODE_DISABLED = 0,

    /* Full decorations for a normal floating window */
    CSD_MODE_FLOATING,

    /* The window is in a tiled/maximized state. Shadow is not shown and the titlebar has sharp
       corners */
    CSD_MODE_TILED,

    /* Enabled but completely hidden (eg. window fullscreen) */
    CSD_MODE_HIDDEN,
} csd_mode_e;

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

    struct wl_callback* active_frame_callback;

    struct org_kde_kwin_blur* kde_kwin_blur;

    struct wl_data_offer*  data_offer;
    struct wl_data_source* data_source;
    struct wl_data_offer*  dnd_data_offer;
    int8_t                 data_offer_mime_idx; // -1 for none
    const char*            data_source_text;

    struct zwp_primary_selection_offer_v1 * primary_offer, *new_primary_offer;
    struct zwp_primary_selection_source_v1* primary_source;
    int8_t      primary_offer_mime_idx, new_primary_offer_mime_idx; // -1 for none
    const char* primary_source_text;

    bool got_discrete_axis_event;

    Map_wl_output_ptr_WlOutputInfo outputs;

    WlOutputInfo* active_output;
    bool          draw_next_frame;

    struct window_wl_csd_t
    {
        csd_mode_e            mode;
        struct wl_surface*    shadow_surf;
        struct wl_subsurface* shadow_subsurf;
        bool                  dragging_button;
        uint32_t              dragging_button_serial;

        /* Temporarily disable reporting focuss loss events.
         *
         * When the surface is beeing resized it loses focus. The application logic should not care
         * about this and continue as if the window was focused */
        bool window_move_inhibits_focus_loss;
    } csd;
} WindowWl;

/* in order of preference */
static const char* ACCEPTED_MIMES[] = {
    "text/uri-list", "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT",
};

static const char* OFFERED_MIMES[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT",
};

static inline xkb_keysym_t keysym_filter_compose(xkb_keysym_t sym)
{
    if (!globalWl->xkb.compose_state || sym == XKB_KEY_NoSymbol) {
        return sym;
    }

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

static void WindowWl_swap_buffers(WindowBase* self);

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
            } else if (errno == EWOULDBLOCK || errno == 0) {
                break;
            } else {
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

    TRY_CALL(self->callbacks.clipboard_handler, self->callbacks.user_data, text.buf);
    Vector_destroy_char(&text);
}

static void primary_selection_source_listener_handle_send(
  void*                                   data,
  struct zwp_primary_selection_source_v1* source,
  const char*                             mime_type,
  int32_t                                 fd)
{
    LOG("wl::primary_source::send{ mime: %s }\n", mime_type);

    WindowWl* w = windowWl(((struct WindowBase*)data));

    bool is_supported_mime = false;
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(OFFERED_MIMES); ++i) {
        if (!strcmp(mime_type, OFFERED_MIMES[i])) {
            is_supported_mime = true;
            break;
        }
    }

    if (w->primary_source_text && is_supported_mime) {
        LOG("writing \'%s\' to fd\n", w->primary_source_text);
        size_t len = strlen(w->primary_source_text);
        if (len <= SSIZE_MAX) {
            ssize_t bytes = write(fd, w->primary_source_text, len);
            if (bytes != (ssize_t)len) {
                WRN("could not write to pipe %s\n", strerror(errno))
            }
        } else {
            WRN("could not write to pipe buffer too large");
        }
    }

    close(fd);
}

static void primary_selection_source_listener_handle_canceled(
  void*                                   data,
  struct zwp_primary_selection_source_v1* source)
{
    WindowWl* w = windowWl(((struct WindowBase*)data));
    LOG("wl::primary_source::cancelled\n");

    zwp_primary_selection_source_v1_destroy(source);
    w->primary_source = NULL;
}

struct zwp_primary_selection_source_v1_listener primary_selection_source_listener = {
    .send      = primary_selection_source_listener_handle_send,
    .cancelled = primary_selection_source_listener_handle_canceled,
};

void primary_selection_offer_handle_offer(void*                                  data,
                                          struct zwp_primary_selection_offer_v1* primary_offer,
                                          const char*                            mime_type)
{
    WindowWl* w = windowWl(((struct WindowBase*)data));

    LOG("wl::primary_selection_offer::offer{ mime_type: %s }", mime_type);

    for (uint_fast8_t i = 0; i < ARRAY_SIZE(ACCEPTED_MIMES); ++i) {
        if (strcmp(mime_type, ACCEPTED_MIMES[i])) {
            continue;
        }

        bool prefferable_mime =
          (w->new_primary_offer_mime_idx == -1 || w->new_primary_offer_mime_idx >= i);

        if (primary_offer != w->new_primary_offer) {
            LOG("- ACCEPTED(new data) }\n");
            w->new_primary_offer          = primary_offer;
            w->new_primary_offer_mime_idx = i;
            return;
        } else if (prefferable_mime) {
            LOG("- ACCEPTED(preffered mime type) }\n");
            w->new_primary_offer          = primary_offer;
            w->new_primary_offer_mime_idx = i;
            return;
        }
    }

    if (w->new_primary_offer_mime_idx == -1) {
        LOG(" - REJECTED(not supported) }\n");
    } else {
        LOG(" - REJECTED(\'%s\' is preffered) }\n", ACCEPTED_MIMES[w->new_primary_offer_mime_idx]);
    }
}

static void wl_buffer_release(void* data, struct wl_buffer* buffer)
{
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = wl_buffer_release,
};

void randname(char* buf)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;

    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

int create_shm_file()
{
    int retries = 32;

    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);

        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);

    return -1;
}

int allocate_shm_file(size_t size)
{
    int fd = create_shm_file();
    if (fd < 0) {
        return -1;
    }

    int ret = 0;

    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

typedef ColorRGBA (*SoftwareShaderFn)(size_t x, size_t y, void* opts);

ColorRGBA SoftwareShaderFn_fill(size_t x, size_t y, void* ColorRGBA_ptr_fill_color)
{
    return *((ColorRGBA*)ColorRGBA_ptr_fill_color);
}

typedef struct
{
    Pair_uint32_t window_surface_size;
    uint16_t      window_surface_radius;
    uint16_t      shadow_margin;
    uint16_t      shadow_offset;
} SoftwareShaderFn_window_shadow_args_t;

ColorRGBA SoftwareShaderFn_window_shadow(size_t x, size_t y, void* args)
{
    SoftwareShaderFn_window_shadow_args_t* a = (SoftwareShaderFn_window_shadow_args_t*)args;

    uint16_t shadow_radius = a->shadow_margin + a->window_surface_radius;

    Pair_uint32_t shadow_srf_dims =
      (Pair_uint32_t){ .first  = a->shadow_margin * 2 + a->window_surface_size.first,
                       .second = a->shadow_margin * 2 + a->window_surface_size.second };

    double       alpha         = 0.0;
    Pair_int32_t this_fragment = { x, y };

#define L_DISTANCE(p1, p2) sqrt(pow(p1.first - p2.first, 2.0) + pow(p1.second - p2.second, 2.0))

    bool h_front = x < shadow_radius;
    bool v_front = y < (size_t)(shadow_radius - a->shadow_offset);
    bool h_end =
      x >= a->shadow_margin + a->window_surface_size.first - (shadow_radius - a->shadow_margin);
    bool v_end = y >= a->shadow_margin - a->shadow_offset + a->window_surface_size.second -
                        (shadow_radius - a->shadow_margin);
    bool h_middle = !h_front && !h_end;
    bool v_middle = !v_front && !v_end;

    bool left   = x < a->shadow_margin;
    bool right  = x >= (shadow_srf_dims.first - a->shadow_margin);
    bool top    = y < (size_t)(a->shadow_margin - a->shadow_offset);
    bool bottom = y >= (a->shadow_margin - a->shadow_offset) + a->window_surface_size.second;

    bool left_titlebar_corner = !top && !left && x < shadow_radius && y < shadow_radius;
    bool right_titlebar_corner =
      !top && !right && x >= (shadow_srf_dims.first - shadow_radius) && y < shadow_radius;

    if (!left && !right && !top && !bottom) {
        if (left_titlebar_corner) {
            Pair_int32_t center = { shadow_radius, shadow_radius };
            double       dist   = L_DISTANCE(center, this_fragment);
            alpha               = (shadow_radius - MIN(dist, shadow_radius)) / a->shadow_margin;
        } else if (right_titlebar_corner) {
            Pair_int32_t center = { shadow_srf_dims.first - shadow_radius, shadow_radius };
            double       dist   = L_DISTANCE(center, this_fragment);
            alpha               = (shadow_radius - MIN(dist, shadow_radius)) / a->shadow_margin;
        } else {
            alpha = 0.0;
        }
    } else {
        if (h_front && v_middle) /* left edge */ {
            alpha = (double)x / a->shadow_margin;
        } else if (h_end && v_middle) /* right edge */ {
            alpha = ((double)shadow_srf_dims.first - x) / a->shadow_margin;
        } else if (v_front && h_middle) /* top edge */ {
            alpha = (double)y / a->shadow_margin;
        } else if (v_end && h_middle) /* bottom edge */ {
            alpha = ((double)shadow_srf_dims.second - y) / a->shadow_margin;
        } else if (h_front && v_front) /* top left corner */ {
            Pair_int32_t center = { shadow_radius, shadow_radius };
            double       dist   = L_DISTANCE(center, this_fragment);
            alpha               = (shadow_radius - MIN(dist, shadow_radius)) / a->shadow_margin;
        } else if (h_end && v_front) /* top right corner */ {
            Pair_int32_t center = { shadow_srf_dims.first - shadow_radius, shadow_radius };
            double       dist   = L_DISTANCE(center, this_fragment);
            alpha               = (shadow_radius - MIN(dist, shadow_radius)) / a->shadow_margin;
        } else if (h_front && v_end) /* bottom left */ {
            Pair_int32_t center = { shadow_radius, shadow_srf_dims.second - shadow_radius };
            double       dist   = L_DISTANCE(center, this_fragment);
            alpha               = (shadow_radius - MIN(dist, shadow_radius)) / a->shadow_margin;
        } else if (h_end && v_end) /* bottom right */ {
            Pair_int32_t center = { shadow_srf_dims.first - shadow_radius,
                                    shadow_srf_dims.second - shadow_radius };
            double       dist   = L_DISTANCE(center, this_fragment);
            alpha               = (shadow_radius - MIN(dist, shadow_radius)) / a->shadow_margin;
        }
    }

    return (ColorRGBA){ 0, 0, 0, UINT8_MAX * POW2(alpha) * (0.2) };
}

struct wl_buffer* make_wl_buffer(GlobalWl*        wl,
                                 size_t           w,
                                 size_t           h,
                                 SoftwareShaderFn shader,
                                 void*            shader_opts)
{
    size_t stride = w * 4;
    size_t size   = stride * h;

    int fd = allocate_shm_file(size);

    if (fd == -1) {
        return NULL;
    }

    uint32_t* data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool* pool = wl_shm_create_pool(wl->shm, fd, size);
    struct wl_buffer*   buffer =
      wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
    /* the actual format is BGRA */

    wl_shm_pool_destroy(pool);
    close(fd);

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            *(ColorRGBA*)(&data[y * w + x]) = shader(x, y, shader_opts);
        }
    }

    if (!buffer) {
        return NULL;
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);

    return buffer;
}

static void WindowWl_enable_csd(WindowBase* self, csd_mode_e initial_mode)
{
    ASSERT(initial_mode != CSD_MODE_DISABLED, "Initial mode is an enabled state");
    windowWl(self)->csd.mode = initial_mode;
}

static bool WindowWl_csd_created(WindowBase* self)
{
    return windowWl(self)->csd.shadow_surf;
}

static bool WindowWl_csd_enabled(WindowBase* self)
{
    return windowWl(self)->csd.mode != CSD_MODE_DISABLED;
}

const uint8_t CSD_SHADOW_MARGIN   = 40;
const uint8_t CSD_SHADOW_V_OFFSET = 6;
const uint8_t CSD_FRAME_THICKNESS = 1;

static void WindowWl_build_csd(WindowBase* self)
{
    WindowWl* win = windowWl(self);

    if (win->csd.mode == CSD_MODE_DISABLED || WindowWl_csd_created(self)) {
        return;
    }

    win->csd.shadow_surf = wl_compositor_create_surface(globalWl->compositor);
    win->csd.shadow_subsurf =
      wl_subcompositor_get_subsurface(globalWl->subcompositor, win->csd.shadow_surf, win->surface);

    SoftwareShaderFn_window_shadow_args_t shader_args = {
        .window_surface_size   = { .first = self->w, .second = self->h },
        .window_surface_radius = 10,
        .shadow_margin         = CSD_SHADOW_MARGIN,
        .shadow_offset         = CSD_SHADOW_V_OFFSET,
    };

    struct wl_buffer* buf = make_wl_buffer(globalWl,
                                           self->w + CSD_SHADOW_MARGIN * 2,
                                           self->h + CSD_SHADOW_MARGIN * 2,
                                           SoftwareShaderFn_window_shadow,
                                           &shader_args);

    /* Always attach at origin. */
    wl_surface_attach(win->csd.shadow_surf, buf, 0, 0);
    wl_subsurface_set_position(win->csd.shadow_subsurf,
                               -CSD_SHADOW_MARGIN,
                               -CSD_SHADOW_MARGIN + CSD_SHADOW_V_OFFSET);
    wl_subsurface_place_below(win->csd.shadow_subsurf, win->surface);

    struct wl_region* region      = wl_compositor_create_region(globalWl->compositor);
    int32_t           side_offset = CSD_SHADOW_MARGIN - UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX;
    int32_t           top_offset =
      CSD_SHADOW_MARGIN - UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX - CSD_SHADOW_V_OFFSET;
    wl_region_add(region,
                  side_offset,
                  top_offset,
                  UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX * 2 + self->w,
                  UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX * 2 + self->h);
    wl_surface_set_input_region(win->csd.shadow_surf, region);
    wl_region_destroy(region);
    wl_subsurface_set_desync(win->csd.shadow_subsurf);
    xdg_surface_set_window_geometry(win->xdg_surface, 0, 0, self->w, self->h);
    wl_surface_commit(win->csd.shadow_surf);
}

static void WindowWl_resize_csd(WindowBase* self)
{
    WindowWl* win = windowWl(self);

    if (win->csd.mode == CSD_MODE_DISABLED || win->csd.mode == CSD_MODE_HIDDEN) {
        return;
    }

    if (win->csd.mode == CSD_MODE_FLOATING) /* Has window shadow */ {
        SoftwareShaderFn_window_shadow_args_t shader_args = {
            .window_surface_size   = { .first = self->w, .second = self->h },
            .window_surface_radius = 10,
            .shadow_margin         = CSD_SHADOW_MARGIN,
            .shadow_offset         = CSD_SHADOW_V_OFFSET,
        };

        struct wl_buffer* buf = make_wl_buffer(globalWl,
                                               self->w + CSD_SHADOW_MARGIN * 2,
                                               self->h + CSD_SHADOW_MARGIN * 2,
                                               SoftwareShaderFn_window_shadow,
                                               &shader_args);
        wl_surface_attach(win->csd.shadow_surf, buf, 0, 0);
        struct wl_region* region      = wl_compositor_create_region(globalWl->compositor);
        int32_t           side_offset = CSD_SHADOW_MARGIN - UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX;
        int32_t           top_offset =
          CSD_SHADOW_MARGIN - UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX - CSD_SHADOW_V_OFFSET;
        wl_region_add(region,
                      side_offset,
                      top_offset,
                      UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX * 2 + self->w,
                      UI_CSD_MOUSE_RESIZE_GRIP_THICKNESS_PX * 2 + self->h);
        wl_surface_set_input_region(win->csd.shadow_surf, region);
        wl_region_destroy(region);
    }

    xdg_surface_set_window_geometry(win->xdg_surface, 0, 0, self->w, self->h);
    wl_surface_commit(win->csd.shadow_surf);
}

static void WindowWl_destroy_csd(WindowBase* self)
{
    WindowWl* win = windowWl(self);

    if (win->csd.mode == CSD_MODE_DISABLED) {
        return;
    }

    if (win->csd.shadow_subsurf) {
        wl_subsurface_destroy(win->csd.shadow_subsurf);
        win->csd.shadow_subsurf = NULL;
    }

    if (win->csd.shadow_surf) {
        wl_surface_destroy(win->csd.shadow_surf);
        win->csd.shadow_surf = NULL;
    }
}

static void WindowWl_hide_csd(WindowBase* self)
{
    WindowWl* win = windowWl(self);

    if (win->csd.mode == CSD_MODE_HIDDEN || win->csd.mode == CSD_MODE_DISABLED) {
        return;
    }

    win->csd.mode = CSD_MODE_HIDDEN;
    wl_surface_attach(win->csd.shadow_surf, NULL, 0, 0);
    wl_surface_commit(win->csd.shadow_surf); // We don't resize when hidden commit surface here
}

static void WindowWl_show_tiled_csd(WindowBase* self)
{
    WindowWl* win = windowWl(self);

    if (win->csd.mode == CSD_MODE_TILED || win->csd.mode == CSD_MODE_DISABLED) {
        return;
    }

    win->csd.mode = CSD_MODE_TILED;
    wl_surface_attach(win->csd.shadow_surf, NULL, 0, 0);
}
static void WindowWl_show_floating_csd(WindowBase* self)
{
    WindowWl* win = windowWl(self);

    if (win->csd.mode == CSD_MODE_FLOATING || win->csd.mode == CSD_MODE_DISABLED) {
        return;
    }

    if (!WindowWl_csd_created(self)) {
        WindowWl_build_csd(self);
    }

    win->csd.mode = CSD_MODE_FLOATING;
    WindowWl_resize_csd(self);
}

struct zwp_primary_selection_offer_v1_listener primary_selection_offer_listener = {
    .offer = primary_selection_offer_handle_offer,
};

/* Data offer changed add a listener to this to get mime types */
static void primary_selection_device_handle_data_offer(
  void*                                   data,
  struct zwp_primary_selection_device_v1* device,
  struct zwp_primary_selection_offer_v1*  offer)
{
    struct WindowBase* self = data;

    windowWl(self)->new_primary_offer = offer;

    zwp_primary_selection_offer_v1_add_listener(offer, &primary_selection_offer_listener, data);
}

/* Got a new offer and mimetypes have been sent, replace the old offer with this */
static void primary_selection_device_handle_selection(
  void*                                   data,
  struct zwp_primary_selection_device_v1* device,
  struct zwp_primary_selection_offer_v1*  offer)
{
    struct WindowBase* self = data;

    if (offer) {
        TRY_CALL(self->callbacks.on_primary_changed, self->callbacks.user_data);
    }

    LOG("wl::primary_selection_offer::selection{ mime_type: %s }\n",
        windowWl(self)->new_primary_offer_mime_idx == -1
          ? "<none>"
          : ACCEPTED_MIMES[windowWl(self)->new_primary_offer_mime_idx]);

    if (windowWl(self)->primary_offer &&
        windowWl(self)->primary_offer != windowWl(self)->new_primary_offer) {
        zwp_primary_selection_offer_v1_destroy(windowWl(self)->primary_offer);
    }

    windowWl(self)->primary_offer              = windowWl(self)->new_primary_offer;
    windowWl(self)->primary_offer_mime_idx     = windowWl(self)->new_primary_offer_mime_idx;
    windowWl(self)->new_primary_offer          = NULL;
    windowWl(self)->new_primary_offer_mime_idx = -1;
}

struct zwp_primary_selection_device_v1_listener primary_selection_device_listener = {
    .data_offer = primary_selection_device_handle_data_offer,
    .selection  = primary_selection_device_handle_selection,
};

static void WindowWl_primary_send(struct WindowBase* self, const char* text)
{
    if (!text) {
        /* We no longer have a selection */
        if (globalWl->primary_manager) {
            zwp_primary_selection_device_v1_set_selection(globalWl->primary_device,
                                                          NULL,
                                                          globalWl->serial);
        }
        return;
    }

    if (!globalWl->primary_manager) {
        free((void*)text);
        return;
    }

    free((void*)windowWl(self)->primary_source_text);
    windowWl(self)->primary_source_text = text;

    if (windowWl(self)->primary_source) {
        zwp_primary_selection_source_v1_destroy(windowWl(self)->primary_source);
    }

    windowWl(self)->primary_source =
      zwp_primary_selection_device_manager_v1_create_source(globalWl->primary_manager);

    zwp_primary_selection_source_v1_add_listener(windowWl(self)->primary_source,
                                                 &primary_selection_source_listener,
                                                 self);

    for (uint_fast8_t i = 0; i < ARRAY_SIZE(OFFERED_MIMES); ++i) {
        zwp_primary_selection_source_v1_offer(windowWl(self)->primary_source, OFFERED_MIMES[i]);
    }

    zwp_primary_selection_device_v1_set_selection(globalWl->primary_device,
                                                  windowWl(self)->primary_source,
                                                  globalWl->serial);
}

static void WindowWl_clipboard_send(struct WindowBase* self, const char* text)
{
    if (!text) {
        return;
    }

    WindowWl* w = windowWl(self);

    LOG("making a data source\n");

    free((void*)w->data_source_text);
    windowWl(self)->data_source_text = text;

    if (windowWl(self)->data_source) {
        wl_data_source_destroy(windowWl(self)->data_source);
    }

    windowWl(self)->data_source =
      wl_data_device_manager_create_data_source(globalWl->data_device_manager);
    wl_data_source_add_listener(w->data_source, &data_source_listener, self);

    for (uint_fast8_t i = 0; i < ARRAY_SIZE(OFFERED_MIMES); ++i) {
        wl_data_source_offer(w->data_source, OFFERED_MIMES[i]);
    }

    wl_data_device_set_selection(globalWl->data_device, w->data_source, globalWl->serial);
}

static void WindowWl_primary_get(struct WindowBase* self)
{
    if (windowWl(self)->primary_offer_mime_idx > -1 && windowWl(self)->primary_offer) {
        LOG("last recorded primary_selection_v1_data_offer mime: \"%s\" \n",
            ACCEPTED_MIMES[windowWl(self)->primary_offer_mime_idx]);

        int fds[2];

        errno = 0;
        if (pipe(fds)) {
            WRN("IO error: %s\n", strerror(errno));
            return;
        }

        const char* offer = ACCEPTED_MIMES[windowWl(self)->primary_offer_mime_idx];

        zwp_primary_selection_offer_v1_receive(windowWl(self)->primary_offer, offer, fds[1]);
        close(fds[1]);
        wl_display_roundtrip(globalWl->display);
        WindowWl_drain_pipe_to_clipboard(self, fds[0], windowWl(self)->data_offer_mime_idx == 0);
        close(fds[0]);
    }
}

static void WindowWl_clipboard_get(struct WindowBase* self)
{
    if (windowWl(self)->data_offer_mime_idx > -1 && windowWl(self)->data_offer) {
        LOG("last recorded wl_data_offer mime: \"%s\" \n",
            ACCEPTED_MIMES[windowWl(self)->data_offer_mime_idx]);

        if (windowWl(self)->data_offer) {
            int fds[2];

            errno = 0;
            if (pipe(fds)) {
                WRN("IO error: %s\n", strerror(errno));
                return;
            }

            wl_data_offer_receive(windowWl(self)->data_offer,
                                  ACCEPTED_MIMES[windowWl(self)->data_offer_mime_idx],
                                  fds[1]);

            close(fds[1]);
            wl_display_roundtrip(globalWl->display);
            WindowWl_drain_pipe_to_clipboard(self,
                                             fds[0],
                                             windowWl(self)->data_offer_mime_idx == 0);
            close(fds[0]);
        }
    }
}

static void frame_handle_done(void* data, struct wl_callback* callback, uint32_t time)
{
    wl_callback_destroy(callback);
    windowWl(((struct WindowBase*)data))->active_frame_callback = NULL;
    windowWl(((struct WindowBase*)data))->draw_next_frame       = true;
}

static struct wl_callback_listener frame_listener = {
    .done = &frame_handle_done,
};

static void wl_surface_handle_enter(void*              data,
                                    struct wl_surface* wl_surface,
                                    struct wl_output*  output)
{
    WindowBase* win_base = data;
    WindowWl*   win      = windowWl(win_base);

    if (wl_surface != win->surface) {
        return;
    }

    WlOutputInfo* info =
      Map_get_wl_output_ptr_WlOutputInfo(&win->outputs, (wl_output_ptr*)(&output));

    uint32_t num_active_outputs = 0;
    for (MapEntryIterator_wl_output_ptr_WlOutputInfo i =
           (MapEntryIterator_wl_output_ptr_WlOutputInfo){ 0, 0 };
         (i = Map_iter_wl_output_ptr_WlOutputInfo(&win->outputs, i)).entry;) {
        if (i.entry->value.is_active) {
            ++num_active_outputs;
        }
    }

    if (info) {
        info->is_active = true;
    }

    FLAG_UNSET(win_base->state_flags, WINDOW_IS_MINIMIZED);

    if (!num_active_outputs) {
        win->active_output     = info;
        win_base->lcd_filter   = win->active_output->lcd_filter;
        win_base->output_index = win->active_output->global_index;
        free(win_base->output_name);
        if (win->active_output->name) {
            win_base->output_name = strdup(win->active_output->name);
        } else {
            win_base->output_name = NULL;
        }
        win_base->dpi = win->active_output->dpi;
        Window_emit_output_change_event(win_base);
    }
}

static void wl_surface_handle_leave(void*              data,
                                    struct wl_surface* wl_surface,
                                    struct wl_output*  output)
{
    struct WindowBase* win_base = data;
    WindowWl*          win      = windowWl(win_base);

    if (wl_surface != win->surface) {
        return;
    }

    win->active_output = NULL;

    for (MapEntryIterator_wl_output_ptr_WlOutputInfo i = { 0, 0 };
         (i = Map_iter_wl_output_ptr_WlOutputInfo(&win->outputs, i)).entry;) {
        WlOutputInfo* info = &i.entry->value;

        if (output == info->output) {
            info->is_active = false;
        } else if (info->is_active) {
            win->active_output = info;
        }
    }

    uint32_t num_active_outputs = 0;
    for (MapEntryIterator_wl_output_ptr_WlOutputInfo i =
           (MapEntryIterator_wl_output_ptr_WlOutputInfo){ 0, 0 };
         (i = Map_iter_wl_output_ptr_WlOutputInfo(&win->outputs, i)).entry;) {
        if (i.entry->value.is_active) {
            ++num_active_outputs;
        }
    }

    if (num_active_outputs == 1) {
        win_base->lcd_filter   = win->active_output->lcd_filter;
        win_base->output_index = win->active_output->global_index;
        free(win_base->output_name);
        if (win->active_output->name) {
            win_base->output_name = strdup(win->active_output->name);
        } else {
            win_base->output_name = NULL;
        }
        win_base->dpi = win->active_output->dpi;
        Window_emit_output_change_event(win_base);
    } else if (num_active_outputs == 0) {
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
    struct WindowBase* win        = data;
    globalWl->moused_over_surface = surface;
    win->pointer_x                = wl_fixed_to_int(x);
    win->pointer_y                = wl_fixed_to_int(y);

    FLAG_UNSET(win->state_flags, WINDOW_IS_POINTER_HIDDEN);
    cursor_set(globalWl->cursor_arrow, serial);
    TRY_CALL(win->callbacks.activity_notify_handler, win->callbacks.user_data);
    globalWl->serial = serial;
    Window_notify_content_change(data);
}

static void pointer_handle_leave(void*              data,
                                 struct wl_pointer* pointer,
                                 uint32_t           serial,
                                 struct wl_surface* surface)
{
    globalWl->moused_over_surface = NULL;
    globalWl->serial              = serial;
}

static enum xdg_toplevel_resize_edge WindowWl_get_resize_edge(WindowBase* self)
{
    bool left   = self->pointer_x < CSD_SHADOW_MARGIN;
    bool top    = self->pointer_y < CSD_SHADOW_MARGIN - CSD_SHADOW_V_OFFSET;
    bool right  = self->pointer_x >= (self->w - CSD_SHADOW_MARGIN);
    bool bottom = self->pointer_y >= (self->h - CSD_SHADOW_MARGIN + CSD_SHADOW_V_OFFSET);

    bool none = !left && !right && !top && !bottom;

    if (none) {
        return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    } else if (top && left) {
        return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    } else if (top && right) {
        return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    } else if (bottom && left) {
        return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    } else if (bottom && right) {
        return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    } else if (top) {
        return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    } else if (bottom) {
        return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    } else if (left) {
        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    } else if (right) {
        return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    }

    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
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

    /* ending a surface move does not emit a button up */
    windowWl(win)->csd.window_move_inhibits_focus_loss = false;

    if (WindowWl_csd_enabled(win) &&
        globalWl->moused_over_surface == windowWl(win)->csd.shadow_surf) {
        windowWl(win)->csd.dragging_button = false;
        enum xdg_toplevel_resize_edge edge = WindowWl_get_resize_edge(win);

        switch (edge) {
            case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_BOTTOM_SIDE);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_TOP_SIDE);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_LEFT_SIDE);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_RIGHT_SIDE);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_TOP_LEFT_CORNER);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_TOP_RIGHT_CORNER);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_BOTTOM_LEFT_CORNER);
                break;
            case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
                WindowWl_set_pointer_style(win, MOUSE_POINTER_BOTTOM_RIGHT_CORNER);
                break;
            default:;
        }
    } else /* motion over main surface */ {
        if (windowWl(win)->csd.dragging_button) {
            windowWl(win)->csd.dragging_button = false;
            Ui_CSD_unhover_all_buttons(win->ui);
            win->ui->csd.damage = true;
            TRY_CALL(win->callbacks.on_framebuffer_damaged, win->callbacks.user_data);
            xdg_toplevel_move(windowWl(win)->xdg_toplevel,
                              globalWl->seat,
                              windowWl(win)->csd.dragging_button_serial);
            return;
        }

        if (FLAG_IS_SET(win->state_flags, WINDOW_IS_POINTER_HIDDEN)) {
            cursor_set(globalWl->cursor_arrow, 0);
            FLAG_UNSET(win->state_flags, WINDOW_IS_POINTER_HIDDEN);
        }

        TRY_CALL(win->callbacks.motion_handler,
                 win->callbacks.user_data,
                 globalWl->last_button_pressed,
                 win->pointer_x,
                 win->pointer_y);
    }
}

static void pointer_handle_button(void*              data,
                                  struct wl_pointer* wl_pointer,
                                  uint32_t           serial,
                                  uint32_t           time,
                                  uint32_t           button,
                                  uint32_t           state)
{
    struct WindowBase* win = data;
    globalWl->serial       = serial;

    windowWl(win)->csd.window_move_inhibits_focus_loss = false;
    windowWl(win)->csd.dragging_button                 = false;

    if (state && WindowWl_csd_enabled(win) &&
        globalWl->moused_over_surface == windowWl(win)->csd.shadow_surf) {
        enum xdg_toplevel_resize_edge resize_edge = WindowWl_get_resize_edge(win);
        if (resize_edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) /* on resize grip */ {
            windowWl(win)->csd.window_move_inhibits_focus_loss = true;
            xdg_toplevel_resize(windowWl(win)->xdg_toplevel, globalWl->seat, serial, resize_edge);
            return; /* block events */
        }
    }

    if (state && WindowWl_csd_enabled(win) &&
        globalWl->moused_over_surface == windowWl(win)->surface &&
        win->pointer_y <= UI_CSD_TITLEBAR_HEIGHT_PX) {

        if (button == 272) {
            ui_csd_titlebar_button_info_t* btn =
              Ui_csd_get_hovered_button(win->ui, win->pointer_x, win->pointer_y);

            windowWl(win)->csd.window_move_inhibits_focus_loss = true;
            if (!btn) {
                xdg_toplevel_move(windowWl(win)->xdg_toplevel, globalWl->seat, serial);
            } else {
                windowWl(win)->csd.dragging_button        = true;
                windowWl(win)->csd.dragging_button_serial = serial;
            }
            return;
        } else if (button == 273) {
            windowWl(win)->csd.window_move_inhibits_focus_loss = true;
            xdg_toplevel_show_window_menu(windowWl(win)->xdg_toplevel,
                                          globalWl->seat,
                                          serial,
                                          win->pointer_x,
                                          win->pointer_y);
            return;
        }
    }

    uint32_t final_mods = 0;

    uint32_t mods = xkb_state_serialize_mods(globalWl->xkb.state, XKB_STATE_MODS_EFFECTIVE);

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

    TRY_CALL(win->callbacks.button_handler,
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
        TRY_CALL(win->callbacks.button_handler,
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

static void pointer_handle_frame(void* data, struct wl_pointer* pointer)
{
    /* the compositor sends this every frame */
}

static void pointer_handle_axis_source(void*              data,
                                       struct wl_pointer* wl_pointer,
                                       uint32_t           axis_source)
{
}

static void pointer_handle_axis_stop(void*              data,
                                     struct wl_pointer* wl_pointer,
                                     uint32_t           time,
                                     uint32_t           axis)
{
}

static void pointer_handle_axis_discrete(void*              data,
                                         struct wl_pointer* wl_pointer,
                                         uint32_t           axis,
                                         int32_t            discrete)
{
    struct WindowBase* win = data;
    /* this is sent before a coresponding axis event, tell it to do nothing */
    windowWl(win)->got_discrete_axis_event = true;

    TRY_CALL(win->callbacks.button_handler,
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

    if (map_str == MAP_FAILED) {
        ERR("Reading keymap info failed");
    }

    globalWl->xkb.keymap = xkb_keymap_new_from_string(globalWl->xkb.ctx,
                                                      map_str,
                                                      XKB_KEYMAP_FORMAT_TEXT_V1,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);

    munmap(map_str, size);
    close(fd);

    if (!globalWl->xkb.keymap) {
        ERR("Failed to generate keymap");
    }

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

    if (!globalWl->xkb.compose_table) {
        ERR("Failed to generate keyboard compose table, is locale \'%s\' "
            "correct?",
            settings.locale.str);
    }

    globalWl->xkb.compose_state =
      xkb_compose_state_new(globalWl->xkb.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);

    if (!globalWl->xkb.compose_state) {
        ERR("Failed to create compose state");
    }

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
    globalWl->serial                                   = serial;
    struct WindowBase* win                             = data;
    windowWl(win)->csd.window_move_inhibits_focus_loss = false;
    FLAG_SET(((struct WindowBase*)data)->state_flags, WINDOW_IS_IN_FOCUS);
    TRY_CALL(win->callbacks.on_focus_changed, win->callbacks.user_data, true);
}

static void keyboard_handle_leave(void*               data,
                                  struct wl_keyboard* keyboard,
                                  uint32_t            serial,
                                  struct wl_surface*  surface)
{
    globalWl->serial            = serial;
    struct WindowBase* win      = data;
    globalWl->keycode_to_repeat = 0;

    if (windowWl(win)->csd.window_move_inhibits_focus_loss) {
        return;
    }

    FLAG_UNSET(((struct WindowBase*)data)->state_flags, WINDOW_IS_IN_FOCUS);
    TRY_CALL(win->callbacks.on_focus_changed, win->callbacks.user_data, false);
}

static void keyboard_handle_key(void*               data,
                                struct wl_keyboard* keyboard,
                                uint32_t            serial,
                                uint32_t            time,
                                uint32_t            key,
                                uint32_t            state)
{
    globalWl->serial = serial;

    bool               is_repeat_event = !keyboard;
    struct WindowBase* win             = data;
    uint32_t           utf, code = key + 8;
    xkb_keysym_t       sym, rawsym, composed_sym;

    if (!is_repeat_event) {
        FLAG_SET(win->state_flags, WINDOW_NEEDS_SWAP);
    }

    sym = composed_sym = xkb_state_key_get_one_sym(globalWl->xkb.state, code);

    if (keysym_is_mod(sym)) {
        return;
    }

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
    // Ctrl + ` and Ctrl + @
    bool utf_conversion_success = utf || ((sym == XKB_KEY_grave || sym == XKB_KEY_at) &&
                                          FLAG_IS_SET(mods, globalWl->xkb.ctrl_mask));

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
        TRY_CALL(win->callbacks.key_handler, win->callbacks.user_data, final, rawsym, final_mods);
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
    struct WindowBase* win      = data;
    globalWl->kbd_repeat_rate   = OR(rate, 30);
    globalWl->kbd_repeat_dealy  = OR(delay, 500);
    win->key_repeat_interval_ms = globalWl->kbd_repeat_rate;
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
{
    LOG("wl::zxdg_toplevel_decoration::configure{ mode: %u }\n", mode);

    /* struct WindowBase* self = data; */
    /* xdg_surface_ack_configure(windowWl(self)->xdg_surface, globalWl->serial); */
}

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
    WindowBase* win  = data;

    if (WindowWl_csd_enabled(win)) {
        if (WindowWl_csd_created(win)) {
            WindowWl_resize_csd(win);
        } else {
            WindowWl_build_csd(win);
        }
    }

    Window_notify_content_change(win);
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
    static bool        init = false;
    struct WindowBase* win  = data;

    enum xdg_toplevel_state* s;

    bool is_wm_size    = false;
    bool is_fullscreen = false;
    bool is_maximized  = false;
    bool is_tiled      = false;

    if (!init && width == 1 && height == 1) {
        return;
    }

    wl_array_for_each(s, states)
    {
        if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            is_fullscreen = true;
        } else if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            is_maximized = true;
        } else if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN || *s == XDG_TOPLEVEL_STATE_TILED_LEFT ||
                   *s == XDG_TOPLEVEL_STATE_TILED_RIGHT || *s == XDG_TOPLEVEL_STATE_TILED_TOP ||
                   *s == XDG_TOPLEVEL_STATE_TILED_BOTTOM) {
            is_tiled = true;
        }
    }

    is_wm_size = is_fullscreen || is_maximized || is_tiled;

    if (WindowWl_csd_enabled(win)) {
        if (is_fullscreen) {
            WindowWl_hide_csd(win);
            win->ui->csd.mode = UI_CSD_MODE_NONE;
            TRY_CALL(win->callbacks.on_csd_style_changed,
                     win->callbacks.user_data,
                     UI_CSD_MODE_NONE);
        } else if (is_maximized || is_tiled) {
            WindowWl_show_tiled_csd(win);
            win->ui->csd.mode = UI_CSD_MODE_TILED;
            TRY_CALL(win->callbacks.on_csd_style_changed,
                     win->callbacks.user_data,
                     UI_CSD_MODE_TILED);
        } else {
            WindowWl_show_floating_csd(win);
            win->ui->csd.mode = UI_CSD_MODE_FLOATING;
            TRY_CALL(win->callbacks.on_csd_style_changed,
                     win->callbacks.user_data,
                     UI_CSD_MODE_FLOATING);
        }
    }

    if (!width && !height) {
        if (win->previous_w && win->previous_h) {
            win->w = win->previous_w;
            win->h = win->previous_h;
        }
        wl_egl_window_resize(windowWl(win)->egl_window, win->w, win->h, 0, 0);
    } else {
        init   = true;
        win->w = width;
        win->h = height;
        if (!is_wm_size) {
            win->previous_w = win->w;
            win->previous_h = win->h;
        }
        wl_egl_window_resize(windowWl(win)->egl_window, win->w, win->h, 0, 0);
        if (is_fullscreen) {
            xdg_surface_set_window_geometry(windowWl(win)->xdg_surface, 0, 0, win->w, win->h);
        }
    }
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

static lcd_filter_e wl_subpixel_to_lcd_filter(int32_t subpixel)
{
    lcd_filter_e filter[] = {
        LCD_FILTER_UNDEFINED, LCD_FILTER_NONE,  LCD_FILTER_H_RGB,
        LCD_FILTER_H_BGR,     LCD_FILTER_V_RGB, LCD_FILTER_V_BGR,
    };
    return filter[subpixel];
}

/* Information about an output is sent (initially or updated) by wl_output.geometry and/or
 * wl_output.mode events that can come in any order. The wl_output.done event is used to
 * indicate that the compositor has sent all info about a specific output. Record data from last
 * received 'mode' and 'output' events so we can apply them on 'done'. If the output parameter
 * of 'done' matches any of the existing wl_output-s we have, interpret it as an update to that
 * output atherwise add it. */
typedef struct
{
    bool         geometry_event_received;
    bool         mode_event_received;
    lcd_filter_e geometry_filter;
    char*        dpy_name;
    double       physical_width_inch;
    int8_t       global_output_index;
    int32_t      frame_time_ms;
    int32_t      pixel_width;
} output_params_t;

static output_params_t last_recorded_output_params = (output_params_t){
    .geometry_event_received = false,
    .mode_event_received     = false,
    .global_output_index     = 0,
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
    last_recorded_output_params.geometry_event_received = true;
    last_recorded_output_params.geometry_filter         = wl_subpixel_to_lcd_filter(subpixel);
    last_recorded_output_params.physical_width_inch     = (double)physical_width * INCH_IN_MM;

    free(last_recorded_output_params.dpy_name);
    last_recorded_output_params.dpy_name = NULL;
    if (model) {
        last_recorded_output_params.dpy_name = strdup(model);
    }

    if (settings.lcd_filter == LCD_FILTER_UNDEFINED) {
        settings.lcd_filter = last_recorded_output_params.geometry_filter;
    }
}

static void output_handle_mode(void*             data,
                               struct wl_output* wl_output,
                               uint32_t          flags,
                               int32_t           w,
                               int32_t           h,
                               int32_t           refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        last_recorded_output_params.mode_event_received = true;
        last_recorded_output_params.pixel_width         = w;

        /* Refresh rate can be set to zero if it doesn't make sense e.g. for virtual outputs. */
        last_recorded_output_params.frame_time_ms =
          refresh ? 1000000 / refresh : WL_FALLBACK_TGT_FRAME_TIME_MS;
    }
}

static void output_handle_done(void* data, struct wl_output* wl_output)
{
    WindowBase* win_base = data;
    WindowWl*   win      = windowWl(win_base);

    bool is_update = false;
    bool is_delete = !last_recorded_output_params.mode_event_received &&
                     !last_recorded_output_params.geometry_event_received;

    if (is_delete) {
        if (win->active_output && win->active_output->output == wl_output) {
            win->active_output = NULL;
        }

        WlOutputInfo* oi =
          Map_get_wl_output_ptr_WlOutputInfo(&win->outputs, (wl_output_ptr*)&wl_output);

        if (oi) {
            uint8_t deleted_idx = oi->global_index;
            Map_remove_wl_output_ptr_WlOutputInfo(&win->outputs, (wl_output_ptr*)&wl_output);

            for (MapEntryIterator_wl_output_ptr_WlOutputInfo i = { 0, 0 };
                 (i = Map_iter_wl_output_ptr_WlOutputInfo(&win->outputs, i)).entry;) {
                WlOutputInfo* info = &i.entry->value;

                if (info->global_index > deleted_idx) {
                    --info->global_index;
                }
            }
            --last_recorded_output_params.global_output_index;
        }
    } else {
        for (MapEntryIterator_wl_output_ptr_WlOutputInfo i = { 0, 0 };
             (i = Map_iter_wl_output_ptr_WlOutputInfo(&win->outputs, i)).entry;) {
            WlOutputInfo* info = &i.entry->value;

            if (info->output == wl_output) {
                is_update = true;

                if (last_recorded_output_params.geometry_event_received) {
                    free(info->name);
                    info->name       = last_recorded_output_params.dpy_name;
                    info->lcd_filter = last_recorded_output_params.geometry_filter;
                    info->width_inch = last_recorded_output_params.physical_width_inch;
                }

                if (last_recorded_output_params.mode_event_received) {
                    info->target_frame_time_ms = last_recorded_output_params.frame_time_ms;
                    info->width_px             = last_recorded_output_params.pixel_width;
                }

                info->dpi = (double)info->width_px / info->width_inch;
            }
            break;
        }
    }

    if (!is_update && last_recorded_output_params.mode_event_received &&
        last_recorded_output_params.geometry_event_received) {
        WlOutputInfo info = {
            .output               = wl_output,
            .is_active            = false,
            .target_frame_time_ms = last_recorded_output_params.frame_time_ms,
            .lcd_filter           = last_recorded_output_params.geometry_filter,
            .name                 = last_recorded_output_params.dpy_name,
            .dpi                  = (double)last_recorded_output_params.pixel_width /
                   last_recorded_output_params.physical_width_inch,
            .width_px     = last_recorded_output_params.pixel_width,
            .width_inch   = last_recorded_output_params.physical_width_inch,
            .global_index = ++last_recorded_output_params.global_output_index,
        };

        win->active_output =
          Map_insert_wl_output_ptr_WlOutputInfo(&win->outputs, (wl_output_ptr)wl_output, info);
    }

    last_recorded_output_params.dpy_name                = NULL;
    last_recorded_output_params.geometry_event_received = false;
    last_recorded_output_params.mode_event_received     = false;

    if (is_update && (last_recorded_output_params.mode_event_received ||
                      last_recorded_output_params.geometry_event_received)) {
        Window_emit_output_change_event((WindowBase*)win);
    }
}

static void output_handle_scale(void* data, struct wl_output* wl_output, int32_t factor)
{
    // TODO: scale everything ourselves and use wl_surface.set_buffer_scale.
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode     = output_handle_mode,
    .done     = output_handle_done,
    .scale    = output_handle_scale,
};

/* End output listener */

/* data device listener */

static void data_offer_handle_offer(void*                 data,
                                    struct wl_data_offer* data_offer,
                                    const char*           mime_type)
{
    WindowWl* w = windowWl(((struct WindowBase*)data));

    LOG("wl.data_offer::offer{ mime_type: %s ", mime_type);

    for (uint_fast8_t i = 0; i < ARRAY_SIZE(ACCEPTED_MIMES); ++i) {

        if (strcmp(mime_type, ACCEPTED_MIMES[i])) {
            continue;
        }

        bool prefferable_mime = (w->data_offer_mime_idx == -1 || w->data_offer_mime_idx >= i);

        if (data_offer != w->data_offer) {
            LOG("- ACCEPTED(new data) }\n");
            w->data_offer          = data_offer;
            w->data_offer_mime_idx = i;
            wl_data_offer_accept(data_offer, 0, mime_type);
            return;
        } else if (prefferable_mime) {
            LOG("- ACCEPTED(preffered mime type) }\n");
            w->data_offer          = data_offer;
            w->data_offer_mime_idx = i;
            wl_data_offer_accept(data_offer, 0, mime_type);
            return;
        }
    }

    if (w->data_offer_mime_idx == -1) {
        LOG(" - REJECTED(not supported) }\n");
    } else {
        LOG(" - REJECTED(\'%s\' is prefferable) }\n", ACCEPTED_MIMES[w->data_offer_mime_idx]);
    }

    wl_data_offer_accept(data_offer, 0, NULL);
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
                                     struct wl_data_offer* data_offer,
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
                wl_data_offer_accept(data_offer,
                                     globalWl->serial,
                                     ACCEPTED_MIMES[w->data_offer_mime_idx]);
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
    LOG("wl::data_device::drop{ ");

    struct WindowBase* base = (struct WindowBase*)data;
    WindowWl*          self = windowWl(base);

    if (!self->dnd_data_offer) {
        LOG("<offer expired> }\n");
        return;
    }

    struct wl_data_offer* offer    = self->dnd_data_offer;
    int                   mime_idx = self->data_offer_mime_idx;

    LOG("mime: %s }\n", ACCEPTED_MIMES[mime_idx]);

    int fds[2];
    errno = 0;
    if (pipe(fds)) {
        WRN("IO error: %s\n", strerror(errno));
        return;
    }
    wl_data_offer_receive(offer, ACCEPTED_MIMES[mime_idx], fds[1]);
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
    LOG("wl::data_device::selection { has_offer: %d }\n", offer != NULL);

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
    LOG("wl::data_source::target\n");
}

static void data_source_handle_send(void*                  data,
                                    struct wl_data_source* wl_data_source,
                                    const char*            mime_type,
                                    int32_t                fd)
{
    LOG("wl::data_source::send{ mime_type: %s }\n", mime_type);

    WindowWl* w = windowWl(((struct WindowBase*)data));

    bool is_supported_mime = false;
    for (uint_fast8_t i = 0; i < ARRAY_SIZE(OFFERED_MIMES); ++i) {
        if (!strcmp(mime_type, OFFERED_MIMES[i])) {
            is_supported_mime = true;
            break;
        }
    }

    if (w->data_source_text && is_supported_mime) {
        LOG("writing \'%s\' to fd\n", w->data_source_text);
        size_t len = strlen(w->data_source_text);
        if (len <= SSIZE_MAX) {
            ssize_t bytes = write(fd, w->data_source_text, len);
            if (bytes != (ssize_t)len) {
                WRN("could not write to pipe %s\n", strerror(errno))
            }
        } else {
            WRN("could not write to pipe buffer too large");
        }
    }

    close(fd);
}

static void data_source_handle_cancelled(void* data, struct wl_data_source* wl_data_source)
{
    WindowWl* w = windowWl(((struct WindowBase*)data));

    wl_data_source_destroy(wl_data_source);
    w->data_source = NULL;

    LOG("wl::data_source::canceled\n");
}

static void data_source_handle_dnd_drop_performed(void* data, struct wl_data_source* wl_data_source)
{
    /* source should NOT be destroyed here */
    LOG("wl::data_source::dnd_drop_performed\n");
}

static void data_source_handle_dnd_finished(void* data, struct wl_data_source* wl_data_source)
{
    // WindowWl* w = windowWl(((struct WindowBase*)data));
    wl_data_source_destroy(wl_data_source);
    LOG("wl::data_source::dnd_finished\n");
}

static void data_source_handle_action(void*                  data,
                                      struct wl_data_source* wl_data_source,
                                      uint32_t               dnd_action)
{
    LOG("wl::data_source::action\n");
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
    bool unused = false;

#ifdef DEBUG
    uint32_t ver_req = 1;
#define L_RQDBG(v) ver_req = v;
#else
#define L_RQDBG(_) ;
#endif

#define L_REQUIRE_VER(v)                                                                           \
    L_RQDBG(v);                                                                                    \
    if ((v) > version) {                                                                           \
        ERR("Wayland interface \'%s\' version to low. Required " #v ", provided %u.",              \
            interface,                                                                             \
            version);                                                                              \
    }

    if (!strcmp(interface, wl_compositor_interface.name)) {
        L_REQUIRE_VER(4);
        globalWl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (!strcmp(interface, wl_shell_interface.name)) {
        globalWl->wl_shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        L_REQUIRE_VER(2); // v2 adds support for tiled toplevels states
        globalWl->xdg_shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
        xdg_wm_base_add_listener(globalWl->xdg_shell, &wm_base_listener, data);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        L_REQUIRE_VER(5);
        globalWl->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(globalWl->seat, &seat_listener, data);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        L_REQUIRE_VER(2);
        globalWl->output = wl_registry_bind(registry, name, &wl_output_interface, 2);
        wl_output_add_listener(globalWl->output, &output_listener, data);
    } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
        globalWl->decoration_manager =
          wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        globalWl->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
        L_REQUIRE_VER(3);
        globalWl->data_device_manager =
          wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3);
    } else if (!strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name)) {
        globalWl->primary_manager =
          wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1);
    } else if (!strcmp(interface, org_kde_kwin_blur_manager_interface.name)) {
        globalWl->kde_kwin_blur_manager =
          wl_registry_bind(registry, name, &org_kde_kwin_blur_manager_interface, 1);
    } else if (!strcmp(interface, wl_subcompositor_interface.name)) {
        globalWl->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    } else {
        unused = true;
    }

    if (unused) {
        LOG("wl::registry{ name: %-45s ver: %2u unused }\n", interface, version);
    } else {
        LOG("wl::registry{ name: %-45s ver: %2u binding to version %u }\n",
            interface,
            version,
            ver_req);
    }
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
    char* ssize = getenv("XCURSOR_SIZE");
    int   size  = WL_DEFAULT_CURSOR_SIZE;

    if (ssize && !(size = atoi(ssize))) {
        size = WL_DEFAULT_CURSOR_SIZE;
    }

    if (!(globalWl->cursor_theme = wl_cursor_theme_load(NULL, size, globalWl->shm))) {
        WRN("Failed to load cursor theme\n");
        return;
    }

    globalWl->cursor_arrow    = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "left_ptr");
    globalWl->cursor_beam     = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "xterm");
    globalWl->cursor_hand     = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "hand1");
    globalWl->cursor_top_side = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "top_side");
    globalWl->cursor_bottom_side =
      wl_cursor_theme_get_cursor(globalWl->cursor_theme, "bottom_side");
    globalWl->cursor_left_side  = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "left_side");
    globalWl->cursor_right_side = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "right_side");
    globalWl->cursor_top_left_corner =
      wl_cursor_theme_get_cursor(globalWl->cursor_theme, "top_left_corner");
    globalWl->cursor_top_right_corner =
      wl_cursor_theme_get_cursor(globalWl->cursor_theme, "top_right_corner");
    globalWl->cursor_bottom_left_corner =
      wl_cursor_theme_get_cursor(globalWl->cursor_theme, "bottom_left_corner");
    globalWl->cursor_bottom_right_corner =
      wl_cursor_theme_get_cursor(globalWl->cursor_theme, "bottom_right_corner");
    globalWl->cursor_move = wl_cursor_theme_get_cursor(globalWl->cursor_theme, "fleur");

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

    if (!globalWl->pointer) {
        return;
    }

    struct wl_buffer*       b;
    struct wl_cursor_image* img = OR(what, globalWl->cursor_arrow)->images[0];

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
struct WindowBase* WindowWl_new(uint32_t w, uint32_t h, gfx_api_t gfx_api, Ui* ui)
{
    global = _calloc(1, sizeof(WindowStatic) + sizeof(GlobalWl) - sizeof(uint8_t));
    global->target_frame_time_ms = 16.6667;

    /* passing NULL grabs WAYLAND_DISPLAY from env */
    globalWl->display = wl_display_connect(NULL);

    if (!globalWl->display) {
        free(global);
        LOG("No wayland displays found\n");
        return NULL;
    }

    globalWl->xkb.ctx = xkb_context_new(0);

    struct WindowBase* win =
      _calloc(1, sizeof(struct WindowBase) + sizeof(WindowWl) + sizeof(uint8_t));

    win->w                 = w;
    win->h                 = h;
    win->ui                = ui;
    windowWl(win)->outputs = Map_new_wl_output_ptr_WlOutputInfo(4);
    FLAG_SET(win->state_flags, WINDOW_IS_IN_FOCUS);
    FLAG_SET(win->state_flags, WINDOW_IS_MINIMIZED);

    win->interface = &window_interface_wayland;

    windowWl(win)->data_offer_mime_idx        = -1;
    windowWl(win)->primary_offer_mime_idx     = -1;
    windowWl(win)->new_primary_offer_mime_idx = -1;

    globalWl->registry = wl_display_get_registry(globalWl->display);
    wl_registry_add_listener(globalWl->registry, &registry_listener, win);
    wl_display_roundtrip(globalWl->display);

    if (!globalWl->decoration_manager) {
        win->h += UI_CSD_TITLEBAR_HEIGHT_PX;
    }

    if (globalWl->data_device_manager) {
        globalWl->data_device =
          wl_data_device_manager_get_data_device(globalWl->data_device_manager, globalWl->seat);

        wl_data_device_add_listener(globalWl->data_device, &data_device_listener, win);
    }

    if (globalWl->primary_manager) {
        globalWl->primary_device =
          zwp_primary_selection_device_manager_v1_get_device(globalWl->primary_manager,
                                                             globalWl->seat);

        zwp_primary_selection_device_v1_add_listener(globalWl->primary_device,
                                                     &primary_selection_device_listener,
                                                     win);
    } else {
        WRN("%s not supported by compositor\n",
            zwp_primary_selection_device_manager_v1_interface.name);
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

    switch (gfx_api.type) {
        case GFX_API_GL:
            if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
                ERR("EGL API binding error %s", egl_get_error_string(eglGetError()));
            }
            break;

        case GFX_API_GLES:
            if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
                ERR("EGL API binding error %s", egl_get_error_string(eglGetError()));
            }
            break;

        case GFX_API_VK:
            ERR("vulkan context not implemented for wayland\n");
            break;
    }

    eglChooseConfig(globalWl->egl_display, cfg_attribs, &config, 1, &num_config);

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION,
        gfx_api.version_major,
        EGL_CONTEXT_MINOR_VERSION,
        gfx_api.version_minor,
        EGL_NONE, /* terminate list */
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

        if (settings.decoration_style != DECORATION_STYLE_NONE) {
            if (globalWl->decoration_manager && !settings.force_csd) {
                /* KDE has it's own SSD protocol extension, but it is only supported by kwin and
                 * wlr, both of them also support zxdg_toplevel_decoration_v1. There is no point
                 * in implementing it. */
                windowWl(win)->toplevel_decoration =
                  zxdg_decoration_manager_v1_get_toplevel_decoration(globalWl->decoration_manager,
                                                                     windowWl(win)->xdg_toplevel);

                zxdg_toplevel_decoration_v1_add_listener(windowWl(win)->toplevel_decoration,
                                                         &zxdg_toplevel_decoration_listener,
                                                         win);

                zxdg_toplevel_decoration_v1_set_mode(windowWl(win)->toplevel_decoration,
                                                     ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
            } else /* no ssd support on server */ {
                WindowWl_enable_csd(win, CSD_MODE_FLOATING);
                /* csd subsurfs will be created after toplevel was configured. At this point we
                 * don't know what window size we will get. */
            }
        }

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

    if (settings.background_blur && globalWl->kde_kwin_blur_manager) {
        windowWl(win)->kde_kwin_blur =
          org_kde_kwin_blur_manager_create(globalWl->kde_kwin_blur_manager, windowWl(win)->surface);
        org_kde_kwin_blur_set_user_data(windowWl(win)->kde_kwin_blur, win);

        // null region implies complete surface
        org_kde_kwin_blur_set_region(windowWl(win)->kde_kwin_blur, NULL);
        org_kde_kwin_blur_commit(windowWl(win)->kde_kwin_blur);
    }

    struct wl_callback* frame_callback = wl_surface_frame(windowWl(win)->surface);
    wl_callback_add_listener(frame_callback, &frame_listener, win);

    return win;
}

struct WindowBase* Window_new_wayland(Pair_uint32_t res,
                                      Pair_uint32_t cell_dims,
                                      gfx_api_t     gfx_api,
                                      Ui*           ui)
{
    struct WindowBase* win = WindowWl_new(res.first, res.second, gfx_api, ui);

    if (!win) {
        return NULL;
    }

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

static void WindowWl_set_incremental_resize(struct WindowBase* self, uint32_t x, uint32_t y) {}

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
        int32_t time_offset = (1000 / globalWl->kbd_repeat_rate);

        globalWl->repeat_point = TimePoint_add_ms(globalWl->repeat_point, time_offset);

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
    static bool initial_event_emited = false;

    if (unlikely(!initial_event_emited && self->callbacks.on_output_changed &&
                 windowWl(self)->active_output &&
                 Map_count_wl_output_ptr_WlOutputInfo(&windowWl(self)->outputs) == 1)) {
        initial_event_emited = true;
        Window_emit_output_change_event(self);
    }

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

static void WindowWl_swap_buffers(WindowBase* self)
{
    self->paint                     = false;
    windowWl(self)->draw_next_frame = false;

    EGLint age;
    eglQuerySurface(globalWl->egl_display, windowWl(self)->egl_surface, EGL_BUFFER_AGE_EXT, &age);

    window_partial_swap_request_t* swap_req = NULL;
    if (likely(self->callbacks.on_redraw_requested)) {
        swap_req = self->callbacks.on_redraw_requested(self->callbacks.user_data, age);
    }

    EGLBoolean result;
    if (eglSwapBuffersWithDamageKHR && swap_req && swap_req->count > 0) {
        result = eglSwapBuffersWithDamageKHR(globalWl->egl_display,
                                             windowWl(self)->egl_surface,
                                             (const EGLint*)swap_req->regions,
                                             swap_req->count);
    } else {
        result = eglSwapBuffers(globalWl->egl_display, windowWl(self)->egl_surface);
    }

    if (unlikely(result != EGL_TRUE)) {
        ERR("EGL buffer swap failed: %s\n", egl_get_error_string(eglGetError()));
    }

    windowWl(self)->active_frame_callback = wl_surface_frame(windowWl(self)->surface);

    wl_callback_add_listener(windowWl(self)->active_frame_callback, &frame_listener, self);
}

static bool WindowWl_maybe_swap(WindowBase* self, bool do_swap)
{
    if (windowWl(self)->draw_next_frame && self->paint && do_swap) {
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

static void WindowWl_set_minimized(struct WindowBase* self)
{
    if (globalWl->xdg_shell) {
        xdg_toplevel_set_minimized(windowWl(self)->xdg_toplevel);
        FLAG_SET(self->state_flags, WINDOW_IS_MINIMIZED);
    } /* not supported in wl_shell */
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
    WindowWl_destroy_csd(self);

    if (windowWl(self)->active_frame_callback) {
        wl_callback_destroy(windowWl(self)->active_frame_callback);
        windowWl(self)->active_frame_callback = NULL;
    }

    wl_display_roundtrip(globalWl->display);
    wl_display_dispatch_pending(globalWl->display);
    wl_display_flush(globalWl->display);

    wl_pointer_release(globalWl->pointer);

    if (globalWl->cursor_theme) {
        wl_surface_destroy(globalWl->cursor_surface);
        wl_cursor_theme_destroy(globalWl->cursor_theme);
    }

    if (windowWl(self)->kde_kwin_blur) {
        org_kde_kwin_blur_destroy(windowWl(self)->kde_kwin_blur);
    }

    if (globalWl->kde_kwin_blur_manager) {
        org_kde_kwin_blur_manager_destroy(globalWl->kde_kwin_blur_manager);
    }

    if (globalWl->decoration_manager && windowWl(self)->toplevel_decoration) {
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

    if (windowWl(self)->data_source) {
        wl_data_source_destroy(windowWl(self)->data_source);
    }

    if (windowWl(self)->primary_source) {
        zwp_primary_selection_source_v1_destroy(windowWl(self)->primary_source);
    }

    wl_egl_window_destroy(windowWl(self)->egl_window);
    eglDestroySurface(globalWl->egl_display, windowWl(self)->egl_surface);
    eglDestroyContext(globalWl->egl_display, windowWl(self)->egl_context);

    if (globalWl->subcompositor) {
        wl_subcompositor_destroy(globalWl->subcompositor);
    }

    wl_registry_destroy(globalWl->registry);
    wl_display_disconnect(globalWl->display);

    eglTerminate(globalWl->egl_display);

    Map_destroy_wl_output_ptr_WlOutputInfo(&windowWl(self)->outputs);

    free((void*)windowWl(self)->data_source_text);
    free((void*)windowWl(self)->primary_source_text);

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
    if (style == MOUSE_POINTER_HIDDEN) {
        FLAG_SET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
    } else {
        FLAG_UNSET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
    }

    switch (style) {
        case MOUSE_POINTER_HIDDEN:
            cursor_set(0, 0);
            break;

        case MOUSE_POINTER_ARROW:
            cursor_set(globalWl->cursor_arrow, 0);
            break;

        case MOUSE_POINTER_I_BEAM:
            cursor_set(globalWl->cursor_beam, 0);
            break;

        case MOUSE_POINTER_HAND:
            cursor_set(globalWl->cursor_hand, 0);
            break;

        case MOUSE_POINTER_TOP_SIDE:
            cursor_set(globalWl->cursor_top_side, 0);
            break;

        case MOUSE_POINTER_BOTTOM_SIDE:
            cursor_set(globalWl->cursor_bottom_side, 0);
            break;

        case MOUSE_POINTER_LEFT_SIDE:
            cursor_set(globalWl->cursor_left_side, 0);
            break;

        case MOUSE_POINTER_RIGHT_SIDE:
            cursor_set(globalWl->cursor_right_side, 0);
            break;

        case MOUSE_POINTER_TOP_LEFT_CORNER:
            cursor_set(globalWl->cursor_top_left_corner, 0);
            break;

        case MOUSE_POINTER_TOP_RIGHT_CORNER:
            cursor_set(globalWl->cursor_top_right_corner, 0);
            break;

        case MOUSE_POINTER_BOTTOM_LEFT_CORNER:
            cursor_set(globalWl->cursor_bottom_left_corner, 0);
            break;

        case MOUSE_POINTER_BOTTOM_RIGHT_CORNER:
            cursor_set(globalWl->cursor_bottom_right_corner, 0);
            break;

        case MOUSE_POINTER_MOVE:
            cursor_set(globalWl->cursor_move, 0);
            break;
    }
}

#endif
