#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <platform.h>

static uint64_t scheduler_timer_ticks[BOOT_SMP_MAX_CPUS];

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

    if (apic_base_msr & (1ULL << 10))
    {
        kprintf("Arx kernel: x2APIC mode is unsupported\n");
        panic();
    }

    apic_base_msr |= IA32_APIC_BASE_ENABLE;
    x86_write_msr(IA32_APIC_BASE_MSR, apic_base_msr);

    phys_addr_t lapic_pa = align_down(cpu_info->arch_info.acpi_lapic_base_addr, PAGE_SIZE);
    virt_addr_t lapic_va = vmm_map_direct_mmio_page(lapic_pa, cpu_info->address_space);

    if (lapic_va == 0)
    {
        panic();
    }

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

bool lapic_timer_calibrate(void)
{
    cpu_info_t* cpu = &platform.cpus[arch_cpu_id()];
    uint64_t    start, finish;

    if (!cpu->arch_info.acpi_has_lapic || cpu->arch_info.acpi_lapic_base_addr == 0 || !arch_time_read_monotonic(&start))
        return false;

    // The HPET is available only after resource initialization. Calibrate here
    // from each CPU's post-init task, retaining the bootstrap timer on failure.
    // IPIs remain enabled; only timer dispatch is masked during measurement.
    cpu_preemption_disable();
    volatile uint8_t* registers                        = lapic_get_base(cpu);
    const uint32_t    previous_lvt                     = REG(uint32_t, registers + LAPIC_REG_LVT_TIMER);
    const uint32_t    previous_initial                 = REG(uint32_t, registers + LAPIC_REG_INITIAL_COUNT);
    REG(uint32_t, registers + LAPIC_REG_LVT_TIMER)     = LAPIC_LVT_MASKED | LAPIC_TIMER_VECTOR;
    REG(uint32_t, registers + LAPIC_REG_INITIAL_COUNT) = UINT32_MAX;

    bool           measured = arch_time_read_monotonic(&start);
    const uint32_t first    = REG(uint32_t, registers + LAPIC_REG_CURRENT_COUNT);
    finish                  = start;
    // A bounded loop must restore timer delivery even if HPET stops advancing.

    for (unsigned attempts = 0; measured && attempts < 1000000; ++attempts)
    {
        measured = arch_time_read_monotonic(&finish);

        if (!measured || finish < start || finish - start >= 10000000ULL)
        {
            break;
        }

        arch_pause();
    }

    const uint32_t last    = REG(uint32_t, registers + LAPIC_REG_CURRENT_COUNT);
    const uint64_t elapsed = finish >= start ? finish - start : 0;
    const uint64_t ticks   = first >= last ? (uint64_t) first - last : 0;
    // Reject expiration, non-advancing timers and excessively interrupted samples.
    measured                                           = measured && last != 0 && ticks != 0 && elapsed >= 10000000ULL && elapsed <= 100000000ULL;
    const uint64_t initial                             = measured ? ticks * 10000000ULL / elapsed : 0;
    measured                                           = measured && initial != 0 && initial <= UINT32_MAX;
    REG(uint32_t, registers + LAPIC_REG_INITIAL_COUNT) = 0;
    REG(uint32_t, registers + LAPIC_REG_LVT_TIMER)     = measured ? LAPIC_LVT_TIMER_PERIODIC | LAPIC_TIMER_VECTOR : previous_lvt;
    REG(uint32_t, registers + LAPIC_REG_INITIAL_COUNT) = measured ? (uint32_t) initial : previous_initial;
    __atomic_store_n(&scheduler_timer_ticks[arch_cpu_id()], measured ? initial : 0, __ATOMIC_RELEASE);
    cpu_preemption_enable();

    if (measured)
    {
        const uint64_t old_microseconds = (uint64_t) previous_initial * elapsed / ticks / 1000;
        KDEBUG("cpu %u LAPIC timer calibrated: previous period~%llu us, target=10000 us, count=%u\n", (unsigned) arch_cpu_id(), (unsigned long long) old_microseconds, (unsigned) initial);
    }
    else
    {
        error_logger("cpu %u LAPIC calibration failed; retaining bootstrap timer", (unsigned) arch_cpu_id());
    }

    return measured;
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

static void lapic_wait_for_delivery(volatile uint8_t* lapic_base)
{
    while ((REG(uint32_t, lapic_base + LAPIC_REG_ICR_LOW) & LAPIC_ICR_DELIVERY_STATUS_PENDING) != 0)
    {
        arch_pause();
    }
}

// The target handler releases the mailbox after consuming the request.
void send_ipi(uint8_t target_cpu_id, uint8_t request_type, const void* request_data)
{
    cpu_info_t* source_cpu_info = &platform.cpus[arch_cpu_id()];

    if (!source_cpu_info->arch_info.acpi_has_lapic || source_cpu_info->arch_info.acpi_lapic_base_addr == 0)
    {
        return;
    }

    if (target_cpu_id >= platform.cpu_count)
    {
        error_logger("Arx kernel: send_ipi rejected invalid target cpu id %u\n", (unsigned) target_cpu_id);
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

    lapic_wait_for_delivery(lapic_base);

    REG(uint32_t, lapic_base + LAPIC_REG_ICR_HIGH) = destination;
    REG(uint32_t, lapic_base + LAPIC_REG_ICR_LOW)  = LAPIC_IPI_VECTOR;

    lapic_wait_for_delivery(lapic_base);
}

void arch_request_reschedule(uint8_t target)
{
    const cpu_info_t* source = &platform.cpus[arch_cpu_id()];
    if (target >= platform.cpu_count || !source->arch_info.acpi_has_lapic || !platform.cpus[target].arch_info.acpi_has_lapic)
    {
        return;
    }
    // Scheduling IPIs carry no mailbox payload and never wait for a target
    // software lock. Serialize the local ICR pair against interrupt handlers.
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    volatile uint8_t* registers = lapic_get_base(source);
    lapic_wait_for_delivery(registers);
    REG(uint32_t, registers + LAPIC_REG_ICR_HIGH) = platform.cpus[target].arch_info.acpi_lapic_id << 24;
    REG(uint32_t, registers + LAPIC_REG_ICR_LOW) = LAPIC_IPI_VECTOR;
    lapic_wait_for_delivery(registers);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

bool arch_scheduler_timer_ready(uint8_t cpu)
{
    return cpu < platform.cpu_count && __atomic_load_n(&scheduler_timer_ticks[cpu], __ATOMIC_ACQUIRE) != 0;
}

bool arch_program_scheduler_timer(uint64_t nanoseconds)
{
    const uint8_t cpu = arch_cpu_id();
    const uint64_t ticks = __atomic_load_n(&scheduler_timer_ticks[cpu], __ATOMIC_ACQUIRE);
    if (ticks == 0 || nanoseconds == 0)
    {
        return false;
    }
    // Scheduling keeps a 10ms heartbeat for sleepers and accounting, and can
    // arm an earlier event for a quantum, reservation or replenishment boundary.
    if (nanoseconds > 10000000ULL)
    {
        nanoseconds = 10000000ULL;
    }
    uint64_t count = (ticks * nanoseconds + 9999999ULL) / 10000000ULL;
    if (count == 0)
    {
        count = 1;
    }
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    volatile uint8_t* registers = lapic_get_base(&platform.cpus[cpu]);
    REG(uint32_t, registers + LAPIC_REG_LVT_TIMER) = LAPIC_TIMER_VECTOR;
    REG(uint32_t, registers + LAPIC_REG_INITIAL_COUNT) = (uint32_t) count;
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
    return true;
}
