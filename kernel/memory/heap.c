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

    uint64_t index;
    bitmap_find_first_clear(slab->object_bitmap, slab->total_objects, &index);
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

    bitmap_clear(slab->object_bitmap, index);
    slab->free_objects++;
}

static void slab_init(slab_t* slab, size_t object_size)
{
    slab->object_size  = object_size;
    slab->total_objects = SLAB_SIZE / object_size;
    slab->free_objects  = slab->total_objects;
    slab->objects_array     = NULL;
    slab->next          = NULL;
    slab->prev          = NULL;

    ILIST_NODE_INIT(slab);

    slab->objects_array = pmm_alloc(SLAB_SIZE);
    slab->object_bitmap = pmm_alloc(BITMAP_BYTES_FOR_BITS(slab->total_objects));

    bitmap_init(slab->object_bitmap, slab->total_objects);
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