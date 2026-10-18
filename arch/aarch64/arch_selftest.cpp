#include <arch/selftest.hpp>
#include <boot/limine.h>
#include <selftests/request/RequestTestSupport.hpp>

using namespace request_selftest;

static_assert(sizeof(boot_smp_cpu_info) == 40 && sizeof(boot_smp_cpu_info) == sizeof(limine_smp_info), "AArch64 boot SMP ABI size");
static_assert(offsetof(boot_smp_cpu_info, mpidr) == offsetof(limine_smp_info, mpidr) && offsetof(boot_smp_cpu_info, goto_address) == offsetof(limine_smp_info, goto_address) && offsetof(boot_smp_cpu_info, extra_argument) == offsetof(limine_smp_info, extra_argument), "AArch64 boot SMP ABI offsets");

uint16_t arch_selftest_elf_machine(void)
{
    return 183;
}

const char* arch_selftest_machine_name(void)
{
    return "aarch64";
}

const virt_addr_t* arch_selftest_invalid_addresses(size_t* count)
{
    *count = 0;
    return nullptr;
}

arch_selftest_page_permissions_t arch_selftest_page_permissions(phys_addr_t, uint64_t)
{
    // Match the unavailable leaf-entry sentinel used by the original fixture.
    return {false, true, false};
}

void arch_selftest_user_frame(arch_syscall_frame_t*, uint64_t, uint64_t, void*)
{
}

void arch_selftest_signal_restorer(arch_syscall_frame_t*)
{
}

const uint8_t* arch_selftest_syscall_instruction(size_t* size)
{
    *size = 0;
    return nullptr;
}

bool arch_selftest_syscall_probe(const arch_syscall_frame_t*)
{
    return false;
}

bool arch_selftest_load_child(process_t*, uint64_t, uint32_t)
{
    return false;
}

bool arch_selftest_load_resource_child(process_t*, uint64_t, uint32_t)
{
    return false;
}

bool arch_selftest_signal_return(process_t*, ResourceLayerCaps*, SignalManager*, const signal_action_t&, uint64_t, uint64_t)
{
    return false;
}

bool arch_selftest_timer_signal(process_t*, task_t*, SignalManager*, uint64_t, uint64_t, uint64_t)
{
    return false;
}

bool arch_selftest_event_signal(process_t*, task_t*, SignalManager*, arch_syscall_frame_t&, const signal_action_t&, uint64_t, uint64_t)
{
    return false;
}

void arch_selftest_set_instruction_pointer(arch_syscall_frame_t* frame, uint64_t instruction)
{
    frame->user_elr = instruction;
}

bool arch_selftest_context_tls_matches(const struct arch_task_context*, uint64_t)
{
    return false;
}

bool arch_selftest_tls(process_t*)
{
    return expect(syscall_call(SYSCALL_arch_prctl) == LINUX_ENOSYS, "arch_prctl x86-only");
}
