#include <memory/heap.h>
#include <klib/klib.h>
#include <klib/intrusive_list.h>
#include <klib/bitmap.h>   
#include <memory/metadata.h>
#include <memory/pmm.h>

const size_t heap_object_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072};

static void* slab_alloc(slab_t* slab)
{
    if (slab == NULL || slab->free_objects == 0)
    {
        return NULL;
    }

    size_t index;
    if (!bitmap_find_first_clear(slab->object_bitmap, slab->total_objects, &index))
    {
        return NULL;
    }

    bitmap_set(slab->object_bitmap, index);
    slab->free_objects--;

    return (void*) ((uintptr_t) slab->objects_array + index * slab->object_size);

}

static void slab_free(slab_t* slab, void* ptr)
{
    if (slab == NULL || ptr == NULL)
    {
        return;
    }

    uint64_t index = ((uintptr_t) ptr - (uintptr_t) slab->objects_array) / slab->object_size;
    if (index >= slab->total_objects)
    {
        return;
    }

    if (!bitmap_test(slab->object_bitmap, index))
    {
        return;
    }

    bitmap_clear(slab->object_bitmap, index);
    slab->free_objects++;
}

static bool slab_init(slab_t* slab, size_t object_size)
{
    if (slab == NULL || object_size == 0)
    {
        return false;
    }

    slab->object_size  = object_size;
    slab->total_objects = SLAB_SIZE / object_size;
    slab->free_objects  = slab->total_objects;
    slab->objects_array     = NULL;
    slab->next          = NULL;
    slab->prev          = NULL;

    ILIST_NODE_INIT(slab);

    slab->objects_array = pmm_alloc(SLAB_SIZE);
    slab->object_bitmap = pmm_alloc(BITMAP_BYTES_FOR_BITS(slab->total_objects));

    if (slab->objects_array == NULL || slab->object_bitmap == NULL)
    {
        return false;
    }

    bitmap_init(slab->object_bitmap, slab->total_objects);
    return true;
}

static slab_t* alloc_slab_struct(cache_t* cache)
{
    if (cache == NULL)
    {
        return NULL;
    }

    slab_t* slab = (slab_t*) metadata_pool_alloc(&cache->slab_metadata_pool);
    if (slab == NULL)
    {
        return NULL;
    }

    slab->allocated = true;
    ILIST_NODE_INIT(slab);
    return slab;
}

static void free_slab_struct(cache_t* cache, slab_t* slab)
{
    if (cache == NULL || slab == NULL || !slab->allocated)
    {
        return;
    }

    slab->allocated = false;
    metadata_pool_free(&cache->slab_metadata_pool, slab);
}

static bool slab_contains_ptr(const slab_t* slab, const void* ptr)
{
    if (slab == NULL || ptr == NULL || slab->objects_array == NULL)
    {
        return false;
    }

    uintptr_t start = (uintptr_t) slab->objects_array;
    uintptr_t end   = start + slab->total_objects * slab->object_size;
    uintptr_t p     = (uintptr_t) ptr;

    if (p < start || p >= end)
    {
        return false;
    }

    return ((p - start) % slab->object_size) == 0;
}

static void* cache_alloc(cache_t* cache)
{
    if (cache == NULL)
    {
        return NULL;
    }

    ILIST_FOR_EACH(slab, cache->partial_slabs)
    {
        void* ptr = slab_alloc(slab);
        if (ptr != NULL)
        {
            return ptr;
        }
    }

    ILIST_FOR_EACH(slab, cache->full_slabs)
    {
        void* ptr = slab_alloc(slab);
        if (ptr != NULL)
        {
            ILIST_REMOVE(cache->full_slabs, slab);
            ILIST_PUSH_FRONT(cache->partial_slabs, slab);
            return ptr;
        }
    }

    for (size_t i = 0; i < INITIAL_SLABS_PER_CACHE; i++)
    {
        slab_t* slab = alloc_slab_struct(cache);
        if (slab == NULL)
        {
            break;
        }

        if (!slab_init(slab, cache->object_size))
        {
            free_slab_struct(cache, slab);
            break;
        }

        ILIST_PUSH_FRONT(cache->full_slabs, slab);
    }

    ILIST_FOR_EACH(slab, cache->full_slabs)
    {
        void* ptr = slab_alloc(slab);
        if (ptr != NULL)
        {
            ILIST_REMOVE(cache->full_slabs, slab);
            ILIST_PUSH_FRONT(cache->partial_slabs, slab);
            return ptr;
        }
    }

    return NULL;

}

static void cache_free(cache_t* cache, void* ptr)
{
    if (cache == NULL || ptr == NULL)
    {
        return;
    }

    ILIST_FOR_EACH(slab, cache->empty_slabs)
    {
        if (!slab_contains_ptr(slab, ptr))
        {
            continue;
        }

        slab_free(slab, ptr);

        if (slab->free_objects > 0)
        {
            ILIST_REMOVE(cache->empty_slabs, slab);
            ILIST_PUSH_FRONT(cache->partial_slabs, slab);
        }

        return;
    }

    ILIST_FOR_EACH(slab, cache->partial_slabs)
    {
        if (!slab_contains_ptr(slab, ptr))
        {
            continue;
        }

        slab_free(slab, ptr);

        if (slab->free_objects == slab->total_objects)
        {
            ILIST_REMOVE(cache->partial_slabs, slab);
            ILIST_PUSH_FRONT(cache->full_slabs, slab);
        }

        return;
    }
}

static void cache_init(cache_t* cache, size_t object_size)
{
    cache->object_size   = object_size;
    cache->partial_slabs = NULL;
    cache->full_slabs    = NULL;
    cache->empty_slabs   = NULL;

    metadata_pool_init(&cache->slab_metadata_pool, sizeof(slab_t), metadata_default_elements_per_chunk(sizeof(slab_t)));

    for (size_t i = 0; i < INITIAL_SLABS_PER_CACHE; i++)
    {
        slab_t* slab = alloc_slab_struct(cache);
        if (slab == NULL)
        {
            kprintf("Arx kernel: failed to allocate slab metadata for cache\n");
            panic();
        }
        if (!slab_init(slab, object_size))
        {
            kprintf("Arx kernel: failed to initialize slab\n");
            panic();
        }

        ILIST_PUSH_FRONT(cache->full_slabs, slab);
    }
}

void heap_init(void)
{
    for (size_t i = 0; i < dispatcher.numa_node_count; i++)
    {
        dispatcher.numa_nodes[i].heap.lock = 0;

        for (size_t j = 0; j < OBJECT_SIZE_CLASS_COUNT; j++)
        {
            cache_init(&dispatcher.numa_nodes[i].heap.caches[j], heap_object_sizes[j]);
        }
    }

    kprintf("Arx kernel: heap initialized\n");
}

void* heap_alloc(kernel_heap_t* heap, size_t size)
{
    if (heap == NULL || size == 0)
    {
        return NULL;
    }

    for (size_t i = 0; i < OBJECT_SIZE_CLASS_COUNT; i++)
    {
        if (size <= heap->caches[i].object_size)
        {
            return cache_alloc(&heap->caches[i]);
        }
    }

    return NULL;
}  

void heap_free(kernel_heap_t* heap, void* ptr)
{
    if (heap == NULL || ptr == NULL)
    {
        return;
    }

    for (size_t i = 0; i < OBJECT_SIZE_CLASS_COUNT; i++)
    {
        cache_free(&heap->caches[i], ptr);
    }
}