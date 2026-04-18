#ifndef CPU_H
#define CPU_H

#include <arch/arch.h>
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

typedef struct cpu_info
{
    uint8_t            id;
    bool               initialized;
    numa_node_t*       numa_node;
    virt_addr_space_t* address_space;
    arch_info_t        arch_info;
    ipi_request_type_t ipi_request;
    spinlock_t         ipi_lock;

} cpu_info_t;

void cpus_init(size_t cpu_count);

#endif