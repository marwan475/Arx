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

    suite.Run("logic/Scheduler/queue_validation", [&](TestCase& t)
    {
        t.Check(!m->EnqueueProcess(arch_cpu_id(), UINT64_MAX), "invalid process cannot queue");
        t.Check(!m->ScheduleProcess(UINT64_MAX), "invalid process cannot dispatch");
        t.Equal(m->WaitForEvent(nullptr, 0, 0), LINUX_EINVAL, "wait requires sequence");
    });
}
