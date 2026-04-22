#include <acpi/acpi.h>
#include <arch/arch.h>
#include <device/device.h>
#include <klib/klib.h>
#include <uacpi/kernel_api.h>

#define PCI_CONFIG_SPACE_FUNCTION_SIZE 4096
#define PCI_VENDOR_DEVICE_ID_OFFSET 0x00
#define PCI_COMMAND_STATUS_OFFSET 0x04
#define PCI_CLASS_REVISION_OFFSET 0x08
#define PCI_HEADER_TYPE_OFFSET_32 0x0C
#define PCI_HEADER_TYPE_OFFSET 0x0E
#define PCI_CAPABILITIES_POINTER_OFFSET 0x34
#define PCI_SUBSYSTEM_IDS_OFFSET 0x2C
#define PCI_INTERRUPT_OFFSET 0x3C
#define PCI_BAR0_OFFSET 0x10

#define PCI_STATUS_CAPABILITIES_LIST (1u << 4)

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

static void pci_fill_bars(
		const pci_ecam_region_t* region, uint8_t bus, uint8_t device, uint8_t function,
		uint8_t header_type, pci_device_t* out_device)
{
	uint8_t bar_regs;
	uint8_t bar;

	bar_regs = ((header_type & 0x7Fu) == 0x01u) ? 2 : 6;
	out_device->bar_count = bar_regs;

	for (bar = 0; bar < bar_regs; bar++)
	{
		uint16_t offset;
		uint32_t raw_low;

		offset = (uint16_t) (PCI_BAR0_OFFSET + (bar * sizeof(uint32_t)));
		if (!pci_ecam_read32(region, bus, device, function, offset, &raw_low))
		{
			continue;
		}

		out_device->bars[bar].raw_low = raw_low;
		out_device->bars[bar].present = (raw_low != 0u && raw_low != 0xFFFFFFFFu) ? 1u : 0u;

		if ((raw_low & 0x1u) != 0)
		{
			out_device->bars[bar].is_io  = 1u;
			out_device->bars[bar].base   = (uint64_t) (raw_low & ~0x3u);
			out_device->bars[bar].is_64bit = 0u;
			continue;
		}

		out_device->bars[bar].is_io        = 0u;
		out_device->bars[bar].prefetchable = (uint8_t) ((raw_low >> 3) & 0x1u);
		out_device->bars[bar].base         = (uint64_t) (raw_low & ~0xFu);

		if (((raw_low >> 1) & 0x3u) == 0x2u && (bar + 1u) < bar_regs)
		{
			uint32_t raw_high;

			if (pci_ecam_read32(region, bus, device, function, (uint16_t) (offset + sizeof(uint32_t)), &raw_high))
			{
				out_device->bars[bar].raw_high = raw_high;
				out_device->bars[bar].base |= ((uint64_t) raw_high << 32);
				out_device->bars[bar].is_64bit = 1u;
			}

			bar++;
		}
	}
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

static bool pci_scan_function(
		const pci_ecam_region_t* region, uint8_t bus, uint8_t dev, uint8_t fn,
		pci_device_t* devices, size_t* inout_device_count)
{
	pci_device_t* dev_entry;
	uint32_t vendor_device;
	uint32_t command_status;
	uint32_t class_revision;
	uint32_t header_type_raw;
	uint32_t interrupt_raw;
	uint16_t vendor_id;
	uint16_t device_id;

	if (!pci_ecam_read32(region, bus, dev, fn, PCI_VENDOR_DEVICE_ID_OFFSET, &vendor_device))
	{
		return true;
	}

	vendor_id = (uint16_t) (vendor_device & 0xFFFFu);
	if (vendor_id == 0xFFFFu)
	{
		return true;
	}

	device_id = (uint16_t) ((vendor_device >> 16) & 0xFFFFu);

	if (*inout_device_count >= MAX_PCI_DEVICES)
	{
		kprintf("PCI: device table full (%u), stopping enumeration\n", (unsigned) MAX_PCI_DEVICES);
		return false;
	}

	dev_entry = &devices[*inout_device_count];
	memset(dev_entry, 0, sizeof(*dev_entry));

	dev_entry->segment   = region->segment;
	dev_entry->vendor_id = vendor_id;
	dev_entry->device_id = device_id;
	dev_entry->bus       = bus;
	dev_entry->device    = dev;
	dev_entry->function  = fn;

	if (pci_ecam_read32(region, bus, dev, fn, PCI_COMMAND_STATUS_OFFSET, &command_status))
	{
		dev_entry->command = (uint16_t) (command_status & 0xFFFFu);
		dev_entry->status  = (uint16_t) ((command_status >> 16) & 0xFFFFu);
	}

	if (pci_ecam_read32(region, bus, dev, fn, PCI_CLASS_REVISION_OFFSET, &class_revision))
	{
		dev_entry->revision_id = (uint8_t) (class_revision & 0xFFu);
		dev_entry->prog_if     = (uint8_t) ((class_revision >> 8) & 0xFFu);
		dev_entry->subclass    = (uint8_t) ((class_revision >> 16) & 0xFFu);
		dev_entry->class_code  = (uint8_t) ((class_revision >> 24) & 0xFFu);
	}

	if (pci_ecam_read32(region, bus, dev, fn, PCI_HEADER_TYPE_OFFSET_32, &header_type_raw))
	{
		dev_entry->header_type   = (uint8_t) ((header_type_raw >> 16) & 0x7Fu);
		dev_entry->multifunction = (uint8_t) (((header_type_raw >> 23) & 0x1u) != 0u);
	}

	if ((dev_entry->status & PCI_STATUS_CAPABILITIES_LIST) != 0u)
	{
		(void) pci_ecam_read8(region, bus, dev, fn, PCI_CAPABILITIES_POINTER_OFFSET, &dev_entry->capabilities_pointer);
	}

	if ((dev_entry->header_type & 0x7Fu) == 0x00u)
	{
		uint32_t subsystem_ids;

		if (pci_ecam_read32(region, bus, dev, fn, PCI_SUBSYSTEM_IDS_OFFSET, &subsystem_ids))
		{
			dev_entry->subsystem_vendor_id = (uint16_t) (subsystem_ids & 0xFFFFu);
			dev_entry->subsystem_id        = (uint16_t) ((subsystem_ids >> 16) & 0xFFFFu);
		}
	}

	if (pci_ecam_read32(region, bus, dev, fn, PCI_INTERRUPT_OFFSET, &interrupt_raw))
	{
		dev_entry->interrupt_line = (uint8_t) (interrupt_raw & 0xFFu);
		dev_entry->interrupt_pin  = (uint8_t) ((interrupt_raw >> 8) & 0xFFu);
	}

	pci_fill_bars(region, bus, dev, fn, dev_entry->header_type, dev_entry);
	(*inout_device_count)++;

	return true;
}

static bool pci_scan_device(const pci_ecam_region_t* region, uint8_t bus, uint8_t dev, pci_device_t* devices, size_t* inout_device_count)
{
	uint8_t header_type;
	uint8_t max_functions;

	if (!pci_ecam_read8(region, bus, dev, 0, PCI_HEADER_TYPE_OFFSET, &header_type))
	{
		return true;
	}

	max_functions = (header_type & 0x80u) ? 8 : 1;

	for (uint8_t fn = 0; fn < max_functions; fn++)
	{
		if (!pci_scan_function(region, bus, dev, fn, devices, inout_device_count))
		{
			return false;
		}
	}

	return true;
}

static bool pci_scan_bus(const pci_ecam_region_t* region, uint8_t bus, pci_device_t* devices, size_t* inout_device_count)
{
	for (uint8_t dev = 0; dev < 32; dev++)
	{
		if (!pci_scan_device(region, bus, dev, devices, inout_device_count))
		{
			return false;
		}
	}

	return true;
}

static bool pci_scan_region(const pci_ecam_region_t* region, pci_device_t* devices, size_t* inout_device_count)
{
	for (uint16_t bus = region->start_bus; bus <= region->end_bus; bus++)
	{
		if (!pci_scan_bus(region, (uint8_t) bus, devices, inout_device_count))
		{
			return false;
		}
	}

	return true;
}

static bool pci_get_device_info(void)
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

		if (!pci_scan_region(region, devices, &device_count))
		{
			dispatcher.pci_devices      = devices;
			dispatcher.pci_device_count = device_count;
			return true;
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

	if (!pci_get_device_info())
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
