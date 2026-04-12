/*
 * boot.h
 *
 * Arx Boot Protocol structures and definitions
 * 
 * - arch_entry will populate the boot_info struct and pass it to kmain
 * - Only limine bootloader currently supported
 *
 * Author: Marwan Mostafa
 *
 */

#ifndef ARX_BOOT_H
#define ARX_BOOT_H

#include <stdint.h>

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define BOOT_MEMMAP_MAX_ENTRIES 256
#define BOOT_SMP_MAX_CPUS 8

enum boot_memmap_type
{
    BOOT_MEMMAP_USABLE                 = 0,
    BOOT_MEMMAP_RESERVED               = 1,
    BOOT_MEMMAP_ACPI_RECLAIMABLE       = 2,
    BOOT_MEMMAP_ACPI_NVS               = 3,
    BOOT_MEMMAP_BAD_MEMORY             = 4,
    BOOT_MEMMAP_BOOTLOADER_RECLAIMABLE = 5,
    BOOT_MEMMAP_KERNEL_AND_MODULES     = 6,
    BOOT_MEMMAP_FRAMEBUFFER            = 7,
};

struct boot_memmap_entry
{
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct boot_smp_cpu_info
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t processor_id;
    uint32_t lapic_id;
#elif defined(__aarch64__)
    uint32_t processor_id;
    uint32_t reserved1;
    uint64_t mpidr;
#else
#error Unsupported architecture for boot_smp_cpu_info
#endif
    uint64_t  reserved;
    uintptr_t goto_address;
    uint64_t  extra_argument;
};

struct boot_smp_info
{
    uint64_t  flags;
    uint64_t  bsp_id;
    uint64_t  cpu_count;
    uintptr_t cpus;
};

struct boot_info
{
    uint64_t             limine_present;
    uint64_t             rsdp_address;
    uint64_t             kernel_start;
    uint64_t             kernel_end;
    uint64_t             hhdm_present;
    uint64_t             hhdm_offset;
    uint64_t             hhdm_start;
    uint64_t             hhdm_end;
    uint64_t             memmap_entry_count;
    uintptr_t            memmap_entries;
    uint64_t             framebuffer_addr;
    uint64_t             framebuffer_width;
    uint64_t             framebuffer_height;
    uint64_t             framebuffer_pitch;
    uint64_t             framebuffer_bpp;
    struct boot_smp_info smp;
};

void kmain(struct boot_info* boot_info, uint64_t cpu_count);
void arch_smp_init(struct boot_info* boot_info);

#endif
