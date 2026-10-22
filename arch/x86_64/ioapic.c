#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <platform.h>
#include <uacpi/acpi.h>

static spinlock_t ioapic_lock;

static volatile uint8_t* ioapic_get_base(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    if (!platform.arch_info.ioapic_initialized || !platform.arch_info.acpi_has_ioapic || platform.arch_info.acpi_ioapic_base_addr == 0)
    {
        return NULL;
    }

    virt_addr_t ioapic_va = pa_to_hhdm(platform.arch_info.acpi_ioapic_base_addr, cpu_info->numa_node->zone.hhdm_present, cpu_info->numa_node->zone.hhdm_offset);
    return (volatile uint8_t*) (uintptr_t) ioapic_va;
}

static void route_irq(volatile uint8_t* ioapic_base, uint32_t pin, uint32_t vector, uint16_t flags, uint8_t destination_lapic_id, uint8_t masked)
{
    uint32_t low  = vector;
    uint32_t high = ((uint32_t) destination_lapic_id) << 24;

    if (masked)
    {
        low |= IOAPIC_REDIR_MASKED;
    }

    if ((flags & ACPI_MADT_POLARITY_MASK) == ACPI_MADT_POLARITY_ACTIVE_LOW)
    {
        low |= IOAPIC_REDIR_POLARITY_LOW;
    }

    if ((flags & ACPI_MADT_TRIGGERING_MASK) == ACPI_MADT_TRIGGERING_LEVEL)
    {
        low |= IOAPIC_REDIR_TRIGGER_LEVEL;
    }

    uint8_t low_index  = (uint8_t) (IOAPIC_REDIR_TABLE_BASE + pin * 2);
    uint8_t high_index = (uint8_t) (low_index + 1);

    REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = high_index;
    REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN)    = high;

    REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = low_index;
    REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN)    = low;

    platform.arch_info.ioapic_redir_count++;
}

static void ioapic_set_vector_mask(uint8_t vector, uint8_t masked)
{
    volatile uint8_t* ioapic_base = ioapic_get_base();

    if (!ioapic_base)
    {
        return;
    }

    spinlock_acquire_preempt(&ioapic_lock);
    REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = IOAPIC_REG_VER;
    uint32_t ioapic_ver                              = REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN);
    uint32_t max_redir                               = (ioapic_ver >> 16) & 0xFF;

    for (uint32_t pin = 0; pin <= max_redir; pin++)
    {
        uint8_t low_index                                = (uint8_t) (IOAPIC_REDIR_TABLE_BASE + pin * 2);
        REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = low_index;
        uint32_t low                                     = REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN);

        if ((uint8_t) (low & 0xFF) != vector)
        {
            continue;
        }

        if (masked)
        {
            low |= IOAPIC_REDIR_MASKED;
        }
        else
        {
            low &= ~IOAPIC_REDIR_MASKED;
        }

        REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = low_index;
        REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN)    = low;
        break;
    }

    spinlock_release_preempt(&ioapic_lock);
}

void ioapic_mask_vector(uint8_t vector)
{
    ioapic_set_vector_mask(vector, 1);
}

void ioapic_unmask_vector(uint8_t vector)
{
    ioapic_set_vector_mask(vector, 0);
}

uint32_t ioapic_register_device_flags(uint32_t gsi, uint16_t flags)
{
    volatile uint8_t* ioapic_base = ioapic_get_base();

    if (!ioapic_base)
    {
        return 0;
    }

    uint32_t ioapic_gsi_base = platform.arch_info.acpi_ioapic_gsi_base;

    if (gsi < ioapic_gsi_base)
    {
        return 0;
    }

    uint32_t pin = gsi - ioapic_gsi_base;

    if (pin > platform.arch_info.ioapic_max_redir)
    {
        return 0;
    }

    spinlock_acquire_preempt(&ioapic_lock);
    uint32_t vector = platform.vector_base;

    if (vector >= 0xFF)
    {
        spinlock_release_preempt(&ioapic_lock);
        return 0;
    }

    uint8_t  bsp_lapic_id = platform.cpus[platform.bsp_id].arch_info.acpi_lapic_id;

    route_irq(ioapic_base, pin, vector, flags, bsp_lapic_id, 1);
    platform.vector_base++;
    spinlock_release_preempt(&ioapic_lock);
    return vector;
}

// route legacy pic irqs to bsp
static void route_legacy_irqs(volatile uint8_t* ioapic_base, uint32_t max_redir, uint32_t ioapic_gsi_base, uint8_t bsp_lapic_id)
{
    for (uint32_t irq = 0; irq < LEGACY_PIC_MAX_IRQS; irq++)
    {
        uint32_t gsi   = ioapic_gsi_base + irq;
        uint16_t flags = ACPI_MADT_POLARITY_CONFORMING | ACPI_MADT_TRIGGERING_CONFORMING;

        if (platform.arch_info.acpi_iso_overrides[irq].present)
        {
            gsi   = platform.arch_info.acpi_iso_overrides[irq].gsi;
            flags = platform.arch_info.acpi_iso_overrides[irq].flags;
        }

        if (gsi < ioapic_gsi_base)
        {
            continue;
        }

        uint32_t pin = gsi - ioapic_gsi_base;

        if (pin > max_redir)
        {
            continue;
        }

        uint32_t vector = IRQ_VECTOR_BASE + irq;
        route_irq(ioapic_base, pin, vector, flags, bsp_lapic_id, 1);
    }
}

void ioapic_init(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    if ((uint64_t) arch_cpu_id() != platform.bsp_id)
    {
        return;
    }

    if (platform.arch_info.ioapic_initialized)
    {
        return;
    }

    if (!platform.arch_info.acpi_has_ioapic || platform.arch_info.acpi_ioapic_base_addr == 0)
    {
        kprintf("Arx kernel: missing IOAPIC info\n");
        panic();
    }

    phys_addr_t ioapic_pa = align_down(platform.arch_info.acpi_ioapic_base_addr, PAGE_SIZE);
    virt_addr_t ioapic_va = vmm_map_direct_mmio_page(ioapic_pa, cpu_info->address_space);

    if (ioapic_va == 0)
    {
        panic();
    }

    volatile uint8_t* ioapic_base = (volatile uint8_t*) (uintptr_t) ioapic_va;

    spinlock_acquire_preempt(&ioapic_lock);
    REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = IOAPIC_REG_VER;
    uint32_t ioapic_ver                              = REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN);
    uint32_t max_redir                               = (ioapic_ver >> 16) & 0xFF;

    if (ioapic_ver == 0 || ioapic_ver == 0xFFFFFFFF)
    {
        error_logger("Arx kernel: cpu %d IOAPIC MMIO read failed ver=0x%x pa=0x%llx\n", arch_cpu_id(), (unsigned) ioapic_ver, (unsigned long long) platform.arch_info.acpi_ioapic_base_addr);
        panic();
    }

    platform.arch_info.ioapic_max_redir   = max_redir;
    platform.arch_info.ioapic_redir_count = 0;
    platform.vector_base                  = VECTOR_DEVICE_BASE;

    uint32_t ioapic_gsi_base = platform.arch_info.acpi_ioapic_gsi_base;
    uint8_t  bsp_lapic_id    = platform.cpus[platform.bsp_id].arch_info.acpi_lapic_id;

    // mask all entries

    for (uint32_t i = 0; i <= max_redir; i++)
    {
        uint8_t low_index  = (uint8_t) (IOAPIC_REDIR_TABLE_BASE + i * 2);
        uint8_t high_index = (uint8_t) (low_index + 1);

        REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = high_index;
        REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN)    = 0;

        REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = low_index;
        REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN)    = IOAPIC_REDIR_MASKED | i;
    }

    route_legacy_irqs(ioapic_base, max_redir, ioapic_gsi_base, bsp_lapic_id);

    platform.arch_info.ioapic_initialized = 1;
    spinlock_release_preempt(&ioapic_lock);

    kprintf("Arx kernel: IOAPIC initialized id=%u pa=0x%llx gsi_base=%u max_redir=%u\n", (unsigned) platform.arch_info.acpi_ioapic_id, (unsigned long long) platform.arch_info.acpi_ioapic_base_addr, (unsigned) platform.arch_info.acpi_ioapic_gsi_base, (unsigned) max_redir);
}

uint32_t ioapic_register_device(uint32_t gsi)
{
    uint32_t vector = ioapic_register_device_flags(gsi, ACPI_MADT_POLARITY_CONFORMING | ACPI_MADT_TRIGGERING_CONFORMING);

    if (vector != 0)
    {
        ioapic_unmask_vector((uint8_t) vector);
    }

    return vector;
}
