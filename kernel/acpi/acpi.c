/*
 * acpi.c
 *
 * ACPI (Advanced Configuration and Power Interface) implementation
 *
 * - uACPI port
 *
 * Author: Marwan Mostafa
 *
 */

#include <acpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/log.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>

#define ACPI_EARLY_TABLE_BUFFER_SIZE 4096

uacpi_phys_addr uacpi_rsdp_address;
void*           uacpi_early_table_buffer;

void acpi_init(phys_addr_t rsdp_address)
{
    uacpi_status status;
    struct acpi_madt* madt;
    uacpi_table       madt_table;

    if (dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present && rsdp_address >= dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset)
    {
        rsdp_address = hhdm_to_pa(rsdp_address, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_present, dispatcher.cpus[arch_cpu_id()].numa_node->zone.hhdm_offset);
    }

    uacpi_rsdp_address = (uacpi_phys_addr) rsdp_address;

    uacpi_early_table_buffer = pmm_alloc(ACPI_EARLY_TABLE_BUFFER_SIZE);

    status = uacpi_setup_early_table_access(uacpi_early_table_buffer, ACPI_EARLY_TABLE_BUFFER_SIZE);
    if (status != UACPI_STATUS_OK)
    {
        kprintf("ACPI: uACPI early init failed: %s (%u)\n", uacpi_status_to_string(status), (unsigned) status);
        return;
    }

    kprintf("ACPI: uACPI barebones early table access initialized\n");

    status = acpi_get_madt(&madt, &madt_table);
    if (status != UACPI_STATUS_OK)
    {
        kprintf("ACPI: failed to find MADT: %s (%u)\n", uacpi_status_to_string(status), (unsigned) status);
        return;
    }

    status = get_lapics_from_mdat(madt);
    uacpi_table_unref(&madt_table);
    if (status != UACPI_STATUS_OK)
    {
        kprintf("ACPI: failed to parse MADT LAPIC entries: %s (%u)\n", uacpi_status_to_string(status), (unsigned) status);
        return;
    }

    kprintf("ACPI: MADT LAPIC entries loaded into dispatcher\n");
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
    uint64_t aligned_pa  = align_down((uint64_t) addr, PAGE_SIZE);
    uint64_t offset      = (uint64_t) addr - aligned_pa;
    uint64_t aligned_len = align_up((uint64_t) len + offset, PAGE_SIZE);

    virt_addr_t va = vmm_reserve_region(dispatcher.cpus[arch_cpu_id()].address_space, aligned_len, VIRT_ADDR_KERNEL);
    if (va == 0)
    {
        return NULL;
    }

    uint64_t map_flags = 0;
    ARCH_PAGE_FLAGS_INIT(map_flags);
    ARCH_PAGE_FLAG_SET_READ(map_flags);
    ARCH_PAGE_FLAG_SET_WRITE(map_flags);

    vmm_map_range(va, aligned_pa, aligned_len, map_flags, dispatcher.cpus[arch_cpu_id()].address_space);
    return (void*) (va + offset);
}

void uacpi_kernel_unmap(void* addr, uacpi_size len)
{
    uint64_t aligned_va  = align_down((uint64_t) addr, PAGE_SIZE);
    uint64_t offset      = (uint64_t) addr - aligned_va;
    uint64_t aligned_len = align_up((uint64_t) len + offset, PAGE_SIZE);

    vmm_unmap_range((virt_addr_t) aligned_va, aligned_len, dispatcher.cpus[arch_cpu_id()].address_space);
    vmm_free_region(dispatcher.cpus[arch_cpu_id()].address_space, (virt_addr_t) aligned_va);
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char* text)
{
    kprintf("uACPI[%u]: %s", (unsigned) level, (const char*) text);
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

uacpi_status get_lapics_from_mdat(struct acpi_madt* madt)
{
    struct acpi_entry_hdr* entry;
    uacpi_u8*              table_end;
    size_t                 lapic_count;

    if (madt == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        dispatcher.cpus[i].acpi_has_lapic     = 0;
        dispatcher.cpus[i].acpi_processor_uid = 0;
        dispatcher.cpus[i].acpi_lapic_id      = 0;
        dispatcher.cpus[i].acpi_lapic_flags   = 0;
    }

    if (madt->hdr.length < sizeof(*madt))
    {
        return UACPI_STATUS_INVALID_TABLE_LENGTH;
    }

    entry       = madt->entries;
    table_end   = (uacpi_u8*) madt + madt->hdr.length;
    lapic_count = 0;

    while ((uacpi_u8*) entry + sizeof(*entry) <= table_end)
    {
        if (entry->length < sizeof(*entry))
        {
            return UACPI_STATUS_INVALID_TABLE_LENGTH;
        }

        if ((uacpi_u8*) entry + entry->length > table_end)
        {
            return UACPI_STATUS_INVALID_TABLE_LENGTH;
        }

        if (entry->type == ACPI_MADT_ENTRY_TYPE_LAPIC)
        {
            struct acpi_madt_lapic* lapic;

            if (entry->length < sizeof(struct acpi_madt_lapic))
            {
                return UACPI_STATUS_INVALID_TABLE_LENGTH;
            }

            lapic = (struct acpi_madt_lapic*) entry;

            if (lapic_count < BOOT_SMP_MAX_CPUS)
            {
                dispatcher.cpus[lapic_count].acpi_has_lapic     = 1;
                dispatcher.cpus[lapic_count].acpi_processor_uid = lapic->uid;
                dispatcher.cpus[lapic_count].acpi_lapic_id      = lapic->id;
                dispatcher.cpus[lapic_count].acpi_lapic_flags   = lapic->flags;
            }

            lapic_count++;
        }

        entry = (struct acpi_entry_hdr*) ((uacpi_u8*) entry + entry->length);
    }

    if (lapic_count == 0)
    {
        return UACPI_STATUS_NOT_FOUND;
    }

    if (lapic_count > BOOT_SMP_MAX_CPUS)
    {
        return UACPI_STATUS_OUT_OF_MEMORY;
    }

    return UACPI_STATUS_OK;
}