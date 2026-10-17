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
}
