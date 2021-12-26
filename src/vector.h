/* See LICENSE for license information. */

#pragma once

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdint.h>

/**
 * Templated dynamic array with iterators
 *
 *  Examples:
 *
 *    Defining:
 *      void Thing_destroy(Thing*);
 *      DEF_VECTOR(Thing, Thing_destroy)
 *      DEF_VECTOR(int32_t, NULL)
 *
 *    Iterators:
 *      for (Thing* i = NULL; (i = Vector_iter_Thing(&vector_of_things, i));)
 *          do_something_with_a_thing(i);
 *
 *    Binding destructor arguments:
 *      void ThingContext_destroy_Thing(ThingContext*, Thing*);
 *      DEF_VECTOR_DA(Thing, ThingContext_destroy_Thing, ThingContext)
 *      Vector_Thing vector_of_things = Vector_new_Thing(&local_thing_context);
 *      Vector_push_Thing(&vector_of_things, local_thing_context.create_thing());
 *
 *  note: iterators are expected to be valid
 *
 * */

#define DEF_VECTOR(t, dtor, ...)                                                                   \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Waddress\"");                                               \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-function\"");                                       \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"");                                      \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        size_t cap, size;                                                                          \
        t*     buf;                                                                                \
    } Vector_##t;                                                                                  \
                                                                                                   \
    static inline Vector_##t Vector_new_##t()                                                      \
    {                                                                                              \
        return (Vector_##t){ .cap = 4, .size = 0, .buf = malloc(sizeof(t) << 2) };                 \
    }                                                                                              \
                                                                                                   \
    static inline Vector_##t Vector_new_with_capacity_##t(size_t init_cap)                         \
    {                                                                                              \
        return (Vector_##t){ .cap = init_cap, .size = 0, .buf = malloc(sizeof(t) * init_cap) };    \
    }                                                                                              \
                                                                                                   \
    static void Vector_push_##t(Vector_##t* self, t arg)                                           \
    {                                                                                              \
        ASSERT(self->buf, "Vector not initialized");                                               \
        if (unlikely(self->cap == self->size)) {                                                   \
            self->buf = realloc(self->buf, (self->cap <<= 1) * sizeof(t));                         \
        }                                                                                          \
        self->buf[self->size++] = arg;                                                             \
    }                                                                                              \
                                                                                                   \
    static void Vector_reserve_##t(Vector_##t* self, size_t cnt)                                   \
    {                                                                                              \
        ASSERT(self->buf, "Vector not initialized");                                               \
        if (likely(cnt > self->cap))                                                               \
            self->buf = realloc(self->buf, (self->cap = cnt) * sizeof(t));                         \
    }                                                                                              \
                                                                                                   \
    static void Vector_reserve_extra_##t(Vector_##t* self, size_t cnt)                             \
    {                                                                                              \
        ASSERT(self->buf, "Vector not initialized");                                               \
        if (cnt + self->size > self->cap)                                                          \
            self->buf = realloc(self->buf, (self->cap = cnt + self->size) * sizeof(t));            \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_remove_at_##t(Vector_##t* self, size_t idx, size_t n)                \
    {                                                                                              \
        if (dtor)                                                                                  \
            for (size_t i = idx; i < n + idx; ++i)                                                 \
                ((void (*)(t*))dtor)(self->buf + i);                                               \
        memmove(self->buf + idx, self->buf + idx + n, sizeof(t) * ((self->size -= n) - idx));      \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_pushv_##t(Vector_##t* self, const t* const argv, size_t n)           \
    {                                                                                              \
        if (unlikely(!n))                                                                          \
            return;                                                                                \
        if (unlikely(self->cap < self->size + n))                                                  \
            self->buf = realloc(self->buf, (self->cap = self->size + n) * sizeof(t));              \
        memcpy(self->buf + self->size, argv, n * sizeof(t));                                       \
        self->size += n;                                                                           \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_pop_n_##t(Vector_##t* self, size_t n)                                \
    {                                                                                              \
        if (unlikely(!self->size))                                                                 \
            return;                                                                                \
        if (dtor)                                                                                  \
            for (size_t i = 0; i < n && self->size > 0; ++i)                                       \
                ((void (*)(t*))dtor)(self->buf + --self->size);                                    \
        else                                                                                       \
            self->size = n > self->size ? 0 : self->size - n;                                      \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_pop_##t(Vector_##t* self)                                            \
    {                                                                                              \
        if (unlikely(!self->size))                                                                 \
            return;                                                                                \
        if (dtor)                                                                                  \
            ((void (*)(t*))dtor)(self->buf + --self->size);                                        \
        else                                                                                       \
            --self->size;                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_destroy_##t(Vector_##t* self)                                        \
    {                                                                                              \
        if (dtor) {                                                                                \
            for (size_t i = 0; i < self->size; ++i)                                                \
                ((void (*)(t*))dtor)(&self->buf[i]);                                               \
        }                                                                                          \
        free(self->buf);                                                                           \
        self->buf = NULL;                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline size_t Vector_index_##t(const Vector_##t* self, t* i)                            \
    {                                                                                              \
        ASSERT(i >= self->buf && (size_t)(i - self->buf) <= self->size,                            \
               "Vector iterator out of range");                                                    \
        return i - self->buf;                                                                      \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_at_##t(Vector_##t* self, size_t idx)                                   \
    {                                                                                              \
        ASSERT(idx <= self->size, "Vector index out of range");                                    \
        return self->buf + idx;                                                                    \
    }                                                                                              \
                                                                                                   \
    static inline const t* Vector_iter_const_##t(const Vector_##t* self, const t* i)               \
    {                                                                                              \
        return unlikely(!self->size)                      ? NULL                                   \
               : !i                                       ? self->buf                              \
               : (size_t)(i - self->buf) + 1 < self->size ? i + 1                                  \
                                                          : NULL;                                  \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_iter_##t(Vector_##t* self, t* i)                                       \
    {                                                                                              \
        return unlikely(!self->size)                      ? NULL                                   \
               : !i                                       ? self->buf                              \
               : (size_t)(i - self->buf) + 1 < self->size ? i + 1                                  \
                                                          : NULL;                                  \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_iter_back_##t(Vector_##t* self, t* i)                                  \
    {                                                                                              \
        return unlikely(!self->size) ? NULL                                                        \
               : !i                  ? &self->buf[self->size - 1]                                  \
               : (i == self->buf)    ? NULL                                                        \
                                     : i - 1;                                                         \
    }                                                                                              \
                                                                                                   \
    static inline const t* Vector_iter_back_const_##t(const Vector_##t* self, const t* i)          \
    {                                                                                              \
        return unlikely(!self->size) ? NULL                                                        \
               : !i                  ? &self->buf[self->size - 1]                                  \
               : (i == self->buf)    ? NULL                                                        \
                                     : i - 1;                                                         \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_insert_##t(Vector_##t* self, t* i, t arg)                              \
    {                                                                                              \
        if (unlikely(!self->size)) {                                                               \
            ASSERT(i == self->buf, "Vector iterator out of range");                                \
            Vector_push_##t(self, arg);                                                            \
            return i + 1;                                                                          \
        } else if (unlikely(self->cap == self->size)) {                                            \
            size_t idx = i - self->buf;                                                            \
            self->buf  = realloc(self->buf, (self->cap <<= 1) * sizeof(t));                        \
            memmove(self->buf + idx + 1, self->buf + idx, (self->size++ - idx) * sizeof(t));       \
            self->buf[idx] = arg;                                                                  \
            return self->buf + idx;                                                                \
        } else {                                                                                   \
            memmove(i + 1, i, (self->size++ - (i - self->buf)) * sizeof(t));                       \
            *i = arg;                                                                              \
            return i;                                                                              \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_insertv_##t(Vector_##t* self, t* i, const t* const argv, size_t n)     \
    {                                                                                              \
        if (unlikely(!self->size)) {                                                               \
            ASSERT(i == self->buf, "Vector iterator out of range");                                \
            Vector_pushv_##t(self, argv, n);                                                       \
            return i + n;                                                                          \
        } else if (unlikely(self->cap + n > self->size)) {                                         \
            size_t idx = i - self->buf;                                                            \
            self->buf  = realloc(self->buf, (self->cap += n) * sizeof(t));                         \
            memmove(self->buf + idx + n, self->buf + idx, ((self->size += n) - idx) * sizeof(t));  \
            memcpy(self->buf + idx, argv, sizeof(t) * n);                                          \
            return self->buf + idx;                                                                \
        } else {                                                                                   \
            memmove(i + n, i, ((self->size += n) - (i - self->buf)) * sizeof(t));                  \
            memcpy(i, argv, sizeof(t) * n);                                                        \
            return i + n;                                                                          \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_insert_at_##t(Vector_##t* self, size_t i, t arg)                     \
    {                                                                                              \
        ASSERT(i <= self->size, "Vector index out of range");                                      \
        if (unlikely(!self->size)) {                                                               \
            Vector_push_##t(self, arg);                                                            \
        } else if (unlikely(self->cap == self->size)) {                                            \
            self->buf = realloc(self->buf, (self->cap <<= 1) * sizeof(t));                         \
            memmove(self->buf + i + 1, self->buf + i, (self->size++ - i) * sizeof(t));             \
            self->buf[i] = arg;                                                                    \
        } else {                                                                                   \
            memmove(self->buf + i + 1, self->buf + i, (self->size++ - i) * sizeof(t));             \
            self->buf[i] = arg;                                                                    \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_insert_front_##t(Vector_##t* self, t arg)                            \
    {                                                                                              \
        Vector_insert_##t(self, self->buf, arg);                                                   \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_insertv_front_##t(Vector_##t* self, t* argv, size_t n)               \
    {                                                                                              \
        Vector_insertv_##t(self, self->buf, argv, n);                                              \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_last_##t(Vector_##t* self)                                             \
    {                                                                                              \
        return !self->size ? NULL : self->buf + (self->size - 1);                                  \
    }                                                                                              \
                                                                                                   \
    static inline const t* Vector_last_const_##t(const Vector_##t* self)                           \
    {                                                                                              \
        return !self->size ? NULL : self->buf + (self->size - 1);                                  \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_first_##t(Vector_##t* self) { return self->buf; }                      \
                                                                                                   \
    static inline const t* Vector_first_const_##t(const Vector_##t* self) { return self->buf; }    \
                                                                                                   \
    static inline void Vector_clear_##t(Vector_##t* self)                                          \
    {                                                                                              \
        if (dtor) {                                                                                \
            for (size_t i = 0; i < self->size; ++i)                                                \
                ((void (*)(t*))dtor)(&self->buf[i]);                                               \
        }                                                                                          \
        self->size = 0;                                                                            \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_force_resize_##t(Vector_##t* self, size_t new_size)                  \
    {                                                                                              \
        ASSERT(self->cap >= new_size, "sufficient capacity");                                      \
        self->size = new_size;                                                                     \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_shrink_##t(Vector_##t* self)                                         \
    {                                                                                              \
        self->buf = realloc(self->buf, (self->cap = self->size) * sizeof(t));                      \
    }                                                                                              \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");

#define DEF_VECTOR_DA(t, dtor, dtorctx_t)                                                          \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Waddress\"");                                               \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-function\"");                                       \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"");                                      \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        size_t cap, size;                                                                          \
        t*     buf;                                                                                \
        void*  dtor_arg;                                                                           \
    } Vector_##t;                                                                                  \
                                                                                                   \
    static inline Vector_##t Vector_new_##t(dtorctx_t* destroy_arg)                                \
    {                                                                                              \
        return (Vector_##t){ .cap      = 4,                                                        \
                             .size     = 0,                                                        \
                             .buf      = malloc(sizeof(t) << 2),                                   \
                             .dtor_arg = destroy_arg };                                            \
    }                                                                                              \
                                                                                                   \
    static inline Vector_##t Vector_new_with_capacity_##t(size_t init_cap, dtorctx_t* destroy_arg) \
    {                                                                                              \
        return (Vector_##t){ .cap      = init_cap,                                                 \
                             .size     = 0,                                                        \
                             .buf      = malloc(sizeof(t) * init_cap),                             \
                             .dtor_arg = destroy_arg };                                            \
    }                                                                                              \
                                                                                                   \
    static void Vector_push_##t(Vector_##t* self, t arg)                                           \
    {                                                                                              \
        ASSERT(self->buf, "Vector not initialized");                                               \
        if (unlikely(self->cap == self->size)) {                                                   \
            self->buf = realloc(self->buf, (self->cap <<= 1) * sizeof(t));                         \
        }                                                                                          \
        self->buf[self->size++] = arg;                                                             \
    }                                                                                              \
                                                                                                   \
    static void Vector_reserve_##t(Vector_##t* self, size_t cnt)                                   \
    {                                                                                              \
        ASSERT(self->buf, "Vector not initialized");                                               \
        if (likely(cnt > self->cap))                                                               \
            self->buf = realloc(self->buf, (self->cap = cnt) * sizeof(t));                         \
    }                                                                                              \
                                                                                                   \
    static void Vector_reserve_extra_##t(Vector_##t* self, size_t cnt)                             \
    {                                                                                              \
        ASSERT(self->buf, "Vector not initialized");                                               \
        if (cnt + self->size > self->cap)                                                          \
            self->buf = realloc(self->buf, (self->cap = cnt + self->size) * sizeof(t));            \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_remove_at_##t(Vector_##t* self, size_t idx, size_t n)                \
    {                                                                                              \
        if (dtor)                                                                                  \
            for (size_t i = idx; i < n + idx; ++i)                                                 \
                ((void (*)(dtorctx_t*, t*))dtor)(self->dtor_arg, self->buf + i);                   \
        memmove(self->buf + idx, self->buf + idx + n, sizeof(t) * ((self->size -= n) - idx));      \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_pushv_##t(Vector_##t* self, const t* const argv, size_t n)           \
    {                                                                                              \
        if (unlikely(!n))                                                                          \
            return;                                                                                \
        if (unlikely(self->cap < self->size + n))                                                  \
            self->buf = realloc(self->buf, (self->cap = self->size + n) * sizeof(t));              \
        memcpy(self->buf + self->size, argv, n * sizeof(t));                                       \
        self->size += n;                                                                           \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_pop_n_##t(Vector_##t* self, size_t n)                                \
    {                                                                                              \
        if (unlikely(!self->size))                                                                 \
            return;                                                                                \
        if (dtor)                                                                                  \
            for (size_t i = 0; i < n && self->size > 0; ++i)                                       \
                ((void (*)(dtorctx_t*, t*))dtor)(self->dtor_arg, self->buf + --self->size);        \
        else                                                                                       \
            self->size = n > self->size ? 0 : self->size - n;                                      \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_pop_##t(Vector_##t* self)                                            \
    {                                                                                              \
        if (unlikely(!self->size))                                                                 \
            return;                                                                                \
        if (dtor)                                                                                  \
            ((void (*)(dtorctx_t*, t*))dtor)(self->dtor_arg, self->buf + --self->size);            \
        else                                                                                       \
            --self->size;                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_destroy_##t(Vector_##t* self)                                        \
    {                                                                                              \
        if (dtor) {                                                                                \
            for (size_t i = 0; i < self->size; ++i)                                                \
                ((void (*)(dtorctx_t*, t*))dtor)(self->dtor_arg, &self->buf[i]);                   \
        }                                                                                          \
        free(self->buf);                                                                           \
        self->buf = NULL;                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline size_t Vector_index_##t(const Vector_##t* self, t* i)                            \
    {                                                                                              \
        ASSERT(i >= self->buf && (size_t)(i - self->buf) <= self->size,                            \
               "Vector iterator out of range");                                                    \
        return i - self->buf;                                                                      \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_at_##t(Vector_##t* self, size_t idx)                                   \
    {                                                                                              \
        ASSERT(idx <= self->size, "Vector index out of range");                                    \
        return self->buf + idx;                                                                    \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_iter_##t(Vector_##t* self, t* i)                                       \
    {                                                                                              \
        return unlikely(!self->size)                      ? NULL                                   \
               : !i                                       ? self->buf                              \
               : (size_t)(i - self->buf) + 1 < self->size ? i + 1                                  \
                                                          : NULL;                                  \
    }                                                                                              \
                                                                                                   \
    static inline const t* Vector_iter_const_##t(const Vector_##t* self, const t* i)               \
    {                                                                                              \
        return unlikely(!self->size)                      ? NULL                                   \
               : !i                                       ? self->buf                              \
               : (size_t)(i - self->buf) + 1 < self->size ? i + 1                                  \
                                                          : NULL;                                  \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_iter_back_##t(Vector_##t* self, t* i)                                  \
    {                                                                                              \
        return unlikely(!self->size) ? NULL                                                        \
               : !i                  ? &self->buf[self->size - 1]                                  \
               : (i == self->buf)    ? NULL                                                        \
                                     : i - 1;                                                         \
    }                                                                                              \
                                                                                                   \
    static inline const t* Vector_iter_back_const_##t(const Vector_##t* self, const t* i)          \
    {                                                                                              \
        return unlikely(!self->size) ? NULL                                                        \
               : !i                  ? &self->buf[self->size - 1]                                  \
               : (i == self->buf)    ? NULL                                                        \
                                     : i - 1;                                                         \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_insert_##t(Vector_##t* self, t* i, t arg)                              \
    {                                                                                              \
        if (unlikely(!self->size)) {                                                               \
            ASSERT(i == self->buf, "Vector iterator out of range");                                \
            Vector_push_##t(self, arg);                                                            \
            return i + 1;                                                                          \
        } else if (unlikely(self->cap == self->size)) {                                            \
            size_t idx = i - self->buf;                                                            \
            self->buf  = realloc(self->buf, (self->cap <<= 1) * sizeof(t));                        \
            memmove(self->buf + idx + 1, self->buf + idx, (self->size++ - idx) * sizeof(t));       \
            self->buf[idx] = arg;                                                                  \
            return self->buf + idx;                                                                \
        } else {                                                                                   \
            memmove(i + 1, i, (self->size++ - (i - self->buf)) * sizeof(t));                       \
            *i = arg;                                                                              \
            return i;                                                                              \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_insertv_##t(Vector_##t* self, t* i, const t* const argv, size_t n)     \
    {                                                                                              \
        if (unlikely(!self->size)) {                                                               \
            ASSERT(i == self->buf, "Vector iterator out of range");                                \
            Vector_pushv_##t(self, argv, n);                                                       \
            return i + n;                                                                          \
        } else if (unlikely(self->cap + n > self->size)) {                                         \
            size_t idx = i - self->buf;                                                            \
            self->buf  = realloc(self->buf, (self->cap += n) * sizeof(t));                         \
            memmove(self->buf + idx + n, self->buf + idx, ((self->size += n) - idx) * sizeof(t));  \
            memcpy(self->buf + idx, argv, sizeof(t) * n);                                          \
            return self->buf + idx;                                                                \
        } else {                                                                                   \
            memmove(i + n, i, ((self->size += n) - (i - self->buf)) * sizeof(t));                  \
            memcpy(i, argv, sizeof(t) * n);                                                        \
            return i + n;                                                                          \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_insert_at_##t(Vector_##t* self, size_t i, t arg)                     \
    {                                                                                              \
        ASSERT(i <= self->size, "Vector index out of range");                                      \
        if (unlikely(!self->size)) {                                                               \
            Vector_push_##t(self, arg);                                                            \
        } else if (unlikely(self->cap == self->size)) {                                            \
            self->buf = realloc(self->buf, (self->cap <<= 1) * sizeof(t));                         \
            memmove(self->buf + i + 1, self->buf + i, (self->size++ - i) * sizeof(t));             \
            self->buf[i] = arg;                                                                    \
        } else {                                                                                   \
            memmove(self->buf + i + 1, self->buf + i, (self->size++ - i) * sizeof(t));             \
            self->buf[i] = arg;                                                                    \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_insert_front_##t(Vector_##t* self, t arg)                            \
    {                                                                                              \
        Vector_insert_##t(self, self->buf, arg);                                                   \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_insertv_front_##t(Vector_##t* self, t* argv, size_t n)               \
    {                                                                                              \
        Vector_insertv_##t(self, self->buf, argv, n);                                              \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_last_##t(Vector_##t* self)                                             \
    {                                                                                              \
        return !self->size ? NULL : self->buf + (self->size - 1);                                  \
    }                                                                                              \
                                                                                                   \
    static inline const t* Vector_last_const_##t(const Vector_##t* self)                           \
    {                                                                                              \
        return !self->size ? NULL : self->buf + (self->size - 1);                                  \
    }                                                                                              \
                                                                                                   \
    static inline t* Vector_first_##t(Vector_##t* self) { return self->buf; }                      \
                                                                                                   \
    static inline const t* Vector_first_const_##t(const Vector_##t* self) { return self->buf; }    \
                                                                                                   \
    static inline void Vector_clear_##t(Vector_##t* self)                                          \
    {                                                                                              \
        if (dtor) {                                                                                \
            for (size_t i = 0; i < self->size; ++i)                                                \
                ((void (*)(dtorctx_t*, t*))dtor)(self->dtor_arg, &self->buf[i]);                   \
        }                                                                                          \
        self->size = 0;                                                                            \
    }                                                                                              \
                                                                                                   \
    static inline void Vector_shrink_##t(Vector_##t* self)                                         \
    {                                                                                              \
        self->buf = realloc(self->buf, (self->cap = self->size) * sizeof(t));                      \
    }                                                                                              \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");
