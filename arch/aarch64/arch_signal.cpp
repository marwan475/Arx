#include <arch/arch.h>
#include <klib/klib.h>

// User signal contexts are not implemented on this architecture yet.
uint64_t arch_signal_validate_action(const signal_action_t*)
{
    return LINUX_EOPNOTSUPP;
}
uint64_t arch_signal_stack_pointer(const arch_syscall_frame_t* frame)
{
    return frame->user_sp;
}

bool arch_signal_user_return(const arch_syscall_frame_t*)
{
    return false;
}
uint64_t arch_signal_prepare(arch_syscall_frame_t*, uint64_t, const signal_action_t*, const signal_stack_t*, const signal_sender_t*, uint64_t, int, bool)
{
    return LINUX_EOPNOTSUPP;
}
uint64_t arch_signal_restore(arch_syscall_frame_t*, uint64_t*, uint64_t*, signal_stack_t*)
{
    return LINUX_EOPNOTSUPP;
}
