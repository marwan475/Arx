#include <arch/arch.h>
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <platform.h>

#define IA32_EFER 0xC0000080u
#define IA32_STAR 0xC0000081u
#define IA32_LSTAR 0xC0000082u
#define IA32_FMASK 0xC0000084u
#define IA32_GS_BASE 0xC0000101u
#define IA32_KERNEL_GS_BASE 0xC0000102u

#define IA32_EFER_SCE (1ULL << 0)

#define RFLAGS_TF (1ULL << 8)
#define RFLAGS_IF (1ULL << 9)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_NT (1ULL << 14)
#define RFLAGS_AC (1ULL << 18)

extern uint64_t selftest_syscall_dispatch(const arch_syscall_frame_t* frame, bool* handled);
extern uint64_t dispatcher_dispatch_syscall(const arch_syscall_frame_t* frame);
extern void     dispatcher_account_mode(bool userMode);

_Static_assert(sizeof(arch_syscall_frame_t) == 176, "syscall frame size must match entry assembly");
_Static_assert(offsetof(arch_syscall_frame_t, rbx) == 96 && offsetof(arch_syscall_frame_t, fxstate) == 160, "signal register extension offsets");
extern uint64_t dispatcher_signal_return(arch_syscall_frame_t* frame, uint64_t result);
_Static_assert(offsetof(arch_syscall_frame_t, arg3) == 32, "R10 syscall argument offset");
_Static_assert(offsetof(arch_info_t, syscall_ctx.active) - offsetof(arch_info_t, syscall_ctx) == 16, "syscall active GS offset");
_Static_assert(offsetof(struct arch_task_context, rflags) == 64, "context RFLAGS offset");
_Static_assert(offsetof(struct arch_task_context, fxState) == 80, "aligned FXSAVE context offset");
_Static_assert(offsetof(struct arch_task_context, fsBase) == 592, "context FS base offset");
_Static_assert(offsetof(struct arch_task_context, userGsBase) == 600, "context user GS base offset");

extern void     arch_x86_64_syscall_entry(void);
extern uint64_t arch_syscall_dispatch(const arch_syscall_frame_t* frame);

uint64_t arch_read_user_tls_base(bool gs)
{
    return x86_read_msr(gs ? IA32_KERNEL_GS_BASE : 0xc0000100U);
}

bool arch_user_random_seed(uint64_t seed[2])
{
    uint32_t eax = 1;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

    if ((ecx & (1U << 30)) == 0)
    {
        return false;
    }

    for (unsigned index = 0; index < 2; ++index)
    {
        unsigned char ready = 0;

        for (unsigned retry = 0; retry < 16 && ready == 0; ++retry)
        {
            __asm__ volatile("rdrand %0; setc %1" : "=r"(seed[index]), "=qm"(ready) : : "cc");
        }

        if (ready == 0)
        {
            return false;
        }
    }

    return true;
}

void arch_write_user_tls_base(bool gs, uint64_t value)
{
    // Kernel GS stays active; swapgs installs this base on return to userspace.
    x86_write_msr(gs ? IA32_KERNEL_GS_BASE : 0xc0000100U, value);
}

void arch_syscall_set_kernel_stack(uint64_t kernel_rsp)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    cpu_info->arch_info.syscall_ctx.kernel_rsp = kernel_rsp & ~0xFULL;
}

void arch_set_user_transition_stack(uint64_t kernel_rsp)
{
    cpu_info_t*    cpu_info    = &platform.cpus[arch_cpu_id()];
    const uint64_t aligned_rsp = kernel_rsp & ~0xFULL;

    arch_syscall_set_kernel_stack(aligned_rsp);
    x86_set_tss_rsp0(&cpu_info->arch_info.tss, aligned_rsp);
}

void arch_syscall_init(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    cpu_info->arch_info.syscall_ctx.kernel_rsp = x86_read_rsp() & ~0xFULL;
    cpu_info->arch_info.syscall_ctx.user_rsp   = 0;
    cpu_info->arch_info.syscall_ctx.active     = 0;

    const uint64_t star  = ((uint64_t) (USER_CS & ~0x3ULL) << 48) | ((uint64_t) KERNEL_CS << 32);
    const uint64_t lstar = (uint64_t) (uintptr_t) arch_x86_64_syscall_entry;
    // Protect the user-to-kernel stack transition. Entry assembly enables
    // interrupts once the syscall frame is safely on the kernel stack.
    const uint64_t fmask = RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_NT | RFLAGS_AC;

    x86_write_msr(IA32_STAR, star);
    x86_write_msr(IA32_LSTAR, lstar);
    x86_write_msr(IA32_FMASK, fmask);

    // Kernel mode always owns the per-CPU GS base. User entry/exit swap it.
    x86_write_msr(IA32_GS_BASE, (uint64_t) (uintptr_t) &cpu_info->arch_info.syscall_ctx);
    x86_write_msr(IA32_KERNEL_GS_BASE, 0);

    uint64_t efer = x86_read_msr(IA32_EFER);
    efer |= IA32_EFER_SCE;
    x86_write_msr(IA32_EFER, efer);
}

uint64_t arch_syscall_dispatch(const arch_syscall_frame_t* frame)
{
    if (frame == NULL)
    {
        return (uint64_t) -22;
    }

    bool     handledBySelftest = false;
    uint64_t selftestResult    = selftest_syscall_dispatch(frame, &handledBySelftest);

    if (handledBySelftest)
    {
        return selftestResult;
    }

    uint64_t result = dispatcher_dispatch_syscall(frame);
    result          = dispatcher_signal_return((arch_syscall_frame_t*) frame, result);
    dispatcher_account_mode(true);
    return result;
}

bool arch_user_tls_supported(void)
{
    return true;
}

bool arch_user_tls_base_valid(uint64_t value)
{
    return value < (1ULL << 47);
}

uint64_t arch_decode_tls_request(uint64_t operation, bool* gs, bool* write)
{
    const int32_t op = (int32_t) operation;
    if (op < 0x1001 || op > 0x1004)
    {
        return LINUX_EINVAL;
    }
    *gs    = op == 0x1001 || op == 0x1004;
    *write = op == 0x1001 || op == 0x1002;
    return 0;
}

void arch_context_set_tls(struct arch_task_context* context, bool gs, uint64_t value)
{
    if (gs)
    {
        context->userGsBase = value;
    }
    else
    {
        context->fsBase = value;
    }
}

uint64_t arch_current_syscall_active(void)
{
    return platform.cpus[arch_cpu_id()].arch_info.syscall_ctx.active;
}

void arch_set_current_syscall_active(uint64_t active)
{
    platform.cpus[arch_cpu_id()].arch_info.syscall_ctx.active = active;
}

uint64_t arch_frame_instruction_pointer(const arch_syscall_frame_t* frame)
{
    return frame->user_rip;
}

void arch_frame_set_stack_pointer(arch_syscall_frame_t* frame, uint64_t stack)
{
    frame->user_rsp = stack;
}

void arch_prepare_exec_frame(arch_syscall_frame_t* frame, uint64_t entry, uint64_t stack)
{
    const uint64_t floating = frame->fxstate;
    memset(frame, 0, sizeof(*frame));
    frame->fxstate     = floating;
    frame->user_rip    = entry;
    frame->user_rsp    = stack;
    frame->user_cs     = USER_CS;
    frame->user_ss     = USER_SS;
    frame->user_rflags = 0x202;
    if (floating != 0)
    {
        memset((void*) floating, 0, 512);
        *(uint16_t*) floating        = 0x37f;
        *(uint32_t*) (floating + 24) = 0x1f80;
    }
}

void arch_prepare_clone_context(struct arch_task_context* context, const arch_syscall_frame_t* frame, void* stack_top, arch_task_entry_t entry, void* arg)
{
    arch_syscall_frame_t* saved = (arch_syscall_frame_t*) arch_clone_frame(stack_top);
    *saved                      = *frame;
    void* floating              = (uint8_t*) saved - 512;
    memset(floating, 0, 512);
    if (frame->fxstate != 0)
    {
        memcpy(floating, (void*) frame->fxstate, 512);
    }
    else
    {
        *(uint16_t*) floating                   = 0x37f;
        *(uint32_t*) ((uint8_t*) floating + 24) = 0x1f80;
    }
    saved->fxstate = (uintptr_t) floating;
    arch_init_context(context, floating, entry, arg);
    arch_context_set_tls(context, false, arch_read_user_tls_base(false));
    arch_context_set_tls(context, true, arch_read_user_tls_base(true));
}

const arch_syscall_frame_t* arch_clone_frame(void* stack_top)
{
    return (const arch_syscall_frame_t*) ((uint8_t*) stack_top - sizeof(arch_syscall_frame_t));
}
