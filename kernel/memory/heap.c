#include <memory/heap.h>
#include <klib/klib.h>
#include <klib/intrusive_list.h>
#include <klib/bitmap.h>   
#include <memory/pmm.h>

const size_t heap_object_sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};

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

static slab_t* alloc_slab_struct(void* metadata, size_t* metadata_count)
{
    for (size_t i = 0; i < MAX_SLABS_PER_CACHE; i++)
    {
        slab_t* slab = &((slab_t*) metadata)[i];
        if (!slab->allocated)
        {
            (*metadata_count)--;
            slab->allocated = true;
            return slab;
        }
    }
    return NULL;
}

static void free_slab_struct(slab_t* slab, size_t* metadata_count)
{
    if (slab == NULL || !slab->allocated)
    {
        return;
    }
    slab->allocated = false;
    (*metadata_count)++;
}

static void cache_init(cache_t* cache, size_t object_size)
{
    cache->object_size   = object_size;
    cache->partial_slabs = NULL;
    cache->full_slabs    = NULL;
    cache->empty_slabs   = NULL;

    cache->slab_metadata = pmm_alloc(MAX_SLABS_PER_CACHE * sizeof(slab_t));
    if (cache->slab_metadata == NULL)
    {
        kprintf("Arx kernel: failed to allocate cache slab metadata\n");
        panic();
    }

    memset(cache->slab_metadata, 0, MAX_SLABS_PER_CACHE * sizeof(slab_t));
    cache->slab_metadata_count = MAX_SLABS_PER_CACHE;

    for (size_t i = 0; i < INITIAL_SLABS_PER_CACHE; i++)
    {
        slab_t* slab = alloc_slab_struct(cache->slab_metadata, &cache->slab_metadata_count);
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
        dispatcher.numa_nodes[i].heap.base = vmalloc(KERNEL_HEAP_SIZE);
        dispatcher.numa_nodes[i].heap.size = KERNEL_HEAP_SIZE;

        if (dispatcher.numa_nodes[i].heap.base == NULL)
        {
            kprintf("Arx kernel: failed to initialize heap for NUMA node %zu\n", i);
            panic();
        }
    }

    kprintf("Arx kernel: heap initialized\n");
}