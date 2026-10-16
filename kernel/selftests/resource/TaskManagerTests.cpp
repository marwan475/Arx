#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/TaskManager.hpp"

void test_TaskManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->taskManager : nullptr;
    ManagerSuite suite("resource/TaskManager", m != nullptr);
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
