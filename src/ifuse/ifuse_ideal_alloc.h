#ifndef IFUSE_IDEAL_ALLOC_H
#define IFUSE_IDEAL_ALLOC_H

/**
 * Fixed-size pool allocator for ideal IFuse tables.
 *
 * One calloc at init; bump + freelist only. No chunk growth or relocation.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char*  arena;
    size_t capacity;
    size_t bump;
    size_t elem_size;
    void*  free_list;
} IfuseIdealAlloc;

static inline void ifuse_ideal_alloc_init(IfuseIdealAlloc* a, size_t elem_size) {
    memset(a, 0, sizeof(*a));
    a->elem_size = elem_size < sizeof(void*) ? sizeof(void*) : elem_size;
}

static inline bool ifuse_ideal_alloc_init_fixed(IfuseIdealAlloc* a, size_t elem_size,
                                                size_t max_elems) {
    ifuse_ideal_alloc_init(a, elem_size);
    if (max_elems == 0U) {
        return false;
    }
    a->capacity = max_elems;
    a->arena    = (char*)calloc(max_elems, a->elem_size);
    return a->arena != NULL;
}

static inline void* ifuse_ideal_alloc_get(IfuseIdealAlloc* a) {
    if (a->free_list) {
        void* out      = a->free_list;
        a->free_list = *(void**)out;
        return out;
    }
    if (!a->arena || a->bump >= a->capacity) {
        return NULL;
    }
    void* out = a->arena + (a->bump++ * a->elem_size);
    return out;
}

static inline void ifuse_ideal_alloc_put(IfuseIdealAlloc* a, void* ptr) {
    if (!ptr) {
        return;
    }
    *(void**)ptr = a->free_list;
    a->free_list = ptr;
}

#endif /* IFUSE_IDEAL_ALLOC_H */