#ifndef HEAP_H
#define HEAP_H

#include <boot/boot.h>
#include <klib/spinlock.h>
#include <stddef.h>
#include <stdbool.h>

#define SLAB_SIZE PAGE_SIZE
#define INITIAL_SLABS_PER_CACHE 4
#define MAX_SLABS_PER_CACHE 128

typedef enum object_size
{
    OBJECT_SIZE_16   = 0,
    OBJECT_SIZE_32   = 1,
    OBJECT_SIZE_64   = 2,
    OBJECT_SIZE_128  = 3,
    OBJECT_SIZE_256  = 4,
    OBJECT_SIZE_512  = 5,
    OBJECT_SIZE_1024 = 6,
    OBJECT_SIZE_2048 = 7,
    OBJECT_SIZE_CLASS_COUNT
} object_size_t; 

typedef struct slab slab_t;

typedef struct slab {
    size_t      object_size;
    size_t      total_objects;
    size_t      free_objects;
    void*       objects_array;
    uint8_t*    object_bitmap;
    slab_t*     next;
    slab_t*     prev;
    bool        allocated;
} slab_t;

typedef struct cache {
    size_t      object_size;
    slab_t*     partial_slabs;
    slab_t*     full_slabs;
    slab_t*     empty_slabs;
    slab_t*     slab_metadata;
    size_t      slab_metadata_count;
} cache_t;

typedef struct kernel_heap {
    spinlock_t lock;
    cache_t caches[OBJECT_SIZE_CLASS_COUNT];
}kernel_heap_t;

void heap_init(void);
void* heap_alloc(kernel_heap_t* heap, size_t size);
void  heap_free(kernel_heap_t* heap, void* ptr);

#endif