#include <platform.h>
#include <acpi/acpi.h>
#include <uacpi/kernel_api.h>

static spinlock_t acpi_pci_lock;

void arch_acpi_flush_cpu_cache(void)
{
    __asm__ volatile("wbinvd" : : : "memory");
}

bool arch_acpi_io_read(uint16_t port, uint8_t width, uint32_t* value)
{
    uint8_t byte;
    uint16_t word;

    switch (width)
    {
        case 1:
            __asm__ volatile("inb %1, %0" : "=a"(byte) : "Nd"(port));
            *value = byte;
            return true;
        case 2:
            __asm__ volatile("inw %1, %0" : "=a"(word) : "Nd"(port));
            *value = word;
            return true;
        case 4:
            __asm__ volatile("inl %1, %0" : "=a"(*value) : "Nd"(port));
            return true;
        default:
            return false;
    }
}

bool arch_acpi_io_write(uint16_t port, uint8_t width, uint32_t value)
{
    switch (width)
    {
        case 1:
            __asm__ volatile("outb %0, %1" : : "a"((uint8_t) value), "Nd"(port));
            return true;
        case 2:
            __asm__ volatile("outw %0, %1" : : "a"((uint16_t) value), "Nd"(port));
            return true;
        case 4:
            __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
            return true;
        default:
            return false;
    }
}

bool arch_acpi_pci_access(uint16_t segment, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t width, bool write, uint32_t* value)
{
    if (device >= 32 || function >= 8 || (width != 1 && width != 2 && width != 4) || offset > 4096 - width || (offset & (width - 1)) != 0 || value == NULL)
    {
        return false;
    }

    // ECAM is available before PCI enumeration. Direct UC aliases are retained
    // exactly like the PCI backend, avoiding conflicting cache attributes.
    uacpi_table table;

    if (uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &table) == UACPI_STATUS_OK)
    {
        struct acpi_mcfg* mcfg = (struct acpi_mcfg*) table.hdr;
        size_t count = 0;

        if (table.hdr->length >= sizeof(*mcfg))
        {
            count = (table.hdr->length - sizeof(*mcfg)) / sizeof(struct acpi_mcfg_allocation);
        }

        for (size_t i = 0; i < count; i++)
        {
            struct acpi_mcfg_allocation* allocation = &mcfg->entries[i];

            if (allocation->segment != segment || bus < allocation->start_bus || bus > allocation->end_bus)
            {
                continue;
            }

            uint64_t displacement = ((uint64_t) bus << 20) | ((uint64_t) device << 15) | ((uint64_t) function << 12);
            uint64_t physical = allocation->address + displacement;
            uacpi_table_unref(&table);

            if (physical < displacement || (physical & (PAGE_SIZE - 1)) != 0)
            {
                return false;
            }

            virt_addr_t mapped = vmm_map_direct_mmio_page(physical, platform.cpus[arch_cpu_id()].address_space);

            if (mapped == 0)
            {
                return false;
            }

            volatile uint8_t* address = (volatile uint8_t*) (mapped + offset);

            if (write)
            {
                if (width == 1)
                {
                    *(volatile uint8_t*) address = (uint8_t) *value;
                }
                else if (width == 2)
                {
                    *(volatile uint16_t*) address = (uint16_t) *value;
                }
                else
                {
                    *(volatile uint32_t*) address = *value;
                }
            }
            else if (width == 1)
            {
                *value = *(volatile uint8_t*) address;
            }
            else if (width == 2)
            {
                *value = *(volatile uint16_t*) address;
            }
            else
            {
                *value = *(volatile uint32_t*) address;
            }

            return true;
        }

        uacpi_table_unref(&table);
    }

    if (segment != 0 || offset > 256 - width)
    {
        return false;
    }

    uint64_t interrupt_state = arch_save_and_disable_interrupts();
    spinlock_acquire(&acpi_pci_lock);
    uint32_t selector = 0x80000000u | ((uint32_t) bus << 16) | ((uint32_t) device << 11) | ((uint32_t) function << 8) | (offset & ~3u);
    arch_acpi_io_write(0xcf8, 4, selector);
    bool success;

    if (write)
    {
        success = arch_acpi_io_write(0xcfc + (offset & 3), width, *value);
    }
    else
    {
        success = arch_acpi_io_read(0xcfc + (offset & 3), width, value);
    }

    spinlock_release(&acpi_pci_lock);
    arch_restore_interrupts(interrupt_state);
    return success;
}

uint32_t arch_acpi_route_interrupt(uint32_t irq)
{
    uint32_t gsi = irq;
    uint16_t flags = ACPI_MADT_POLARITY_ACTIVE_LOW | ACPI_MADT_TRIGGERING_LEVEL;

    if (irq < LEGACY_PIC_MAX_IRQS && platform.arch_info.acpi_iso_overrides[irq].present)
    {
        gsi = platform.arch_info.acpi_iso_overrides[irq].gsi;
        flags = platform.arch_info.acpi_iso_overrides[irq].flags;

        if ((flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_CONFORMING)
        {
            flags |= ACPI_MADT_POLARITY_ACTIVE_LOW;
        }

        if ((flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_CONFORMING)
        {
            flags |= ACPI_MADT_TRIGGERING_LEVEL;
        }
    }

    uint64_t state = arch_save_and_disable_interrupts();
    uint32_t vector = ioapic_register_device_flags(gsi, flags);
    arch_restore_interrupts(state);
    return vector;
}

void arch_acpi_mask_interrupt(uint32_t vector, bool masked)
{
    uint64_t state = arch_save_and_disable_interrupts();

    if (masked)
    {
        ioapic_mask_vector((uint8_t) vector);
    }
    else
    {
        ioapic_unmask_vector((uint8_t) vector);
    }

    arch_restore_interrupts(state);
}

// Advances only across records whose generic header fits in the table.
static uacpi_status madt_next_entry(uacpi_u8** cursor, uacpi_u8* table_end, struct acpi_entry_hdr** out_entry)
{
    if (*cursor + sizeof(struct acpi_entry_hdr) > table_end)
    {
        return UACPI_STATUS_NOT_FOUND;
    }

    struct acpi_entry_hdr* entry = (struct acpi_entry_hdr*) *cursor;

    if (entry->length < sizeof(*entry) || *cursor + entry->length > table_end)
    {
        return UACPI_STATUS_INVALID_TABLE_LENGTH;
    }

    *out_entry = entry;
    *cursor += entry->length;
    return UACPI_STATUS_OK;
}

static uacpi_status get_lapic_base_addr(struct acpi_madt* madt, uint64_t* out_lapic_base_addr)
{
    struct acpi_entry_hdr* entry;
    uacpi_u8*              table_end;
    uint64_t               lapic_base_addr;

    if (madt == NULL || out_lapic_base_addr == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (madt->hdr.length < sizeof(*madt))
    {
        return UACPI_STATUS_INVALID_TABLE_LENGTH;
    }

    lapic_base_addr = madt->local_interrupt_controller_address;
    entry           = madt->entries;
    table_end       = (uacpi_u8*) madt + madt->hdr.length;

    uacpi_u8*    cursor = (uacpi_u8*) entry;
    uacpi_status walk_status;

    while ((walk_status = madt_next_entry(&cursor, table_end, &entry)) == UACPI_STATUS_OK)
    {
        if (entry->type == ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE)
        {
            struct acpi_madt_lapic_address_override* lapic_override;

            if (entry->length < sizeof(struct acpi_madt_lapic_address_override))
            {
                return UACPI_STATUS_INVALID_TABLE_LENGTH;
            }

            lapic_override  = (struct acpi_madt_lapic_address_override*) entry;
            lapic_base_addr = lapic_override->address;
        }
    }

    if (walk_status != UACPI_STATUS_NOT_FOUND)
    {
        return walk_status;
    }

    *out_lapic_base_addr = lapic_base_addr;
    return UACPI_STATUS_OK;
}

static uacpi_status get_lapics_from_madt(struct acpi_madt* madt)
{
    struct acpi_entry_hdr* entry;
    uacpi_u8*              table_end;
    size_t                 lapic_count;
    uint64_t               lapic_base_addr;
    uacpi_status           status;

    if (madt == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    status = get_lapic_base_addr(madt, &lapic_base_addr);

    if (status != UACPI_STATUS_OK)
    {
        return status;
    }

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        platform.cpus[i].arch_info.acpi_has_lapic       = 0;
        platform.cpus[i].arch_info.acpi_lapic_base_addr = 0;
        platform.cpus[i].arch_info.acpi_processor_uid   = 0;
        platform.cpus[i].arch_info.acpi_lapic_id        = 0;
        platform.cpus[i].arch_info.acpi_lapic_flags     = 0;
    }

    entry       = madt->entries;
    table_end   = (uacpi_u8*) madt + madt->hdr.length;
    lapic_count = 0;

    uacpi_u8*    cursor = (uacpi_u8*) entry;
    uacpi_status walk_status;

    while ((walk_status = madt_next_entry(&cursor, table_end, &entry)) == UACPI_STATUS_OK)
    {
        if (entry->type == ACPI_MADT_ENTRY_TYPE_LAPIC)
        {
            struct acpi_madt_lapic* lapic;

            if (entry->length < sizeof(struct acpi_madt_lapic))
            {
                return UACPI_STATUS_INVALID_TABLE_LENGTH;
            }

            lapic = (struct acpi_madt_lapic*) entry;

            for (size_t logical = 0; logical < platform.cpu_count; ++logical)
            {
                if (platform.cpus[logical].hardware_id != lapic->id)
                {
                    continue;
                }

                platform.cpus[logical].arch_info.acpi_has_lapic       = 1;
                platform.cpus[logical].arch_info.acpi_lapic_base_addr = lapic_base_addr;
                platform.cpus[logical].arch_info.acpi_processor_uid   = lapic->uid;
                platform.cpus[logical].arch_info.acpi_lapic_id        = lapic->id;
                platform.cpus[logical].arch_info.acpi_lapic_flags     = lapic->flags;
            }

            lapic_count++;
        }
    }

    if (walk_status != UACPI_STATUS_NOT_FOUND)
    {
        return walk_status;
    }

    if (lapic_count == 0)
    {
        return UACPI_STATUS_NOT_FOUND;
    }

    return UACPI_STATUS_OK;
}

static uacpi_status get_ioapic_from_madt(struct acpi_madt* madt, struct acpi_madt_ioapic** out_ioapic)
{
    struct acpi_entry_hdr* entry;
    uacpi_u8*              table_end;

    if (madt == NULL || out_ioapic == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (madt->hdr.length < sizeof(*madt))
    {
        *out_ioapic = NULL;
        return UACPI_STATUS_INVALID_TABLE_LENGTH;
    }

    entry     = madt->entries;
    table_end = (uacpi_u8*) madt + madt->hdr.length;

    uacpi_u8*    cursor = (uacpi_u8*) entry;
    uacpi_status walk_status;

    while ((walk_status = madt_next_entry(&cursor, table_end, &entry)) == UACPI_STATUS_OK)
    {
        if (entry->type == ACPI_MADT_ENTRY_TYPE_IOAPIC)
        {
            if (entry->length < sizeof(struct acpi_madt_ioapic))
            {
                *out_ioapic = NULL;
                return UACPI_STATUS_INVALID_TABLE_LENGTH;
            }

            *out_ioapic = (struct acpi_madt_ioapic*) entry;
            return UACPI_STATUS_OK;
        }
    }

    if (walk_status != UACPI_STATUS_NOT_FOUND)
    {
        *out_ioapic = NULL;
        return walk_status;
    }

    *out_ioapic = NULL;
    return UACPI_STATUS_NOT_FOUND;
}

static uacpi_status get_iso_overrides_from_madt(struct acpi_madt* madt)
{
    struct acpi_entry_hdr* entry;
    uacpi_u8*              table_end;

    if (madt == NULL)
    {
        return UACPI_STATUS_INVALID_ARGUMENT;
    }

    if (madt->hdr.length < sizeof(*madt))
    {
        return UACPI_STATUS_INVALID_TABLE_LENGTH;
    }

    platform.arch_info.acpi_iso_override_count = 0;

    for (size_t i = 0; i < 16; i++)
    {
        platform.arch_info.acpi_iso_overrides[i].present = 0;
        platform.arch_info.acpi_iso_overrides[i].source  = 0;
        platform.arch_info.acpi_iso_overrides[i].flags   = 0;
        platform.arch_info.acpi_iso_overrides[i].gsi     = 0;
    }

    entry     = madt->entries;
    table_end = (uacpi_u8*) madt + madt->hdr.length;

    uacpi_u8*    cursor = (uacpi_u8*) entry;
    uacpi_status walk_status;

    while ((walk_status = madt_next_entry(&cursor, table_end, &entry)) == UACPI_STATUS_OK)
    {
        if (entry->type == ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE)
        {
            struct acpi_madt_interrupt_source_override* iso;

            if (entry->length < sizeof(struct acpi_madt_interrupt_source_override))
            {
                return UACPI_STATUS_INVALID_TABLE_LENGTH;
            }

            iso = (struct acpi_madt_interrupt_source_override*) entry;

            if (iso->bus == 0 && iso->source < 16)
            {
                uint8_t source = iso->source;

                if (!platform.arch_info.acpi_iso_overrides[source].present)
                {
                    platform.arch_info.acpi_iso_override_count++;
                }

                platform.arch_info.acpi_iso_overrides[source].present = 1;
                platform.arch_info.acpi_iso_overrides[source].source  = source;
                platform.arch_info.acpi_iso_overrides[source].flags   = iso->flags;
                platform.arch_info.acpi_iso_overrides[source].gsi     = iso->gsi;
            }
        }
    }

    if (walk_status != UACPI_STATUS_NOT_FOUND)
    {
        return walk_status;
    }

    return UACPI_STATUS_OK;
}

uacpi_status arch_acpi_init(struct acpi_madt* madt)
{
    struct acpi_madt_ioapic* ioapic;
    uacpi_status             status;

    status = get_lapics_from_madt(madt);

    if (status != UACPI_STATUS_OK)
    {
        return status;
    }

    status = get_ioapic_from_madt(madt, &ioapic);

    if (status != UACPI_STATUS_OK)
    {
        return status;
    }

    status = get_iso_overrides_from_madt(madt);

    if (status != UACPI_STATUS_OK)
    {
        return status;
    }

    platform.arch_info.acpi_has_ioapic       = 1;
    platform.arch_info.acpi_ioapic_id        = ioapic->id;
    platform.arch_info.acpi_ioapic_gsi_base  = ioapic->gsi_base;
    platform.arch_info.acpi_ioapic_base_addr = ioapic->address;

    return UACPI_STATUS_OK;
}
