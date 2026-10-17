#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"

void test_Scheduler(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->scheduler : nullptr;
    ManagerSuite suite("logic/Scheduler", m != nullptr);
    suite.Run("logic/Scheduler/initialization", [&](TestCase& t)
    {
        t.Check(!m->IsSchedulingActive(), "timer scheduling stays inactive during layer unit tests");
        t.Check(resources->processManager->GetCurrentProcess() != nullptr, "BSP process seeded");
        t.Check(resources->taskManager->GetCurrentTask() != nullptr, "BSP task seeded");
    });

    suite.Run("logic/Scheduler/priority_permissions",
              [&](TestCase& t)
              {
                  TestFixture f(resources);
                  if (!f.Valid(t))
                  {
                      return;
                  }
                  t.Equal(m->SetUserNice(f.Task, 10), 0, "root can set ordinary priority");
                  f.Process->credentials.user.effective = 1000;
                  t.Equal(m->SetUserNice(f.Task, 5), static_cast<uint64_t>(-13), "unprivileged priority improvement rejected");
                  t.Equal(f.Task->baseNice, 10, "rejected priority request preserves base priority");
                  t.Equal(m->SetUserNice(f.Task, 15), 0, "unprivileged priority reduction accepted");
                  f.Process->credentials.user.effective = 0;
                  t.Equal(m->SetUserNice(f.Task, -5), 0, "privileged priority improvement accepted");
                  t.Equal(f.Task->effectiveNice, -5, "effective priority follows base without donors");
              });

    suite.Run("logic/Scheduler/queue_validation", [&](TestCase& t)
    {
        t.Check(!m->EnqueueProcess(arch_cpu_id(), UINT64_MAX), "invalid process cannot queue");
        t.Check(!m->ScheduleProcess(UINT64_MAX), "invalid process cannot dispatch");
        t.Equal(m->WaitForEvent(nullptr, 0, 0), LINUX_EINVAL, "wait requires sequence");
    });

    suite.Run("logic/Scheduler/deferred_retirement", [&](TestCase& test)
    {
        VirtualMemoryManager* memory    = resources->virtualMemoryManager;
        ProcessManager*       processes = resources->processManager;
        TaskManager*          tasks     = resources->taskManager;
        virt_addr_space_t*    space     = memory->CreateUserAddressSpace();

        if (!test.Check(space != nullptr, "create retirement address space"))
        {
            return;
        }

        process_t* process = processes->CreateProcess(space);
        task_t*    task    = tasks->AllocateTask();

        if (test.Check(process != nullptr && task != nullptr, "create retirement process and task"))
        {
            process_t* retained = processes->AcquireProcess(process->id);
            const bool attached = processes->AddTask(process, task);
            const bool claimed  = attached && tasks->RetainForDispatch(task);

            test.Check(retained == process && claimed, "retain process and pending dispatch");

            if (retained != nullptr && claimed)
            {
                bool retired = true;
                process->retirementActive = true;
                test.Check(!m->KillProcess(process->id, 0, false, &retired) && !retired, "active retirement does not report completion");
                process->retirementActive = false;

                test.Check(m->KillProcess(process->id, 0, false, &retired), "exit request accepts a pending dispatch");
                test.Check(!retired && process->tasks == task && task->allocated, "dispatch claim defers task retirement");
                tasks->ReleaseDispatchClaim(task);
                test.Check(m->KillProcess(process->id, 0, false, &retired) && retired, "retry completes retirement after dispatch release");
                test.Check(process->tasks == nullptr && !task->allocated, "retirement unlinks and releases the task slot");
            }
            else
            {
                if (claimed)
                {
                    tasks->ReleaseDispatchClaim(task);
                }

                if (attached)
                {
                    processes->RemoveTask(process, task);
                }
            }

            if (retained != nullptr)
            {
                processes->ReleaseProcess(retained);
            }
        }

        if (task != nullptr && task->allocated)
        {
            test.Check(tasks->FreeTask(task), "release retirement test task");
        }

        if (process != nullptr)
        {
            test.Check(processes->FreeProcess(process), "release retirement test process");
        }

        memory->DestroyUserAddressSpace(space);
    });
}
