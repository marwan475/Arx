#pragma once

#include "layers/Resource/SignalState.hpp"

#include <arch/arch.h>
#include <boot/boot.h>
#include <klib/spinlock.h>
#include <stddef.h>
#include <stdint.h>

struct process_t;

struct task_t
{
    enum state_t
    {
        READY,
        RUNNING,
        SWITCHING,
        SUSPENDED,
        DYING
    };
    state_t  state;
    int16_t  ownerCpu;
    uint32_t dispatchUsers;
    bool     dying;
    struct user_launch_context_t
    {
        uint64_t userRip;
        uint64_t userRsp;
        uint64_t arg0;
        uint64_t arg1;
    } userLaunchContext;

    bool             allocated;
    bool             isUserTask;
    bool             sleepPending;
    bool             sleepBlocked;
    uint64_t         syscallActive;
    uint32_t         preemptionDepth;
    uint64_t         id;
    char             name[16];
    process_t*       process;
    uint64_t         signalMask;
    // Interrupted masked waits restore this through the handler's signal frame.
    uint64_t         signalSavedMask;
    bool             signalRestoreMask;
    uint64_t         signalPending;
    uint64_t         signalSequence;
    signal_sender_t* signalSenders;
    signal_stack_t   signalStack;
    uint64_t         signalDepth;
    uint64_t         robustHead;
    process_t*       robustOwner;
    void*            retirementContext;
    void (*retirementCleanup)(void*, task_t*);
    struct arch_task_context taskContext;
    void*                    stack;
    task_t*                  next;
    task_t*                  prev;
};

class TaskManager
{
public:
    static constexpr size_t MAX_TASKS = 128;

    TaskManager();

    task_t*     AllocateTask();
    task_t*     CreateKernelTask(arch_task_entry_t entry, void* arg);
    task_t*     CreateUserBootstrapTask(uint64_t userRip, uint64_t userRsp, uint64_t arg0, uint64_t arg1);
    bool        FreeTask(task_t* task, void** retiredStack = nullptr);
    void        CleanupCurrentTask();
    bool        CleanupRetiredTask(task_t* task);
    static void ReleaseTaskStack(void* stack);
    bool        ExecuteTask(task_t* task, bool releaseDispatchClaim = false);
    bool        RetainForDispatch(task_t* task);
    void        ReleaseDispatchClaim(task_t* task);
    void        MarkDying(task_t* task);
    void        CompleteSwitch();
    bool        MarkSleeping(task_t* task);
    void        FinishSleeping(task_t* task);

    task_t* GetRunningTask(uint8_t cpuId) const;
    bool    SetRunningTask(uint8_t cpuId, task_t* task);
    task_t* GetCurrentTask() const;

    task_t*       GetTasks();
    const task_t* GetTasks() const;

    size_t GetCapacity() const;
    size_t CountTasks() const;
    bool   GetCurrentName(char* name);
    bool   SetCurrentName(const char* name);
    bool   GetCurrentTlsBase(bool gs, uint64_t* value);
    bool   SetCurrentTlsBase(bool gs, uint64_t value);

private:
    static void ResetTaskSlot(task_t* task, uint64_t id, bool allocated);
    task_t*     CreateTask(arch_task_entry_t entry, void* arg, const task_t::user_launch_context_t* userLaunch);
    task_t*     AllocateTaskUnlocked();
    void        LockManager() const;
    void        UnlockManager() const;

    mutable spinlock_t ManagerLock;
    task_t             Tasks[MAX_TASKS];
    task_t*            RunningTasks[BOOT_SMP_MAX_CPUS];
    task_t*            SwitchingTasks[BOOT_SMP_MAX_CPUS];
};
