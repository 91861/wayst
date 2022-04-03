#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"
#include "vector.h"

/**
 * Generic hash map
 *
 *  Example:
 *
 *      typedef struct {int a, b;} Key;
 *      typedef struct {...} Value;
 *
 *      size_t Key_hash(Key* k) { return a; }
 *      bool Key_eq(Key* k, Key* o) { return k->a == o->k && k->b == o->b; }
 *      void Value_destroy(Value* v) {...}
 *
 *      DEF_MAP(Key, Value, Key_hash, Key_eq, Value_destroy)
 *      // DEF_MAP(Key, Value, Key_hash, Key_eq, NULL)
 *
 *      Map_Key_Value my_map = Map_new_Key_Value(10);
 *      Value* inserted_value = Map_insert_Key_Value(&my_map,(Key){1,2}, (Value){...});
 *      Value* val = Map_get_Key_Value(&my_map,(Key){1,2});
 *      bool was_found_and_removed = Map_remove_Key_Value(&my_map,(Key){1,2});
 *
 **/

#define DEF_MAP(k, v, hash_func, compare_func, dtor)                                               \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Waddress\"");                                               \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-function\"");                                       \
    _Pragma("GCC diagnostic push");                                                                \
    _Pragma("GCC diagnostic ignored \"-Wunused-parameter\"");                                      \
    typedef struct                                                                                 \
    {                                                                                              \
        k key;                                                                                     \
        v value;                                                                                   \
    } MapEntry_##k##_##v;                                                                          \
                                                                                                   \
    static void MapEntry_destroy_##k##_##v(MapEntry_##k##_##v* self)                               \
    {                                                                                              \
        if (dtor) {                                                                                \
            ((void (*)(v*))dtor)(&self->value);                                                    \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    DEF_VECTOR(MapEntry_##k##_##v, MapEntry_destroy_##k##_##v)                                     \
    DEF_VECTOR(Vector_MapEntry_##k##_##v, Vector_destroy_MapEntry_##k##_##v)                       \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        MapEntry_##k##_##v*        entry;                                                          \
        Vector_MapEntry_##k##_##v* _bucket;                                                        \
    } MapEntryIterator_##k##_##v;                                                                  \
                                                                                                   \
    typedef struct                                                                                 \
    {                                                                                              \
        Vector_Vector_MapEntry_##k##_##v buckets;                                                  \
    } Map_##k##_##v;                                                                               \
                                                                                                   \
    static MapEntryIterator_##k##_##v Map_iter_##k##_##v(Map_##k##_##v*             self,          \
                                                         MapEntryIterator_##k##_##v i)             \
    {                                                                                              \
        if (i.entry == NULL) {                                                                     \
            if (self->buckets.size == 0) {                                                         \
                return (MapEntryIterator_##k##_##v){ ._bucket = NULL, .entry = NULL };             \
            }                                                                                      \
            for (Vector_MapEntry_##k##_##v* bucket = NULL;                                         \
                 (bucket = Vector_iter_Vector_MapEntry_##k##_##v(&self->buckets, bucket));) {      \
                MapEntry_##k##_##v* entry = Vector_iter_MapEntry_##k##_##v(bucket, NULL);          \
                if (entry)                                                                         \
                    return (MapEntryIterator_##k##_##v){ ._bucket = bucket, .entry = entry };      \
            }                                                                                      \
            return (MapEntryIterator_##k##_##v){ ._bucket = NULL, .entry = NULL };                 \
        } else {                                                                                   \
            MapEntry_##k##_##v* entry = Vector_iter_MapEntry_##k##_##v(i._bucket, i.entry);        \
            if (entry) {                                                                           \
                return (MapEntryIterator_##k##_##v){ ._bucket = i._bucket, .entry = entry };       \
            }                                                                                      \
            for (Vector_MapEntry_##k##_##v* bucket = i._bucket;                                    \
                 (bucket = Vector_iter_Vector_MapEntry_##k##_##v(&self->buckets, bucket));) {      \
                entry = Vector_iter_MapEntry_##k##_##v(bucket, NULL);                              \
                if (entry)                                                                         \
                    return (MapEntryIterator_##k##_##v){ ._bucket = bucket, .entry = entry };      \
            }                                                                                      \
            return (MapEntryIterator_##k##_##v){ ._bucket = NULL, .entry = NULL };                 \
        }                                                                                          \
    }                                                                                              \
                                                                                                   \
    static bool Map_is_empty_##k##_##v(Map_##k##_##v* self)                                        \
    {                                                                                              \
        return (Map_iter_##k##_##v(self, (MapEntryIterator_##k##_##v){ 0, 0 })).entry == NULL;     \
    }                                                                                              \
                                                                                                   \
    static Map_##k##_##v Map_new_##k##_##v(size_t n_buckets)                                       \
    {                                                                                              \
        Map_##k##_##v self;                                                                        \
        self.buckets = Vector_new_with_capacity_Vector_MapEntry_##k##_##v(n_buckets);              \
        for (size_t i = 0; i < n_buckets; ++i) {                                                   \
            Vector_push_Vector_MapEntry_##k##_##v(&self.buckets, Vector_new_MapEntry_##k##_##v()); \
        }                                                                                          \
        return self;                                                                               \
    }                                                                                              \
                                                                                                   \
    static Vector_MapEntry_##k##_##v* Map_select_bucket_##k##_##v(Map_##k##_##v* self,             \
                                                                  const k*       key)              \
    {                                                                                              \
        return &self->buckets.buf[(self->buckets.size - 1) % hash_func(key)];                      \
    }                                                                                              \
                                                                                                   \
    static size_t Map_count_##k##_##v(Map_##k##_##v* self)                                         \
    {                                                                                              \
        size_t ctr = 0;                                                                            \
        for (MapEntryIterator_##k##_##v i = (MapEntryIterator_##k##_##v){ 0, 0 };                  \
             (i = Map_iter_##k##_##v(self, i)).entry;) {                                           \
            ++ctr;                                                                                 \
        }                                                                                          \
        return ctr;                                                                                \
    }                                                                                              \
                                                                                                   \
    static v* Map_insert_entry_##k##_##v(Map_##k##_##v* self, MapEntry_##k##_##v entry)            \
    {                                                                                              \
        Vector_MapEntry_##k##_##v* bucket = Map_select_bucket_##k##_##v(self, &entry.key);         \
        for (MapEntry_##k##_##v* i = NULL; (i = Vector_iter_MapEntry_##k##_##v(bucket, i));) {     \
            if (compare_func(&i->key, &entry.key)) {                                               \
                if (dtor) {                                                                        \
                    ((void (*)(v*))dtor)(&i->value);                                               \
                }                                                                                  \
                i->value = entry.value;                                                            \
                return &i->value;                                                                  \
            }                                                                                      \
        }                                                                                          \
        Vector_push_MapEntry_##k##_##v(bucket, entry);                                             \
        return &Vector_last_MapEntry_##k##_##v(bucket)->value;                                     \
    }                                                                                              \
                                                                                                   \
    static v* Map_insert_##k##_##v(Map_##k##_##v* self, k key, v value)                            \
    {                                                                                              \
        return Map_insert_entry_##k##_##v(self,                                                    \
                                          (MapEntry_##k##_##v){ .key = key, .value = value });     \
    }                                                                                              \
                                                                                                   \
    static MapEntry_##k##_##v* Map_get_entry_##k##_##v(Map_##k##_##v* self, const k* key)          \
    {                                                                                              \
        Vector_MapEntry_##k##_##v* bucket = Map_select_bucket_##k##_##v(self, key);                \
        for (MapEntry_##k##_##v* i = NULL; (i = Vector_iter_MapEntry_##k##_##v(bucket, i));) {     \
            if (compare_func(&i->key, key)) {                                                      \
                return i;                                                                          \
            }                                                                                      \
        }                                                                                          \
        return NULL;                                                                               \
    }                                                                                              \
                                                                                                   \
    static v* Map_get_##k##_##v(Map_##k##_##v* self, const k* key)                                 \
    {                                                                                              \
        MapEntry_##k##_##v* entry = Map_get_entry_##k##_##v(self, key);                            \
        return entry ? &entry->value : NULL;                                                       \
    }                                                                                              \
                                                                                                   \
    /**                                                                                            \
     * @return value was found and removed */                                                      \
    static bool Map_remove_##k##_##v(Map_##k##_##v* self, k* key)                                  \
    {                                                                                              \
        Vector_MapEntry_##k##_##v* bucket = Map_select_bucket_##k##_##v(self, key);                \
        for (size_t i = 0; i < bucket->size; ++i) {                                                \
            if (compare_func(&bucket->buf[i].key, key)) {                                          \
                Vector_remove_at_MapEntry_##k##_##v(bucket, i, 1);                                 \
                return true;                                                                       \
            }                                                                                      \
        }                                                                                          \
        return false;                                                                              \
    }                                                                                              \
                                                                                                   \
    static void Map_destroy_##k##_##v(Map_##k##_##v* self)                                         \
    {                                                                                              \
        Vector_destroy_Vector_MapEntry_##k##_##v(&self->buckets);                                  \
    }                                                                                              \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");                                                                 \
    _Pragma("GCC diagnostic pop");
