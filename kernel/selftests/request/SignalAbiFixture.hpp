#pragma once

// Independent Linux x86-64 signal ABI used by signal and event contracts.
namespace request_selftest
{
#if defined(__x86_64__)
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
#endif
} // namespace request_selftest
