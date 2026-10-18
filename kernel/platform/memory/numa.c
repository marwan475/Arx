#include <klib/klib.h>
#include <memory/numa.h>
#include <platform.h>
#include <uacpi/acpi.h>

static bool checksum_valid(const void* pointer, size_t length)
{
    const uint8_t* bytes = pointer;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i)
    {
        sum += bytes[i];
    }
    return sum == 0;
}

static int domain_node(numa_topology_t* topology, uint32_t domain)
{
    for (size_t i = 0; i < topology->node_count; ++i)
    {
        if (topology->domains[i] == domain)
        {
            return i;
        }
    }
    if (topology->node_count == MAX_NUMA_NODES)
    {
        return -1;
    }
    topology->domains[topology->node_count] = domain;
    return topology->node_count++;
}

bool numa_parse_srat(const void* table, size_t length, const uint32_t* hardware_ids, size_t cpu_count, numa_topology_t* topology)
{
    if (table == NULL || topology == NULL || hardware_ids == NULL || cpu_count > BOOT_SMP_MAX_CPUS || length < sizeof(struct acpi_srat))
    {
        return false;
    }

    const struct acpi_srat* srat = table;

    if (memcmp(srat->hdr.signature, "SRAT", 4) != 0 || srat->hdr.length != length || !checksum_valid(table, length))
    {
        return false;
    }

    memset(topology, 0, sizeof(*topology));

    for (size_t i = 0; i < cpu_count; ++i)
    {
        topology->cpu_nodes[i] = UINT32_MAX;
    }

    for (size_t offset = sizeof(*srat); offset < length;)
    {
        if (length - offset < 2)
        {
            return false;
        }

        const uint8_t* entry = (const uint8_t*) table + offset;
        const uint8_t type = entry[0], size = entry[1];

        if (size < 2 || size > length - offset)
        {
            return false;
        }

        if (type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY)
        {
            if (size != sizeof(struct acpi_srat_memory_affinity))
            {
                return false;
            }

            const struct acpi_srat_memory_affinity* memory = (const void*) entry;

            if ((memory->flags & ACPI_SRAT_MEMORY_ENABLED) != 0 && memory->length != 0)
            {
                // Persistent memory and hotplug capacity are not allocatable RAM.
                if ((memory->flags & (ACPI_SRAT_MEMORY_HOTPLUGGABLE | ACPI_SRAT_MEMORY_NON_VOLATILE)) != 0 || memory->address > UINT64_MAX - memory->length || ((memory->address | memory->length) & (PAGE_SIZE - 1)) != 0 || topology->range_count == NUMA_MAX_MEMORY_RANGES)
                {
                    return false;
                }

                const int node = domain_node(topology, memory->proximity_domain);

                if (node < 0)
                {
                    return false;
                }

                const uint64_t end = memory->address + memory->length;

                for (size_t i = 0; i < topology->range_count; ++i)
                {
                    if (memory->address < topology->ranges[i].end && end > topology->ranges[i].start)
                    {
                        return false;
                    }
                }

                topology->ranges[topology->range_count++] = (numa_memory_range_t) {memory->address, end, node};
            }
        }
        else if (type == ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY || type == ACPI_SRAT_ENTRY_TYPE_X2APIC_AFFINITY)
        {
            uint32_t domain, id, flags;

            if (type == ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY)
            {
                if (size != sizeof(struct acpi_srat_processor_affinity))
                {
                    return false;
                }

                const struct acpi_srat_processor_affinity* cpu = (const void*) entry;
                domain = cpu->proximity_domain_low;

                if (srat->hdr.revision >= 2)
                {
                    domain |= (uint32_t) cpu->proximity_domain_high[0] << 8 | (uint32_t) cpu->proximity_domain_high[1] << 16 | (uint32_t) cpu->proximity_domain_high[2] << 24;
                }

                id = cpu->id;
                flags = cpu->flags;
            }
            else
            {
                if (size != sizeof(struct acpi_srat_x2apic_affinity))
                {
                    return false;
                }

                const struct acpi_srat_x2apic_affinity* cpu = (const void*) entry;
                domain = cpu->proximity_domain;
                id = cpu->id;
                flags = cpu->flags;
            }

            if ((flags & ACPI_SRAT_PROCESSOR_ENABLED) != 0)
            {
                for (size_t i = 0; i < cpu_count; ++i)
                {
                    if (hardware_ids[i] == id)
                    {
                        const int node = domain_node(topology, domain);

                        if (node < 0 || topology->cpu_nodes[i] != UINT32_MAX)
                        {
                            return false;
                        }

                        topology->cpu_nodes[i] = node;
                    }
                }
            }
        }

        offset += size;
    }

    for (size_t i = 0; i < cpu_count; ++i)
    {
        if (topology->cpu_nodes[i] == UINT32_MAX)
        {
            return false;
        }
    }

    return topology->node_count != 0 && topology->range_count != 0;
}

static const void* boot_table_memory(const struct boot_info* boot, uint64_t physical, size_t length)
{
    if (!boot->hhdm_present || physical > UINT64_MAX - length || physical > UINT64_MAX - boot->hhdm_offset)
    {
        return NULL;
    }
    const struct boot_memmap_entry* entries = (const void*) boot->memmap_entries;
    for (size_t i = 0; i < boot->memmap_entry_count; ++i)
    {
        const struct boot_memmap_entry* entry = &entries[i];
        if (entry->type != BOOT_MEMMAP_BAD_MEMORY && entry->type != BOOT_MEMMAP_FRAMEBUFFER && physical >= entry->base && physical - entry->base <= entry->length && length <= entry->length - (physical - entry->base))
        {
            return arch_boot_map_physical((struct boot_info*) boot, physical, length) ? (const void*) (uintptr_t) (physical + boot->hhdm_offset) : NULL;
        }
    }
    return NULL;
}

static const struct acpi_sdt_hdr* boot_table(const struct boot_info* boot, uint64_t physical)
{
    const struct acpi_sdt_hdr* header = boot_table_memory(boot, physical, sizeof(*header));
    if (header == NULL || header->length < sizeof(*header) || header->length > 1024 * 1024)
    {
        return NULL;
    }
    header = boot_table_memory(boot, physical, header->length);
    return header != NULL && checksum_valid(header, header->length) ? header : NULL;
}

bool numa_boot_topology(const struct boot_info* boot, numa_topology_t* topology)
{
    if (!arch_boot_numa_supported())
    {
        return false;
    }

    uint64_t rsdp_address = boot->rsdp_address;

    if (boot->hhdm_present && rsdp_address >= boot->hhdm_offset)
    {
        rsdp_address -= boot->hhdm_offset;
    }

    const struct acpi_rsdp* rsdp = boot_table_memory(boot, rsdp_address, 20);

    if (rsdp == NULL || memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_valid(rsdp, 20))
    {
        return false;
    }

    bool extended = rsdp->revision >= 2;

    if (extended)
    {
        rsdp = boot_table_memory(boot, rsdp_address, sizeof(*rsdp));

        if (rsdp == NULL || rsdp->length < sizeof(*rsdp) || rsdp->length > 4096 || boot_table_memory(boot, rsdp_address, rsdp->length) == NULL || !checksum_valid(rsdp, rsdp->length))
        {
            return false;
        }

        extended = rsdp->xsdt_addr != 0;
    }

    const struct acpi_sdt_hdr* root = boot_table(boot, extended ? rsdp->xsdt_addr : rsdp->rsdt_addr);
    const size_t stride = extended ? 8 : 4;

    if (root == NULL || memcmp(root->signature, extended ? "XSDT" : "RSDT", 4) != 0 || (root->length - sizeof(*root)) % stride != 0)
    {
        return false;
    }

    uint32_t ids[BOOT_SMP_MAX_CPUS];

    for (size_t i = 0; i < platform.cpu_count; ++i)
    {
        ids[i] = platform.cpus[i].hardware_id;
    }

    for (size_t offset = sizeof(*root); offset < root->length; offset += stride)
    {
        uint64_t address = 0;
        memcpy(&address, (const uint8_t*) root + offset, stride);
        const struct acpi_sdt_hdr* table = boot_table(boot, address);

        if (table != NULL && memcmp(table->signature, "SRAT", 4) == 0)
        {
            return numa_parse_srat(table, table->length, ids, platform.cpu_count, topology);
        }
    }

    return false;
}
