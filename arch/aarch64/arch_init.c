#include <arch/arch.h>
#include <klib/klib.h>

__attribute__((noreturn)) void arch_set_stack(void* stack_top, arch_stack_entry_t entry, void* arg)
{
    uintptr_t aligned_stack_top = (uintptr_t) stack_top & ~(uintptr_t) 0xFul;

    __asm__ volatile("mov sp, %0\n"
                     "mov x0, %2\n"
                     "blr %1\n"
                     :
                     : "r"(aligned_stack_top), "r"(entry), "r"(arg)
                     : "x0", "memory");

    for (;;)
    {
        arch_halt();
    }
}

bool arch_init(void)
{
    return false;
}