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

#include <stdint.h>
#include <arch/arch.h>

typedef struct numa_node       numa_node_t;
typedef struct virt_addr_space virt_addr_space_t;

typedef struct cpu_info
{
    uint8_t            id;
    numa_node_t*       numa_node;
    virt_addr_space_t* address_space;
    arch_info_t       arch_info;
} cpu_info_t;

#endif