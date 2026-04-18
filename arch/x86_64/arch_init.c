#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/pmm.h>
#include <memory/vmm.h>
#include <uacpi/acpi.h>

static const uint8_t PIC_MASK_ALL_IRQS = 0xFF;

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
        ISR0,   ISR1,   ISR2,   ISR3,   ISR4,   ISR5,   ISR6,   ISR7,   ISR8,   ISR9,   ISR10,  ISR11,  ISR12,  ISR13,  ISR14,  ISR15,  ISR16,  ISR17,  ISR18,  ISR19,  ISR20,  ISR21,  ISR22,  ISR23,  ISR24,  ISR25,  ISR26,  ISR27,  ISR28,  ISR29,  ISR30,  ISR31,
        ISR32,  ISR33,  ISR34,  ISR35,  ISR36,  ISR37,  ISR38,  ISR39,  ISR40,  ISR41,  ISR42,  ISR43,  ISR44,  ISR45,  ISR46,  ISR47,  ISR48,  ISR49,  ISR50,  ISR51,  ISR52,  ISR53,  ISR54,  ISR55,  ISR56,  ISR57,  ISR58,  ISR59,  ISR60,  ISR61,  ISR62,  ISR63,
        ISR64,  ISR65,  ISR66,  ISR67,  ISR68,  ISR69,  ISR70,  ISR71,  ISR72,  ISR73,  ISR74,  ISR75,  ISR76,  ISR77,  ISR78,  ISR79,  ISR80,  ISR81,  ISR82,  ISR83,  ISR84,  ISR85,  ISR86,  ISR87,  ISR88,  ISR89,  ISR90,  ISR91,  ISR92,  ISR93,  ISR94,  ISR95,
        ISR96,  ISR97,  ISR98,  ISR99,  ISR100, ISR101, ISR102, ISR103, ISR104, ISR105, ISR106, ISR107, ISR108, ISR109, ISR110, ISR111, ISR112, ISR113, ISR114, ISR115, ISR116, ISR117, ISR118, ISR119, ISR120, ISR121, ISR122, ISR123, ISR124, ISR125, ISR126, ISR127,
        ISR128, ISR129, ISR130, ISR131, ISR132, ISR133, ISR134, ISR135, ISR136, ISR137, ISR138, ISR139, ISR140, ISR141, ISR142, ISR143, ISR144, ISR145, ISR146, ISR147, ISR148, ISR149, ISR150, ISR151, ISR152, ISR153, ISR154, ISR155, ISR156, ISR157, ISR158, ISR159,
        ISR160, ISR161, ISR162, ISR163, ISR164, ISR165, ISR166, ISR167, ISR168, ISR169, ISR170, ISR171, ISR172, ISR173, ISR174, ISR175, ISR176, ISR177, ISR178, ISR179, ISR180, ISR181, ISR182, ISR183, ISR184, ISR185, ISR186, ISR187, ISR188, ISR189, ISR190, ISR191,
        ISR192, ISR193, ISR194, ISR195, ISR196, ISR197, ISR198, ISR199, ISR200, ISR201, ISR202, ISR203, ISR204, ISR205, ISR206, ISR207, ISR208, ISR209, ISR210, ISR211, ISR212, ISR213, ISR214, ISR215, ISR216, ISR217, ISR218, ISR219, ISR220, ISR221, ISR222, ISR223,
        ISR224, ISR225, ISR226, ISR227, ISR228, ISR229, ISR230, ISR231, ISR232, ISR233, ISR234, ISR235, ISR236, ISR237, ISR238, ISR239, ISR240, ISR241, ISR242, ISR243, ISR244, ISR245, ISR246, ISR247, ISR248, ISR249, ISR250, ISR251, ISR252, ISR253, ISR254, ISR255,
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

void init_interrupts()
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    idt_init();
    disable_pic();

    __asm__ __volatile__("lidt %[idt]" : : [idt] "m"(cpu_info->arch_info.idt_desc) : "memory");

    lapic_init();
    ioapic_init();
    lapic_timer_init();
    arch_enable_interrupts();

    kprintf("Arx kernel: cpu %d interrupts initialized\n", arch_cpu_id());
}

bool arch_init(void)
{
    gdt_init();
    init_interrupts();

    dispatcher.cpus[arch_cpu_id()].initialized = true;

    kprintf("Arx kernel: cpu %d architecture initialized\n", arch_cpu_id());

    return true;
}