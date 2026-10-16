#pragma once

#include <boot/boot.h>
#include <cstddef>
#include <klib/spinlock.h>
#include <stdint.h>

struct ResourceLayerCaps;
class EventManager;
struct task_t;
struct process_t;
struct virt_addr_space;

class Scheduler
{
public:
    explicit Scheduler(ResourceLayerCaps* resourceLayerCaps);
    ~Scheduler() = default;

    void ActivateScheduling();
    void SetEventManager(EventManager* events);
    bool IsSchedulingActive() const;

    bool              ScheduleProcess(uint64_t processId);
    bool              EnqueueProcess(uint8_t cpuId, uint64_t processId);
    bool              PublishTask(process_t* process, task_t* task);
    bool              RunNextReadyProcess(uint8_t cpuId);
    bool              KillProcess(uint64_t processId, int32_t exitStatus, bool onlyIfExited = false);
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
    bool        ScheduleTask(process_t* process, task_t* task);
    void        LockQueues();
    void        UnlockQueues();

    static constexpr size_t READY_QUEUE_CAPACITY = 64;
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

    struct ready_queue_t
    {
        uint64_t processIds[READY_QUEUE_CAPACITY];
        size_t   head;
        size_t   tail;
        size_t   count;
    };

    bool     RemoveProcessFromQueue(ready_queue_t* queue, uint64_t processId);
    uint64_t Sleep(uint64_t deadline, const uint64_t* sequence, uint64_t expected, bool uninterruptible = false);

    ready_queue_t ReadyQueues[BOOT_SMP_MAX_CPUS];
    bool          SchedulingActive[BOOT_SMP_MAX_CPUS];
    spinlock_t    QueueLock;
    sleep_entry_t SleepQueue[SLEEP_QUEUE_CAPACITY];
    size_t        SleepHeads[BOOT_SMP_MAX_CPUS];
    task_t*       IdleTasks[BOOT_SMP_MAX_CPUS];
    // The boot kernel root stays alive while idle contexts are suspended.
    virt_addr_space* IdleAddressSpace;

    ResourceLayerCaps* ResourceLayerImportCaps;
    EventManager*      Events;
};
