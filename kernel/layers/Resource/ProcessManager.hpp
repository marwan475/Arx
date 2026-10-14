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
    void (*release)(file_handle_t);
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

struct process_file_limit_t
{
    uint64_t current;
    uint64_t maximum;
};
static_assert(sizeof(process_file_limit_t) == 16, "Linux LP64 rlimit layout");

struct process_identity_t
{
    uint32_t real;
    uint32_t effective;
    uint32_t saved;
    uint32_t filesystem;
};

struct process_credentials_t
{
    process_identity_t user;
    process_identity_t group;
};

// Immutable after publication. Shared by inherited credentials until replaced.
struct process_group_list_t
{
    uint32_t references;
    uint32_t count;
    uint32_t ids[];
};

struct process_t
{
    bool                  allocated;
    uint64_t              id;
    bool                  hasParent;
    uint64_t              parentId;
    bool                  exited;
    bool                  reclaiming;
    bool                  retirementActive;
    int16_t               ownerCpu;
    int32_t               exitStatus;
    virt_addr_space_t*    addressSpace;
    void*                 elfMetadata;
    uint64_t              breakBase;
    uint64_t              programBreak;
    uint32_t              memoryUsers;
    int64_t               timerIntervalSeconds[3];
    int64_t               timerIntervalMicroseconds[3];
    task_t*               tasks;
    file_descriptor_t*    fileDescriptors;
    spinlock_t            fileDescriptorLock;
    uint64_t              fileDescriptorCount;
    uint32_t              umask;
    process_file_limit_t  fileLimit;
    spinlock_t            credentialLock;
    process_credentials_t credentials;
    process_group_list_t* supplementaryGroups;
    char                  cwdPath[256];
    spinlock_t signalLock;
    uint64_t signalPending;
    uint64_t signalIgnored;
    signal_action_t signalActions[32];
    signal_sender_t signalSenders[32];
};

inline uint64_t pending_signals(const process_t* process, const task_t* task)
{
    if (process == nullptr || task == nullptr)
    {
        return 0;
    }

    return (__atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) | __atomic_load_n(&task->signalPending, __ATOMIC_ACQUIRE))
        & ~(__atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE) & ~SIGNAL_UNBLOCKABLE)
        & ~__atomic_load_n(&process->signalIgnored, __ATOMIC_ACQUIRE);
}

// A descriptor pointer is borrowed only inside this transaction. Holding the
// table lock keeps its file reference alive and prevents table relocation.
class FileDescriptorGuard
{
public:
    explicit FileDescriptorGuard(process_t* process);
    ~FileDescriptorGuard();
    FileDescriptorGuard(const FileDescriptorGuard&)            = delete;
    FileDescriptorGuard& operator=(const FileDescriptorGuard&) = delete;

private:
    process_t*        Process;
    process_t*        Previous;
    bool              Owner;
    static process_t* Active[BOOT_SMP_MAX_CPUS];
};

class ProcessManager
{
public:
    static constexpr size_t   MAX_PROCESSES                 = 64;
    static constexpr uint64_t DEFAULT_FILE_DESCRIPTOR_COUNT = 32;
    static constexpr uint64_t MAX_FILE_DESCRIPTORS          = 1024 * 1024;
    static constexpr uint32_t MAX_SUPPLEMENTARY_GROUPS      = 65536;
    static constexpr size_t   MAX_CWD_PATH_LENGTH           = 256;

    ProcessManager();

    process_t*                AllocateProcess();
    process_t*                CreateProcess(virt_addr_space_t* addressSpace);
    bool                      FreeProcess(process_t* process, bool onlyIfExited = false);
    bool                      AddTask(process_t* process, task_t* task);
    int64_t                   AddFileDescriptor(process_t* process, file_handle_t file, uint32_t flags, void (*release)(file_handle_t) = nullptr);
    int64_t                   AddFileDescriptorFrom(process_t* process, file_handle_t file, uint32_t flags, uint64_t minFd, void (*release)(file_handle_t) = nullptr);
    static file_descriptor_t* GetFileDescriptor(process_t* process, uint64_t fd);
    bool                      EnsureFileDescriptorCapacity(process_t* process, uint64_t requiredIndex);
    bool                      BuildUserInitialStack(process_t* process, const process_user_stack_layout_t* layout, uint64_t* outUserRsp);
    bool                      ActivateProcessAddressSpace(process_t* process);
    task_t*                   GetTasks(process_t* process) const;

    process_t* GetRunningProcess(uint8_t cpuId) const;
    bool       SetRunningProcess(uint8_t cpuId, process_t* process);
    process_t* GetCurrentProcess() const;
    process_t* AcquireCurrentMemoryProcess(bool requireExclusive = true);
    process_t* AcquireProcess(uint64_t id);
    void       ReleaseMemoryProcess(process_t* process);
    uint64_t   GetCurrentProgramBreak() const;

    process_t*       GetProcesses();
    const process_t* GetProcesses() const;

    bool IsProcessRunning(const process_t* process) const;
    bool TryReapExitedProcess(uint64_t processId);

    size_t                       GetCapacity() const;
    uint32_t                     ExchangeUmask(process_t* process, uint32_t mask);
    uint64_t                     ChangeFileLimit(process_t* process, const process_file_limit_t* proposed, process_file_limit_t* previous);
    process_t*                   AcquireCurrentCredentials();
    static void                  ReleaseCredentials(process_t* process);
    static process_group_list_t* CreateGroupList(const uint32_t* ids, uint32_t count);
    static void                  ReleaseGroupList(process_group_list_t* groups);

private:
    static void ResetProcessSlot(process_t* process, uint64_t id, bool allocated);
    process_t*  AllocateProcessUnlocked();
    void        LockManager() const;
    void        UnlockManager() const;

    mutable spinlock_t ManagerLock;
    process_t          Processes[MAX_PROCESSES];
    process_t*         RunningProcesses[BOOT_SMP_MAX_CPUS];
};
