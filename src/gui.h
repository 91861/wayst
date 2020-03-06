/* See LICENSE for license information. */


#pragma once

#include "settings.h"
#include "util.h"


#define WINDOW_CLOSED         (1 << 0)
#define WINDOW_FULLSCREEN     (1 << 1)
#define WINDOW_NEEDS_SWAP     (1 << 2)
#define WINDOW_IN_FOCUS       (1 << 3)
#define WINDOW_MAXIMIZED      (1 << 4)
#define WINDOW_POINTER_HIDDEN (1 << 5)


#define MOUSE_BUTTON_RELEASE (1 << 0)
#define MOUSE_BUTTON_1       (1 << 1)
#define MOUSE_BUTTON_2       (1 << 2)
#define MOUSE_BUTTON_3       (1 << 3)


#define MODIFIER_SHIFT   (1 << 0)
#define MODIFIER_ALT     (1 << 1)
#define MODIFIER_CONTROL (1 << 2)



typedef struct
{
    uint32_t target_frame_time_ms;

    __attribute__((aligned(8)))
    uint8_t subclass_data;

} GuiGlobal;


struct WindowBase;


struct IWindowSubclass
{
    void  (*set_fullscreen)(struct WindowBase* self, bool fullscreen);
    void  (*resize)(struct WindowBase* self, uint32_t w, uint32_t h);
    void  (*events)(struct WindowBase* self);
    void  (*set_title)(struct WindowBase* self, const char* title);
    void  (*set_app_id)(struct WindowBase* self, const char* app_id);
    void  (*maybe_swap)(struct WindowBase* self);
    void  (*destroy)(struct WindowBase* self);
    int   (*get_connection_fd)(struct WindowBase* self);
    void  (*clipboard_send)(struct WindowBase* self, const char* text);
    void  (*clipboard_get)(struct WindowBase* self);
    void  (*set_swap_interval)(struct WindowBase* self, int val);
    void* (*get_gl_ext_proc_adress)(struct WindowBase* self, const char* name);
};


typedef struct WindowBase
{
    int32_t w, h, x, y;

    int32_t pointer_x, pointer_y;

    uint16_t state_flags;

    bool paint;

    int32_t repeat_count;

    void (*key_handler)(void* user_data, uint32_t code, uint32_t mods);
    
    void (*button_handler)(void* user_data,
                           uint32_t code,
                           bool state,
                           int32_t x,
                           int32_t y,
                           int32_t ammount,
                           uint32_t mods);
    
    void (*motion_handler)(void* user_data,
                           uint32_t code,
                           int32_t x,
                           int32_t y);

    void (*clipboard_handler)(void* user_data,
                              const char* text);

    void* user_data;

    char* title;

    struct IWindowSubclass* subclass_interface;

    __attribute__((aligned(8)))
    uint8_t subclass_data;

} Window_;


static void
Window_update_title(void* self, const char* title)
{
    if (settings.dynamic_title) {
        char* tmp = asprintf(settings.title_format, settings.title, title);
        ((struct WindowBase*)self)->subclass_interface->set_title(self, tmp);
        free(tmp);
    }
}


/* Froward interface functions */
__attribute__((always_inline))
static inline void*
Window_get_proc_adress(struct WindowBase* self, const char* procname)
{
    return self->subclass_interface->get_gl_ext_proc_adress(self, procname);
}


__attribute__((always_inline))
static inline void
Window_set_fullscreen(struct WindowBase* self, bool fullscreen)
{
    self->subclass_interface->set_fullscreen(self, fullscreen);
}

__attribute__((always_inline))
static inline void
Window_set_swap_interval(struct WindowBase* self, bool value)
{
    self->subclass_interface->set_swap_interval(self, value);
}


__attribute__((always_inline))
static inline void
Window_resize(struct WindowBase* self, uint32_t w, uint32_t h)
{
    self->subclass_interface->resize(self, w, h);
}


__attribute__((always_inline))
static inline void
Window_events(struct WindowBase* self)
{
    self->subclass_interface->events(self);
}


__attribute__((always_inline))
static inline void
Window_set_title(struct WindowBase* self, const char* title)
{
    self->subclass_interface->set_title(self, title);
}


__attribute__((always_inline))
static inline void
Window_set_app_id(struct WindowBase* self, const char* app_id)
{
    self->subclass_interface->set_app_id(self, app_id);
}


__attribute__((always_inline))
static inline void
Window_maybe_swap(struct WindowBase* self)
{
    self->subclass_interface->maybe_swap(self);
}


__attribute__((always_inline))
static inline void
Window_destroy(struct WindowBase* self)
{
    self->subclass_interface->destroy(self);
}


__attribute__((always_inline))
static inline int
get_connection_fd(struct WindowBase* self)
{
    return self->subclass_interface->get_connection_fd(self);
}


/* Trivial base functions */
__attribute__((always_inline))
static inline void*
Window_subclass_data_ptr(struct WindowBase* self)
{
    return &self->subclass_data;
}

__attribute__((always_inline))
static inline int
Window_get_connection_fd(struct WindowBase* self)
{
    return self->subclass_interface->get_connection_fd(self);
}

__attribute__((always_inline))
static inline void
Window_set_user_data(struct WindowBase* self, void* user_data)
{
    self->user_data = user_data;
}

__attribute__((always_inline))
static inline void
Window_set_key_handler(struct WindowBase* self,
                       void (*handler)(void*, uint32_t, uint32_t))
{
    self->key_handler = handler;
}

__attribute__((always_inline))
static inline bool
Window_closed(struct WindowBase* self)
{
    return FLAG_IS_SET(self->state_flags, WINDOW_CLOSED);
}

__attribute__((always_inline))
static inline bool
Window_needs_repaint(struct WindowBase* self)
{
    return self->paint;
}

__attribute__((always_inline))
static inline Pair_uint32_t
Window_size(struct WindowBase* self)
{
    return (Pair_uint32_t){ .first = self->w, .second = self->h };
}


__attribute__((always_inline))
static inline Pair_uint32_t
Window_size2(void* self)
{
    return Window_size(self);
}


__attribute__((always_inline))
static inline Pair_uint32_t
Window_position(struct WindowBase* self)
{
    return (Pair_uint32_t){ .first = self->x, .second = self->y };
}


__attribute__((always_inline))
static inline Pair_uint32_t
Window_position2(void* self)
{
    return Window_position(self);
}


__attribute__((always_inline))
static inline void
Window_notify_content_change(struct WindowBase* self)
{
    self->paint = true;
}


__attribute__((always_inline))
static inline void
Window_notify_content_change2(void* self)
{
    Window_notify_content_change((struct WindowBase*)self);
}
