#ifndef PLATFORM_H
#define PLATFORM_H

#include <arch/arch.h>
#include <debug/kernel/stats/PlatformStats.h>
#include <device/device.h>
#include <dma/dma.h>
#include <klib/spinlock.h>
#include <memory/pmm.h>
#include <stddef.h>
#include <stdint.h>
#include <terminal/terminal.h>

typedef struct cpu_info cpu_info_t;
struct flanterm_context;

typedef enum arch_type
{
    ARCH_X86_64,
    ARCH_AARCH64,
} arch_type_t;

typedef struct platform
{
    cpu_info_t*              cpus;
    size_t                   cpu_count;
    uint8_t                  cpus_initialized;
    volatile uint8_t         bsp_kmain_exited;
    uint64_t                 bsp_id;
    numa_node_t              numa_nodes[MAX_NUMA_NODES];
    size_t                   numa_node_count;
    kernel_framebuffer_t     framebuffer;
    uint32_t                 vector_base;
    struct flanterm_context* terminal_context;
    spinlock_t               terminal_lock;
    uintptr_t                initramfs_address;
    uint64_t                 initramfs_size;
    void*                    dispacher;
    arch_type_t              arch;
    pci_device_t*            pci_devices;
    size_t                   pci_device_count;
    arch_platform_info_t     arch_info;
    platform_dma_ops_t       dma_ops;
    ARX_DEBUG_ONLY(arx_platform_debug_stats_t debugStats;)
    ARX_DEBUG_ONLY(arx_pmm_debug_stats_t debugPmm;)
    ARX_DEBUG_ONLY(arx_heap_debug_stats_t debugHeap;)
} platform_t;

extern platform_t platform;

#endif
