#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/ProcessController.hpp"

void test_ProcessController(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->processController : nullptr;
    ManagerSuite suite("logic/ProcessController", m != nullptr);
    suite.Run("logic/ProcessController/group_identity", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        t.Equal(m->Group(0, 0), f.Process->groupId, "query current process group");
        t.Equal(m->Group(2, 0), f.Process->sessionId, "query current session");
        t.Equal(m->Group(0, - 1), LINUX_ESRCH, "negative process selector rejected");
    });

    suite.Run("logic/ProcessController/anonymous_map_protect_unmap", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t address = m->MapProcessMemory(0x50000000, PAGE_SIZE, 3, 0x22, - 1, 0);

        if (!t.Check((int64_t) address > 0, "map private anonymous memory"))
        {
            return;
        }

        t.Check(resources->virtualMemoryManager->VirtToPhys(address, f.Space) != 0, "mapped memory translates");
        t.Equal(m->ProtectProcessMemory(address, PAGE_SIZE, 1), 0, "protect owned range");
        t.Equal(m->UnmapProcessMemory(address, PAGE_SIZE), 0, "unmap owned range");
        t.Equal(resources->virtualMemoryManager->VirtToPhys(address, f.Space), 0, "unmap removes mapping");
        t.Equal(m->ProtectProcessMemory(address + 1, PAGE_SIZE, 1), LINUX_EINVAL, "unaligned protection rejected");
    });

    suite.Run("logic/ProcessController/child_tid", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        t.Equal(m->SetTidAddress(TestFixture::Buffer), f.Task->id, "set_tid_address returns task identity");
        t.Equal(f.Task->clearChildTid, TestFixture::Buffer, "clear-child-tid ownership");
    });
}
