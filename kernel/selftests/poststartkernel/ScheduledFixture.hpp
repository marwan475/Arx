#pragma once

#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"

// Unlike TestFixture, these tasks have real execution contexts and may sleep.
// Both tasks share one test-owned process/root; no locks span dispatch or waits.
class ScheduledFixture
{
public:
    using Action = void (*)(ScheduledFixture&);
    ScheduledFixture(TestCase& test, ResourceLayerCaps* r, LogicLayerCaps* l, Action main, Action peer)
        : T(test), R(r), L(l), Space(nullptr), Process(nullptr), Main(nullptr), Peer(nullptr), PeerDone(false), Word(0), Result(0), Deadline(0), MainAction(main), PeerAction(peer),
          ReturnTask(r && r->taskManager ? r->taskManager->GetCurrentTask() : nullptr), Published(false), Completed(false)
    {
        if (!R || !R->processManager || !R->taskManager || !R->virtualMemoryManager || !L || !L->scheduler)
        {
            return;
        }

        cpu_preemption_disable();
        Space = R->virtualMemoryManager->CreateUserAddressSpace();

        if (Space)
        {
            Process = R->processManager->CreateProcess(Space);
        }

        if (Process)
        {
            Retained = R->processManager->AcquireProcess(Process->id) != nullptr;
            Main     = R->taskManager->CreateKernelTask(MainEntry, this);
            Peer     = R->taskManager->CreateKernelTask(PeerEntry, this);

            if (Main && !R->processManager->AddTask(Process, Main))
            {
                MainAttached = false;
            }
            else
            {
                MainAttached = Main != nullptr;
            }
        }

        cpu_preemption_enable();
    }

    ~ScheduledFixture()
    {
        cpu_preemption_disable();

        if (Published && !PeerDone)
        {
            SelfTestManager::Get().AbortUnsafe("scheduled peer did not finish");
        }

        if (Process && MainAttached)
        {
            // Remove every ready-queue entry before the process ID is reused.
            // The lifetime reference prevents a timer reaper freeing our slot.
            (void) L->scheduler->KillProcess(Process->id, 0);

            if (Process->tasks)
            {
                for (task_t* task = Process->tasks; task != nullptr; task = task->next)
                {
                    kprintf("[DEBUG-sibling] active=%u task=%llu state=%u dying=%u claims=%u sleep=%u\n", Process->retirementActive, task->id, task->state, task->dying, task->dispatchUsers, task->sleepPending);
                }
                SelfTestManager::Get().AbortUnsafe("scheduled tasks did not retire");
            }
        }

        if (Main && Main->allocated)
        {
            if (!R->taskManager->FreeTask(Main))
            {
                SelfTestManager::Get().AbortUnsafe("scheduled main task still active");
            }
        }

        if (Peer && Peer->allocated)
        {
            if (Peer->process == Process)
            {
                R->processManager->RemoveTask(Process, Peer);
            }

            if (!R->taskManager->FreeTask(Peer))
            {
                SelfTestManager::Get().AbortUnsafe("scheduled peer task still active");
            }
        }

        if (Process && Retained)
        {
            R->processManager->ReleaseProcess(Process);
        }

        if (Process && !R->processManager->FreeProcess(Process))
        {
            SelfTestManager::Get().AbortUnsafe("scheduled process not reclaimable");
        }

        if (Space)
        {
            R->virtualMemoryManager->DestroyUserAddressSpace(Space);
        }

        cpu_preemption_enable();
    }

    bool Run(uint64_t deadline)
    {
        Deadline = deadline;

        if (!T.Check(Main && MainAttached && Peer && ReturnTask && Retained, "create scheduled fixture"))
        {
            return false;
        }

        spinlock_acquire_preempt(&Space->operation_lock);
        bool mapped = R->virtualMemoryManager->AllocateAnonymousPage(Space, TestFixture::Buffer, 3, false);
        spinlock_release_preempt(&Space->operation_lock);

        if (!T.Check(mapped, "map scheduled fixture buffer"))
        {
            return false;
        }

        bool dispatched = L->scheduler->ScheduleProcess(Process->id);

        if (!Completed && dispatched)
        {
            SelfTestManager::Get().AbortUnsafe("scheduled main returned without completion");
        }

        return T.Check(dispatched && Completed, "scheduled scenario returned to BSP");
    }

    bool PublishPeer(process_t* owner = nullptr)
    {
        Published = L->scheduler->PublishTask(owner != nullptr ? owner : Process, Peer);
        return T.Check(Published, "publish runnable peer");
    }

    TestCase&          T;
    ResourceLayerCaps* R;
    LogicLayerCaps*    L;
    virt_addr_space_t* Space;
    process_t*         Process;
    task_t*            Main;
    task_t*            Peer;
    bool               PeerDone;
    uint32_t           Word;
    uint64_t           Result;
    uint64_t           Deadline;

private:
    static void MainEntry(void* argument)
    {
        auto& self = *static_cast<ScheduledFixture*>(argument);
        self.MainAction(self);
        cpu_preemption_disable();
        __atomic_store_n(&self.Process->exited, true, __ATOMIC_RELEASE);
        self.Completed = true;

        if (!self.R->taskManager->ExecuteTask(self.ReturnTask))
        {
            SelfTestManager::Get().AbortUnsafe("cannot return scheduled scenario to BSP");
        }

        SelfTestManager::Get().AbortUnsafe("completed scenario was dispatched again");
    }

    static void PeerEntry(void* argument)
    {
        auto& self = *static_cast<ScheduledFixture*>(argument);
        cpu_preemption_disable();
        self.PeerAction(self);
        self.PeerDone = true;
        self.L->scheduler->ExitCurrentTask();
    }

    Action  MainAction;
    Action  PeerAction;
    task_t* ReturnTask;
    bool    Published;
    bool    Completed;
    bool    MainAttached = false;
    bool    Retained     = false;
};
