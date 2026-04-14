#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <uacpi/acpi.h>

static const uint8_t PIC_MASK_ALL_IRQS = 0xFF;

#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_ENABLE (1ULL << 11)

#define LAPIC_REG_TPR 0x80
#define LAPIC_REG_EOI 0xB0
#define LAPIC_REG_SVR 0xF0
#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_LVT_LINT0 0x350
#define LAPIC_REG_LVT_LINT1 0x360
#define LAPIC_REG_LVT_ERROR 0x370
#define LAPIC_REG_INITIAL_COUNT 0x380
#define LAPIC_REG_DIVIDE_CONFIG 0x3E0

#define LAPIC_SVR_SW_ENABLE (1U << 8)
#define LAPIC_LVT_MASKED (1U << 16)
#define LAPIC_LVT_TIMER_PERIODIC (1U << 17)

#define LAPIC_TIMER_VECTOR 0xE0
#define LAPIC_TIMER_DIVIDE_BY_16 0x3
#define LAPIC_TIMER_INITIAL_COUNT 10000000U

#define IOAPIC_REG_IOREGSEL 0x00
#define IOAPIC_REG_IOWIN 0x10

#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01

#define IOAPIC_REDIR_TABLE_BASE 0x10
#define IOAPIC_REDIR_MASKED (1U << 16)
#define IOAPIC_REDIR_POLARITY_LOW (1U << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1U << 15)

#define IRQ_VECTOR_BASE 0x20
#define IRQ_DEVICE_VECTOR_BASE 0x50
#define IRQ_DEVICE_VECTOR_LIMIT (LAPIC_TIMER_VECTOR - 1)

static arch_irq_handler_t ioapic_irq_handlers[NUM_IDT_ENTRIES];
static uint8_t            ioapic_next_device_vector = IRQ_DEVICE_VECTOR_BASE;

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

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void build_tss_descriptor(const tss_t* tss, tss_discriptor_t* tss_descriptor)
{
    uint64_t tss_address = (uint64_t) tss;
    uint32_t tss_limit   = (uint32_t) (sizeof(tss_t) - 1);

    tss_descriptor->descriptor.value = 0;

    tss_descriptor->descriptor.value |= (uint64_t) (tss_limit & 0xFFFF);
    tss_descriptor->descriptor.value |= (uint64_t) (tss_address & 0xFFFFFF) << 16;
    tss_descriptor->descriptor.value |= (uint64_t) 0x9 << 40;
    tss_descriptor->descriptor.value |= (uint64_t) 1 << 47;
    tss_descriptor->descriptor.value |= (uint64_t) ((tss_limit >> 16) & 0xF) << 48;
    tss_descriptor->descriptor.value |= (uint64_t) ((tss_address >> 24) & 0xFF) << 56;

    tss_descriptor->base_high = (uint32_t) ((tss_address >> 32) & 0xFFFFFFFF);
    tss_descriptor->reserved  = 0;
}

void build_gdt(gdt_t* gdt, const tss_discriptor_t* tss_descriptor)
{
    gdt->null.value           = 0x0000000000000000;
    gdt->kernel_code_64.value = 0x00AF9A000000FFFF;
    gdt->kernel_data_64.value = 0x00CF92000000FFFF;
    gdt->user_code_64.value   = 0x00AFFA000000FFFF;
    gdt->user_data_64.value   = 0x00CFF2000000FFFF;
    gdt->tss                  = *tss_descriptor;
}

void gdt_init()
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    build_tss_descriptor(&cpu_info->arch_info.tss, &cpu_info->arch_info.tss_descriptor);
    build_gdt(&cpu_info->arch_info.gdt, &cpu_info->arch_info.tss_descriptor);

    cpu_info->arch_info.gdt_reg.limit = sizeof(gdt_t) - 1;
    cpu_info->arch_info.gdt_reg.base  = (uint64_t) (uintptr_t) &cpu_info->arch_info.gdt;

    __asm__ __volatile__("cli\n"
                         "lgdt %[gdt]\n"
                         "ltr %[tss]\n"

                         "pushq $0x8\n"
                         "leaq 1f(%%rip), %%rax\n"
                         "pushq %%rax\n"
                         "lretq\n"

                         "1:\n"

                         "movw $0x10, %%ax\n"
                         "movw %%ax, %%ds\n"
                         "movw %%ax, %%es\n"
                         "movw %%ax, %%ss\n"

                         "movw %%ax, %%fs\n"
                         "movw %%ax, %%gs\n"
                         :
                         : [gdt] "m"(cpu_info->arch_info.gdt_reg), [tss] "r"((uint16_t) offsetof(gdt_t, tss))
                         : "rax", "memory");
}

static void (*const isr_handlers[NUM_IDT_ENTRIES])() = {
        ISR0,   ISR1,   ISR2,   ISR3,   ISR4,   ISR5,   ISR6,   ISR7,   ISR8,   ISR9,   ISR10,  ISR11,  ISR12,  ISR13,  ISR14,  ISR15,  ISR16,  ISR17,  ISR18,  ISR19,  ISR20,  ISR21,  ISR22,  ISR23,
        ISR24,  ISR25,  ISR26,  ISR27,  ISR28,  ISR29,  ISR30,  ISR31,  ISR32,  ISR33,  ISR34,  ISR35,  ISR36,  ISR37,  ISR38,  ISR39,  ISR40,  ISR41,  ISR42,  ISR43,  ISR44,  ISR45,  ISR46,  ISR47,
        ISR48,  ISR49,  ISR50,  ISR51,  ISR52,  ISR53,  ISR54,  ISR55,  ISR56,  ISR57,  ISR58,  ISR59,  ISR60,  ISR61,  ISR62,  ISR63,  ISR64,  ISR65,  ISR66,  ISR67,  ISR68,  ISR69,  ISR70,  ISR71,
        ISR72,  ISR73,  ISR74,  ISR75,  ISR76,  ISR77,  ISR78,  ISR79,  ISR80,  ISR81,  ISR82,  ISR83,  ISR84,  ISR85,  ISR86,  ISR87,  ISR88,  ISR89,  ISR90,  ISR91,  ISR92,  ISR93,  ISR94,  ISR95,
        ISR96,  ISR97,  ISR98,  ISR99,  ISR100, ISR101, ISR102, ISR103, ISR104, ISR105, ISR106, ISR107, ISR108, ISR109, ISR110, ISR111, ISR112, ISR113, ISR114, ISR115, ISR116, ISR117, ISR118, ISR119,
        ISR120, ISR121, ISR122, ISR123, ISR124, ISR125, ISR126, ISR127, ISR128, ISR129, ISR130, ISR131, ISR132, ISR133, ISR134, ISR135, ISR136, ISR137, ISR138, ISR139, ISR140, ISR141, ISR142, ISR143,
        ISR144, ISR145, ISR146, ISR147, ISR148, ISR149, ISR150, ISR151, ISR152, ISR153, ISR154, ISR155, ISR156, ISR157, ISR158, ISR159, ISR160, ISR161, ISR162, ISR163, ISR164, ISR165, ISR166, ISR167,
        ISR168, ISR169, ISR170, ISR171, ISR172, ISR173, ISR174, ISR175, ISR176, ISR177, ISR178, ISR179, ISR180, ISR181, ISR182, ISR183, ISR184, ISR185, ISR186, ISR187, ISR188, ISR189, ISR190, ISR191,
        ISR192, ISR193, ISR194, ISR195, ISR196, ISR197, ISR198, ISR199, ISR200, ISR201, ISR202, ISR203, ISR204, ISR205, ISR206, ISR207, ISR208, ISR209, ISR210, ISR211, ISR212, ISR213, ISR214, ISR215,
        ISR216, ISR217, ISR218, ISR219, ISR220, ISR221, ISR222, ISR223, ISR224, ISR225, ISR226, ISR227, ISR228, ISR229, ISR230, ISR231, ISR232, ISR233, ISR234, ISR235, ISR236, ISR237, ISR238, ISR239,
        ISR240, ISR241, ISR242, ISR243, ISR244, ISR245, ISR246, ISR247, ISR248, ISR249, ISR250, ISR251, ISR252, ISR253, ISR254, ISR255,
};

void set_idt_entry(int interrupt, void (*base)(), uint16_t segment, uint8_t flags, idt_entry_t* idt)
{
    uintptr_t base_addr        = (uintptr_t) base;
    idt[interrupt].offset_low  = (uint16_t) (base_addr & 0xFFFF);
    idt[interrupt].selector    = segment;
    idt[interrupt].ist         = 0;
    idt[interrupt].type_attr   = flags;
    idt[interrupt].offset_mid  = (uint16_t) ((base_addr >> 16) & 0xFFFF);
    idt[interrupt].offset_high = (uint32_t) ((base_addr >> 32) & 0xFFFFFFFF);
    idt[interrupt].zero        = 0;
}

void enable_idt_entry(int interrupt, idt_entry_t* idt)
{
    idt[interrupt].type_attr |= IDT_FLAG_PRESENT;
}

void disable_idt_entry(int interrupt, idt_entry_t* idt)
{
    idt[interrupt].type_attr &= ~IDT_FLAG_PRESENT;
}

void isr_init(idt_entry_t* idt)
{ 

    for (int interrupt = 0; interrupt < NUM_IDT_ENTRIES; interrupt++)
    {
        set_idt_entry(interrupt, isr_handlers[interrupt], GDT_CODE_SEGMENT, IDT_DEFAULT_GATE_FLAGS, idt);
        enable_idt_entry(interrupt, idt);
    }

}

void idt_init()
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    cpu_info->arch_info.idt_desc = (idt_description_t){sizeof(cpu_info->arch_info.idt) - 1, cpu_info->arch_info.idt};

    isr_init(cpu_info->arch_info.idt);

}

void disable_pic()
{
    outb(PIC_MASTER_DATA_PORT, PIC_MASK_ALL_IRQS);
    outb(PIC_SLAVE_DATA_PORT, PIC_MASK_ALL_IRQS);
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
}

int32_t register_device_irq(arch_irq_handler_t callback)
{
    if (callback == NULL)
    {
        return -1;
    }

    uint8_t vector = ioapic_next_device_vector;
    while (vector <= IRQ_DEVICE_VECTOR_LIMIT && ioapic_irq_handlers[vector] != NULL)
    {
        vector++;
    }

    if (vector > IRQ_DEVICE_VECTOR_LIMIT)
    {
        return -1;
    }

    ioapic_irq_handlers[vector] = callback;
    ioapic_next_device_vector   = vector + 1;

    return vector;
}

// route legacy pic irqs to bsp
static void route_legacy_irqs(volatile uint8_t* ioapic_base, uint32_t max_redir, uint32_t ioapic_gsi_base, uint8_t bsp_lapic_id)
{
    for (uint32_t irq = 0; irq < LEGACY_PIC_MAX_IRQS; irq++)
    {
        uint32_t gsi   = ioapic_gsi_base + irq;
        uint16_t flags = ACPI_MADT_POLARITY_CONFORMING | ACPI_MADT_TRIGGERING_CONFORMING;

        if (dispatcher.arch_info.acpi_iso_overrides[irq].present)
        {
            gsi   = dispatcher.arch_info.acpi_iso_overrides[irq].gsi;
            flags = dispatcher.arch_info.acpi_iso_overrides[irq].flags;
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
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    if (arch_cpu_id() != 0)
    {
        return;
    }

    if (dispatcher.arch_info.ioapic_initialized)
    {
        return;
    }

    if (!dispatcher.arch_info.acpi_has_ioapic || dispatcher.arch_info.acpi_ioapic_base_addr == 0)
    {
        kprintf("Arx kernel: missing IOAPIC info\n");
        panic();
    }

    phys_addr_t ioapic_pa = align_down(dispatcher.arch_info.acpi_ioapic_base_addr, PAGE_SIZE);
    virt_addr_t ioapic_va = pa_to_hhdm(ioapic_pa, cpu_info->numa_node->zone.hhdm_present, cpu_info->numa_node->zone.hhdm_offset);

    if (vmm_virt_to_phys(ioapic_va, cpu_info->address_space) == 0)
    {
        uint64_t map_flags = 0;
        ARCH_PAGE_FLAGS_INIT(map_flags);
        ARCH_PAGE_FLAG_SET_READ(map_flags);
        ARCH_PAGE_FLAG_SET_WRITE(map_flags);
        ARCH_PAGE_FLAG_SET_NOCACHE(map_flags);
        ARCH_PAGE_FLAG_SET_GLOBAL(map_flags);

        vmm_map_page(ioapic_va, ioapic_pa, map_flags, cpu_info->address_space);
    }

    volatile uint8_t* ioapic_base = (volatile uint8_t*) (uintptr_t) ioapic_va;

    REG(uint32_t, ioapic_base + IOAPIC_REG_IOREGSEL) = IOAPIC_REG_VER;
    uint32_t ioapic_ver = REG(uint32_t, ioapic_base + IOAPIC_REG_IOWIN);
    uint32_t max_redir  = (ioapic_ver >> 16) & 0xFF;
    uint32_t ioapic_gsi_base = dispatcher.arch_info.acpi_ioapic_gsi_base;
    uint8_t  bsp_lapic_id    = dispatcher.cpus[0].arch_info.acpi_lapic_id;

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

    dispatcher.arch_info.ioapic_initialized = 1;

    kprintf("Arx kernel: IOAPIC initialized id=%u pa=0x%llx gsi_base=%u max_redir=%u\n", (unsigned) dispatcher.arch_info.acpi_ioapic_id, (unsigned long long) dispatcher.arch_info.acpi_ioapic_base_addr, (unsigned) dispatcher.arch_info.acpi_ioapic_gsi_base,
            (unsigned) max_redir);
}

void init_interrupts()
{

    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    idt_init();
    disable_pic();

    __asm__ __volatile__("lidt %[idt]"
                         :
                         : [idt] "m"(cpu_info->arch_info.idt_desc)
                         : "memory");   

    lapic_init();
    ioapic_init();
    lapic_timer_init();

    arch_enable_interrupts();

    kprintf("Arx kernel: cpu %d interrupts initialized\n", arch_cpu_id());
}

void pagefault_debug(registers_t* reg)
{
    uint64_t cr2;
    uint64_t cr3;
    uint64_t error_code;

    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    error_code = reg->error_code;

    kprintf("Arx kernel: PAGE FAULT DEBUG\n");
    kprintf("  cr2 (fault addr): 0x%llx\n", (unsigned long long) cr2);
    kprintf("  cr3 (pt base):    0x%llx\n", (unsigned long long) cr3);
    kprintf("  rip:              0x%llx\n", (unsigned long long) reg->rip);
    kprintf("  rsp:              0x%llx\n", (unsigned long long) reg->rsp);
    kprintf("  rflags:           0x%llx\n", (unsigned long long) reg->rflags);
    kprintf("  cs:ss:            0x%llx:0x%llx\n", (unsigned long long) reg->cs, (unsigned long long) reg->ss);
    kprintf("  error_code:       0x%llx\n", (unsigned long long) error_code);
    kprintf("  error bits: P=%u W/R=%u U/S=%u RSVD=%u I/D=%u PK=%u SS=%u SGX=%u\n", (unsigned) ((error_code >> 0) & 1), (unsigned) ((error_code >> 1) & 1), (unsigned) ((error_code >> 2) & 1), (unsigned) ((error_code >> 3) & 1),
            (unsigned) ((error_code >> 4) & 1), (unsigned) ((error_code >> 5) & 1), (unsigned) ((error_code >> 6) & 1), (unsigned) ((error_code >> 15) & 1));

    kprintf("  regs: rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n", (unsigned long long) reg->rax, (unsigned long long) reg->rbx, (unsigned long long) reg->rcx, (unsigned long long) reg->rdx);
    kprintf("        rsi=0x%llx rdi=0x%llx rbp=0x%llx\n", (unsigned long long) reg->rsi, (unsigned long long) reg->rdi, (unsigned long long) reg->rbp);
    kprintf("        r8 =0x%llx r9 =0x%llx r10=0x%llx r11=0x%llx\n", (unsigned long long) reg->r8, (unsigned long long) reg->r9, (unsigned long long) reg->r10, (unsigned long long) reg->r11);
    kprintf("        r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n", (unsigned long long) reg->r12, (unsigned long long) reg->r13, (unsigned long long) reg->r14, (unsigned long long) reg->r15);
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


void ISRHANDLER(registers_t* reg)
{
    uint64_t irq = reg->interrupt_number;

    if (reg->interrupt_number >= 32)
    {
        lapic_eoi();
    }

    if (reg->interrupt_number == LAPIC_TIMER_VECTOR)
    {
        return;
    }

    if (reg->interrupt_number == 14)
    {
        pagefault_debug(reg);
    }

    if (irq < NUM_IDT_ENTRIES && ioapic_irq_handlers[irq] != NULL)
    {
        ioapic_irq_handlers[irq](reg);
        return;
    }

    kprintf("Arx kernel: received interrupt: %llu\n", (unsigned long long) reg->interrupt_number);
    panic();
}

void arch_init(void)
{
    gdt_init();
    init_interrupts();

    kprintf("Arx kernel: cpu %d architecture initialized\n", arch_cpu_id());
}