/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>

/**
 * Generic dynamic array with iterators
 *
 *  Examples:
 *
 *      Defining:
 *
 *      DEF_VECTOR(Thing, Thing_destroy);
 *      DEF_VECTOR(int32_t, NULL);
 *
 *      Iterators:
 *
 *      for (Thing* i = NULL; (i = Vector_iter_Thing(&vector_of_things, i));)
 *          do_something_with_a_thing(i);
 *
 *  note: iterators are expected to be valid
 *
 * */
#define DEF_VECTOR(t, dtor)                                                    \
    typedef struct                                                             \
    {                                                                          \
        size_t cap, size;                                                      \
        t*     buf;                                                            \
    } Vector_##t;                                                              \
                                                                               \
    static inline Vector_##t Vector_new_##t()                                  \
    {                                                                          \
        return (Vector_##t){ 4, 0, malloc(sizeof(t) << 2) };                   \
    }                                                                          \
                                                                               \
    static inline Vector_##t Vector_new_with_capacity_##t(size_t init_cap)     \
    {                                                                          \
        return (Vector_##t){ init_cap, 0, malloc(sizeof(t) * init_cap) };      \
    }                                                                          \
                                                                               \
    static void Vector_push_##t(Vector_##t* self, t arg)                       \
    {                                                                          \
        if (unlikely(self->cap == self->size)) {                               \
            self->buf = realloc(self->buf, (self->cap <<= 1) * sizeof(t));     \
            ASSERT(self->buf, "Vector not initialized");                       \
        }                                                                      \
        self->buf[self->size++] = arg;                                         \
    }                                                                          \
                                                                               \
    static inline void Vector_remove_at_##t(Vector_##t* self, size_t idx,      \
                                            size_t n)                          \
    {                                                                          \
        if (dtor)                                                              \
            for (size_t i = idx; i < n + idx; ++i)                             \
                ((void (*)(t*))dtor)(self->buf + i);                           \
        memmove(self->buf + idx, self->buf + idx + n,                          \
                sizeof(t) * ((self->size -= n) - idx));                        \
    }                                                                          \
                                                                               \
    static inline void Vector_pushv_##t(Vector_##t* self, const t* const argv, \
                                        size_t n)                              \
    {                                                                          \
        for (size_t i = 0; i < n; ++i)                                         \
            Vector_push_##t(self, argv[i]);                                    \
    }                                                                          \
                                                                               \
    static inline void Vector_pop_n_##t(Vector_##t* self, size_t n)            \
    {                                                                          \
        if (unlikely(!self->size))                                             \
            return;                                                            \
        if (dtor)                                                              \
            for (size_t i = 0; i < n && self->size > 0; ++i)                   \
                ((void (*)(t*))dtor)(self->buf + --self->size);                \
        else                                                                   \
            self->size = n > self->size ? 0 : self->size - n;                  \
    }                                                                          \
                                                                               \
    static inline void Vector_pop_##t(Vector_##t* self)                        \
    {                                                                          \
        if (unlikely(!self->size))                                             \
            return;                                                            \
        if (dtor)                                                              \
            ((void (*)(t*))dtor)(self->buf + --self->size);                    \
        else                                                                   \
            --self->size;                                                      \
    }                                                                          \
                                                                               \
    static inline void Vector_destroy_##t(Vector_##t* self)                    \
    {                                                                          \
        if (dtor) {                                                            \
            for (size_t i = 0; i < self->size; ++i)                            \
                ((void (*)(t*))dtor)(&self->buf[i]);                           \
        }                                                                      \
        free(self->buf);                                                       \
    }                                                                          \
                                                                               \
    static inline size_t Vector_index_##t(Vector_##t* self, t* i)              \
    {                                                                          \
        ASSERT(i >= self->buf && (size_t)(i - self->buf) <= self->size,        \
               "Vector iterator out of range");                                \
        return i - self->buf;                                                  \
    }                                                                          \
                                                                               \
    static inline t* Vector_at_##t(Vector_##t* self, size_t idx)               \
    {                                                                          \
        ASSERT(idx <= self->size, "Vector index out of range");                \
        return self->buf + idx;                                                \
    }                                                                          \
                                                                               \
    static inline t* Vector_iter_##t(Vector_##t* self, t* i)                   \
    {                                                                          \
        return unlikely(!self->size)                                           \
                 ? NULL                                                        \
                 : !i ? self->buf                                              \
                      : (size_t)(i - self->buf) + 1 < self->size ? i + 1       \
                                                                 : NULL;       \
    }                                                                          \
                                                                               \
    static inline t* Vector_iter_back_##t(Vector_##t* self, t* i)              \
    {                                                                          \
        return unlikely(!self->size) ? NULL                                    \
                                     : !i ? &self->buf[self->size - 1]         \
                                          : (i == self->buf) ? NULL : i - 1;   \
    }                                                                          \
                                                                               \
    static inline t* Vector_insert_##t(Vector_##t* self, t* i, t arg)          \
    {                                                                          \
        if (unlikely(!self->size)) {                                           \
            ASSERT(i == self->buf, "Vector iterator out of range");            \
            Vector_push_##t(self, arg);                                        \
            return i + 1;                                                      \
        } else if (unlikely(self->cap == self->size)) {                        \
            size_t idx = i - self->buf;                                        \
            self->buf  = realloc(self->buf, (self->cap <<= 1) * sizeof(t));    \
            memmove(self->buf + idx + 1, self->buf + idx,                      \
                    (self->size++ - idx) * sizeof(t));                         \
            self->buf[idx] = arg;                                              \
            return self->buf + idx;                                            \
        } else {                                                               \
            memmove(i + 1, i, (self->size++ - (i - self->buf)) * sizeof(t));   \
            *i = arg;                                                          \
            return i;                                                          \
        }                                                                      \
    }
