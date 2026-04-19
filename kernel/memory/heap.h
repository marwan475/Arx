#ifndef HEAP_H
#define HEAP_H

#include <boot/boot.h>
#include <stddef.h>

#define KERNEL_HEAP_SIZE (250 * PAGE_SIZE) // 1 mb

#define SLAB_SIZE PAGE_SIZE

typedef struct slab slab_t;

typedef struct slab {
    size_t      object_size;
    size_t      total_objects;
    size_t      free_objects;
    void*       objects_array;
    uint8_t*    object_bitmap;
    slab_t*     next;
    slab_t*     prev;
} slab_t;

typedef struct cache {} cache_t;


typedef struct kernel_heap {
    void*  base;
    size_t size;
}kernel_heap_t;

void heap_init(void);

#endif