#ifndef KLIB_H
#define KLIB_H

#include <arch/arch.h>
#include <boot/boot.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <terminal/terminal.h>
#include <memory/heap.h>
#include <klib/spinlock.h>
#include <cpu/cpu.h>
#include "debug.h"

// panic
static inline void panic(void)
{
    for (;;)
    {
        arch_halt();
    }
}

// logging
int  kprintf(const char* format, ...);
void kterm_write(const char* msg);
int  kterm_printf(const char* format, ...);
int  kterm_vprintf(const char* format, va_list args);

// alignment
static inline uint64_t align_up(uint64_t x, uint64_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static inline uint64_t align_down(uint64_t x, uint64_t a)
{
    return x & ~(a - 1);
}

// memory
void*     memset(void* dest, int value, size_t count);
void*     memcpy(void* dest, const void* src, size_t count);
int       memcmp(const void* lhs, const void* rhs, size_t count);
size_t    strlen(const char* str);
uintptr_t pa_to_hhdm(uintptr_t pa, bool hhdm_present, uint64_t hhdm_offset);
uintptr_t hhdm_to_pa(uintptr_t hhdm_addr, bool hhdm_present, uint64_t hhdm_offset);

// memory allocation

// Allocate large contiguous blocks of virtual memory (slow)
void* vmalloc(size_t size);
void  vfree(void* ptr);

void* kmalloc(size_t size);
void* kzalloc(size_t size);
void  kfree(void* ptr);

#endif
