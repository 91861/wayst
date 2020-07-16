/* See LICENSE for license information. */

/**
 * struct WindowBase - window interface/base class
 */

#pragma once

#include "settings.h"
#include "util.h"

#define WINDOW_IS_CLOSED         (1 << 0)
#define WINDOW_IS_FULLSCREEN     (1 << 1)
#define WINDOW_NEEDS_SWAP        (1 << 2)
#define WINDOW_IS_IN_FOCUS       (1 << 3)
#define WINDOW_IS_MAXIMIZED      (1 << 4)
#define WINDOW_IS_POINTER_HIDDEN (1 << 5)
#define WINDOW_IS_MINIMIZED      (1 << 6)

#define MOUSE_BUTTON_RELEASE (1 << 0)
#define MOUSE_BUTTON_1       (1 << 1)
#define MOUSE_BUTTON_2       (1 << 2)
#define MOUSE_BUTTON_3       (1 << 3)

typedef struct
{
    uint32_t target_frame_time_ms;

    __attribute__((aligned(8))) uint8_t subclass_data;

} WindowStatic;

enum MousePointerStyle
{
    MOUSE_POINTER_HIDDEN,
    MOUSE_POINTER_ARROW,
    MOUSE_POINTER_I_BEAM,
};

struct WindowBase;

struct IWindow
{
    void (*set_fullscreen)(struct WindowBase* self, bool fullscreen);
    void (*resize)(struct WindowBase* self, uint32_t w, uint32_t h);
    void (*events)(struct WindowBase* self);
    void (*set_title)(struct WindowBase* self, const char* title);
    void (*set_app_id)(struct WindowBase* self, const char* app_id);
    void (*maybe_swap)(struct WindowBase* self);
    void (*destroy)(struct WindowBase* self);
    int (*get_connection_fd)(struct WindowBase* self);
    void (*clipboard_send)(struct WindowBase* self, const char* text);
    void (*clipboard_get)(struct WindowBase* self);
    void (*set_swap_interval)(struct WindowBase* self, int val);
    void (*set_pointer_style)(struct WindowBase* self, enum MousePointerStyle);
    void* (*get_gl_ext_proc_adress)(struct WindowBase* self, const char* name);
    uint32_t (*get_keycode_from_name)(struct WindowBase* self, char* name);
};

typedef struct WindowBase
{
    int32_t  w, h, x, y;
    int32_t  pointer_x, pointer_y;
    uint16_t state_flags;
    bool     paint;
    struct window_external_data
    {
        void* user_data;
        void (*key_handler)(void* user_data, uint32_t code, uint32_t rawcode, uint32_t mods);
        void (*button_handler)(void*    user_data,
                               uint32_t code,
                               bool     state,
                               int32_t  x,
                               int32_t  y,
                               int32_t  ammount,
                               uint32_t mods);
        void (*motion_handler)(void* user_data, uint32_t code, int32_t x, int32_t y);
        void (*clipboard_handler)(void* user_data, const char* text);
        void (*activity_notify_handler)(void* user_data);
        void (*on_redraw_requested)(void* user_data);
    } callbacks;

    char* title;

    struct IWindow* interface;

    __attribute__((aligned(8))) uint8_t extend_data;

} Window_;

static void Window_update_title(struct WindowBase* self, const char* title)
{
    if (settings.dynamic_title) {
        char* tmp = asprintf(settings.title_format.str, settings.title.str, title);
        self->interface->set_title(self, tmp);
        free(tmp);
    }
}

/* Froward interface functions */
static inline void* Window_get_proc_adress(struct WindowBase* self, const char* procname)
{
    return self->interface->get_gl_ext_proc_adress(self, procname);
}

static inline void Window_set_fullscreen(struct WindowBase* self, bool fullscreen)
{
    self->interface->set_fullscreen(self, fullscreen);
}

static inline void Window_set_swap_interval(struct WindowBase* self, bool value)
{
    self->interface->set_swap_interval(self, value);
}

static inline void Window_resize(struct WindowBase* self, uint32_t w, uint32_t h)
{
    self->interface->resize(self, w, h);
}

static inline void Window_events(struct WindowBase* self)
{
    self->interface->events(self);
}

static inline void Window_set_title(struct WindowBase* self, const char* title)
{
    self->interface->set_title(self, title);
}

static inline void Window_set_app_id(struct WindowBase* self, const char* app_id)
{
    self->interface->set_app_id(self, app_id);
}

static inline void Window_maybe_swap(struct WindowBase* self)
{
    self->interface->maybe_swap(self);
}

static inline void Window_destroy(struct WindowBase* self)
{
    self->interface->destroy(self);
}

static inline void Window_clipboard_get(struct WindowBase* self)
{
    self->interface->clipboard_get(self);
}

static inline void Window_clipboard_send(struct WindowBase* self, const char* text)
{
    self->interface->clipboard_send(self, text);
}

static inline void Window_set_pointer_style(struct WindowBase* self, enum MousePointerStyle style)
{
    if (style == MOUSE_POINTER_HIDDEN && FLAG_IS_SET(self->state_flags, WINDOW_IS_POINTER_HIDDEN)) {
        return;
    }
    self->interface->set_pointer_style(self, style);
}

static inline uint32_t Window_get_keysym_from_name(struct WindowBase* self, char* name)
{
    return self->interface->get_keycode_from_name(self, name);
}

/* Trivial base functions */
static inline void* Window_subclass_data_ptr(struct WindowBase* self)
{
    return &self->extend_data;
}

static inline int Window_get_connection_fd(struct WindowBase* self)
{
    return self->interface->get_connection_fd(self);
}

static inline bool Window_is_closed(struct WindowBase* self)
{
    return FLAG_IS_SET(self->state_flags, WINDOW_IS_CLOSED);
}

static inline bool Window_is_minimized(struct WindowBase* self)
{
    return FLAG_IS_SET(self->state_flags, WINDOW_IS_MINIMIZED);
}

static inline bool Window_is_focused(struct WindowBase* self)
{
    return FLAG_IS_SET(self->state_flags, WINDOW_IS_IN_FOCUS);
}

static inline bool Window_is_fullscreen(struct WindowBase* self)
{
    return FLAG_IS_SET(self->state_flags, WINDOW_IS_FULLSCREEN);
}

static inline bool Window_is_pointer_hidden(struct WindowBase* self)
{
    return FLAG_IS_SET(self->state_flags, WINDOW_IS_POINTER_HIDDEN);
}

static inline bool Window_needs_repaint(struct WindowBase* self)
{
    return self->paint;
}

static inline Pair_uint32_t Window_size(struct WindowBase* self)
{
    return (Pair_uint32_t){ .first = self->w, .second = self->h };
}

static inline Pair_uint32_t Window_position(struct WindowBase* self)
{
    return (Pair_uint32_t){ .first = self->x, .second = self->y };
}

static inline void Window_notify_content_change(struct WindowBase* self)
{
    self->paint = true;
}
