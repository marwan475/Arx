#ifndef KLIB_H
#define KLIB_H

#include "debug.h"
#include "error_logger.h"

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

// Shared Linux-style syscall error values.
static const uint64_t LINUX_ENOENT       = (uint64_t) -2;
static const uint64_t LINUX_ESRCH        = (uint64_t) -3;
static const uint64_t LINUX_EIO          = (uint64_t) -5;
static const uint64_t LINUX_EBADF        = (uint64_t) -9;
static const uint64_t LINUX_EAGAIN       = (uint64_t) -11;
static const uint64_t LINUX_ENOMEM       = (uint64_t) -12;
static const uint64_t LINUX_EACCES       = (uint64_t) -13;
static const uint64_t LINUX_EFAULT       = (uint64_t) -14;
static const uint64_t LINUX_ENOTDIR      = (uint64_t) -20;
static const uint64_t LINUX_EINVAL       = (uint64_t) -22;
static const uint64_t LINUX_EMFILE       = (uint64_t) -24;
static const uint64_t LINUX_ENOTTY       = (uint64_t) -25;
static const uint64_t LINUX_ERANGE       = (uint64_t) -34;
static const uint64_t LINUX_ENAMETOOLONG = (uint64_t) -36;
static const uint64_t LINUX_ENOSYS       = (uint64_t) -38;
static const uint64_t LINUX_EOVERFLOW    = (uint64_t) -75;
static const uint64_t LINUX_EOPNOTSUPP   = (uint64_t) -95;

struct process_t;
struct virt_addr_space;

typedef struct user_memory_access
{
    void*                   process_manager;
    struct process_t*       process;
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

    static inline bool add_overflows_u64(uint64_t a, uint64_t b)
    {
        return a > UINT64_MAX - b;
    }

    // Inclusive bounds; validates address arithmetic, not mapping permissions.
    bool range_within_u64(uint64_t address, uint64_t length, uint64_t lower, uint64_t upper);

    // Extend a wrapping sample. Sample at least once per full 32-bit period;
    // callers sharing previous across CPUs must serialize sampling/publication.
    uint64_t extend_counter32(uint64_t previous, uint32_t sample);

    // Safe current-process copies: validate userspace bounds and page permissions,
    // pin the process, and serialize mapping changes for the complete copy.
    bool safe_read_user(uint64_t address, void* destination, size_t length);
    bool safe_write_user(uint64_t address, const void* source, size_t length);
    bool safe_read_user_string(uint64_t address, char* destination, size_t capacity, size_t* out_length);
    // Multi-copy transactions. Pair every successful begin with end, and never
    // retain the operation lock across a sleep or context switch.
    bool begin_user_memory(user_memory_access_t* access);
    void end_user_memory(user_memory_access_t* access);
    bool safe_read_user_locked(const user_memory_access_t* access, uint64_t address, void* destination, size_t length);
    bool safe_user_memory_accessible_locked(const user_memory_access_t* access, uint64_t address, size_t length, bool write);
    bool safe_atomic_load_user32_locked(const user_memory_access_t* access, uint64_t address, uint32_t* value);
    bool safe_atomic_compare_exchange_user32_locked(const user_memory_access_t* access, uint64_t address, uint32_t* expected, uint32_t desired);
    bool safe_write_user_locked(const user_memory_access_t* access, uint64_t address, const void* source, size_t length);

    // Explicit-address-space initialization; caller owns mapping lifetime.
    bool copy_to_process_virtual(struct virt_addr_space* addressSpace, uint64_t destinationVirtualAddress, const void* source, uint64_t size);

    // memory
    void*     memset(void* dest, int value, size_t count);
    void*     memcpy(void* dest, const void* src, size_t count);
    int       memcmp(const void* lhs, const void* rhs, size_t count);
    size_t    strlen(const char* str);
    int       strcmp(const char* lhs, const char* rhs);
    char*     kstrdup(const char* str);
    uintptr_t pa_to_hhdm(uintptr_t pa, bool hhdm_present, uint64_t hhdm_offset);
    uintptr_t hhdm_to_pa(uintptr_t hhdm_addr, bool hhdm_present, uint64_t hhdm_offset);

    // In-place ascending order, bounded O(n log n) time, no allocations.
    void sort_u32(uint32_t* values, size_t count);

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
