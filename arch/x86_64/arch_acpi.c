#include <acpi/acpi.h>

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

        entry = (struct acpi_entry_hdr*) ((uacpi_u8*) entry + entry->length);
    }

    *out_lapic_base_addr = lapic_base_addr;
    return UACPI_STATUS_OK;
}

static uacpi_status get_lapics_from_mdat(struct acpi_madt* madt)
{
    struct acpi_entry_hdr* entry;
    uacpi_u8*              table_end;
    size_t                 lapic_count;
    size_t                 lapic_stored_count;
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
        dispatcher.cpus[i].arch_info.acpi_has_lapic       = 0;
        dispatcher.cpus[i].arch_info.acpi_lapic_base_addr = 0;
        dispatcher.cpus[i].arch_info.acpi_processor_uid   = 0;
        dispatcher.cpus[i].arch_info.acpi_lapic_id        = 0;
        dispatcher.cpus[i].arch_info.acpi_lapic_flags     = 0;
    }

    entry              = madt->entries;
    table_end          = (uacpi_u8*) madt + madt->hdr.length;
    lapic_count        = 0;
    lapic_stored_count = 0;

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

            if (lapic_stored_count < BOOT_SMP_MAX_CPUS)
            {
                dispatcher.cpus[lapic_stored_count].arch_info.acpi_has_lapic       = 1;
                dispatcher.cpus[lapic_stored_count].arch_info.acpi_lapic_base_addr = lapic_base_addr;
                dispatcher.cpus[lapic_stored_count].arch_info.acpi_processor_uid   = lapic->uid;
                dispatcher.cpus[lapic_stored_count].arch_info.acpi_lapic_id        = lapic->id;
                dispatcher.cpus[lapic_stored_count].arch_info.acpi_lapic_flags     = lapic->flags;
                lapic_stored_count++;
            }

            lapic_count++;
        }

        entry = (struct acpi_entry_hdr*) ((uacpi_u8*) entry + entry->length);
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

    while ((uacpi_u8*) entry + sizeof(*entry) <= table_end)
    {
        if (entry->length < sizeof(*entry))
        {
            *out_ioapic = NULL;
            return UACPI_STATUS_INVALID_TABLE_LENGTH;
        }

        if ((uacpi_u8*) entry + entry->length > table_end)
        {
            *out_ioapic = NULL;
            return UACPI_STATUS_INVALID_TABLE_LENGTH;
        }

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

        entry = (struct acpi_entry_hdr*) ((uacpi_u8*) entry + entry->length);
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

    dispatcher.arch_info.acpi_iso_override_count = 0;

    for (size_t i = 0; i < 16; i++)
    {
        dispatcher.arch_info.acpi_iso_overrides[i].present = 0;
        dispatcher.arch_info.acpi_iso_overrides[i].source  = 0;
        dispatcher.arch_info.acpi_iso_overrides[i].flags   = 0;
        dispatcher.arch_info.acpi_iso_overrides[i].gsi     = 0;
    }

    entry     = madt->entries;
    table_end = (uacpi_u8*) madt + madt->hdr.length;

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

                if (!dispatcher.arch_info.acpi_iso_overrides[source].present)
                {
                    dispatcher.arch_info.acpi_iso_override_count++;
                }

                dispatcher.arch_info.acpi_iso_overrides[source].present = 1;
                dispatcher.arch_info.acpi_iso_overrides[source].source  = source;
                dispatcher.arch_info.acpi_iso_overrides[source].flags   = iso->flags;
                dispatcher.arch_info.acpi_iso_overrides[source].gsi     = iso->gsi;
            }
        }

        entry = (struct acpi_entry_hdr*) ((uacpi_u8*) entry + entry->length);
    }

    return UACPI_STATUS_OK;
}

uacpi_status arch_acpi_init(struct acpi_madt* madt)
{
    struct acpi_madt_ioapic* ioapic;
    uacpi_status             status;

    status = get_lapics_from_mdat(madt);
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

    dispatcher.arch_info.acpi_has_ioapic       = 1;
    dispatcher.arch_info.acpi_ioapic_id        = ioapic->id;
    dispatcher.arch_info.acpi_ioapic_gsi_base  = ioapic->gsi_base;
    dispatcher.arch_info.acpi_ioapic_base_addr = ioapic->address;

    return UACPI_STATUS_OK;
}
