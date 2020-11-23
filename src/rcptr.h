/* See LICENSE for license information. */

/* Reference counted 'shared' pointer */

#pragma once

#include "util.h"
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define DEF_RC_PTR(t, dtor)                                                                        \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Waddress\"");                                               \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-function\"");                                       \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"");                                      \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        uint32_t refcount;                                                                         \
        alignas(alignof(void*)) t payload;                                                         \
    } RcPtrBlock_##t;                                                                              \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        RcPtrBlock_##t* block;                                                                     \
    } RcPtr_##t;                                                                                   \
                                                                                                   \
    static RcPtr_##t RcPtr_new_##t()                                                               \
    {                                                                                              \
        RcPtrBlock_##t* block = malloc(sizeof(RcPtrBlock_##t));                                    \
        block->refcount       = 1;                                                                 \
        return (RcPtr_##t){ .block = block };                                                      \
    }                                                                                              \
                                                                                                   \
    static void RcPtr_destroy_##t(RcPtr_##t* self)                                                 \
    {                                                                                              \
        if (!self->block)                                                                          \
            return;                                                                                \
        ASSERT(self->block->refcount, "is valid");                                                 \
        --self->block->refcount;                                                                   \
                                                                                                   \
        if (!self->block->refcount) {                                                              \
            if (dtor) {                                                                            \
                ((void (*)(t*))dtor)(&self->block->payload);                                       \
            }                                                                                      \
            free(self->block);                                                                     \
        }                                                                                          \
        self->block = NULL;                                                                        \
    }                                                                                              \
                                                                                                   \
    static bool RcPtr_is_unique_##t(RcPtr_##t* self)                                               \
    {                                                                                              \
        return self->block ? self->block->refcount == 1 : true;                                    \
    }                                                                                              \
                                                                                                   \
    static void RcPtr_new_in_place_of_##t(RcPtr_##t* self)                                         \
    {                                                                                              \
        RcPtr_destroy_##t(self);                                                                   \
        RcPtrBlock_##t* block = malloc(sizeof(RcPtrBlock_##t));                                    \
        block->refcount       = 1;                                                                 \
        self->block           = block;                                                             \
    }                                                                                              \
                                                                                                   \
    static RcPtr_##t RcPtr_new_shared_##t(RcPtr_##t* source)                                       \
    {                                                                                              \
        if (source->block) {                                                                       \
            ASSERT(source->block->refcount, "is valid");                                           \
            ++source->block->refcount;                                                             \
        }                                                                                          \
        return (RcPtr_##t){ .block = source->block };                                              \
    }                                                                                              \
                                                                                                   \
    static void RcPtr_new_shared_in_place_of_##t(RcPtr_##t* self, RcPtr_##t* source)               \
    {                                                                                              \
        RcPtr_destroy_##t(self);                                                                   \
        if (source->block) {                                                                       \
            ASSERT(source->block->refcount, "is valid");                                           \
            ++source->block->refcount;                                                             \
        }                                                                                          \
        self->block = source->block;                                                               \
    }                                                                                              \
                                                                                                   \
    static t* RcPtr_get_##t(RcPtr_##t* self)                                                       \
    {                                                                                              \
        if (unlikely(!self))                                                                       \
            return NULL;                                                                           \
        ASSERT(!self->block || self->block->refcount, "is valid");                                 \
        return self->block ? &self->block->payload : NULL;                                         \
    }                                                                                              \
                                                                                                   \
    static const t* RcPtr_get_const_##t(const RcPtr_##t* self)                                     \
    {                                                                                              \
        if (unlikely(!self))                                                                       \
            return NULL;                                                                           \
        ASSERT(!self->block || self->block->refcount, "is valid");                                 \
        return self->block ? &self->block->payload : NULL;                                         \
    }                                                                                              \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");

#define DEF_RC_PTR_DA(t, dtor, dtorctx_t)                                                          \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Waddress\"");                                               \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-function\"");                                       \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"");                                      \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        void*    dtor_arg;                                                                         \
        uint32_t refcount;                                                                         \
        alignas(alignof(void*)) t payload;                                                         \
    } RcPtrBlock_##t;                                                                              \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        RcPtrBlock_##t* block;                                                                     \
    } RcPtr_##t;                                                                                   \
                                                                                                   \
    static RcPtr_##t RcPtr_new_##t(dtorctx_t* destroy_arg)                                         \
    {                                                                                              \
        RcPtrBlock_##t* block = malloc(sizeof(RcPtrBlock_##t));                                    \
        block->refcount       = 1;                                                                 \
        block->dtor_arg       = destroy_arg;                                                       \
        return (RcPtr_##t){ .block = block };                                                      \
    }                                                                                              \
                                                                                                   \
    static void RcPtr_destroy_##t(RcPtr_##t* self)                                                 \
    {                                                                                              \
        if (!self->block)                                                                          \
            return;                                                                                \
        ASSERT(self->block->refcount, "is valid");                                                 \
        --self->block->refcount;                                                                   \
                                                                                                   \
        if (!self->block->refcount) {                                                              \
            if (dtor) {                                                                            \
                ((void (*)(dtorctx_t*, t*))dtor)(self->block->dtor_arg, &self->block->payload);    \
            }                                                                                      \
            free(self->block);                                                                     \
        }                                                                                          \
        self->block = NULL;                                                                        \
    }                                                                                              \
                                                                                                   \
    static bool RcPtr_is_unique_##t(RcPtr_##t* self)                                               \
    {                                                                                              \
        return self->block ? self->block->refcount == 1 : true;                                    \
    }                                                                                              \
                                                                                                   \
    static void RcPtr_new_in_place_of_##t(RcPtr_##t* self)                                         \
    {                                                                                              \
        RcPtr_destroy_##t(self);                                                                   \
        RcPtrBlock_##t* block = malloc(sizeof(RcPtrBlock_##t));                                    \
        block->refcount       = 1;                                                                 \
        self->block           = block;                                                             \
    }                                                                                              \
                                                                                                   \
    static RcPtr_##t RcPtr_new_shared_##t(RcPtr_##t* source)                                       \
    {                                                                                              \
        if (source->block) {                                                                       \
            ASSERT(source->block->refcount, "is valid");                                           \
            ++source->block->refcount;                                                             \
        }                                                                                          \
        return (RcPtr_##t){ .block = source->block };                                              \
    }                                                                                              \
                                                                                                   \
    static void RcPtr_new_shared_in_place_of_##t(RcPtr_##t* self, RcPtr_##t* source)               \
    {                                                                                              \
        RcPtr_destroy_##t(self);                                                                   \
        if (source->block) {                                                                       \
            ASSERT(source->block->refcount, "is valid");                                           \
            ++source->block->refcount;                                                             \
        }                                                                                          \
        self->block = source->block;                                                               \
    }                                                                                              \
                                                                                                   \
    static t* RcPtr_get_##t(RcPtr_##t* self)                                                       \
    {                                                                                              \
        if (unlikely(!self))                                                                       \
            return NULL;                                                                           \
        ASSERT(!self->block || self->block->refcount, "is valid");                                 \
        return self->block ? &self->block->payload : NULL;                                         \
    }                                                                                              \
                                                                                                   \
    static const t* RcPtr_get_const_##t(const RcPtr_##t* self)                                     \
    {                                                                                              \
        if (unlikely(!self))                                                                       \
            return NULL;                                                                           \
        ASSERT(!self->block || self->block->refcount, "is valid");                                 \
        return self->block ? &self->block->payload : NULL;                                         \
    }                                                                                              \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");
