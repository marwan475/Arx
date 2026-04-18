#include <arch/arch.h>
#include <klib/klib.h>

static void pagefault_debug(registers_t* reg)
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
    kprintf("  error bits: P=%u W/R=%u U/S=%u RSVD=%u I/D=%u PK=%u SS=%u SGX=%u\n", (unsigned) ((error_code >> 0) & 1), (unsigned) ((error_code >> 1) & 1), (unsigned) ((error_code >> 2) & 1), (unsigned) ((error_code >> 3) & 1), (unsigned) ((error_code >> 4) & 1), (unsigned) ((error_code >> 5) & 1),
            (unsigned) ((error_code >> 6) & 1), (unsigned) ((error_code >> 15) & 1));

    kprintf("  regs: rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n", (unsigned long long) reg->rax, (unsigned long long) reg->rbx, (unsigned long long) reg->rcx, (unsigned long long) reg->rdx);
    kprintf("        rsi=0x%llx rdi=0x%llx rbp=0x%llx\n", (unsigned long long) reg->rsi, (unsigned long long) reg->rdi, (unsigned long long) reg->rbp);
    kprintf("        r8 =0x%llx r9 =0x%llx r10=0x%llx r11=0x%llx\n", (unsigned long long) reg->r8, (unsigned long long) reg->r9, (unsigned long long) reg->r10, (unsigned long long) reg->r11);
    kprintf("        r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n", (unsigned long long) reg->r12, (unsigned long long) reg->r13, (unsigned long long) reg->r14, (unsigned long long) reg->r15);
}

// cpus ipi is locked when this is called. so handler needs to unlock it
void handle_ipi(registers_t* reg)
{
    cpu_info_t* cpu_info = &dispatcher.cpus[arch_cpu_id()];

    if (!cpu_info->arch_info.acpi_has_lapic)
    {
        return;
    }

    if (reg->interrupt_number != LAPIC_IPI_VECTOR)
    {
        return;
    }

    ipi_request_type_t request_type = IPI_REQUEST_NONE;

    request_type = cpu_info->ipi_request;

    kprintf("Arx kernel: cpu %d received IPI with request type %d\n", arch_cpu_id(), (int) request_type);

    cpu_info->ipi_request = IPI_REQUEST_NONE;
    spinlock_release(&cpu_info->ipi_lock);
}

void ISRHANDLER(registers_t* reg)
{
    if (reg->interrupt_number >= 32)
    {
        lapic_eoi();
    }

    if (reg->interrupt_number == LAPIC_TIMER_VECTOR)
    {
        return;
    }

    if (reg->interrupt_number == LAPIC_IPI_VECTOR)
    {
        handle_ipi(reg);
        return;
    }

    if (reg->interrupt_number == 14)
    {
        pagefault_debug(reg);
    }

    kprintf("Arx kernel: received interrupt: %llu\n", (unsigned long long) reg->interrupt_number);
    panic();
}
