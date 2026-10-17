#ifndef NUMA_H
#define NUMA_H

#include <boot/boot.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NUMA_MAX_MEMORY_RANGES 128

typedef struct numa_memory_range
{
    uint64_t start;
    uint64_t end;
    uint32_t node;
} numa_memory_range_t;

typedef struct numa_topology
{
    uint32_t domains[MAX_NUMA_NODES];
    uint32_t cpu_nodes[BOOT_SMP_MAX_CPUS];
    size_t node_count;
    size_t range_count;
    numa_memory_range_t ranges[NUMA_MAX_MEMORY_RANGES];
} numa_topology_t;

bool numa_parse_srat(const void* table, size_t length, const uint32_t* hardware_ids, size_t cpu_count, numa_topology_t* topology);
bool numa_boot_topology(const struct boot_info* boot, numa_topology_t* topology);

#endif
