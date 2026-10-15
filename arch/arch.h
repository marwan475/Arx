#ifndef ARCH_H
#define ARCH_H

#if defined(__x86_64__)
#include <arch/x86_64/x86_64.h>
#elif defined(__aarch64__)
#include <arch/aarch64/aarch64.h>
#else
#error Unsupported architecture
#endif

#include <stdbool.h>
#include <stdint.h>
#include <uacpi/status.h>

struct acpi_madt;
typedef struct signal_action_t signal_action_t;
typedef struct signal_stack_t signal_stack_t;
typedef struct signal_sender_t signal_sender_t;

#ifndef PHYS_ADDR_T_DEFINED
#define PHYS_ADDR_T_DEFINED
typedef uint64_t phys_addr_t;
#endif

#ifndef VIRT_ADDR_T_DEFINED
#define VIRT_ADDR_T_DEFINED
typedef uint64_t virt_addr_t;
#endif

#ifndef REG
#define REG(type, x) (*((volatile type*) (uintptr_t) (x)))
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#if defined(__x86_64__)
#define ARCH_PAGE_FLAGS_INIT(flags) ((flags) = (1ULL << 63))

#define ARCH_PAGE_FLAG_SET_READ(flags) ((void) (flags))
#define ARCH_PAGE_FLAG_SET_WRITE(flags) ((flags) |= (1ULL << 1))
#define ARCH_PAGE_FLAG_SET_EXEC(flags) ((flags) &= ~(1ULL << 63))
#define ARCH_PAGE_FLAG_SET_USER(flags) ((flags) |= (1ULL << 2))
#define ARCH_PAGE_FLAG_SET_GLOBAL(flags) ((flags) |= (1ULL << 8))
#define ARCH_PAGE_FLAG_SET_NOCACHE(flags) ((flags) |= (1ULL << 4) | (1ULL << 3))
#define ARCH_PAGE_FLAG_SET_WRITETHROUGH(flags) ((flags) |= (1ULL << 3))
#elif defined(__aarch64__)
#define AARCH64_PAGE_FLAG_ATTRINDX_MASK (0x7ULL << 2)

#define ARCH_PAGE_FLAGS_INIT(flags) ((flags) = (1ULL << 10) | (3ULL << 8) | (1ULL << 53) | (1ULL << 54))

#define ARCH_PAGE_FLAG_SET_READ(flags) ((void) (flags))
#define ARCH_PAGE_FLAG_SET_WRITE(flags) ((flags) &= ~(1ULL << 7))
#define ARCH_PAGE_FLAG_SET_EXEC(flags) ((flags) &= ~((1ULL << 53) | (1ULL << 54)))
#define ARCH_PAGE_FLAG_SET_USER(flags) ((flags) |= (1ULL << 6) | (1ULL << 11))
#define ARCH_PAGE_FLAG_SET_GLOBAL(flags) ((flags) &= ~(1ULL << 11))
#define ARCH_PAGE_FLAG_SET_NOCACHE(flags) ((flags) = ((flags) & ~AARCH64_PAGE_FLAG_ATTRINDX_MASK) | (1ULL << 2))
#define ARCH_PAGE_FLAG_SET_WRITETHROUGH(flags) ((flags) = ((flags) & ~AARCH64_PAGE_FLAG_ATTRINDX_MASK) | (2ULL << 2))
#else
#error Unsupported architecture
#endif

    // Architecture owns the user context ABI; status is separate from the restored return value.
    uint64_t arch_signal_validate_action(const signal_action_t* action);
    uint64_t arch_signal_stack_pointer(const arch_syscall_frame_t* frame);
    bool     arch_signal_user_return(const arch_syscall_frame_t* frame);
    uint64_t arch_signal_prepare(arch_syscall_frame_t* frame, uint64_t result, const signal_action_t* action, const signal_stack_t* stack, const signal_sender_t* sender, uint64_t mask, int signal, bool restartable);
    uint64_t arch_signal_restore(arch_syscall_frame_t* frame, uint64_t* result, uint64_t* mask, signal_stack_t* stack);

    void arch_halt(void);
    void arch_pause(void);

    uint8_t arch_cpu_id(void);

    // Page table should be raw physical address
    bool arch_map_page(virt_addr_t va, phys_addr_t pa, uint64_t flags, phys_addr_t page_table);
    void arch_unmap_page(virt_addr_t va, phys_addr_t page_table);

    // returns/takes physical address of page table
    phys_addr_t arch_get_pt(void);
    void        arch_set_pt(phys_addr_t pt);

    // Optimization for mapping/unmapping large ranges of pages, to avoid redundant page table walks and cache clears
    bool arch_map_range(virt_addr_t va_start, phys_addr_t pa_start, uint64_t size, uint64_t flags, phys_addr_t page_table);
    void arch_unmap_range(virt_addr_t va_start, uint64_t size, phys_addr_t page_table);

    // Update page flags
    void arch_protect(virt_addr_t va, uint64_t flags, phys_addr_t page_table);
    void arch_protect_range(virt_addr_t va_start, uint64_t size, uint64_t flags, phys_addr_t page_table);

    // Returns physical address for a mapped virtual address, or 0 if unmapped.
    phys_addr_t arch_virt_to_phys(virt_addr_t va, phys_addr_t page_table);
    bool        arch_user_page_accessible(virt_addr_t va, phys_addr_t page_table, bool write);

    bool arch_time_init(uint64_t* realtimeSeconds, bool* realtimeAvailable, uint64_t* resolutionNanoseconds);
    bool arch_time_read_monotonic(uint64_t* nanoseconds);
#if defined(__x86_64__)
    uint64_t arch_read_user_tls_base(bool gs);
    void     arch_write_user_tls_base(bool gs, uint64_t value);
#endif
    // Isolated user roots share only kernel-half tables. Destroy after removing user leaves.
    phys_addr_t arch_create_user_page_table(void);
    void        arch_destroy_user_page_table(phys_addr_t page_table);

    typedef void (*arch_stack_entry_t)(void* arg);
    typedef void (*arch_task_entry_t)(void* arg);
    __attribute__((noreturn)) void arch_set_stack(void* stack_top, arch_stack_entry_t entry, void* arg);
    __attribute__((noreturn)) void arch_enter_user_mode(uint64_t user_rip, uint64_t user_rsp, uint64_t arg0, uint64_t arg1);
    bool arch_user_random_seed(uint64_t seed[2]);
    void                           arch_set_user_transition_stack(uint64_t kernel_rsp);
    void                           arch_init_context(struct arch_task_context* context, void* stack_top, arch_task_entry_t entry, void* arg);
    uint64_t                       arch_task_context_stack_pointer(const struct arch_task_context* context);
    void                           arch_save_switch_and_execute_context(struct arch_task_context* out_current_context, const struct arch_task_context* new_context);

    void arch_enable_interrupts();
    void arch_disable_interrupts();
    bool arch_device_init(void);

    uacpi_status arch_acpi_init(struct acpi_madt* madt);

    bool arch_init(void);

#ifdef __cplusplus
}

#endif

#endif
