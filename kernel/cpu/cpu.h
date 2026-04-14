/*
 * cpu.h
 *
 * Per cpu/core information
 *
 * - Provides structures to store per cpu/core information
 *
 * Author: Marwan Mostafa
 *
 */

#ifndef CPU_H
#define CPU_H

#include <arch/arch.h>
#include <stdint.h>

typedef struct numa_node       numa_node_t;
typedef struct virt_addr_space virt_addr_space_t;

typedef struct cpu_info
{
    uint8_t            id;
    uint8_t            acpi_has_lapic;
    numa_node_t*       numa_node;
    virt_addr_space_t* address_space;
    uint32_t           acpi_processor_uid;
    uint32_t           acpi_lapic_id;
    uint32_t           acpi_lapic_flags;
    arch_info_t        arch_info;
} cpu_info_t;

void cpus_init(size_t cpu_count);

#endif