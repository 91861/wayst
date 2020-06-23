#pragma once


#include <stdbool.h>
#include <stdint.h>

#include "util.h"
#include "vt.h"

typedef struct
{
    struct IGfx* interface;

    __attribute__((aligned(8))) uint8_t extend_data;

} Gfx;



typedef struct 
{
    bool visible, dragging;

    enum AutoscrollDir
    {
        AUTOSCROLL_NONE = 0,
        AUTOSCROLL_UP   = 1,
        AUTOSCROLL_DN   = -1,

    } autoscroll;

    uint8_t   width;
    float     top;
    float     length;
    float     drag_position;
    TimePoint hide_time;
    TimePoint autoscroll_next_step;

} Scrollbar;

struct IGfx
{
    void (*draw)                        (Gfx* self, const Vt*, Scrollbar* scrollbar);
    void (*resize)                      (Gfx* self, uint32_t w, uint32_t h);
    Pair_uint32_t (*get_char_size)      (Gfx* self);
    void (*init_with_context_activated) (Gfx* self);
    void (*reload_font)                 (Gfx* self);
    bool (*update_timers)               (Gfx* self, Vt* vt, Scrollbar* scrollbar);
    void (*notify_action)               (Gfx* self);
    bool (*set_focus)                   (Gfx* self, bool in_focus);
    void (*flash)                       (Gfx* self);
    Pair_uint32_t (*pixels)             (Gfx* self, uint32_t rows, uint32_t columns);
    void (*destroy)                     (Gfx* self);
    void (*destroy_proxy)               (Gfx* self, int32_t proxy[static 4]);
};

static void Gfx_draw(Gfx* self, const Vt* vt, Scrollbar* scrollbar)
{
    self->interface->draw(self, vt, scrollbar);
}

static void Gfx_resize(Gfx* self, uint32_t w, uint32_t h)
{
    self->interface->resize(self, w, h);
}

static Pair_uint32_t Gfx_get_char_size(Gfx* self)
{
    return self->interface->get_char_size(self);
}

static void Gfx_init_with_context_activated(Gfx* self)
{
    self->interface->init_with_context_activated(self);
}

static void Gfx_reload_font(Gfx* self)
{
    self->interface->reload_font(self);
}

static bool Gfx_update_timers(Gfx* self, Vt* vt, Scrollbar* scrollbar)
{
    return self->interface->update_timers(self, vt, scrollbar);
}

static void Gfx_notify_action(Gfx* self)
{
    self->interface->notify_action(self);
}

static bool Gfx_set_focus(Gfx* self, bool in_focus)
{
    return self->interface->set_focus(self, in_focus);
}

static void Gfx_flash(Gfx* self)
{
    self->interface->flash(self);
}

static Pair_uint32_t Gfx_pixels(Gfx* self, uint32_t rows, uint32_t columns)
{
    return self->interface->pixels(self, rows, columns);
}

static void Gfx_destroy(Gfx* self)
{
    self->interface->destroy(self);
    free(self);
}

static void Gfx_destroy_proxy(Gfx* self,  int32_t proxy[static 4])
{
    self->interface->destroy_proxy(self, proxy);
}

