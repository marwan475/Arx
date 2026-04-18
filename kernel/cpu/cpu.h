#ifndef CPU_H
#define CPU_H

#include <arch/arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SPINLOCK_T_DEFINED
#define SPINLOCK_T_DEFINED
typedef _Atomic uint8_t spinlock_t;
#endif

typedef struct numa_node       numa_node_t;
typedef struct virt_addr_space virt_addr_space_t;

typedef enum ipi_request_type
{
    IPI_REQUEST_NONE,
    IPI_REQUEST_RESCHEDULE,
    IPI_REQUEST_INVALIDATE_TLB,
} ipi_request_type_t;

typedef enum ipi_tlb_invalidation_type
{
    IPI_TLB_INVALIDATE_SINGLE_PAGE,
    IPI_TLB_INVALIDATE_RANGE,
} ipi_tlb_invalidation_type_t;

typedef struct ipi_tlb_invalidation_data
{
    phys_addr_t                  page_table;
    virt_addr_t                  va_start;
    uint64_t                     size;
    bool                         requires_page_flush;
    ipi_tlb_invalidation_type_t  tlb_invalidation_type;
} ipi_tlb_invalidation_data_t;

typedef struct ipi_request_data
{
    ipi_request_type_t type;
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

} cpu_info_t;

void cpus_init(size_t cpu_count);

#endif