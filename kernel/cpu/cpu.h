#ifndef CPU_H
#define CPU_H

#include <stdint.h>

typedef struct numa_node numa_node_t;
typedef struct virt_addr_space virt_addr_space_t;

typedef struct cpu_info
{
    uint8_t id;
    numa_node_t* numa_node;
    virt_addr_space_t* address_space;
} cpu_info_t;

#endif