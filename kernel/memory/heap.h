#ifndef HEAP_H
#define HEAP_H

#include <klib/klib.h>

typedef struct kernel_heap {
    void*  base;
    size_t size;
}kernel_heap_t;

#endif