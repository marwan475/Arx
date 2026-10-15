#include "layers/Logic/SignalManager.hpp"

#include <arch/arch.h>
#include <kernel/platform/memory/vmm.h>
#include <klib/klib.h>

namespace
{
struct signal_context_t
{
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, di, si, bp, bx, dx, ax, cx, sp, ip, flags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};
struct signal_ucontext_t
{
    uint64_t         flags, link;
    signal_stack_t   stack;
    signal_context_t context;
    uint64_t         mask;
};
struct signal_frame_t
{
    uint64_t          restorer;
    signal_ucontext_t uc;
    uint8_t           info[128];
};
static_assert(sizeof(signal_context_t) == 256 && sizeof(signal_ucontext_t) == 304 && sizeof(signal_frame_t) == 440, "Linux x86-64 rt signal frame ABI");
static_assert(offsetof(signal_ucontext_t, context) == 40 && offsetof(signal_context_t, r13) == 40, "user fixture R13 context offset");

constexpr uint64_t RESTORER = 0x04000000, ONSTACK = 0x08000000, RESTART = 0x10000000;
bool               user_address(uint64_t address)
{
    return address >= CANONICAL_USER_BASE && address <= CANONICAL_USER_END;
}
bool on_stack(const signal_stack_t& stack, uint64_t sp)
{
    return !(stack.flags & 2) && sp >= stack.sp && sp - stack.sp < stack.size;
}
} // namespace

uint64_t arch_signal_validate_action(const signal_action_t* action)
{
    if (!user_address(action->handler))
    {
        return LINUX_EFAULT;
    }

    if (!(action->flags & RESTORER) || !user_address(action->restorer))
    {
        return LINUX_EOPNOTSUPP;
    }

    return 0;
}

uint64_t arch_signal_stack_pointer(const arch_syscall_frame_t* frame)
{
    return frame->user_rsp;
}

bool arch_signal_user_return(const arch_syscall_frame_t* frame)
{
    return (frame->user_cs & 3) == 3;
}

uint64_t arch_signal_prepare(arch_syscall_frame_t* frame, uint64_t result, const signal_action_t* actionPtr, const signal_stack_t* stackPtr, const signal_sender_t* senderPtr, uint64_t mask, int signal, bool restartable)
{
    if (frame->fxstate == 0)
    {
        return LINUX_EINVAL;
    }

    const auto&    action = *actionPtr;
    const auto&    stack  = *stackPtr;
    const auto&    sender = *senderPtr;
    signal_frame_t saved  = {};
    saved.restorer        = action.restorer;
    saved.uc.stack        = stack;

    if (on_stack(stack, frame->user_rsp))
    {
        saved.uc.stack.flags = 1;
    }

    saved.uc.mask = mask;
    auto& c       = saved.uc.context;
    c.r8          = frame->arg4;
    c.r9          = frame->arg5;
    c.r10         = frame->arg3;
    c.r11         = frame->r11;
    c.r12         = frame->r12;
    c.r13         = frame->r13;
    c.r14         = frame->r14;
    c.r15         = frame->r15;
    c.di          = frame->arg0;
    c.si          = frame->arg1;
    c.bp          = frame->rbp;
    c.bx          = frame->rbx;
    c.dx          = frame->arg2;
    c.ax          = result;
    c.cx          = frame->rcx;
    c.sp          = frame->user_rsp;
    c.ip          = frame->user_rip;
    c.flags       = frame->user_rflags;
    c.cs          = USER_CS;
    c.ss          = USER_SS;
    c.oldmask     = saved.uc.mask;
    // Restart only syscalls whose state was not consumed, and only for an
    // actual SYSCALL instruction. Poll/select/nanosleep/pause retain EINTR.
    uint16_t instruction = 0;

    if (result == (uint64_t) -4 && restartable && (action.flags & RESTART) && c.ip >= 2 && safe_read_user(c.ip - 2, &instruction, 2) && instruction == 0x050f)
    {
        c.ip -= 2;
        c.ax = frame->syscall_number;
    }
    uint64_t top = frame->user_rsp;

    if ((action.flags & ONSTACK) && !on_stack(stack, top) && !(stack.flags & 2))
    {
        top = stack.sp + stack.size;
    }

    if (top < 128 + 512 + sizeof(saved) + 128)
    {
        return LINUX_EFAULT;
    }

    const uint64_t fp      = align_down(top - 128 - 512, 64);
    const uint64_t address = align_down(fp - sizeof(saved), 16) - 8;
    c.fpstate              = fp;
    int32_t info[3]        = {signal, 0, sender.code};
    memcpy(saved.info, info, sizeof(info));
    memcpy(saved.info + 16, &sender.pid, 4);
    memcpy(saved.info + 20, &sender.uid, 4);
    uint8_t floating[512] = {};
    memcpy(floating, (const void*) frame->fxstate, 416);

    if (!safe_write_user(fp, floating, sizeof(floating)) || !safe_write_user(address, &saved, sizeof(saved)))
    {
        return LINUX_EFAULT;
    }

    frame->user_rip    = action.handler;
    frame->user_rsp    = address;
    frame->arg0        = signal;
    frame->arg1        = address + offsetof(signal_frame_t, info);
    frame->arg2        = address + offsetof(signal_frame_t, uc);
    frame->user_rflags = (frame->user_rflags & 0x250dd5ULL & ~((1ULL << 8) | (1ULL << 10))) | 0x202;
    return 0;
}

uint64_t arch_signal_restore(arch_syscall_frame_t* frame, uint64_t* result, uint64_t* mask, signal_stack_t* restoredStack)
{
    if (frame->fxstate == 0 || frame->user_rsp < 8)
    {
        return LINUX_EINVAL;
    }

    const uint64_t address = frame->user_rsp - 8;
    signal_frame_t saved;

    if (!safe_read_user(address, &saved, sizeof(saved)))
    {
        return LINUX_EFAULT;
    }

    if (saved.uc.flags != 0)
    {
        return LINUX_EOPNOTSUPP; // XSAVE/extended context formats are absent.
    }

    auto& c = saved.uc.context;

    if (!user_address(c.ip) || !user_address(c.sp) || c.cs != USER_CS || c.ss != USER_SS)
    {
        return LINUX_EINVAL;
    }

    uint8_t floating[512];

    if (c.fpstate == 0 || !safe_read_user(c.fpstate, floating, sizeof(floating)))
    {
        return LINUX_EFAULT;
    }

    uint32_t mxcsr, mxcsrMask;
    memcpy(&mxcsr, floating + 24, 4);
    memcpy(&mxcsrMask, (const uint8_t*) frame->fxstate + 28, 4);

    if (mxcsr & ~(mxcsrMask != 0 ? mxcsrMask : 0xffbfU))
    {
        return LINUX_EINVAL;
    }

    signal_stack_t stack = saved.uc.stack;

    if (stack.flags == 1)
    {
        stack.flags = 0;
    }

    if (stack.flags != 0 && stack.flags != 2)
    {
        return LINUX_EINVAL;
    }

    if (stack.flags == 0 && (stack.size < 2048 || !range_within_u64(stack.sp, stack.size, CANONICAL_USER_BASE, CANONICAL_USER_END)))
    {
        return LINUX_EINVAL;
    }

    memcpy((void*) frame->fxstate, floating, sizeof(floating));
    frame->arg4        = c.r8;
    frame->arg5        = c.r9;
    frame->arg3        = c.r10;
    frame->r11         = c.r11;
    frame->r12         = c.r12;
    frame->r13         = c.r13;
    frame->r14         = c.r14;
    frame->r15         = c.r15;
    frame->arg0        = c.di;
    frame->arg1        = c.si;
    frame->rbp         = c.bp;
    frame->rbx         = c.bx;
    frame->arg2        = c.dx;
    frame->rcx         = c.cx;
    frame->user_rsp    = c.sp;
    frame->user_rip    = c.ip;
    frame->user_cs     = USER_CS;
    frame->user_ss     = USER_SS;
    frame->user_rflags = (c.flags & 0x250dd5ULL) | 0x202;
    *mask              = saved.uc.mask;
    *restoredStack     = stack;
    *result            = c.ax;
    return 0;
}

extern "C" uint64_t dispatcher_signal_return(arch_syscall_frame_t*, uint64_t);
extern "C" void     arch_signal_interrupt(registers_t* r, void* floating)
{
    arch_syscall_frame_t f = {};
    f.syscall_number       = UINT64_MAX;
    f.arg0                 = r->rdi;
    f.arg1                 = r->rsi;
    f.arg2                 = r->rdx;
    f.arg3                 = r->r10;
    f.arg4                 = r->r8;
    f.arg5                 = r->r9;
    f.rbx                  = r->rbx;
    f.rbp                  = r->rbp;
    f.r12                  = r->r12;
    f.r13                  = r->r13;
    f.r14                  = r->r14;
    f.r15                  = r->r15;
    f.rcx                  = r->rcx;
    f.r11                  = r->r11;
    f.user_rip             = r->rip;
    f.user_rsp             = r->rsp;
    f.user_rflags          = r->rflags;
    f.user_cs              = r->cs;
    f.user_ss              = r->ss;
    f.fxstate              = (uint64_t) floating;
    const uint64_t result  = dispatcher_signal_return(&f, r->rax);
    r->rax                 = result;
    r->rdi                 = f.arg0;
    r->rsi                 = f.arg1;
    r->rdx                 = f.arg2;
    r->r10                 = f.arg3;
    r->r8                  = f.arg4;
    r->r9                  = f.arg5;
    r->rbx                 = f.rbx;
    r->rbp                 = f.rbp;
    r->r12                 = f.r12;
    r->r13                 = f.r13;
    r->r14                 = f.r14;
    r->r15                 = f.r15;
    r->rcx                 = f.rcx;
    r->r11                 = f.r11;
    r->rip                 = f.user_rip;
    r->rsp                 = f.user_rsp;
    r->rflags              = f.user_rflags;
}
