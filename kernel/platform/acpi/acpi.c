#include <acpi/acpi.h>
#include <platform.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>

#define ACPI_EARLY_TABLE_BUFFER_SIZE 4096

uacpi_phys_addr uacpi_rsdp_address;
void*           uacpi_early_table_buffer;
static struct boot_memmap_entry acpi_memory_map[BOOT_MEMMAP_MAX_ENTRIES];
static size_t acpi_memory_map_count;

static bool acpi_page_is_ram(uint64_t physical)
{
    for (size_t i = 0; i < acpi_memory_map_count; i++)
    {
        const struct boot_memmap_entry* entry = &acpi_memory_map[i];
        bool ram = entry->type == BOOT_MEMMAP_USABLE || entry->type == BOOT_MEMMAP_ACPI_RECLAIMABLE ||
                   entry->type == BOOT_MEMMAP_ACPI_NVS || entry->type == BOOT_MEMMAP_BOOTLOADER_RECLAIMABLE ||
                   entry->type == BOOT_MEMMAP_KERNEL_AND_MODULES;

        if (ram && physical >= entry->base && physical - entry->base <= entry->length && PAGE_SIZE <= entry->length - (physical - entry->base))
        {
            return true;
        }
    }

    return false;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out_rsdp_address)
{
    if (uacpi_rsdp_address == 0)
    {
        *out_rsdp_address = 0;
        return UACPI_STATUS_NOT_FOUND;
    }

    *out_rsdp_address = uacpi_rsdp_address;
    return UACPI_STATUS_OK;
}

void* uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len)
{
    if (len == 0 || (uint64_t) len > UINT64_MAX - (addr & (PAGE_SIZE - 1)) - (PAGE_SIZE - 1) || addr > UINT64_MAX - len)
    {
        return NULL;
    }

    uint64_t aligned_pa  = align_down((uint64_t) addr, PAGE_SIZE);
    uint64_t offset      = (uint64_t) addr - aligned_pa;
    uint64_t aligned_len = align_up((uint64_t) len + offset, PAGE_SIZE);

    virt_addr_t va = vmm_reserve_region(platform.cpus[arch_cpu_id()].address_space, aligned_len, VIRT_ADDR_KERNEL);

    if (va == 0)
    {
        return NULL;
    }

    virt_addr_space_t* space = vmm_kernel_space();
    bool mapped = true;

    for (uint64_t page = 0; page < aligned_len; page += PAGE_SIZE)
    {
        bool uncached = !acpi_page_is_ram(aligned_pa + page);

        if (uncached && vmm_map_direct_mmio_page(aligned_pa + page, space) == 0)
        {
            mapped = false;
            break;
        }

        if (!vmm_map_range(va + page, aligned_pa + page, PAGE_SIZE, vmm_kernel_page_flags(uncached, false), space))
        {
            mapped = false;
            break;
        }
    }

    if (!mapped)
    {
        vmm_unmap_range(va, aligned_len, platform.cpus[arch_cpu_id()].address_space);
        vmm_free_region(platform.cpus[arch_cpu_id()].address_space, va);
        return NULL;
    }
    return (void*) (va + offset);
}

void uacpi_kernel_unmap(void* addr, uacpi_size len)
{
    if (addr == NULL || len == 0)
    {
        return;
    }

    uint64_t aligned_va  = align_down((uint64_t) addr, PAGE_SIZE);
    uint64_t offset      = (uint64_t) addr - aligned_va;
    uint64_t aligned_len = align_up((uint64_t) len + offset, PAGE_SIZE);

    vmm_unmap_range((virt_addr_t) aligned_va, aligned_len, platform.cpus[arch_cpu_id()].address_space);
    vmm_free_region(platform.cpus[arch_cpu_id()].address_space, (virt_addr_t) aligned_va);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* text)
{
    if (level == UACPI_LOG_ERROR || level == UACPI_LOG_WARN)
    {
        error_logger("uACPI[%u]: %s", (unsigned) level, (const char*) text);
    }
    else
    {
        kprintf("uACPI[%u]: %s", (unsigned) level, (const char*) text);
    }
}

uacpi_status acpi_get_madt(struct acpi_madt** out_madt, uacpi_table* out_table)
{
    uacpi_status status;

    if (out_madt == NULL || out_table == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    status = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, out_table);

    if (status != UACPI_STATUS_OK)
    {
        *out_madt = NULL;
        return status;
    }

    *out_madt = (struct acpi_madt*) out_table->hdr;
    return UACPI_STATUS_OK;
}

uacpi_status acpi_get_mcfg(struct acpi_mcfg** out_mcfg, uacpi_table* out_table)
{
    uacpi_status status;

    if (out_mcfg == NULL || out_table == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    status = uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, out_table);

    if (status != UACPI_STATUS_OK)
    {
        *out_mcfg = NULL;
        return status;
    }

    *out_mcfg = (struct acpi_mcfg*) out_table->hdr;
    return UACPI_STATUS_OK;
}

void acpi_init(phys_addr_t rsdp_address, const struct boot_info* boot)
{
    uacpi_status status;

    acpi_memory_map_count = boot->memmap_entry_count;

    if (acpi_memory_map_count > BOOT_MEMMAP_MAX_ENTRIES)
    {
        acpi_memory_map_count = BOOT_MEMMAP_MAX_ENTRIES;
    }

    memcpy(acpi_memory_map, (const void*) boot->memmap_entries, acpi_memory_map_count * sizeof(acpi_memory_map[0]));

    if (platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_present && rsdp_address >= platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset)
    {
        rsdp_address = hhdm_to_pa(rsdp_address, platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, platform.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
    }

    uacpi_rsdp_address = (uacpi_phys_addr) rsdp_address;

    uacpi_early_table_buffer = pmm_alloc(ACPI_EARLY_TABLE_BUFFER_SIZE);

    status = uacpi_setup_early_table_access(uacpi_early_table_buffer, ACPI_EARLY_TABLE_BUFFER_SIZE);

    if (status != UACPI_STATUS_OK)
    {
        error_logger("ACPI: uACPI early init failed: %s (%u)\n", uacpi_status_to_string(status), (unsigned) status);
        panic();
    }

    kprintf("ACPI: uACPI early table access initialized\n");
}
