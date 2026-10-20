#pragma once

#include <debug/kernel/stats/ProcessDebugStats.hpp>

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

    const dispatcher_file_operations_t* operations;
};

struct descriptor_table_t
{
    uint32_t           references;
    spinlock_t         lock;
    file_descriptor_t* entries;
    uint64_t           count;
};

struct filesystem_context_t
{
    uint32_t   references;
    spinlock_t lock;
    uint32_t   umask;
    char       cwdPath[256];
};

struct signal_dispositions_t
{
    uint32_t        references;
    spinlock_t      lock;
    uint64_t        ignored;
    signal_action_t actions[64];
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
    // Nanoseconds consumed by all threads, retained after their task slots retire.
    uint64_t               userCpuNanoseconds;
    uint64_t               kernelCpuNanoseconds;
    uint64_t               minorFaults;
    uint64_t               childUsage[18];
    process_file_limit_t   cpuLimit;
    uint64_t               cpuLimitNotification;
    uint64_t               cpuTimerDeadline[2];
    uint64_t               cpuTimerInterval[2];
    bool                   allocated;
    bool                   hasParent;
    bool                   ownsAddressSpace;
    uint64_t               id;
    uint64_t               parentId;
    uint64_t               parentTaskId;
    uint64_t               groupId;
    uint64_t               sessionId;
    uint64_t               childSequence;
    uint64_t               execSequence;
    uint64_t               waitOwner;
    uint32_t               exitSignal;
    process_t*             exitRecipient;
    int32_t                terminationSignal;
    bool                   waitable;
    bool                   waitConsumed;
    bool                   exitNotified; // Published after task and process resources retire; gates wait.
    bool                   stopped;
    bool                   stopPending;
    bool                   continuedPending;
    int32_t                stopSignal;
    uint64_t               stopSequence;
    uint64_t               taskCursor;
    bool                   exited;
    bool                   reclaiming;
    bool                   retirementActive;
    int16_t                ownerCpu;
    int32_t                exitStatus;
    virt_addr_space_t*     addressSpace;
    void*                  elfMetadata;
    uint64_t               breakBase;
    uint64_t               programBreak;
    uint32_t               memoryUsers;   // Also prevents attaching shared address-space roots.
    uint32_t               lifetimeUsers; // Retains the process without claiming its mappings.
    spinlock_t             timerLock;
    uint64_t               realTimerDeadline;
    uint64_t               realTimerInterval;
    bool                   realTimerArmed;
    bool                   realTimerExpired;
    int64_t                timerIntervalSeconds[3];
    int64_t                timerIntervalMicroseconds[3];
    task_t*                tasks;
    descriptor_table_t*    descriptorTable;
    filesystem_context_t*  filesystemContext;
    process_file_limit_t   fileLimit;
    spinlock_t             credentialLock;
    process_credentials_t  credentials;
    process_group_list_t*  supplementaryGroups;
    signal_dispositions_t* signalDispositions;
    mutable spinlock_t     resourceLock;
    uint64_t               signalPending;
    signal_sender_t        signalSenders[32];
    signal_queue_t*        signalQueue;

    ARX_DEBUG_ONLY(ProcessSlotDebug debug;)

    descriptor_table_t*    Files() const;
    uint64_t               DescriptorLimit() const;
    filesystem_context_t*  Fs() const;
    signal_dispositions_t* Signals() const;
};

// Path resolution uses a coherent snapshot; publishing chdir never exposes a
// partially written path to another owner of a shared filesystem context.
struct WorkingDirectorySnapshot
{
    char path[256];

    explicit WorkingDirectorySnapshot(const process_t* process);
};

uint64_t pending_signals(const process_t* process, const task_t* task);

class SignalDispositionGuard
{
public:
    explicit SignalDispositionGuard(const process_t* process);
    ~SignalDispositionGuard();
    void                          Release();
    static signal_dispositions_t* Current(const process_t* process);
    SignalDispositionGuard(const SignalDispositionGuard&) = delete;
    SignalDispositionGuard& operator=(const SignalDispositionGuard&) = delete;

private:
    const process_t*               Process;
    signal_dispositions_t*         Dispositions;
    SignalDispositionGuard*        Previous;
    bool                           Owner;
    bool                           Held;
    static SignalDispositionGuard* Active[BOOT_SMP_MAX_CPUS];
};

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
    descriptor_table_t*        Table;
    descriptor_table_t*        Previous;
    bool                       Owner;
    static descriptor_table_t* Active[BOOT_SMP_MAX_CPUS];
};

class ProcessManager
{
public:
    static void                  SnapshotUsage(const process_t* process, uint64_t usage[18], bool descendants = true);
    static void                  ReapUsage(process_t* parent, const process_t* child);

    static constexpr size_t   MAX_PROCESSES                 = 64;
    static constexpr uint64_t DEFAULT_FILE_DESCRIPTOR_COUNT = 32;
    static constexpr uint64_t MAX_FILE_DESCRIPTORS          = 1024 * 1024;
    static constexpr uint32_t MAX_SUPPLEMENTARY_GROUPS      = 65536;
    static constexpr size_t   MAX_CWD_PATH_LENGTH           = 256;

    ProcessManager();
    ARX_DEBUG_ONLY(
        ProcessDebugStats DebugStats{};
        void DebugAllocated(process_t* process);
        void DebugParent(process_t* process, bool changed = false);
        void DebugPublished(process_t* process);
        void DebugExit(process_t* process);
        void PublishDebug(DebugManager& manager, TaskManager* tasks, const void* vfsRoute);
    )

    static descriptor_table_t*    CreateDescriptorTable();
    static descriptor_table_t*    CopyDescriptorTable(descriptor_table_t* source);
    static void                   ReleaseDescriptorTable(descriptor_table_t* table);
    static filesystem_context_t*  CopyFilesystemContext(filesystem_context_t* source);
    static void                   ReleaseFilesystemContext(filesystem_context_t* context);
    static signal_dispositions_t* CopySignalDispositions(signal_dispositions_t* source);
    static void                   ReleaseSignalDispositions(signal_dispositions_t* dispositions);
    static void                   ReleaseProcessResources(process_t* process);

    process_t*                AllocateProcess();
    process_t*                CreateProcess(virt_addr_space_t* addressSpace, uint64_t preferredId = UINT64_MAX);
    bool                      FreeProcess(process_t* process, bool onlyIfExited = false);
    bool                      AddTask(process_t* process, task_t* task);
    void                      RemoveTask(process_t* process, task_t* task);
    int64_t                   AddFileDescriptor(process_t* process, file_handle_t file, uint32_t flags, const dispatcher_file_operations_t* operations = nullptr);
    int64_t                   AddFileDescriptorFrom(process_t* process, file_handle_t file, uint32_t flags, uint64_t minFd, const dispatcher_file_operations_t* operations = nullptr);
    static file_descriptor_t* GetFileDescriptor(process_t* process, uint64_t fd);
    bool                      EnsureFileDescriptorCapacity(process_t* process, uint64_t requiredIndex);
    bool                      BuildUserInitialStack(process_t* process, const process_user_stack_layout_t* layout, uint64_t* outUserRsp);
    bool                      ActivateProcessAddressSpace(process_t* process);
    task_t*                   GetTasks(process_t* process) const;

    process_t* GetRunningProcess(uint8_t cpuId) const;
    bool       SetRunningProcess(uint8_t cpuId, process_t* process);
    process_t* GetCurrentProcess() const;
    process_t* AcquireCurrentMemoryProcess(bool requireExclusive = true);
    process_t* AcquireProcess(uint64_t id, bool requireRealTimer = false);
    void       ReleaseProcess(process_t* process);
    void       ReleaseMemoryProcess(process_t* process);
    void       RetainMemoryProcess(process_t* process);
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
