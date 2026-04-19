#ifndef KLIB_H
#define KLIB_H

#include <arch/arch.h>
#include <boot/boot.h>
#include <kernel/cpu/cpu.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <terminal/terminal.h>
#include "debug.h"

#define _force_inline inline __attribute__((always_inline))

#ifndef SPINLOCK_T_DEFINED
#define SPINLOCK_T_DEFINED
typedef _Atomic uint8_t spinlock_t;
#endif

typedef enum arch_type
{
    ARCH_X86_64,
    ARCH_AARCH64,
} arch_type_t;

typedef struct zone            zone_t;
typedef struct virt_addr_space virt_addr_space_t;
struct flanterm_context;

typedef struct dispatcher
{
    cpu_info_t               cpus[BOOT_SMP_MAX_CPUS];
    size_t                   cpu_count;
    kernel_framebuffer_t     framebuffer;
    uint32_t                 vector_base;
    struct flanterm_context* terminal_context;
    spinlock_t               terminal_lock;
    arch_type_t              arch;
    arch_dispatcher_info_t   arch_info;
} dispatcher_t;

extern dispatcher_t dispatcher;

// panic
static _force_inline void panic(void)
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
int       memcmp(const void* lhs, const void* rhs, size_t count);
size_t    strlen(const char* str);
uintptr_t pa_to_hhdm(uintptr_t pa, bool hhdm_present, uint64_t hhdm_offset);
uintptr_t hhdm_to_pa(uintptr_t hhdm_addr, bool hhdm_present, uint64_t hhdm_offset);

// memory allocation

// Allocate large contiguous blocks of virtual memory (slow)
void* vmalloc(size_t size);
void  vfree(void* ptr);

#endif
