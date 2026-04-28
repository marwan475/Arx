#include <memory/metadata.h>
#include <memory/pmm.h>
#include <klib/klib.h>

size_t metadata_default_elements_per_chunk(size_t element_size)
{
    if (element_size == 0)
    {
        return 0;
    }

    size_t count = METADATA_CHUNK_SIZE / element_size;
    return count == 0 ? 1 : count;
}

static bool metadata_pool_grow(metadata_pool_t* pool)
{
    if (pool == NULL || pool->element_size == 0 || pool->elements_per_chunk == 0)
    {
        return false;
    }

    metadata_chunk_t* chunk = (metadata_chunk_t*) pmm_alloc(sizeof(metadata_chunk_t));
    if (chunk == NULL)
    {
        return false;
    }

    size_t chunk_bytes = pool->element_size * pool->elements_per_chunk;
    chunk->data = pmm_alloc(chunk_bytes);
    if (chunk->data == NULL)
    {
        return false;
    }

    chunk->size = chunk_bytes;
    chunk->next = pool->chunks;
    chunk->prev = NULL;

    if (pool->chunks != NULL)
    {
        pool->chunks->prev = chunk;
    }

    pool->chunks = chunk;

    memset(chunk->data, 0, chunk->size);

    for (size_t i = 0; i < pool->elements_per_chunk; i++)
    {
        void* element = (void*) ((uintptr_t) chunk->data + i * pool->element_size);
        *(void**) element = pool->free_list;
        pool->free_list = element;
    }

    return true;
}

void metadata_pool_init(metadata_pool_t* pool, size_t element_size, size_t elements_per_chunk)
{
    if (pool == NULL)
    {
        return;
    }

    pool->chunks = NULL;
    pool->free_list = NULL;
    pool->element_size = element_size;
    pool->elements_per_chunk = elements_per_chunk;
}

void* metadata_pool_alloc(metadata_pool_t* pool)
{
    if (pool == NULL || pool->element_size == 0 || pool->elements_per_chunk == 0)
    {
        return NULL;
    }

    if (pool->free_list == NULL)
    {
        if (!metadata_pool_grow(pool))
        {
            return NULL;
        }
    }

    void* element = pool->free_list;
    pool->free_list = *(void**) element;
    memset(element, 0, pool->element_size);
    return element;
}

void metadata_pool_free(metadata_pool_t* pool, void* element)
{
    if (pool == NULL || element == NULL)
    {
        return;
    }

    *(void**) element = pool->free_list;
    pool->free_list = element;
}
