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

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low  = 0;
    uint32_t high = 0;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t) high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low  = (uint32_t) value;
    uint32_t high = (uint32_t) (value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static uint64_t read_rsp(void)
{
    uint64_t rsp = 0;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

extern void     arch_x86_64_syscall_entry(void);
extern uint64_t arch_syscall_dispatch(uint64_t syscall_number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

void arch_syscall_set_kernel_stack(uint64_t kernel_rsp)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    cpu_info->arch_info.syscall_ctx.kernel_rsp = kernel_rsp & ~0xFULL;
}

void arch_syscall_init(void)
{
    cpu_info_t* cpu_info = &platform.cpus[arch_cpu_id()];

    cpu_info->arch_info.syscall_ctx.kernel_rsp = read_rsp() & ~0xFULL;
    cpu_info->arch_info.syscall_ctx.user_rsp   = 0;

    const uint64_t star = ((uint64_t) (USER_CS & ~0x3ULL) << 48) | ((uint64_t) KERNEL_CS << 32);
    const uint64_t lstar = (uint64_t) (uintptr_t) arch_x86_64_syscall_entry;
    const uint64_t fmask = RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_NT | RFLAGS_AC;

    wrmsr(IA32_STAR, star);
    wrmsr(IA32_LSTAR, lstar);
    wrmsr(IA32_FMASK, fmask);

    wrmsr(IA32_GS_BASE, 0);
    wrmsr(IA32_KERNEL_GS_BASE, (uint64_t) (uintptr_t) &cpu_info->arch_info.syscall_ctx);

    uint64_t efer = rdmsr(IA32_EFER);
    efer |= IA32_EFER_SCE;
    wrmsr(IA32_EFER, efer);
}

uint64_t arch_syscall_dispatch(uint64_t syscall_number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    (void) arg0;
    (void) arg1;
    (void) arg2;
    (void) arg3;
    (void) arg4;
    (void) arg5;

    switch (syscall_number)
    {
        default:
            return (uint64_t) -38;
    }
}