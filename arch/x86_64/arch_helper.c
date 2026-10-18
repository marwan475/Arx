#include <arch/arch.h>
#include <klib/klib.h>

extern void arch_x86_64_enter_user_mode(uint64_t user_rip, uint64_t user_rsp, uint64_t arg0, uint64_t arg1) __attribute__((noreturn));

uint64_t arch_task_context_stack_pointer(const struct arch_task_context* context)
{
    if (context == NULL)
    {
        return 0;
    }

    return context->rsp;
}

static bool is_low_half_user_va(uint64_t va)
{
    return (va & 0xFFFF800000000000ULL) == 0;
}

__attribute__((weak, noreturn)) void task_exit(void)
{
    for (;;)
    {
        arch_halt();
    }
}

__attribute__((noreturn)) void arch_set_stack(void* stack_top, arch_stack_entry_t entry, void* arg)
{
    __asm__ volatile("mov %0, %%rsp\n"
                     "andq $-16, %%rsp\n"
                     "mov %2, %%rdi\n"
                     "call *%1\n"
                     :
                     : "r"(stack_top), "r"(entry), "r"(arg)
                     : "rdi", "memory");

    for (;;)
    {
        arch_halt();
    }
}

__attribute__((noreturn)) void arch_enter_user_mode(uint64_t user_rip, uint64_t user_rsp, uint64_t arg0, uint64_t arg1)
{
    if (!is_low_half_user_va(user_rip) || !is_low_half_user_va(user_rsp))
    {
        kprintf("Arx kernel: arch_enter_user_mode rejected non-user VA rip=0x%llx rsp=0x%llx\n", (unsigned long long) user_rip, (unsigned long long) user_rsp);
        panic();
    }

    if ((user_rsp & 0xFULL) != 0)
    {
        kprintf("Arx kernel: arch_enter_user_mode rejected user rsp (expected rsp%%16 == 0 for ELF entry): 0x%llx\n", (unsigned long long) user_rsp);
        panic();
    }

    arch_x86_64_enter_user_mode(user_rip, user_rsp, arg0, arg1);

    __builtin_unreachable();
}

bool arch_task_context_supported(void)
{
    return true;
}

bool arch_boot_numa_supported(void)
{
    return true;
}

uint16_t arch_elf_machine(void)
{
    return 62; // EM_X86_64
}

uint64_t arch_save_and_disable_interrupts(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void arch_restore_interrupts(uint64_t flags)
{
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

bool arch_interrupts_enabled(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (flags & (1ULL << 9)) != 0;
}

void arch_idle_wait(void)
{
    // STI's interrupt shadow closes the wakeup race before HLT.
    __asm__ volatile("sti; hlt" : : : "memory");
}
