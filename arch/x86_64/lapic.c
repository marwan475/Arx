#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t) high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low  = (uint32_t) value;
    uint32_t high = (uint32_t) (value >> 32);

    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

void lapic_init(void)
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic || cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        kprintf("Arx kernel: cpu %d missing LAPIC info\n", arch_cpu_id());
        panic();
    }

    uint64_t apic_base_msr = rdmsr(IA32_APIC_BASE_MSR);
    apic_base_msr |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, apic_base_msr);

    phys_addr_t lapic_pa = align_down(cpu_info->arch_info.acpi_lapic_base_addr, PAGE_SIZE);
    virt_addr_t lapic_va = pa_to_hhdm(lapic_pa, cpu_info->numa_node->zone.hhdm_present, cpu_info->numa_node->zone.hhdm_offset);

    if (vmm_virt_to_phys(lapic_va, cpu_info->address_space) == 0)
    {
        uint64_t map_flags = 0;
        ARCH_PAGE_FLAGS_INIT(map_flags);
        ARCH_PAGE_FLAG_SET_READ(map_flags);
        ARCH_PAGE_FLAG_SET_WRITE(map_flags);
        ARCH_PAGE_FLAG_SET_NOCACHE(map_flags);
        ARCH_PAGE_FLAG_SET_GLOBAL(map_flags);

        vmm_map_page(lapic_va, lapic_pa, map_flags, cpu_info->address_space);
    }

    volatile uint8_t* lapic_base = (volatile uint8_t*) (uintptr_t) lapic_va;

    REG(uint32_t, lapic_base + LAPIC_REG_TPR) = 0;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_LINT0) = LAPIC_LVT_MASKED;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_LINT1) = LAPIC_LVT_MASKED;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_ERROR) = LAPIC_LVT_MASKED;
    REG(uint32_t, lapic_base + LAPIC_REG_SVR) = LAPIC_SVR_SW_ENABLE | 0xFF;

    kprintf("Arx kernel: cpu %d LAPIC initialized at pa=0x%llx\n", arch_cpu_id(), (unsigned long long) cpu_info->arch_info.acpi_lapic_base_addr);
}

void lapic_timer_init(void)
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic || cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        return;
    }

    virt_addr_t lapic_va = pa_to_hhdm(cpu_info->arch_info.acpi_lapic_base_addr, cpu_info->numa_node->zone.hhdm_present, cpu_info->numa_node->zone.hhdm_offset);
    volatile uint8_t* lapic_base = (volatile uint8_t*) (uintptr_t) lapic_va;

    REG(uint32_t, lapic_base + LAPIC_REG_DIVIDE_CONFIG) = LAPIC_TIMER_DIVIDE_BY_16;
    REG(uint32_t, lapic_base + LAPIC_REG_LVT_TIMER) = LAPIC_LVT_TIMER_PERIODIC | LAPIC_TIMER_VECTOR;
    REG(uint32_t, lapic_base + LAPIC_REG_INITIAL_COUNT) = LAPIC_TIMER_INITIAL_COUNT;
}

void lapic_eoi(void)
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic || cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        return;
    }

    virt_addr_t lapic_va = pa_to_hhdm(cpu_info->arch_info.acpi_lapic_base_addr, cpu_info->numa_node->zone.hhdm_present, cpu_info->numa_node->zone.hhdm_offset);
    volatile uint8_t* lapic_base = (volatile uint8_t*) (uintptr_t) lapic_va;

    REG(uint32_t, lapic_base + LAPIC_REG_EOI) = 0;
}
