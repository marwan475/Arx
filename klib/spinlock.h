#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <arch/arch.h>
#include <stdint.h>

typedef _Atomic uint8_t spinlock_t;

static inline void spinlock_acquire(spinlock_t* lock)
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

static inline void spinlock_release(spinlock_t* lock)
{
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

#endif