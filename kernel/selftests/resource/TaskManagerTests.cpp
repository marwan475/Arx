#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/TaskManager.hpp"

void test_TaskManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->taskManager : nullptr;
    ManagerSuite suite("resource/TaskManager", m != nullptr);
    suite.Run("resource/TaskManager/system_task_statistics", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t now = 0;
        uint64_t loads[3];
        uint16_t initial = 0, count = 0;
        (void) arch_time_read_monotonic(&now);
        m->ReadSystemStatistics(now, loads, &initial);
        task_t* peer = m->AllocateTask();

        if (!t.Check(peer != nullptr, "allocate statistics fixture task"))
        {
            return;
        }

        m->ReadSystemStatistics(now, loads, &count);
        t.Equal(count, initial, "unbound idle and reserved task slots are excluded from sysinfo procs");

        if (t.Check(resources->processManager->AddTask(f.Process, peer), "attach live statistics peer"))
        {
            m->ReadSystemStatistics(now, loads, &count);
            t.Equal(count, initial + 1, "sysinfo counts live threads rather than only process groups");
            m->MarkDying(peer);
            m->ReadSystemStatistics(now, loads, &count);
            t.Equal(count, initial, "exiting thread is removed from live system statistics");
            resources->processManager->RemoveTask(f.Process, peer);
        }

        t.Check(m->FreeTask(peer), "free statistics fixture task");
    });

    suite.Run("resource/TaskManager/thread_cpu_accounting", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t now;

        if (!arch_time_read_monotonic(&now))
        {
            t.Skip("CPU accounting clock unsupported");
            return;
        }

        task_t* peer = m->AllocateTask();

        if (!t.Check(peer != nullptr && resources->processManager->AddTask(f.Process, peer), "attach accounting peer thread"))
        {
            if (peer != nullptr)
            {
                m->FreeTask(peer);
            }

            return;
        }

        f.Task->isUserTask = true;
        peer->isUserTask   = true;
        m->AccountCurrent(true);

        for (unsigned index = 0; index < 4096; ++index)
        {
            arch_pause();
        }

        m->AccountCurrent(false);
        const uint64_t firstUser = f.Process->userCpuNanoseconds;
        t.Check(firstUser > 0, "user execution consumes user CPU time");
        resources->processManager->SetRunningProcess(arch_cpu_id(), nullptr);

        for (unsigned index = 0; index < 4096; ++index)
        {
            arch_pause();
        }

        m->AccountCurrent(false);
        t.Check(f.Process->kernelCpuNanoseconds > 0, "switch bookkeeping charges task owner even after current process mapping changes");
        resources->processManager->SetRunningProcess(arch_cpu_id(), f.Process);
        t.Check(m->SetRunningTask(arch_cpu_id(), peer), "switch synthetic identity to peer");
        m->AccountCurrent(true);

        for (unsigned index = 0; index < 4096; ++index)
        {
            arch_pause();
        }

        m->AccountCurrent(false);
        t.Check(f.Process->userCpuNanoseconds > firstUser, "thread user CPU aggregates into the same process");
        t.Check(!f.Task->accountingRunning, "suspended thread cannot accrue elapsed sleep time");
        t.Check(m->SetRunningTask(arch_cpu_id(), f.Task), "restore fixture task");
        f.Task->isUserTask = false;
        resources->processManager->RemoveTask(f.Process, peer);
        t.Check(m->FreeTask(peer), "retire thread without discarding process CPU totals");
        t.Check(f.Process->userCpuNanoseconds > firstUser, "retired thread CPU remains visible");
    });

    suite.Run("resource/TaskManager/allocation_lookup", [&](TestCase& t)
    {
        size_t before = m->CountTasks();
        auto*  task   = m->AllocateTask();

        if (!t.Check(task != nullptr, "allocate task"))
        {
            return;
        }

        uint64_t id = task->id;
        t.Check(m->FindTask(id) == task, "task lookup");
        t.Equal(m->CountTasks(), before + 1, "allocated count");

        if (!m->FreeTask(task))
        {
            SelfTestManager::Get().AbortUnsafe("free allocated task");
        }

        t.Check(m->FindTask(id) == nullptr, "freed task not discoverable");
        t.Equal(m->CountTasks(), before, "task count restored");
    });

    suite.Run("resource/TaskManager/dispatch_claim", [&](TestCase& t)
    {
        auto* task = m->AllocateTask();

        if (!t.Check(task != nullptr, "allocate dispatch fixture"))
        {
            return;
        }

        bool retained = m->RetainForDispatch(task);
        t.Check(retained, "acquire dispatch claim");

        if (retained)
        {
            t.Check(!m->FreeTask(task), "claimed task cannot be freed");
            m->ReleaseDispatchClaim(task);
        }

        if (!m->FreeTask(task))
        {
            SelfTestManager::Get().AbortUnsafe("dispatch claim not released");
        }

        t.Check(!m->FreeTask(task), "double free rejected");
    });

    suite.Run("resource/TaskManager/retirement_cleanup_once", [&](TestCase& t)
    {
        task_t* task = m->AllocateTask();
        if (!t.Check(task != nullptr, "allocate retirement fixture"))
        {
            return;
        }
        unsigned calls = 0;
        task->retirementContext = &calls;
        task->retirementCleanup = [](void* context, task_t*)
        {
            ++*static_cast<unsigned*>(context);
        };
        m->MarkDying(task);
        t.Check(m->CleanupRetiredTask(task), "cleanup outside scheduler lock");
        t.Check(task->priorityCleanupDone, "priority retirement completed before freeing");
        t.Check(m->CleanupRetiredTask(task), "repeated retirement is safe");
        t.Equal(calls, 1U, "retirement callback runs once");
        t.Check(m->FreeTask(task), "free without repeating priority retirement");
        t.Equal(calls, 1U, "free preserves single cleanup");
    });

    suite.Run("resource/TaskManager/name_and_current_identity", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        t.Check(m->GetCurrentTask() == f.Task, "fixture task is current");
        t.Check(!m->FreeTask(f.Task), "running task protected");
        t.Check(m->SetCurrentName("manager-unit"), "set current task name");
        char name[16] = {};
        t.Check(m->GetCurrentName(name), "read current task name");
        t.Check(strcmp(name, "manager-unit") == 0, "name round trip");
        t.Equal(m->GetCapacity(), TaskManager::MAX_TASKS, "task capacity");
    });
}
