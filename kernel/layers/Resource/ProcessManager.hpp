#pragma once

#include "layers/Resource/TaskManager.hpp"

#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/spinlock.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#include <memory/vmm.h>
}
#endif

using file_handle_t = void*;

enum file_descriptor_flags_t : uint32_t
{
    FD_FLAG_NONE    = 0,
    FD_FLAG_CLOEXEC = (1U << 0),
};

struct file_descriptor_t
{
    file_handle_t file;
    uint32_t      flags;
};

struct process_user_auxv_entry_t
{
    uint64_t type;
    uint64_t value;
};

struct process_user_stack_layout_t
{
    uint64_t stackBase;
    uint64_t stackSize;

    const char* const* argv;
    uint64_t           argc;

    const char* const* envp;
    uint64_t           envc;

    const process_user_auxv_entry_t* auxv;
    uint64_t                         auxvCount;
};

struct process_t
{
    bool               allocated;
    uint64_t           id;
    bool               hasParent;
    uint64_t           parentId;
    bool               exited;
    int32_t            exitStatus;
    virt_addr_space_t* addressSpace;
    void*              elfMetadata;
    task_t*            tasks;
    file_descriptor_t* fileDescriptors;
    uint64_t           fileDescriptorCount;
    char               cwdPath[256];
};

class ProcessManager
{
public:
    static constexpr size_t   MAX_PROCESSES                 = 64;
    static constexpr uint64_t DEFAULT_FILE_DESCRIPTOR_COUNT = 32;
    static constexpr size_t   MAX_CWD_PATH_LENGTH           = 256;

    ProcessManager();

    process_t* AllocateProcess();
    process_t* CreateProcess(virt_addr_space_t* addressSpace);
    bool       FreeProcess(process_t* process);
    bool       AddTask(process_t* process, task_t* task);
    int64_t    AddFileDescriptor(process_t* process, file_handle_t file, uint32_t flags);
    bool       BuildUserInitialStack(process_t* process, const process_user_stack_layout_t* layout, uint64_t* outUserRsp);
    bool       ActivateProcessAddressSpace(process_t* process);
    task_t*    GetTasks(process_t* process) const;

    process_t* GetRunningProcess(uint8_t cpuId) const;
    bool       SetRunningProcess(uint8_t cpuId, process_t* process);
    process_t* GetCurrentProcess() const;

    process_t*       GetProcesses();
    const process_t* GetProcesses() const;

    bool IsProcessRunning(const process_t* process) const;
    bool TryReapExitedProcess(uint64_t processId);

    size_t GetCapacity() const;

private:
    process_t* AllocateProcessUnlocked();
    void       LockManager() const;
    void       UnlockManager() const;

    mutable spinlock_t ManagerLock;
    process_t  Processes[MAX_PROCESSES];
    process_t* RunningProcesses[BOOT_SMP_MAX_CPUS];
};
