#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <arch/arch.h>
#include <stdint.h>

typedef uint8_t spinlock_t;

#ifdef __cplusplus
extern "C"
{
#endif
    void cpu_preemption_disable(void);
    void cpu_preemption_enable(void);
#ifdef __cplusplus
}

#endif

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

// Ordinary locks are owned by the acquiring task. IPI mailboxes use the raw
// primitives above because a different CPU releases them.
static inline void spinlock_acquire_preempt(spinlock_t* lock)
{
    cpu_preemption_disable();
    spinlock_acquire(lock);
}

static inline void spinlock_release_preempt(spinlock_t* lock)
{
    spinlock_release(lock);
    cpu_preemption_enable();
}

#endif
