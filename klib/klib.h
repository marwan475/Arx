#ifndef KLIB_H
#define KLIB_H

#include "debug.h"

#include <arch/arch.h>
#include <boot/boot.h>
#include <cpu/cpu.h>
#include <klib/spinlock.h>
#include <memory/heap.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <terminal/terminal.h>

struct process_t;
struct virt_addr_space;

typedef struct user_memory_access
{
    void* process_manager;
    struct process_t* process;
    struct virt_addr_space* space;
} user_memory_access_t;

#ifdef __cplusplus
extern "C"
{
#endif

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

    // Inclusive bounds; validates address arithmetic, not mapping permissions.
    bool range_within_u64(uint64_t address, uint64_t length, uint64_t lower, uint64_t upper);

    // Safe current-process copies: validate userspace bounds and page permissions,
    // pin the process, and serialize mapping changes for the complete copy.
    bool safe_read_user(uint64_t address, void* destination, size_t length);
    bool safe_write_user(uint64_t address, const void* source, size_t length);
    // Multi-copy transactions. Pair every successful begin with end, and never
    // retain the operation lock across a sleep or context switch.
    bool begin_user_memory(user_memory_access_t* access);
    void end_user_memory(user_memory_access_t* access);
    bool safe_read_user_locked(const user_memory_access_t* access, uint64_t address, void* destination, size_t length);
    bool safe_write_user_locked(const user_memory_access_t* access, uint64_t address, const void* source, size_t length);

    // memory
    void*     memset(void* dest, int value, size_t count);
    void*     memcpy(void* dest, const void* src, size_t count);
    int       memcmp(const void* lhs, const void* rhs, size_t count);
    size_t    strlen(const char* str);
    int       strcmp(const char* lhs, const char* rhs);
    char*     kstrdup(const char* str);
    uintptr_t pa_to_hhdm(uintptr_t pa, bool hhdm_present, uint64_t hhdm_offset);
    uintptr_t hhdm_to_pa(uintptr_t hhdm_addr, bool hhdm_present, uint64_t hhdm_offset);

    // memory allocation

    // Allocate large contiguous blocks of virtual memory (slow)
    void* vmalloc(size_t size);
    void  vfree(void* ptr);

    // Allocate smaller blocks of memory from the kernel heap (fast)
    // Returns null if allocation is too big for the heap
    void* kmalloc(size_t size);
    void* kzalloc(size_t size);
    void  kfree(void* ptr);

#ifdef __cplusplus
}
#endif

#endif
