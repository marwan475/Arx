#ifndef HEAP_H
#define HEAP_H

#include <boot/boot.h>
#include <stddef.h>

#define KERNEL_HEAP_SIZE (250 * PAGE_SIZE) // 1 mb

typedef struct kernel_heap {
    void*  base;
    size_t size;
}kernel_heap_t;

void heap_init(void);

#endif