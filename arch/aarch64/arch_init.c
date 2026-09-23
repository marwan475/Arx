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

__attribute__((noreturn)) void arch_enter_user_mode(uint64_t user_rip, uint64_t user_rsp, uint64_t arg0, uint64_t arg1)
{
    (void) user_rip;
    (void) user_rsp;
    (void) arg0;
    (void) arg1;

    panic();
}

void arch_init_context(struct arch_task_context* context, void* stack_top, arch_task_entry_t entry, void* arg)
{
    (void) context;
    (void) stack_top;
    (void) entry;
    (void) arg;
}

void arch_save_switch_and_execute_context(struct arch_task_context* out_current_context, const struct arch_task_context* new_context)
{
    (void) out_current_context;
    (void) new_context;
}

bool arch_init(void)
{
    return false;
}

bool arch_device_init(void)
{
    return false;
}