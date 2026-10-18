#include <arch/arch.h>
#include <klib/klib.h>
#include <platform.h>

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

void arch_set_user_transition_stack(uint64_t kernel_rsp)
{
    (void) kernel_rsp;
}

void arch_init_context(struct arch_task_context* context, void* stack_top, arch_task_entry_t entry, void* arg)
{
    (void) context;
    (void) stack_top;
    (void) entry;
    (void) arg;
}

uint64_t arch_task_context_stack_pointer(const struct arch_task_context* context)
{
    if (context == NULL)
    {
        return 0;
    }

    return context->sp;
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

bool arch_task_context_supported(void)
{
    return false;
}

void arch_request_reschedule(uint8_t cpu)
{
    (void) cpu;
}

bool arch_scheduler_timer_ready(uint8_t cpu)
{
    (void) cpu;
    return false;
}

bool arch_program_scheduler_timer(uint64_t nanoseconds)
{
    (void) nanoseconds;
    return false;
}

void arch_init_cpu_info(void)
{
    platform.arch = ARCH_AARCH64;
}

void arch_boot_cpu_hardware_ids(const struct boot_info* boot)
{
    (void) boot;
}

bool arch_boot_numa_supported(void)
{
    return false;
}

uint16_t arch_elf_machine(void)
{
    return 183; // EM_AARCH64
}

void arch_calibrate_scheduler_timer(void)
{
}

uint64_t arch_save_and_disable_interrupts(void)
{
    uint64_t state;
    __asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(state) : : "memory");
    return state;
}

void arch_restore_interrupts(uint64_t state)
{
    __asm__ volatile("msr daif, %0" : : "r"(state) : "memory");
}

bool arch_interrupts_enabled(void)
{
    uint64_t state;
    __asm__ volatile("mrs %0, daif" : "=r"(state));
    return (state & (1ULL << 7)) == 0;
}

void arch_idle_wait(void)
{
    arch_enable_interrupts();
    arch_halt();
}

bool arch_user_tls_supported(void)
{
    return false;
}

bool arch_user_tls_base_valid(uint64_t value)
{
    (void) value;
    return false;
}

uint64_t arch_decode_tls_request(uint64_t operation, bool* gs, bool* write)
{
    (void) operation;
    (void) gs;
    (void) write;
    return LINUX_ENOSYS;
}

uint64_t arch_read_user_tls_base(bool gs)
{
    (void) gs;
    return 0;
}

void arch_write_user_tls_base(bool gs, uint64_t value)
{
    (void) gs;
    (void) value;
}

void arch_context_set_tls(struct arch_task_context* context, bool gs, uint64_t value)
{
    (void) context;
    (void) gs;
    (void) value;
}

uint64_t arch_current_syscall_active(void)
{
    return 0;
}

void arch_set_current_syscall_active(uint64_t active)
{
    (void) active;
}

uint64_t arch_frame_instruction_pointer(const arch_syscall_frame_t* frame)
{
    return frame->user_elr;
}

void arch_frame_set_stack_pointer(arch_syscall_frame_t* frame, uint64_t stack)
{
    frame->user_sp = stack;
}

void arch_prepare_exec_frame(arch_syscall_frame_t* frame, uint64_t entry, uint64_t stack)
{
    (void) frame;
    (void) entry;
    (void) stack;
}

void arch_prepare_clone_context(struct arch_task_context* context, const arch_syscall_frame_t* frame, void* stack_top, arch_task_entry_t entry, void* arg)
{
    (void) context;
    (void) frame;
    (void) stack_top;
    (void) entry;
    (void) arg;
}

__attribute__((noreturn)) void arch_resume_user_frame(const arch_syscall_frame_t* frame)
{
    (void) frame;
    for (;;)
    {
        arch_halt();
    }
}

void arch_init_platform_dma(void)
{
}

const arch_syscall_frame_t* arch_clone_frame(void* stack_top)
{
    (void) stack_top;
    return NULL;
}

// This target has no coherent DMA platform backend yet.
bool arch_dma_coherent_memory_supported(void)
{
    return false;
}

void arch_dma_write_barrier(void)
{
    __asm__ volatile("dmb oshst" : : : "memory");
}

void arch_dma_read_barrier(void)
{
    __asm__ volatile("dmb oshld" : : : "memory");
}
