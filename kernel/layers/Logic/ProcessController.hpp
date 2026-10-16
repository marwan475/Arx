#pragma once

#include <arch/arch.h>
#include <klib/spinlock.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;
struct process_t;
struct task_t;
struct process_file_limit_t;

struct process_clone_arguments_t
{
    uint64_t flags;
    uint64_t pidfd;
    uint64_t childTid;
    uint64_t parentTid;
    uint64_t exitSignal;
    uint64_t stack;
    uint64_t stackSize;
    uint64_t tls;
    uint64_t setTid;
    uint64_t setTidSize;
    uint64_t cgroup;
};
static_assert(sizeof(process_clone_arguments_t) == 88, "Linux clone_args ABI");

class ProcessController
{
public:
    ProcessController(ResourceLayerCaps* resources, LogicLayerCaps* logic);

    uint64_t Clone(const arch_syscall_frame_t* frame, const process_clone_arguments_t& arguments);
    uint64_t Execute(arch_syscall_frame_t* frame);
    uint64_t Wait(int64_t selector, uint64_t status, uint32_t options, uint64_t usage, bool waitid, uint32_t type = 0);
    uint64_t Group(uint32_t operation, int32_t pid, int32_t group = 0);
    uint64_t SetTidAddress(uint64_t address);
    uint64_t Exit(int32_t status, bool group);
    uint64_t MapProcessMemory(uint64_t address, uint64_t length, uint32_t protection, uint32_t flags, int fd, uint64_t offset);
    uint64_t UnmapProcessMemory(uint64_t address, uint64_t length);
    uint64_t ProtectProcessMemory(uint64_t address, uint64_t length, uint32_t protection);
    uint64_t QueryProcessMemoryResidency(uint64_t address, uint64_t length, uint64_t vector);
    uint64_t AdviseProcessMemory(uint64_t address, uint64_t length, int advice);
    uint64_t ResizeProcessBreak(uint64_t address);
    static uint64_t ResourceLimit(ResourceLayerCaps* resources, int32_t pid, uint32_t resource, const process_file_limit_t* proposed, process_file_limit_t* previous);

private:
    process_t* BeginMemoryOperation();
    uint64_t   EndMemoryOperation(process_t* process, uint64_t result);

    ResourceLayerCaps* Resources;
    LogicLayerCaps*    Logic;
    spinlock_t        Lock;
};
