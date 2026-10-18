#pragma once

#include <arch/arch.h>

#ifdef __cplusplus
extern "C"
{
#endif
    // Independent architecture expectations used only by selftests.
    uint16_t           arch_selftest_elf_machine(void);
    const char*        arch_selftest_machine_name(void);
    const virt_addr_t* arch_selftest_invalid_addresses(size_t* count);
#ifdef __cplusplus
}

struct process_t;
struct task_t;
struct ResourceLayerCaps;
class SignalManager;

// Storage for the independent user floating-point fixture.
struct alignas(16) arch_selftest_floating_state_t
{
    uint8_t bytes[512];
};

bool arch_selftest_tls(process_t* process);

struct arch_selftest_page_permissions_t
{
    bool writable;
    bool executable;
    bool user;
};
arch_selftest_page_permissions_t arch_selftest_page_permissions(phys_addr_t root, uint64_t address);
void                             arch_selftest_user_frame(arch_syscall_frame_t* frame, uint64_t instruction, uint64_t stack, void* floating = nullptr);
void                             arch_selftest_set_instruction_pointer(arch_syscall_frame_t* frame, uint64_t instruction);
void                             arch_selftest_signal_restorer(arch_syscall_frame_t* frame);
const uint8_t*                   arch_selftest_syscall_instruction(size_t* size);
bool                             arch_selftest_syscall_probe(const arch_syscall_frame_t* frame);
bool                             arch_selftest_context_tls_matches(const struct arch_task_context* context, uint64_t value);
bool                             arch_selftest_load_child(process_t* process, uint64_t code, uint32_t exit_number = 60);
bool                             arch_selftest_load_resource_child(process_t* process, uint64_t code, uint32_t descriptor);
bool                             arch_selftest_signal_return(process_t* process, ResourceLayerCaps* resources, SignalManager* signals, const signal_action_t& action, uint64_t base, uint64_t input);
bool                             arch_selftest_timer_signal(process_t* process, task_t* task, SignalManager* signals, uint64_t base, uint64_t oldMask, uint64_t alarmBit);
bool                             arch_selftest_event_signal(process_t* process, task_t* task, SignalManager* signals, arch_syscall_frame_t& frame, const signal_action_t& action, uint64_t base, uint64_t blockedMask);
#endif
