#include <memory/heap.h>
#include <klib/klib.h>

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