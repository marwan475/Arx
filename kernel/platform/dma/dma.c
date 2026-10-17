#include <arch/arch.h>
#include <dma/dma.h>
#include <klib/klib.h>
#include <platform.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>

bool dma_range_valid(dma_address_t address, size_t size, uint64_t mask, size_t alignment, size_t boundary)
{
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 || (boundary != 0 && (boundary & (boundary - 1)) != 0))
    {
        return false;
    }

    if (address.value > mask || size - 1 > mask - address.value || (address.value & (alignment - 1)) != 0)
    {
        return false;
    }

    uint64_t end = address.value + size - 1;
    return boundary == 0 || address.value / boundary == end / boundary;
}

static bool DirectAvailable;

static platform_dma_status_t Query(void* context, const dma_device_t* device, platform_dma_caps_t* caps)
{
    (void) context;

    if (device == NULL || caps == NULL)
    {
        return DMA_INVALID_ARGUMENT;
    }

    *caps = (platform_dma_caps_t) { 0 };

    if (!DirectAvailable)
    {
        return DMA_UNSUPPORTED;
    }

    for (size_t i = 0; i < platform.pci_device_count; i++)
    {
        if (device->pciDevice == &platform.pci_devices[i])
        {
            caps->coherent            = true;
            caps->physicalAddressMask = device->addressMask;
            return DMA_SUCCESS;
        }
    }

    return DMA_UNSUPPORTED;
}

static platform_dma_status_t Map(void* context, const dma_device_t* device, phys_addr_t physical, size_t size, size_t alignment, size_t boundary, platform_dma_mapping_t* mapping)
{
    platform_dma_caps_t   caps;
    platform_dma_status_t status = Query(context, device, &caps);

    if (status != DMA_SUCCESS)
    {
        return status;
    }

    dma_address_t address = { physical };

    if (mapping == NULL || !dma_range_valid(address, size, device->addressMask, alignment, boundary))
    {
        return DMA_MAPPING_FAILED;
    }

    mapping->address = address;
    mapping->cookie  = NULL;
    return DMA_SUCCESS;
}

static platform_dma_status_t Unmap(void* context, const dma_device_t* device, const platform_dma_mapping_t* mapping, size_t size)
{
    (void) context;
    (void) device;
    (void) mapping;
    (void) size;
    return DMA_SUCCESS;
}

static void WriteBarrier(void* context)
{
    (void) context;
    arch_dma_write_barrier();
}

static void ReadBarrier(void* context)
{
    (void) context;
    arch_dma_read_barrier();
}

void platform_dma_init(void)
{
    // Initial contract: conventional coherent PC PCI with identity host DMA.
    // Never bypass advertised firmware remapping, even if it may be inactive.
    DirectAvailable          = true;
    const char* signatures[] = { "DMAR", "IVRS" };

    for (size_t i = 0; i < 2; i++)
    {
        uacpi_table  table;
        uacpi_status status = uacpi_table_find_by_signature(signatures[i], &table);

        if (status == UACPI_STATUS_OK)
        {
            uacpi_table_unref(&table);
        }

        if (status != UACPI_STATUS_NOT_FOUND)
        {
            DirectAvailable = false;
        }
    }

    if (!arch_dma_coherent_memory_supported())
    {
        DirectAvailable = false;
    }

    platform.dma_ops = (platform_dma_ops_t) { NULL, Query, Map, Unmap, WriteBarrier, ReadBarrier };
    kprintf("Arx kernel: coherent direct DMA %s (conventional PCI platform)\n", DirectAvailable ? "available" : "unsupported");
}
