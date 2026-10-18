#ifndef ARX_PLATFORM_STATS_H
#define ARX_PLATFORM_STATS_H

#include <debug/kernel/DebugRecords.h>

#if DEBUG

#include <boot/boot.h>

struct platform;

typedef enum arx_platform_stage
{
    ARX_PLATFORM_NOT_STARTED,
    ARX_PLATFORM_BOOT_ENTERED,
    ARX_PLATFORM_CPU_TABLE_READY,
    ARX_PLATFORM_ARCH_INITIALIZING,
    ARX_PLATFORM_DEVICES_ENUMERATING,
    ARX_PLATFORM_WAITING_FOR_CPUS,
    ARX_PLATFORM_SELFTESTS,
    ARX_PLATFORM_STACK_BARRIER,
    ARX_PLATFORM_DISPATCHER_PUBLISHED,
    ARX_PLATFORM_PROVIDERS_PUBLISHED,
    ARX_PLATFORM_BSP_HANDOFF
} arx_platform_stage_t;

typedef enum arx_cpu_stage
{
    ARX_CPU_NOT_STARTED,
    ARX_CPU_ARCH_INITIALIZING,
    ARX_CPU_ARCH_INITIALIZED,
    ARX_CPU_STACK_ARRIVED
} arx_cpu_stage_t;

typedef enum arx_pci_outcome
{
    ARX_PCI_NOT_STARTED,
    ARX_PCI_ENUMERATING,
    ARX_PCI_SUCCEEDED,
    ARX_PCI_NO_REGIONS,
    ARX_PCI_FAILED
} arx_pci_outcome_t;

// v1 native contract: roots borrow platform's kernel-lifetime stored RAM.
// Counts are read from source, checked against capacity before traversal.
// Stages/counters are observations, not a coherent multi-field snapshot.
// arx.platform.cpus/1: descriptor root is arx_cpu_debug_stats_t, count=0
// (dynamic count), capacity=BOOT_SMP_MAX_CPUS. source->cpus is a contiguous
// cpu_info_t array with source->cpu_count configured slots. stages use the same
// slot indices. CPU logical IDs and hardware IDs are separate stored identities.
// NUMA/address-space links are borrowed, not owned; no task root is supplied.
// arx.platform.pci/1: root is arx_pci_debug_stats_t, count=0 (dynamic count),
// capacity=MAX_PCI_DEVICES. source->pci_devices holds source->pci_device_count
// captured pci_device_t entries in enumeration order. Segment+BDF is identity;
// array index/address locates the entry. bar_count bounds at most MAX_PCI_BARS
// register slots, not resource sizes. Never follow BAR/ECAM/APIC addresses.
// Both inventories live for the kernel lifetime; no index reuse is introduced.
typedef struct arx_cpu_debug_stats
{
    struct platform* source;
    uint64_t         capacity;
    uint64_t         tableReady;
    uint64_t         architectureInitialized;
    uint64_t         stackArrivals;
    uint64_t         stages[BOOT_SMP_MAX_CPUS];
} arx_cpu_debug_stats_t;

typedef struct arx_pci_debug_stats
{
    struct platform* source;
    uint64_t         capacity;
    uint64_t         allocated;
    uint64_t         outcome;
    uint64_t         capacityExhausted;
} arx_pci_debug_stats_t;

typedef struct arx_platform_debug_stats
{
    uint64_t              stage;
    uint64_t              dispatcherPublished;
    uint64_t              providersPublished;
    uint64_t              bspHandoff;
    arx_cpu_debug_stats_t cpus;
    arx_pci_debug_stats_t pci;
} arx_platform_debug_stats_t;

#endif
#endif
