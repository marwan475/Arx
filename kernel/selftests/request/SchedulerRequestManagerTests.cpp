#include "RequestTestSupport.hpp"

namespace request_selftest
{
static bool validate_sched_yield(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t dispatchedFrame = *(ctx->frame);

    dispatchedFrame.syscall_number = SYSCALL_sched_yield;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&dispatchedFrame);

    return REQUEST_EXPECT(dispatchResult == 0);
}

static bool validate_getcpu(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    uint32_t cpuValue  = UINT32_MAX;
    uint32_t nodeValue = UINT32_MAX;

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_getcpu;
    frame.arg0           = request_user_address(&cpuValue, sizeof(cpuValue));
    frame.arg1           = request_user_address(&nodeValue, sizeof(nodeValue));
    frame.arg2           = 0;

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);

    bool ok = equal(dispatchResult, 0, "getcpu succeeds with valid outputs");

    ok &= equal(cpuValue, arch_cpu_id(), "getcpu reports the executing CPU");

    ok &= equal(nodeValue, 0, "getcpu reports the boot NUMA node");

    frame.arg0 = CANONICAL_KERNEL_BASE;

    ok &= equal(dispatcher_dispatch_syscall(&frame), LINUX_EFAULT, "getcpu rejects kernel output pointers");

    return ok;
}

static bool validate_sched_getaffinity(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    uint8_t mask[sizeof(uint64_t)] = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_sched_getaffinity;
    frame.arg0           = 0;
    frame.arg1           = sizeof(mask);
    frame.arg2           = request_user_address(mask, sizeof(mask));

    const uint64_t dispatchResult = dispatcher_dispatch_syscall(&frame);

    if (dispatchResult != sizeof(uint64_t))
    {
        return false;
    }

    uint64_t affinityMask = 0;

    memcpy(&affinityMask, mask, sizeof(affinityMask));

    const uint64_t cpuCount     = platform.cpu_count < BOOT_SMP_MAX_CPUS ? platform.cpu_count : BOOT_SMP_MAX_CPUS;
    const uint64_t expectedMask = cpuCount == 64 ? UINT64_MAX : (1ULL << cpuCount) - 1;

    bool ok = equal(affinityMask, expectedMask, "affinity contains the boot CPU set");

    arch_syscall_frame_t shortFrame = frame;

    shortFrame.arg1 = sizeof(uint32_t);

    const uint64_t shortResult = dispatcher_dispatch_syscall(&shortFrame);

    ok &= equal(shortResult, LINUX_EINVAL, "affinity rejects undersized CPU sets");

    frame.arg2 = CANONICAL_KERNEL_BASE;

    ok &= equal(dispatcher_dispatch_syscall(&frame), LINUX_EFAULT, "affinity rejects kernel output pointers");

    return ok;
}

void test_scheduler_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<SchedulerRequestManager> tests[] = {
        {"HandleSched_yieldRequest", &SchedulerRequestManager::HandleSched_yieldRequest, SYSCALL_sched_yield, validate_sched_yield},
        {"HandleSched_getaffinityRequest", &SchedulerRequestManager::HandleSched_getaffinityRequest, SYSCALL_sched_getaffinity, validate_sched_getaffinity},
        {"HandleGetcpuRequest", &SchedulerRequestManager::HandleGetcpuRequest, SYSCALL_getcpu, validate_getcpu},
    };

    run_syscalls(caps->schedulerRequestManager, tests);
}

} // namespace request_selftest
