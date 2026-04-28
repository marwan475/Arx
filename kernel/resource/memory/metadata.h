#ifndef METADATA_H
#define METADATA_H

#include <boot/boot.h>
#include <stdbool.h>
#include <stddef.h>

#define METADATA_CHUNK_SIZE (16 * PAGE_SIZE)

typedef struct metadata_chunk metadata_chunk_t;

typedef struct metadata_chunk
{
    metadata_chunk_t* next;
    metadata_chunk_t* prev;
    void*             data;
    size_t            size;
} metadata_chunk_t;

typedef struct metadata_pool
{
    metadata_chunk_t* chunks;
    void*             free_list;
    size_t            element_size;
    size_t            elements_per_chunk;
} metadata_pool_t;

size_t metadata_default_elements_per_chunk(size_t element_size);
void  metadata_pool_init(metadata_pool_t* pool, size_t element_size, size_t elements_per_chunk);
void* metadata_pool_alloc(metadata_pool_t* pool);
void  metadata_pool_free(metadata_pool_t* pool, void* element);

#endif
