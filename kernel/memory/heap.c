#include <memory/heap.h>
#include <klib/klib.h>

void heap_init(void)
{
    void* heap_base = vmalloc(KERNEL_HEAP_SIZE);
    if (heap_base == NULL)
    {
        kprintf("Arx kernel: failed to allocate kernel heap\n");
        panic();
    }

    memset(heap_base, 0, KERNEL_HEAP_SIZE);

    dispatcher.heap.base = heap_base;
    dispatcher.heap.size = KERNEL_HEAP_SIZE;

    kprintf("Arx kernel: kernel heap initialized at %p with size %zu bytes\n", heap_base, KERNEL_HEAP_SIZE);
}