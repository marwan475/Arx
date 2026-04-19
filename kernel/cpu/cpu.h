#ifndef CPU_H
#define CPU_H

#include <arch/arch.h>
#include <klib/spinlock.h>
#include <memory/pmm.h>
#include <memory/heap.h>
#include <terminal/terminal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CPU_KERNEL_STACK_SIZE (16 * PAGE_SIZE) //64 kb

typedef struct numa_node       numa_node_t;
typedef struct virt_addr_space virt_addr_space_t;
struct flanterm_context;

typedef enum arch_type
{
    ARCH_X86_64,
    ARCH_AARCH64,
} arch_type_t;

typedef enum ipi_request_type
{
    IPI_REQUEST_NONE,
    IPI_REQUEST_RESCHEDULE,
    IPI_REQUEST_INVALIDATE_TLB,
    IPI_REQUEST_EXCEPTION,
} ipi_request_type_t;

typedef enum ipi_tlb_invalidation_type
{
    IPI_TLB_INVALIDATE_SINGLE_PAGE,
    IPI_TLB_INVALIDATE_RANGE,
} ipi_tlb_invalidation_type_t;

typedef struct ipi_tlb_invalidation_data
{
    virt_addr_t                 va_start;
    uint64_t                    size;
    bool                        requires_page_flush;
    ipi_tlb_invalidation_type_t tlb_invalidation_type;
} ipi_tlb_invalidation_data_t;

typedef struct ipi_request_data
{
    ipi_request_type_t          type;
    ipi_tlb_invalidation_data_t tlb_invalidation;
} ipi_request_data_t;

typedef struct cpu_info
{
    uint8_t            id;
    bool               initialized;
    numa_node_t*       numa_node;
    virt_addr_space_t* address_space;
    arch_info_t        arch_info;
    ipi_request_data_t ipi_request_data;
    spinlock_t         ipi_lock;
    void*              kernel_stack_base;
    size_t             kernel_stack_size;
} cpu_info_t;

typedef struct dispatcher
{
    cpu_info_t*              cpus;
    size_t                   cpu_count;
    uint8_t                  cpus_initialized;
    numa_node_t              numa_nodes[MAX_NUMA_NODES];
    size_t                   numa_node_count;
    kernel_framebuffer_t     framebuffer;
    uint32_t                 vector_base;
    struct flanterm_context* terminal_context;
    spinlock_t               terminal_lock;
    arch_type_t              arch;
    arch_dispatcher_info_t   arch_info;
    kernel_heap_t            heap;
} dispatcher_t;

extern dispatcher_t dispatcher;

void cpus_init(size_t cpu_count);
__attribute__((noreturn)) void cpu_init_stack(arch_stack_entry_t entry, void* arg);

#endif