/* See LICENSE for license information. */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

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
    return self->tv_nsec / SEC_IN_NSECS + self->tv_sec;
}

static inline int64_t TimePoint_get_nsecs(TimePoint self)
{
    return self.tv_nsec + self.tv_sec * SEC_IN_NSECS;
}

static inline int64_t TimePoint_get_ms(TimePoint self)
{
    return (self.tv_nsec + self.tv_sec * SEC_IN_NSECS) / MS_IN_NSECS;
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

/**
 * Check if given time point was reached */
static inline bool TimePoint_passed(TimePoint t)
{
    return TimePoint_is_earlier(t, TimePoint_now());
}

/**
 * A pair of 'TimePoints', allows you to check the position of a time
 * point in relation to them. */
typedef struct _Timer
{
    TimePoint start, end;
} Timer;

static inline Timer Timer_new(TimePoint start, TimePoint end)
{
    return (Timer){ .start = start, .end = end };
}

static inline Timer Timer_from_now_to(TimePoint end)
{
    return (Timer){ .start = TimePoint_now(), .end = end };
}

static inline Timer Timer_from_now_to_ms_from_now(uint32_t ms_offset)
{
    return (Timer){ .start = TimePoint_now(), .end = TimePoint_ms_from_now(ms_offset) };
}

static inline float Timer_get_fraction_for(Timer* self, TimePoint point)
{
    TimePoint point_minus_start = point, end_minus_start = self->end;
    TimePoint_subtract(&point_minus_start, self->start);
    TimePoint_subtract(&end_minus_start, self->start);
    return (float)TimePoint_get_nsecs(point_minus_start) / TimePoint_get_nsecs(end_minus_start);
}

static inline float Timer_get_fraction_now(Timer* self)
{
    return Timer_get_fraction_for(self, TimePoint_now());
}

static inline float Timer_get_fraction_clamped_for(Timer* self, TimePoint point)
{
    float result = Timer_get_fraction_for(self, point);
    return result < 0.0f ? 0.0f : result > 1.0f ? 1.0f : result;
}

static inline float Timer_get_fraction_clamped_now(Timer* self)
{
    return Timer_get_fraction_clamped_for(self, TimePoint_now());
}
