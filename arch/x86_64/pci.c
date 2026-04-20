#include <acpi/acpi.h>
#include <arch/arch.h>
#include <device/device.h>
#include <klib/klib.h>
#include <uacpi/kernel_api.h>

#define PCI_CONFIG_SPACE_FUNCTION_SIZE 4096
#define PCI_VENDOR_DEVICE_ID_OFFSET 0x00
#define PCI_HEADER_TYPE_OFFSET 0x0E

static bool pci_ecam_read8(const pci_ecam_region_t* region, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint8_t* out_value)
{
	uint64_t bus_index;
	uint64_t function_offset;
	uint64_t config_offset;

	if (region == NULL || out_value == NULL)
	{
		return false;
	}

	if (region->mapped_base == NULL)
	{
		return false;
	}

	if (bus < region->start_bus || bus > region->end_bus)
	{
		return false;
	}

	if (offset >= PCI_CONFIG_SPACE_FUNCTION_SIZE)
	{
		return false;
	}

	bus_index     = (uint64_t) (bus - region->start_bus);
	function_offset = (bus_index << 20) + ((uint64_t) device << 15) + ((uint64_t) function << 12);
	config_offset   = function_offset + offset;

	if (config_offset >= region->mapped_size)
	{
		return false;
	}

	*out_value = *((volatile uint8_t*) ((uintptr_t) region->mapped_base + config_offset));

	return true;
}

static bool pci_ecam_read32(const pci_ecam_region_t* region, uint8_t bus, uint8_t device, uint8_t function, uint16_t offset, uint32_t* out_value)
{
	uint64_t bus_index;
	uint64_t function_offset;
	uint64_t config_offset;

	if (region == NULL || out_value == NULL)
	{
		return false;
	}

	if (region->mapped_base == NULL)
	{
		return false;
	}

	if (bus < region->start_bus || bus > region->end_bus)
	{
		return false;
	}

	if ((offset & 0x3u) != 0)
	{
		return false;
	}

	if (offset >= (PCI_CONFIG_SPACE_FUNCTION_SIZE - sizeof(uint32_t) + 1))
	{
		return false;
	}

	bus_index     = (uint64_t) (bus - region->start_bus);
	function_offset = (bus_index << 20) + ((uint64_t) device << 15) + ((uint64_t) function << 12);
	config_offset   = function_offset + offset;

	if ((config_offset + sizeof(uint32_t)) > region->mapped_size)
	{
		return false;
	}

	*out_value = *((volatile uint32_t*) ((uintptr_t) region->mapped_base + config_offset));

	return true;
}

static void pci_unmap_regions(void)
{
	for (size_t i = 0; i < dispatcher.arch_info.pci_region_count; i++)
	{
		if (dispatcher.arch_info.pci_regions[i].mapped_base != NULL && dispatcher.arch_info.pci_regions[i].mapped_size != 0)
		{
			uacpi_kernel_unmap(dispatcher.arch_info.pci_regions[i].mapped_base, dispatcher.arch_info.pci_regions[i].mapped_size);
			dispatcher.arch_info.pci_regions[i].mapped_base = NULL;
			dispatcher.arch_info.pci_regions[i].mapped_size = 0;
		}
	}
}

static bool pci_map_regions(void)
{
	for (size_t i = 0; i < dispatcher.arch_info.pci_region_count; i++)
	{
		uint64_t bus_count;
		uint64_t map_size;
		void*    mapped;

		if (dispatcher.arch_info.pci_regions[i].end_bus < dispatcher.arch_info.pci_regions[i].start_bus)
		{
			kprintf("PCI: invalid ECAM bus range for segment %u\n", dispatcher.arch_info.pci_regions[i].segment);
			return false;
		}

		bus_count = (uint64_t) (dispatcher.arch_info.pci_regions[i].end_bus - dispatcher.arch_info.pci_regions[i].start_bus) + 1;
		map_size  = bus_count << 20;

		mapped = uacpi_kernel_map((uacpi_phys_addr) dispatcher.arch_info.pci_regions[i].base_address, map_size);
		if (mapped == NULL)
		{
			kprintf("PCI: failed to map ECAM region %u\n", (unsigned) i);
			return false;
		}

		dispatcher.arch_info.pci_regions[i].mapped_base = mapped;
		dispatcher.arch_info.pci_regions[i].mapped_size = map_size;
	}

	return true;
}

static bool pci_store_devices_from_regions(void)
{
	pci_device_t* devices;
	size_t        device_count;

	devices = (pci_device_t*) kmalloc(MAX_PCI_DEVICES * sizeof(pci_device_t));
	if (devices == NULL)
	{
		kprintf("PCI: failed to allocate pci device array\n");
		return false;
	}

	device_count = 0;

	for (size_t region_index = 0; region_index < dispatcher.arch_info.pci_region_count; region_index++)
	{
		const pci_ecam_region_t* region = &dispatcher.arch_info.pci_regions[region_index];

		for (uint16_t bus = region->start_bus; bus <= region->end_bus; bus++)
		{
			for (uint8_t dev = 0; dev < 32; dev++)
			{
				uint8_t header_type;
				uint8_t max_functions;

				if (!pci_ecam_read8(region, (uint8_t) bus, dev, 0, PCI_HEADER_TYPE_OFFSET, &header_type))
				{
					continue;
				}

				max_functions = (header_type & 0x80u) ? 8 : 1;

				for (uint8_t fn = 0; fn < max_functions; fn++)
				{
					uint32_t vendor_device;
					uint16_t vendor_id;
					uint16_t device_id;

					if (!pci_ecam_read32(region, (uint8_t) bus, dev, fn, PCI_VENDOR_DEVICE_ID_OFFSET, &vendor_device))
					{
						continue;
					}

					vendor_id = (uint16_t) (vendor_device & 0xFFFFu);
					if (vendor_id == 0xFFFFu)
					{
						continue;
					}

					device_id = (uint16_t) ((vendor_device >> 16) & 0xFFFFu);

					if (device_count >= MAX_PCI_DEVICES)
					{
						kprintf("PCI: device table full (%u), stopping enumeration\n", (unsigned) MAX_PCI_DEVICES);
						dispatcher.pci_devices      = devices;
						dispatcher.pci_device_count = device_count;
						return true;
					}

					devices[device_count].vendor_id = vendor_id;
					devices[device_count].device_id = device_id;
					devices[device_count].bus       = (uint8_t) bus;
					devices[device_count].device    = dev;
					devices[device_count].function  = fn;
					device_count++;
				}
			}
		}
	}

	dispatcher.pci_devices      = devices;
	dispatcher.pci_device_count = device_count;

	kprintf("PCI: discovered %u device(s)\n", (unsigned) device_count);
	return true;
}

bool pci_get_mcfg_region_count(size_t* out_region_count)
{
	struct acpi_mcfg* mcfg;
	uacpi_table       mcfg_table;
	uacpi_status      status;
	size_t            entries_size;

	if (out_region_count == NULL)
	{
		return false;
	}

	*out_region_count = 0;

	status = acpi_get_mcfg(&mcfg, &mcfg_table);
	if (status != UACPI_STATUS_OK)
	{
		return false;
	}

	if (mcfg->hdr.length < sizeof(*mcfg))
	{
		uacpi_table_unref(&mcfg_table);
		return false;
	}

	entries_size = (size_t) (mcfg->hdr.length - sizeof(*mcfg));
	if ((entries_size % sizeof(struct acpi_mcfg_allocation)) != 0)
	{
		uacpi_table_unref(&mcfg_table);
		return false;
	}

	*out_region_count = entries_size / sizeof(struct acpi_mcfg_allocation);

	uacpi_table_unref(&mcfg_table);
	return true;
}

bool pci_get_regions_from_mcfg(pci_ecam_region_t* out_entries, size_t max_entries, size_t* out_entry_count)
{
	struct acpi_mcfg* mcfg;
	uacpi_table       mcfg_table;
	uacpi_status      status;
	size_t            entry_count;
	size_t            entries_size;

	if (out_entries == NULL || out_entry_count == NULL)
	{
		return false;
	}

	*out_entry_count = 0;

	status = acpi_get_mcfg(&mcfg, &mcfg_table);
	if (status != UACPI_STATUS_OK)
	{
		return false;
	}

	if (mcfg->hdr.length < sizeof(*mcfg))
	{
		uacpi_table_unref(&mcfg_table);
		return false;
	}

	entries_size = (size_t) (mcfg->hdr.length - sizeof(*mcfg));
	if ((entries_size % sizeof(struct acpi_mcfg_allocation)) != 0)
	{
		uacpi_table_unref(&mcfg_table);
		return false;
	}

	entry_count = entries_size / sizeof(struct acpi_mcfg_allocation);
	*out_entry_count = entry_count;

	if (entry_count > max_entries)
	{
		uacpi_table_unref(&mcfg_table);
		return false;
	}

	for (size_t i = 0; i < entry_count; i++)
	{
		out_entries[i].base_address = mcfg->entries[i].address;
		out_entries[i].segment      = mcfg->entries[i].segment;
		out_entries[i].start_bus    = mcfg->entries[i].start_bus;
		out_entries[i].end_bus      = mcfg->entries[i].end_bus;
		out_entries[i].mapped_base  = NULL;
		out_entries[i].mapped_size  = 0;
	}

	uacpi_table_unref(&mcfg_table);
	return true;
}

bool pci_init(void)
{
	size_t            region_count;
	size_t            extracted_count;
	pci_ecam_region_t* regions;

	dispatcher.pci_devices      = NULL;
	dispatcher.pci_device_count = 0;

	region_count = 0;
	if (!pci_get_mcfg_region_count(&region_count))
	{
		kprintf("PCI: failed to read MCFG region count\n");
		return false;
	}

	if (region_count == 0)
	{
		dispatcher.arch_info.pci_regions      = NULL;
		dispatcher.arch_info.pci_region_count = 0;
		kprintf("PCI: no ECAM regions present\n");
		return true;
	}

	regions = (pci_ecam_region_t*) kmalloc(region_count * sizeof(pci_ecam_region_t));
	if (regions == NULL)
	{
		kprintf("PCI: failed to allocate region table (%u entries)\n", (unsigned) region_count);
		return false;
	}

	extracted_count = 0;
	if (!pci_get_regions_from_mcfg(regions, region_count, &extracted_count))
	{
		kfree(regions);
		kprintf("PCI: failed to extract MCFG regions\n");
		return false;
	}

	dispatcher.arch_info.pci_regions      = regions;
	dispatcher.arch_info.pci_region_count = extracted_count;

    KDEBUG("PCI: extracted %u ECAM region(s) from MCFG\n", (unsigned) extracted_count);

	if (!pci_map_regions())
	{
		kfree(dispatcher.arch_info.pci_regions);
		dispatcher.arch_info.pci_regions      = NULL;
		dispatcher.arch_info.pci_region_count = 0;
		return false;
	}

	if (!pci_store_devices_from_regions())
	{
		pci_unmap_regions();
		kfree(dispatcher.arch_info.pci_regions);
		dispatcher.arch_info.pci_regions      = NULL;
		dispatcher.arch_info.pci_region_count = 0;
		return false;
	}

	kprintf("PCI: loaded %u ECAM region(s) from MCFG\n", (unsigned) extracted_count);
	return true;
}
