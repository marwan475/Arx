#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <platform.h>

static volatile uint8_t* lapic_get_base(const cpu_info_t* cpu)
{
    const zone_t* zone = &cpu->numa_node->zone;
    return (volatile uint8_t*) (uintptr_t) pa_to_hhdm(cpu->arch_info.acpi_lapic_base_addr, zone->hhdm_present, zone->hhdm_offset);
}

void lapic_init(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic || cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        kprintf("Arx kernel: cpu %d missing LAPIC info\n", arch_cpu_id());
        panic();
    }

    uint64_t apic_base_msr = x86_read_msr(IA32_APIC_BASE_MSR);
    apic_base_msr |= IA32_APIC_BASE_ENABLE;
    x86_write_msr(IA32_APIC_BASE_MSR, apic_base_msr);

    phys_addr_t lapic_pa = align_down(cpu_info->arch_info.acpi_lapic_base_addr, PAGE_SIZE);
    virt_addr_t lapic_va = vmm_map_direct_mmio_page(lapic_pa, cpu_info->address_space, true);

    volatile uint8_t* lapic_base = (volatile uint8_t*) (uintptr_t) lapic_va;

    REG(uint32_t, lapic_base + LAPIC_REG_TPR)       = 0;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_LINT0) = LAPIC_LVT_MASKED;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_LINT1) = LAPIC_LVT_MASKED;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_ERROR) = LAPIC_LVT_MASKED;
    REG(uint32_t, lapic_base + LAPIC_REG_SVR)       = LAPIC_SVR_SW_ENABLE | 0xFF;

    kprintf("Arx kernel: cpu %d LAPIC initialized at pa=0x%llx\n", arch_cpu_id(), (unsigned long long) cpu_info->arch_info.acpi_lapic_base_addr);
}

void lapic_timer_init(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic || cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        return;
    }

    volatile uint8_t* lapic_base = lapic_get_base(cpu_info);

    REG(uint32_t, lapic_base + LAPIC_REG_DIVIDE_CONFIG) = LAPIC_TIMER_DIVIDE_BY_16;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_TIMER)     = LAPIC_LVT_TIMER_PERIODIC | LAPIC_TIMER_VECTOR;
    REG(uint32_t, lapic_base + LAPIC_REG_INITIAL_COUNT) = LAPIC_TIMER_INITIAL_COUNT;
}

void lapic_eoi(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic || cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        return;
    }

    volatile uint8_t* lapic_base = lapic_get_base(cpu_info);

    REG(uint32_t, lapic_base + LAPIC_REG_EOI) = 0;
}

// we lock target ipi lock but handler needs to unclock it
void send_ipi(uint8_t target_cpu_id, uint8_t request_type, const void* request_data)
{
    cpu_info_t* source_cpu_info = &platform.cpus[arch_cpu_id()];

    if (!source_cpu_info->arch_info.acpi_has_lapic || source_cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        return;
    }

    if (target_cpu_id >= platform.cpu_count)
    {
        kprintf("Arx kernel: send_ipi rejected invalid target cpu id %u\n", (unsigned) target_cpu_id);
        return;
    }

    cpu_info_t* target_cpu_info = &platform.cpus[target_cpu_id];
    if (!target_cpu_info->arch_info.acpi_has_lapic)
    {
        kprintf("Arx kernel: send_ipi rejected target cpu %u without LAPIC\n", (unsigned) target_cpu_id);
        return;
    }

    spinlock_acquire(&target_cpu_info->ipi_lock);
    if (request_data != NULL)
    {
        target_cpu_info->ipi_request_data      = *((const ipi_request_data_t*) request_data);
        target_cpu_info->ipi_request_data.type = (ipi_request_type_t) request_type;
    }
    else
    {
        memset(&target_cpu_info->ipi_request_data, 0, sizeof(target_cpu_info->ipi_request_data));
        target_cpu_info->ipi_request_data.type = (ipi_request_type_t) request_type;
    }

    volatile uint8_t* lapic_base  = lapic_get_base(source_cpu_info);
    const uint32_t    destination = ((uint32_t) target_cpu_info->arch_info.acpi_lapic_id) << 24;

    while ((REG(uint32_t, lapic_base + LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS_PENDING) != 0)
    {
        arch_pause();
    }

    REG(uint32_t, lapic_base + LAPIC_REG_ICR_HIGH) = destination;
    REG(uint32_t, lapic_base + LAPIC_REG_ICR_LOW)  = LAPIC_IPI_VECTOR;

    while ((REG(uint32_t, lapic_base + LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS_PENDING) != 0)
    {
        arch_pause();
    }
}
