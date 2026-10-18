#include <arch/selftest.hpp>
#include <boot/limine.h>
#include <selftests/request/RequestTestSupport.hpp>

using namespace request_selftest;

static_assert(sizeof(boot_smp_cpu_info) == 32 && sizeof(boot_smp_cpu_info) == sizeof(limine_smp_info), "x86-64 boot SMP ABI size");
static_assert(offsetof(boot_smp_cpu_info, lapic_id) == offsetof(limine_smp_info, lapic_id) && offsetof(boot_smp_cpu_info, goto_address) == offsetof(limine_smp_info, goto_address) && offsetof(boot_smp_cpu_info, extra_argument) == offsetof(limine_smp_info, extra_argument),
              "x86-64 boot SMP ABI offsets");

// Independent Linux x86-64 ABI fixture for inspecting delivered user signal frames.
struct signal_context_fixture_t
{
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15, di, si, bp, bx, dx, ax, cx, sp, ip, flags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2, fpstate, reserved[8];
};

struct signal_ucontext_fixture_t
{
    uint64_t                 flags, link;
    signal_stack_t           stack;
    signal_context_fixture_t context;
    uint64_t                 mask;
};

struct signal_frame_fixture_t
{
    uint64_t                  restorer;
    signal_ucontext_fixture_t uc;
    uint8_t                   info[128];
};

static_assert(sizeof(signal_context_fixture_t) == 256 && sizeof(signal_ucontext_fixture_t) == 304 && sizeof(signal_frame_fixture_t) == 440, "Linux x86-64 rt signal frame ABI");

static_assert(offsetof(signal_ucontext_fixture_t, context) == 40 && offsetof(signal_context_fixture_t, r13) == 40, "user fixture R13 context offset");

static_assert(sizeof(arch_syscall_frame_t) == 176, "x86-64 syscall frame size must match entry assembly");

static_assert(offsetof(arch_syscall_frame_t, arg0) == 8, "RDI syscall argument offset");

static_assert(offsetof(arch_syscall_frame_t, arg1) == 16, "RSI syscall argument offset");

static_assert(offsetof(arch_syscall_frame_t, arg2) == 24, "RDX syscall argument offset");

static_assert(offsetof(arch_syscall_frame_t, arg3) == 32, "R10 syscall argument offset");

static_assert(offsetof(arch_syscall_frame_t, arg4) == 40, "R8 syscall argument offset");

static_assert(offsetof(arch_syscall_frame_t, arg5) == 48, "R9 syscall argument offset");

bool arch_selftest_signal_return(process_t* process, ResourceLayerCaps* resources, SignalManager* signals, const signal_action_t& action, uint64_t base, uint64_t input)
{
    bool                ok            = true;
    alignas(16) uint8_t floating[512] = {};

    uint32_t mxcsr = 0x1f80, mask = 0xffbf;

    memcpy(floating + 24, &mxcsr, 4);

    memcpy(floating + 28, &mask, 4);

    floating[160] = 0x5a;

    arch_syscall_frame_t f = {};

    f.syscall_number = SYSCALL_futex;
    f.user_cs        = USER_CS;
    f.user_ss        = USER_SS;
    f.user_rflags    = 0x202;
    f.user_rip       = base + 514;
    f.user_rsp       = base + 6 * PAGE_SIZE - 16;
    f.rbx            = 0x123456789abcdef0ULL;
    f.r13            = 0xaabbccdd;
    f.fxstate        = (uint64_t) floating;

    const uint16_t syscallInstruction = 0x050f;

    fixture_store(process, base + 512, &syscallInstruction, 2);

    uint8_t        queuedInfo[128] = {};
    const int32_t  queuedCode      = -1;
    const int32_t  queuedPid       = process->id;
    const uint64_t queuedPayload   = 0x1122334455667788ULL;
    memcpy(queuedInfo + 8, &queuedCode, 4);
    memcpy(queuedInfo + 16, &queuedPid, 4);
    memcpy(queuedInfo + 24, &queuedPayload, 8);
    fixture_store(process, input, queuedInfo, sizeof(queuedInfo));
    ok &= expect(syscall_call(SYSCALL_rt_sigqueueinfo, process->id, 10, input) == 0, "signal return queued payload fixture");

    ok &= expect(signals->Deliver(&f, (uint64_t) -4, true) == 0 && f.user_rip == action.handler && f.arg0 == 10, "signal frame delivery");

    const uint64_t         frameAddress = f.user_rsp;
    signal_frame_fixture_t saved        = {};

    fixture_load(process, frameAddress, &saved, sizeof(saved));
    uint64_t deliveredPayload = 0;
    memcpy(&deliveredPayload, saved.info + 24, 8);
    ok &= expect(deliveredPayload == queuedPayload, "SA_SIGINFO handler receives full queued payload");

    ok &= expect(saved.uc.context.ax == SYSCALL_futex && saved.uc.context.ip == base + 512 && saved.uc.context.bx == 0x123456789abcdef0ULL && (frameAddress & 15) == 8, "SA_RESTART rewind and full register context");

    f.syscall_number = SYSCALL_rt_sigreturn;
    f.user_rsp       = frameAddress + 8;

    const uint64_t goodIp = saved.uc.context.ip;

    saved.uc.context.ip = CANONICAL_KERNEL_BASE;

    fixture_store(process, frameAddress, &saved, sizeof(saved));

    ok &= expect(dispatcher_dispatch_syscall(&f) == LINUX_EINVAL && f.user_rsp == frameAddress + 8 && resources->taskManager->GetCurrentTask()->signalDepth == 1, "sigreturn rejects kernel instruction pointer without consuming frame");

    saved.uc.context.ip    = goodIp;
    saved.uc.context.flags = UINT64_MAX;

    fixture_store(process, frameAddress, &saved, sizeof(saved));

    uint32_t invalidMxcsr = UINT32_MAX;

    fixture_store(process, saved.uc.context.fpstate + 24, &invalidMxcsr, 4);

    ok &= expect(dispatcher_dispatch_syscall(&f) == LINUX_EINVAL, "sigreturn rejects reserved MXCSR bits");

    fixture_store(process, saved.uc.context.fpstate + 24, &mxcsr, 4);

    floating[160] = 0;

    ok &= expect(dispatcher_dispatch_syscall(&f) == SYSCALL_futex && f.rbx == 0x123456789abcdef0ULL && f.r13 == 0xaabbccdd && floating[160] == 0x5a && (f.user_rflags & 0x27000) == 0, "sigreturn GP/FP restoration and privilege flag sanitization");

    // Negative user return values must not be confused with restore errors.
    saved.uc.context.ax = (uint64_t) -4;
    saved.uc.mask       = UINT64_MAX;

    fixture_store(process, frameAddress, &saved, sizeof(saved));

    task_t* restoredTask = resources->taskManager->GetCurrentTask();

    restoredTask->signalDepth = 1;
    f.user_rsp                = frameAddress + 8;

    ok &= expect(dispatcher_dispatch_syscall(&f) == (uint64_t) -4 && restoredTask->signalDepth == 0 && restoredTask->signalMask == (UINT64_MAX & ~SIGNAL_UNBLOCKABLE), "sigreturn restores negative result and sanitizes mask");

    restoredTask->signalMask = 0;

    return ok;
}

bool arch_selftest_timer_signal(process_t* process, task_t* task, SignalManager* signals, uint64_t base, uint64_t oldMask, uint64_t alarmBit)
{
    bool                ok            = true;
    alignas(16) uint8_t floating[512] = {};

    const uint32_t mxcsr     = 0x1f80;
    const uint32_t mxcsrMask = 0xffbf;

    memcpy(floating + 24, &mxcsr, sizeof(mxcsr));

    memcpy(floating + 28, &mxcsrMask, sizeof(mxcsrMask));

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_getitimer;
    frame.user_cs        = USER_CS;
    frame.user_ss        = USER_SS;
    frame.user_rflags    = 0x202;
    frame.user_rip       = base + 1024;
    frame.user_rsp       = base + 2 * PAGE_SIZE - 16;
    frame.fxstate        = (uint64_t) floating;

    __atomic_store_n(&task->signalMask, oldMask & ~alarmBit, __ATOMIC_RELEASE);

    ok &= expect(signals->Deliver(&frame, 0) == 0 && frame.arg0 == 14 && process->realTimerArmed && !process->realTimerExpired, "SIGALRM delivery rearms periodic timer");

    frame.user_rsp += 8;

    ok &= expect(signals->Restore(&frame) == 0, "real timer signal frame restores context");
    return ok;
}

bool arch_selftest_event_signal(process_t* process, task_t* task, SignalManager* signals, arch_syscall_frame_t& frame, const signal_action_t& action, uint64_t base, uint64_t blockedMask)
{
    bool                ok            = true;
    alignas(16) uint8_t floating[512] = {};

    const uint32_t mxcsr     = 0x1f80;
    const uint32_t mxcsrMask = 0xffbf;

    memcpy(floating + 24, &mxcsr, sizeof(mxcsr));

    memcpy(floating + 28, &mxcsrMask, sizeof(mxcsrMask));

    frame.user_cs     = USER_CS;
    frame.user_ss     = USER_SS;
    frame.user_rflags = 0x202;
    frame.user_rip    = base + 1792;
    frame.user_rsp    = base + 2 * PAGE_SIZE - 16;
    frame.fxstate     = (uint64_t) floating;

    const uint64_t interrupted = dispatcher_dispatch_syscall(&frame);

    ok &= expect(interrupted == (uint64_t) -4 && task->signalRestoreMask && task->signalMask == 0, "user event caller defers mask restoration until signal delivery");

    ok &= expect(signals->Deliver(&frame, interrupted) == 0 && frame.user_rip == action.handler, "event EINTR delivers handler under temporary mask");

    signal_frame_fixture_t saved = {};

    fixture_load(process, frame.user_rsp, &saved, sizeof(saved));

    ok &= expect(saved.uc.mask == blockedMask && saved.uc.context.ax == (uint64_t) -4, "event signal frame saves original mask and EINTR");

    ok &= expect(!task->signalRestoreMask && task->signalMask == (1ULL << 9), "handler mask derives from temporary mask");

    frame.user_rsp += 8;

    ok &= expect(signals->Restore(&frame) == (uint64_t) -4 && task->signalMask == blockedMask, "event sigreturn restores original mask");

    ok &= expect(signals->BeginTemporaryMask(0) == 0, "event no-pending restoration fixture");

    signals->FinishTemporaryMask(true);

    ok &= expect(signals->Deliver(&frame, (uint64_t) -4) == (uint64_t) -4 && task->signalMask == blockedMask && !task->signalRestoreMask, "signal return restores deferred mask when pending signal disappears");
    return ok;
}

bool arch_selftest_load_child(process_t* process, uint64_t address, uint32_t exit_number)
{
    uint8_t code[] = {
            0x48, 0xbb, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc7, 0x03, 0x78, 0x56, 0x00, 0x00, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf, 0x25, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x0f, 0x0b,
    };
    memcpy(code + 17, &exit_number, sizeof(exit_number));
    return copy_to_process_virtual(process->addressSpace, address, code, sizeof(code));
}

bool arch_selftest_load_resource_child(process_t* process, uint64_t address, uint32_t descriptor)
{
    uint8_t resourceCode[] = {
            0xb8, 0x5f, 0, 0, 0, 0xbf, 2, 0, 0, 0, 0x0f, 0x05, 0x48, 0xbb, 0x28, 0, 0x41, 0, 0, 0, 0, 0, 0x48, 0x89, 0x03, 0xb8, 3, 0, 0, 0, 0xbf, 0, 0, 0, 0, 0x0f, 0x05, 0x48, 0xbb, 0x20, 0, 0x41, 0, 0, 0, 0, 0, 0x48, 0x89, 0x03, 0xb8, 0x3c, 0, 0, 0, 0xbf, 0x25, 0, 0, 0, 0x0f, 0x05, 0x0f, 0x0b,
    };
    const uint32_t childFd = descriptor;

    memcpy(resourceCode + 31, &childFd, sizeof(childFd));

    return copy_to_process_virtual(process->addressSpace, address, resourceCode, sizeof(resourceCode));
}

uint16_t arch_selftest_elf_machine(void)
{
    return 62;
}

const char* arch_selftest_machine_name(void)
{
    return "x86_64";
}

const virt_addr_t* arch_selftest_invalid_addresses(size_t* count)
{
    static const virt_addr_t addresses[] = {0x0000800000000000ULL, 0xffff000000000000ULL};
    *count                               = sizeof(addresses) / sizeof(addresses[0]);
    return addresses;
}

arch_selftest_page_permissions_t arch_selftest_page_permissions(phys_addr_t root, uint64_t address)
{
    uint64_t table = root;
    for (int shift = 39; shift >= 12; shift -= 9)
    {
        uint64_t*      entries = reinterpret_cast<uint64_t*>(pa_to_hhdm(table, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset));
        const uint64_t entry   = entries[(address >> shift) & 511];
        if ((entry & 1) == 0 || shift == 12)
        {
            return {(entry & 2) != 0, (entry & (1ULL << 63)) == 0, (entry & 4) != 0};
        }
        table = entry & 0x000ffffffffff000ULL;
    }
    return {false, true, false};
}

void arch_selftest_user_frame(arch_syscall_frame_t* frame, uint64_t instruction, uint64_t stack, void* floating)
{
    // Independent expected Linux user selectors and initial flags.
    frame->user_cs     = 0x1b;
    frame->user_ss     = 0x23;
    frame->user_rflags = 0x202;
    frame->user_rip    = instruction;
    frame->user_rsp    = stack;
    if (floating != nullptr)
    {
        frame->fxstate = reinterpret_cast<uint64_t>(floating);
    }
}

void arch_selftest_set_instruction_pointer(arch_syscall_frame_t* frame, uint64_t instruction)
{
    frame->user_rip = instruction;
}

void arch_selftest_signal_restorer(arch_syscall_frame_t* frame)
{
    // A handler's RET consumes the saved restorer address.
    frame->user_rsp += 8;
}

const uint8_t* arch_selftest_syscall_instruction(size_t* size)
{
    static const uint8_t instruction[] = {0x0f, 0x05};
    *size                              = sizeof(instruction);
    return instruction;
}

bool arch_selftest_syscall_probe(const arch_syscall_frame_t* frame)
{
    uint64_t flags = 0;
    uint32_t mxcsr = 0;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    const bool valid = frame->arg0 == 0xA11E && frame->arg1 == 0xBEEF && frame->arg2 == 0xCDEF && frame->arg3 == 0xDEAD && frame->arg4 == 0xBABE && frame->arg5 == 0xFEED && (flags & (1ULL << 9)) != 0 && (flags & (1ULL << 10)) == 0 && mxcsr == 0x1f80
                       && platform.cpus[arch_cpu_id()].arch_info.syscall_ctx.active == 1;
    // Raw syscalls preserve user SIMD state even when kernel C clobbers it.
    __asm__ volatile("pxor %%xmm0, %%xmm0; pxor %%xmm7, %%xmm7; pxor %%xmm15, %%xmm15" : : : "xmm0", "xmm7", "xmm15");
    return valid;
}

bool arch_selftest_context_tls_matches(const struct arch_task_context* context, uint64_t value)
{
    return context->fsBase == value;
}

bool arch_selftest_tls(process_t* process)
{
    uint64_t base = fixture_map(2);

    if ((int64_t) base < 0)
    {
        return false;
    }

    Dispatcher*  dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    TaskManager* tasks      = dispatcher->GetResourceLayerCaps()->taskManager;
    uint64_t     savedFs = 0, savedGs = 0;

    if (!tasks->GetCurrentTlsBase(false, &savedFs) || !tasks->GetCurrentTlsBase(true, &savedGs))
    {
        return false;
    }

    bool ok = true;

    for (uint64_t gs = 0; gs < 2; ++gs)
    {
        const uint64_t set   = gs ? 0x1001 : 0x1002;
        const uint64_t get   = gs ? 0x1004 : 0x1003;
        const uint64_t value = base + 128 + gs * 8;

        ok &= expect(syscall_call(SYSCALL_arch_prctl, set, value) == 0, "arch_prctl sets TLS base");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, base + PAGE_SIZE - 4) == 0, "arch_prctl cross-page output");

        uint64_t actual = 0;

        fixture_load(process, base + PAGE_SIZE - 4, &actual, sizeof(actual));

        ok &= expect(actual == value, "arch_prctl TLS roundtrip");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, set, 1ULL << 47) == (uint64_t) -1, "arch_prctl rejects non-user bases");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, base) == 0, "arch_prctl reads after failed set");

        fixture_load(process, base, &actual, sizeof(actual));

        ok &= expect(actual == value, "arch_prctl failed set preserves hardware state");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, CANONICAL_KERNEL_BASE) == LINUX_EFAULT, "arch_prctl rejects kernel output");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, get, 0) == LINUX_EFAULT, "arch_prctl rejects null output");

        ok &= expect(syscall_call(SYSCALL_arch_prctl, set, 0) == 0, "arch_prctl permits zero base");
    }

    ok &= expect(syscall_call(SYSCALL_arch_prctl, 0) == LINUX_EINVAL, "arch_prctl invalid operation");

    syscall_call(SYSCALL_mprotect, base, PAGE_SIZE, 1);

    ok &= expect(syscall_call(SYSCALL_arch_prctl, 0x1003, base) == LINUX_EFAULT, "arch_prctl readonly output");

    require_cleanup(tasks->SetCurrentTlsBase(false, savedFs), "restore caller FS base");

    require_cleanup(tasks->SetCurrentTlsBase(true, savedGs), "restore caller GS base");

    return ok;
}
