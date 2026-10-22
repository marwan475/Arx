#include <acpi/runtime.h>
#include <klib/klib.h>
#include <uacpi/kernel_api.h>

static uint64_t read_integer(const uint8_t* bytes, size_t width)
{
    uint64_t value = 0;

    for (size_t i = 0; i < width; i++)
    {
        value |= (uint64_t) bytes[i] << (i * 8);
    }

    return value;
}

static void decode_descriptor(acpi_resource_t* resource, const uint8_t* bytes, size_t length, uint8_t tag)
{
    resource->understood = true;

    if (tag == 0x04 && (length == 2 || length == 3))
    {
        resource->kind = ARX_ACPI_RESOURCE_IRQ;
        uint16_t mask = read_integer(bytes, 2);

        for (uint8_t irq = 0; irq < 16; irq++)
        {
            if (mask & (1u << irq))
            {
                resource->interrupts[resource->interrupt_count++] = irq;
            }
        }

        uint8_t flags = length == 3 ? bytes[2] : 1;
        resource->level_triggered = (flags & 1) == 0;
        resource->active_low = (flags & 8) != 0;
        resource->shared = (flags & 16) != 0;
    }
    else if (tag == 0x89 && length >= 2 && length >= 2 + (size_t) bytes[1] * 4)
    {
        resource->kind = ARX_ACPI_RESOURCE_IRQ;
        resource->interrupt_count = bytes[1];
        resource->level_triggered = (bytes[0] & 2) == 0;
        resource->active_low = (bytes[0] & 4) != 0;
        resource->shared = (bytes[0] & 8) != 0;

        for (size_t i = 0; i < bytes[1]; i++)
        {
            resource->interrupts[i] = read_integer(bytes + 2 + i * 4, 4);
        }
    }
    else if (tag == 0x05 && length == 2)
    {
        resource->kind = ARX_ACPI_RESOURCE_DMA;
        resource->minimum = bytes[0];
    }
    else if (tag == 0x08 && length == 7)
    {
        resource->kind = ARX_ACPI_RESOURCE_IO;
        resource->minimum = read_integer(bytes + 1, 2);
        resource->maximum = read_integer(bytes + 3, 2);
        resource->alignment = bytes[5];
        resource->length = bytes[6];
    }
    else if (tag == 0x09 && length == 3)
    {
        resource->kind = ARX_ACPI_RESOURCE_IO;
        resource->minimum = read_integer(bytes, 2);
        resource->maximum = resource->minimum;
        resource->length = bytes[2];
    }
    else if ((tag == 0x81 && length == 9) || (tag == 0x85 && length == 17))
    {
        size_t width = tag == 0x81 ? 2 : 4;
        uint64_t scale = tag == 0x81 ? 256 : 1;
        resource->kind = ARX_ACPI_RESOURCE_MEMORY;
        resource->minimum = read_integer(bytes + 1, width) * scale;
        resource->maximum = read_integer(bytes + 1 + width, width) * scale;
        resource->alignment = read_integer(bytes + 1 + width * 2, width) * scale;
        resource->length = read_integer(bytes + 1 + width * 3, width) * scale;
    }
    else if (tag == 0x86 && length == 9)
    {
        resource->kind = ARX_ACPI_RESOURCE_MEMORY;
        resource->minimum = read_integer(bytes + 1, 4);
        resource->maximum = resource->minimum;
        resource->length = read_integer(bytes + 5, 4);
    }
    else if ((tag == 0x88 && length >= 13) || (tag == 0x87 && length >= 23) || (tag == 0x8a && length >= 43) || (tag == 0x8b && length == 53))
    {
        size_t width = tag == 0x88 ? 2 : (tag == 0x87 ? 4 : 8);
        size_t start = tag == 0x8b ? 5 : 3;
        resource->kind = ARX_ACPI_RESOURCE_ADDRESS;
        resource->granularity = read_integer(bytes + start, width);
        resource->minimum = read_integer(bytes + start + width, width);
        resource->maximum = read_integer(bytes + start + width * 2, width);
        resource->translation = read_integer(bytes + start + width * 3, width);
        resource->length = read_integer(bytes + start + width * 4, width);
    }
    else if (tag == 0x06 && length <= 1)
    {
        resource->kind = ARX_ACPI_RESOURCE_DEPENDENT_START;
    }
    else if (tag == 0x07 && length == 0)
    {
        resource->kind = ARX_ACPI_RESOURCE_DEPENDENT_END;
    }
    else if (tag == 0x0f && length == 1)
    {
        resource->kind = ARX_ACPI_RESOURCE_END;
    }
    else
    {
        resource->kind = ARX_ACPI_RESOURCE_UNKNOWN;
        resource->understood = false;
    }
}

acpi_status_t platform_acpi_decode_resources(const uint8_t* bytes, size_t size, acpi_resources_t* output)
{
    if (bytes == NULL || output == NULL || size < 2 || output->entries != NULL || output->template_bytes != NULL)
    {
        return ACPI_INVALID_ARGUMENT;
    }

    size_t count = 0;
    size_t offset = 0;
    bool ended = false;

    while (offset < size)
    {
        uint8_t tag = bytes[offset];
        size_t header = (tag & 0x80) != 0 ? 3 : 1;

        if (size - offset < header)
        {
            return ACPI_FIRMWARE_ERROR;
        }

        size_t length = header == 3 ? read_integer(bytes + offset + 1, 2) : tag & 7;

        if (length > size - offset - header)
        {
            return ACPI_FIRMWARE_ERROR;
        }

        count++;
        offset += header + length;

        if ((tag & 0xf8) == 0x78)
        {
            if (length != 1)
            {
                return ACPI_FIRMWARE_ERROR;
            }

            if (bytes[offset - 1] != 0)
            {
                uint8_t checksum = 0;

                for (size_t i = 0; i < offset; i++)
                {
                    checksum += bytes[i];
                }

                if (checksum != 0)
                {
                    return ACPI_FIRMWARE_ERROR;
                }
            }

            ended = true;
            break;
        }
    }

    if (!ended || count > SIZE_MAX / sizeof(acpi_resource_t))
    {
        return ACPI_FIRMWARE_ERROR;
    }

    acpi_resources_t resources = {0};
    resources.template_bytes = uacpi_kernel_alloc(size);
    resources.entries = uacpi_kernel_alloc(count * sizeof(acpi_resource_t));

    if (resources.template_bytes == NULL || resources.entries == NULL)
    {
        platform_acpi_free_resources(&resources);
        return ACPI_NO_MEMORY;
    }

    memcpy(resources.template_bytes, bytes, size);
    memset(resources.entries, 0, count * sizeof(acpi_resource_t));
    resources.template_size = size;
    resources.count = count;
    offset = 0;

    for (size_t i = 0; i < count; i++)
    {
        uint8_t tag = bytes[offset];
        size_t header = (tag & 0x80) != 0 ? 3 : 1;
        size_t length = header == 3 ? read_integer(bytes + offset + 1, 2) : tag & 7;
        acpi_resource_t* resource = &resources.entries[i];
        resource->offset = offset;
        resource->size = header + length;
        decode_descriptor(resource, bytes + offset + header, length, header == 3 ? tag : tag >> 3);
        resources.has_unknown |= !resource->understood;
        offset += header + length;
    }

    *output = resources;
    return resources.has_unknown ? ACPI_INCOMPLETE : ACPI_SUCCESS;
}

void platform_acpi_free_resources(acpi_resources_t* resources)
{
    if (resources == NULL)
    {
        return;
    }

    uacpi_kernel_free(resources->template_bytes);
    uacpi_kernel_free(resources->entries);
    memset(resources, 0, sizeof(*resources));
}
