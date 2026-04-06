#ifndef KLIB_H
#define KLIB_H

#include <boot/boot.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define _force_inline inline __attribute__((always_inline))

typedef _Atomic uint8_t spinlock_t;

// panic
static _force_inline void panic(void)
{
    for (;;)
    {
        arch_halt();
    }
}

// logging
int kprintf(const char* format, ...);

// spinlock
static _force_inline void spinlock_acquire(spinlock_t* lock)
{
    for (;;)
    {
        uint8_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            return;
        }

        while (__atomic_load_n(lock, __ATOMIC_RELAXED))
        {
            arch_pause();
        }
    }
}

static _force_inline void spinlock_release(spinlock_t* lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

// alignment
static _force_inline uint64_t align_up(uint64_t x, uint64_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static _force_inline uint64_t align_down(uint64_t x, uint64_t a)
{
    return x & ~(a - 1);
}

// memory
void*     memset(void* dest, int value, size_t count);
void*     memcpy(void* dest, const void* src, size_t count);
uintptr_t pa_to_hhdm(uintptr_t pa, struct boot_info* boot_info);

#endif