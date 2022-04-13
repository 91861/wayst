/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "util.h"
#include "vector.h"

#define MS_IN_NSECS  1000000
#define SEC_IN_MS    1000
#define SEC_IN_NSECS 1000000000

typedef struct timespec TimePoint;

static inline TimePoint TimePoint_now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static inline void TimePoint_add(TimePoint* self, TimePoint offset)
{
    self->tv_sec += offset.tv_sec;
    self->tv_nsec += offset.tv_nsec;
    if (self->tv_nsec >= SEC_IN_NSECS) {
        self->tv_nsec -= SEC_IN_NSECS;
        ++self->tv_sec;
    }
}

static inline int64_t TimePoint_get_secs(TimePoint* self)
{
    return (int64_t)self->tv_nsec / SEC_IN_NSECS + self->tv_sec;
}

static inline int64_t TimePoint_get_min(TimePoint* self)
{
    return TimePoint_get_secs(self) / 60;
}
static inline int64_t TimePoint_get_hour(TimePoint* self)
{
    return TimePoint_get_min(self) / 60;
}

static inline int64_t TimePoint_get_nsecs(TimePoint self)
{
    return self.tv_nsec + self.tv_sec * SEC_IN_NSECS;
}

static inline int64_t TimePoint_get_ms(TimePoint self)
{
    return (((int64_t)self.tv_nsec) + ((int64_t)self.tv_sec) * SEC_IN_NSECS) / MS_IN_NSECS;
}

static inline void TimePoint_subtract(TimePoint* self, TimePoint other)
{
    self->tv_sec -= other.tv_sec;
    if (self->tv_nsec < other.tv_nsec) {
        --self->tv_sec;
        int64_t rest  = other.tv_nsec - self->tv_nsec;
        self->tv_nsec = SEC_IN_NSECS - rest;
    } else {
        self->tv_nsec -= other.tv_nsec;
    }
}

/**
 * Create a time point in the future */
static inline TimePoint TimePoint_ms_from_now(uint32_t ms_offset)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    TimePoint offset = { .tv_sec  = ms_offset / SEC_IN_MS,
                         .tv_nsec = (ms_offset % SEC_IN_MS) * MS_IN_NSECS };
    TimePoint_add(&t, offset);
    return t;
}

/**
 * Create a time point in the future */
static inline TimePoint TimePoint_s_from_now(uint32_t s_offset)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    TimePoint_add(&t, (TimePoint){ .tv_sec = s_offset, .tv_nsec = 0 });
    return t;
}

static inline int64_t TimePoint_is_nsecs_ahead(TimePoint t)
{
    TimePoint_subtract(&t, TimePoint_now());
    return TimePoint_get_nsecs(t);
}

static inline int64_t TimePoint_is_ms_ahead(TimePoint t)
{
    TimePoint_subtract(&t, TimePoint_now());
    return TimePoint_get_ms(t);
}

static inline bool TimePoint_is_earlier(TimePoint t, TimePoint other)
{
    return ((t.tv_sec < other.tv_sec) || (t.tv_sec == other.tv_sec && t.tv_nsec < other.tv_nsec));
}

#define TimePoint_is_later(t, other) (!TimePoint_is_earlier(t, other))

static inline TimePoint TimePoint_min(TimePoint a, TimePoint b)
{
    return TimePoint_is_earlier(a, b) ? a : b;
}

static inline TimePoint TimePoint_max(TimePoint a, TimePoint b)
{
    return TimePoint_is_earlier(a, b) ? b : a;
}

/**
 * Check if given time point was reached */
static inline bool TimePoint_passed(TimePoint t)
{
    return TimePoint_is_earlier(t, TimePoint_now());
}

/**
 * A pair of 'TimePoints', allows you to check the position of a time
 * point in relation to them. */
typedef struct
{
    TimePoint start, end;
} TimeSpan;

static inline TimeSpan TimeSpan_new(TimePoint start, TimePoint end)
{
    return (TimeSpan){ .start = start, .end = end };
}

static inline TimeSpan TimeSpan_from_now_to(TimePoint end)
{
    return (TimeSpan){ .start = TimePoint_now(), .end = end };
}

static inline TimeSpan TimeSpan_from_now_to_ms_from_now(uint32_t ms_offset)
{
    return (TimeSpan){ .start = TimePoint_now(), .end = TimePoint_ms_from_now(ms_offset) };
}

static inline bool TimeSpan_point_is_within(TimeSpan* self, TimePoint point)
{
    return TimePoint_is_later(point, self->start) && TimePoint_is_earlier(point, self->end);
}

static inline bool TimeSpan_is_active_now(TimeSpan* self)
{
    return TimeSpan_point_is_within(self, TimePoint_now());
}

static inline double TimeSpan_get_fraction_for(TimeSpan* self, TimePoint point)
{
    TimePoint point_minus_start = point, end_minus_start = self->end;
    TimePoint_subtract(&point_minus_start, self->start);
    TimePoint_subtract(&end_minus_start, self->start);
    return (double)TimePoint_get_nsecs(point_minus_start) /
           (double)TimePoint_get_nsecs(end_minus_start);
}

static inline float TimeSpan_get_fraction_now(TimeSpan* self)
{
    return TimeSpan_get_fraction_for(self, TimePoint_now());
}

static inline double TimeSpan_get_fraction_clamped_for(TimeSpan* self, TimePoint point)
{
    float result = TimeSpan_get_fraction_for(self, point);
    return result < 0.0 ? 0.0 : result > 1.0 ? 1.0 : result;
}

static inline double TimeSpan_get_fraction_clamped_now(TimeSpan* self)
{
    return TimeSpan_get_fraction_clamped_for(self, TimePoint_now());
}

static inline TimePoint TimeSpan_get_duration(const TimeSpan* self)
{
    TimePoint tmp = self->end;
    TimePoint_subtract(&tmp, self->start);
    return tmp;
}

/**
 * Caller should free() */
static char* TimeSpan_duration_string_approx(const TimeSpan* self)
{
    TimePoint tmp = TimeSpan_get_duration(self);
    int64_t   major, minor;
    char*     retval;
    if ((major = TimePoint_get_hour(&tmp)) > 1) {
        minor  = TimePoint_get_min(&tmp) - 60 * major;
        retval = asprintf("%luh %lumin", major, minor);
    } else if ((major = TimePoint_get_min(&tmp)) > 1) {
        minor  = TimePoint_get_secs(&tmp) - 60 * major;
        retval = asprintf("%lum %lus", major, minor);
    } else if ((major = TimePoint_get_secs(&tmp)) > 1) {
        minor  = TimePoint_get_ms(tmp) - 1000 * major;
        retval = asprintf("%lus %lums", major, minor);
    } else {
        retval = asprintf("%lums", TimePoint_get_ms(tmp));
    }
    return retval;
}

typedef uint8_t Timer;

typedef enum
{
    TIMER_TYPE_TWEEN,
    TIMER_TYPE_POINT,
} timer_type_e;

typedef enum
{
    TWEEN_INTERPOLATION_TYPE_LINEAR,
    TWEEN_INTERPOLATION_TYPE_LINEAR_INV,
    TWEEN_INTERPOLATION_TYPE_LINEAR_IN_OUT,
    TWEEN_INTERPOLATION_TYPE_SIN_IN,
    TWEEN_INTERPOLATION_TYPE_SIN_EASE,
    TWEEN_INTERPOLATION_TYPE_SIN_IN_OUT,
    TWEEN_INTERPOLATION_TYPE_POW2,
    TWEEN_INTERPOLATION_TYPE_POW2_INV,
    TWEEN_INTERPOLATION_TYPE_POW4,
    TWEEN_INTERPOLATION_TYPE_POW4_INV,
} tween_interpolation_type_e;

static double apply_tween_interpolation(double value, tween_interpolation_type_e type)
{
    switch (type) {
        case TWEEN_INTERPOLATION_TYPE_LINEAR:
            return value;

        case TWEEN_INTERPOLATION_TYPE_LINEAR_INV:
            return 1.0 - value;

        case TWEEN_INTERPOLATION_TYPE_LINEAR_IN_OUT:
            return value < 0.5 ? value * 2.0 : value * -2.0 + 2.0;

        case TWEEN_INTERPOLATION_TYPE_SIN_IN:
            return sin(value * M_PI_2);

        case TWEEN_INTERPOLATION_TYPE_SIN_EASE:
            return 1.0 - cos(value * M_PI);

        case TWEEN_INTERPOLATION_TYPE_SIN_IN_OUT:
            return 0.5 - cos(value * M_PI * 2.0) / 2.0;

        case TWEEN_INTERPOLATION_TYPE_POW2:
            return pow(value, 2.0);

        case TWEEN_INTERPOLATION_TYPE_POW2_INV:
            return pow(1.0 - value, 2.0);

        case TWEEN_INTERPOLATION_TYPE_POW4:
            return pow(value, 4.0);

        case TWEEN_INTERPOLATION_TYPE_POW4_INV:
            return pow(1.0 - value, 4.0);
    }

    ASSERT_UNREACHABLE;
}

typedef void (*point_timer_completed_callback_func_t)(void*);
typedef void (*tween_timer_updated_callback_func_t)(void*, double fraction, bool completed);

typedef struct
{
    union
    {
        struct tween_timer_data
        {
            tween_timer_updated_callback_func_t updated_callback;
            TimeSpan                            time_span;
            tween_interpolation_type_e          interpolation;
        } tween_data;

        struct point_timer_data
        {
            point_timer_completed_callback_func_t completed_callback;
            TimePoint                             trigger_time;
        } point_data;
    } data;

    timer_type_e type : 2;
    bool         completed : 1;
} timer_data_t;

DEF_VECTOR(timer_data_t, NULL);

typedef struct
{
    Vector_timer_data_t timers;
    void*               user_data;
} TimerManager;

static TimerManager TimerManager_new(void* user_data)
{
    return (TimerManager){
        .timers    = Vector_new_with_capacity_timer_data_t(8),
        .user_data = user_data,
    };
}

static Timer TimerManager_create_timer(TimerManager* self, timer_type_e type, void* callback)
{
    timer_data_t tmr = {
        .type      = type,
        .completed = true,
    };

    switch (type) {
        case TIMER_TYPE_POINT:
            tmr.data.point_data = (struct point_timer_data){
                .trigger_time       = { 0, 0 },
                .completed_callback = callback,
            };
            break;
        case TIMER_TYPE_TWEEN:
            tmr.data.tween_data = (struct tween_timer_data){
                .time_span = {
                    .start = {0,0},
                    .end = {0,0},
                },
                .updated_callback = callback,
                .interpolation = TWEEN_INTERPOLATION_TYPE_LINEAR,
            };
            break;
    }

    Vector_push_timer_data_t(&self->timers, tmr);
    return self->timers.size - 1;
}

static inline void TimerManager_set_interpolation_func(TimerManager*              self,
                                                       Timer                      timer,
                                                       tween_interpolation_type_e interpolation)
{
    ASSERT(self->timers.size > timer, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, timer);
    ASSERT(tmr && tmr->type == TIMER_TYPE_TWEEN, "is tween type");
    tmr->data.tween_data.interpolation = interpolation;
}

static bool TimerManager_is_pending(TimerManager* self, Timer timer)
{
    ASSERT(self->timers.size > timer, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, timer);
    return !tmr->completed;
}

static void TimerManager_cancel(TimerManager* self, Timer timer)
{
    ASSERT(self->timers.size > timer, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, timer);
    tmr->completed    = true;
}

static bool TimerManager_is_tween_tmrating(TimerManager* self, Timer animation)
{
    ASSERT(self->timers.size > animation, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, animation);
    ASSERT(tmr && tmr->type == TIMER_TYPE_TWEEN, "is tween type");
    return tmr->completed ? false : TimeSpan_is_active_now(&tmr->data.tween_data.time_span);
}

static double _TimerManager_get_tween_fraction_for_data(TimerManager* self, timer_data_t* data)
{
    return apply_tween_interpolation(
      TimeSpan_get_fraction_clamped_now(&data->data.tween_data.time_span),
      data->data.tween_data.interpolation);
}

static double TimerManager_get_tween_fraction(TimerManager* self, Timer timer)
{
    ASSERT(self->timers.size > timer, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, timer);
    ASSERT(tmr && tmr->type == TIMER_TYPE_TWEEN, "is tween type");
    return _TimerManager_get_tween_fraction_for_data(self, tmr);
}

static void TimerManager_schedule_point(TimerManager* self, Timer timer, TimePoint time)
{
    ASSERT(self->timers.size > timer, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, timer);
    ASSERT(tmr && tmr->type == TIMER_TYPE_POINT, "is point type");

    tmr->data.point_data.trigger_time = time;
    tmr->completed                    = false;
}

static void TimerManager_schedule_tween(TimerManager* self,
                                        Timer         timer,
                                        TimePoint     begin_time,
                                        TimePoint     end_time)
{
    ASSERT(self->timers.size > timer, "exists");
    timer_data_t* tmr = Vector_at_timer_data_t(&self->timers, timer);
    ASSERT(tmr && tmr->type == TIMER_TYPE_TWEEN, "is tween type");

    tmr->data.tween_data.time_span = (TimeSpan){ begin_time, end_time };
    tmr->completed                 = false;
}

static inline void TimerManager_schedule_tween_from_now(TimerManager* self,
                                                        Timer         timer,
                                                        TimePoint     end_time)
{
    TimerManager_schedule_tween(self, timer, TimePoint_now(), end_time);
}

static inline void TimerManager_schedule_tween_to_ms(TimerManager* self,
                                                     Timer         timer,
                                                     int32_t       offset_ms)
{
    TimerManager_schedule_tween_from_now(self, timer, TimePoint_ms_from_now(offset_ms));
}

static inline void TimerManager_update(TimerManager* self)
{
    for (timer_data_t* i = NULL; (i = Vector_iter_timer_data_t(&self->timers, i));) {

        if (i->completed) {
            continue;
        }

        switch (i->type) {
            case TIMER_TYPE_POINT: {
                if (TimePoint_passed(i->data.point_data.trigger_time)) {
                    i->completed = true;
                    i->data.point_data.completed_callback(self->user_data);
                }
            } break;

            case TIMER_TYPE_TWEEN: {
                if (TimeSpan_is_active_now(&i->data.tween_data.time_span)) {
                    i->data.tween_data.updated_callback(
                      self->user_data,
                      _TimerManager_get_tween_fraction_for_data(self, i),
                      false);
                } else if (TimePoint_passed(i->data.tween_data.time_span.end)) {
                    i->completed = true;
                    i->data.tween_data.updated_callback(self->user_data, 1.0, true);
                }
            } break;
        }
    }
}

typedef struct
{
    TimePoint* payload;
} time_point_ptr_t;

#define TIME_POINT_PTR(tp) (&(time_point_ptr_t){ .payload = tp })

#define TIMER_MANAGER_NO_ACTION_PENDING INT64_MIN

__attribute__((sentinel)) static int64_t
TimerManager_get_next_action_ms(TimerManager* self, time_point_ptr_t* external_frame, ...)
{
    int64_t   ret              = TIMER_MANAGER_NO_ACTION_PENDING;
    TimePoint next_frame_point = { 0, 0 };
    bool      has_next_frame   = false;

    TimePoint now = TimePoint_now();
    for (timer_data_t* i = NULL; (i = Vector_iter_timer_data_t(&self->timers, i));) {
        if (!ret) {
            return 0;
        }

        if (i->completed) {
            continue;
        }

        switch (i->type) {
            case TIMER_TYPE_POINT: {
                next_frame_point = !has_next_frame ? i->data.point_data.trigger_time
                                                   : TimePoint_min(i->data.point_data.trigger_time,
                                                                   next_frame_point);
                has_next_frame   = true;
            } break;

            case TIMER_TYPE_TWEEN: {
                if (TimeSpan_point_is_within(&i->data.tween_data.time_span, now)) {
                    has_next_frame   = true;
                    ret              = 0;
                    next_frame_point = now;
                } else {
                    next_frame_point =
                      !has_next_frame
                        ? i->data.tween_data.time_span.start
                        : TimePoint_min(i->data.tween_data.time_span.start, next_frame_point);
                    has_next_frame = true;
                }
            } break;
        }
    }

    va_list           ap;
    time_point_ptr_t* tw = external_frame;
    va_start(ap, external_frame);
    if (tw && tw->payload) {
        do {
            if (tw && tw->payload) {
                if (has_next_frame && TimePoint_is_earlier(*tw->payload, next_frame_point)) {
                    next_frame_point = *tw->payload;
                } else {
                    next_frame_point = *tw->payload;
                    has_next_frame   = true;
                }
            }
        } while ((tw = va_arg(ap, time_point_ptr_t*)));
    }
    va_end(ap);

    if (!has_next_frame) {
        return TIMER_MANAGER_NO_ACTION_PENDING;
    } else {
        int64_t nfp = TimePoint_is_ms_ahead(next_frame_point);
        return MAX(0, nfp);
    }
}

static void TimerManager_destroy(TimerManager* self)
{
    Vector_destroy_timer_data_t(&self->timers);
}
