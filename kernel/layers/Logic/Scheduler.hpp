#pragma once

#include "layers/Resource/TaskScheduling.hpp"

#include <boot/boot.h>
#include <cstddef>
#include <klib/spinlock.h>
#include <stdint.h>

struct ResourceLayerCaps;
class EventManager;
class SynchronizationManager;
struct task_t;
struct process_t;
struct virt_addr_space;

class Scheduler
{
public:
    explicit Scheduler(ResourceLayerCaps* resourceLayerCaps);
    ~Scheduler() = default;

    uint64_t UserPriority(uint32_t selector, uint32_t who, bool modify, int32_t nice);
    uint64_t ConfigurePolicy(task_t* task, const scheduling_attributes_t& attributes);
    uint64_t ReadPolicy(task_t* task, scheduling_attributes_t* attributes);
    uint64_t ValidateClonePolicy(task_t* parent) const;
    void InheritPolicy(task_t* parent, task_t* child);
    void ApplyPriorityDonation(task_t* task, int32_t nice, uint32_t policy, uint32_t priority, task_t* donor);
    void RetirePolicy(task_t* task);
    void AccountSchedulingCurrent(bool beginExecution = false);
    uint64_t        AcquireTask(int64_t tid, bool modify, task_t** target);
    uint64_t        SetUserNice(task_t* task, int32_t nice);
    uint64_t        GetCpu(uint32_t* cpu, uint32_t* node) const;
    void            SetPriorityManager(SynchronizationManager* manager);
    static uint64_t AvailableCpuMask();
    uint64_t        SetAffinity(task_t* task, uint64_t mask);
    uint64_t        SetNice(task_t* task, int32_t nice);
    void ActivateScheduling();
    void SetEventManager(EventManager* events);
    bool IsSchedulingActive() const;

    enum class scheduling_reason_t
    {
        PUBLISH,
        WAKE,
        YIELD,
        BLOCK,
        TIMER,
        AFFINITY,
        PRIORITY,
        RESCHEDULE
    };

    struct scheduling_decision_t
    {
        task_t* task;
        process_t* process;
        uint64_t nextEvent;
        uint8_t cpu;
        bool accepted;
        bool local;
    };

    // A selected task owns a dispatch claim, consumed by ScheduleTask. This
    // function owns policy and placement; it never switches execution contexts.
    scheduling_decision_t RunSchedulingAlgorithm(uint8_t cpuId, scheduling_reason_t reason, process_t* publish = nullptr, task_t* placement = nullptr);
    void RequestReschedule(uint64_t cpus);
    void ServicePendingReschedule(bool syscallBoundary = false);
    bool              ScheduleProcess(uint64_t processId);
    bool              EnqueueProcess(uint8_t cpuId, uint64_t processId);
    bool              PublishTask(process_t* process, task_t* task);
    bool              RunNextReadyProcess(uint8_t cpuId, scheduling_reason_t reason = scheduling_reason_t::RESCHEDULE);
    // A successful exit request may leave tasks awaiting quiescence. The
    // optional output reports completion of task and resource retirement.
    bool              KillProcess(uint64_t processId, int32_t exitStatus, bool onlyIfExited = false, bool* retired = nullptr);
    uint64_t          SleepUntil(uint64_t monotonicDeadline);
    uint64_t          WaitForEvent(const uint64_t* sequence, uint64_t expected, uint64_t monotonicDeadline = UINT64_MAX, bool uninterruptible = false);
    void              WakeEventWaiters(const uint64_t* sequence);
    void              WakeSignalWaiters(process_t* process);
    void              WakeSleepingTasks(uint8_t cpuId);
    void              AccountCurrent(bool userMode);
    [[noreturn]] void ExitCurrentTask();
    void              ReapExitedProcesses();
    void              NotifyProcessExit(process_t* process);
    void              ChangeStoppedState(process_t* process, int signal, bool stopped);

private:
    void        InitializeBspProcessAndTask();
    static void IdleEntry(void* argument);
    bool        ScheduleTask(process_t* process, task_t* task, bool dispatchClaim = false);
    void        LockQueues();
    void        UnlockQueues();

    static constexpr size_t SLEEP_QUEUE_CAPACITY = 128;
    static constexpr size_t NO_SLEEPER           = static_cast<size_t>(-1);

    struct sleep_entry_t
    {
        process_t*      process;
        task_t*         task;
        uint64_t        deadline;
        uint64_t        result;
        const uint64_t* sequence;
        uint64_t        expectedSequence;
        size_t          next;
        uint8_t         cpuId;
        bool            waiting;
        bool            dispatched;
        bool            uninterruptible;
    };

    void AccountSchedulingLocked(uint8_t cpu, uint64_t now);
    void ReplenishReservationsLocked(uint64_t now);
    uint64_t ReservationUtilization(uint64_t runtime, uint64_t period) const;
    int FindReservationCpuLocked(task_t* task, uint64_t mask, uint64_t utilization) const;
    bool MoreUrgent(task_t* task, task_t* selected, uint8_t cpu) const;
    bool PolicyEligible(task_t* task, uint8_t cpu) const;
    uint64_t NextSchedulingEventLocked(task_t* task, uint8_t cpu, uint64_t now) const;
    uint64_t ReadySequence;
    uint64_t DeadlineUtilization[BOOT_SMP_MAX_CPUS];
    uint64_t RealtimeWindow[BOOT_SMP_MAX_CPUS];
    uint64_t RealtimeConsumed[BOOT_SMP_MAX_CPUS];
    uint64_t ExecutionStamp[BOOT_SMP_MAX_CPUS];
    task_t* ExecutionTask[BOOT_SMP_MAX_CPUS];

    uint64_t Sleep(uint64_t deadline, const uint64_t* sequence, uint64_t expected, bool uninterruptible = false);

    bool          SchedulingActive[BOOT_SMP_MAX_CPUS];
    spinlock_t    QueueLock;
    sleep_entry_t SleepQueue[SLEEP_QUEUE_CAPACITY];
    size_t        SleepHeads[BOOT_SMP_MAX_CPUS];
    task_t*       IdleTasks[BOOT_SMP_MAX_CPUS];
    // The boot kernel root stays alive while idle contexts are suspended.
    virt_addr_space* IdleAddressSpace;

    ResourceLayerCaps* ResourceLayerImportCaps;
    EventManager*      Events;
    SynchronizationManager* PriorityManager;
};
