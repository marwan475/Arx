#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SynchronizationManager.hpp"

void test_SynchronizationManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->synchronizationManager : nullptr;
    ManagerSuite suite("logic/SynchronizationManager", m != nullptr);
    suite.Run("logic/SynchronizationManager/wait_mismatch_wake_empty", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t) || !t.Check(f.Map(), "map futex word"))
        {
            return;
        }

        uint32_t value = 17;
        t.Check(safe_write_user(TestFixture::Buffer, &value, sizeof(value)), "initialize futex word");
        t.Equal(m->Wait(TestFixture::Buffer, 18, UINT32_MAX, 0), LINUX_EAGAIN, "mismatch never blocks");
        t.Equal(m->Wake(TestFixture::Buffer, 1, UINT32_MAX), 0, "wake empty queue");
        t.Equal(m->Requeue(TestFixture::Buffer, TestFixture::Buffer + 4, 0, 1, false, 0), 0, "requeue empty queue");
    });

    suite.Run("logic/SynchronizationManager/validation", [&](TestCase& t)
    {
        t.Equal(m->Wait(1, 0, UINT32_MAX, 0), LINUX_EINVAL, "unaligned futex");
        t.Equal(m->Wait(TestFixture::Buffer, 0, 0, 0), LINUX_EINVAL, "empty bitset");
        t.Equal(m->Wake(1, 1, UINT32_MAX), LINUX_EINVAL, "unaligned wake");
        t.Equal(m->Requeue(TestFixture::Buffer, TestFixture::Buffer + 4, - 1, 0, false, 0), LINUX_EINVAL, "negative requeue count");
    });

    suite.Run("logic/SynchronizationManager/robust_metadata", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t head = 1;

        uint64_t length = 0;
        t.Equal(m->SetRobustList(0), 0, "clear robust list");
        t.Equal(m->GetRobustList(0, &head, &length), 0, "query current robust list");
        t.Equal(head, 0, "empty robust head");
        t.Equal(length, 24, "robust-list ABI size");
        t.Equal(m->GetRobustList(-1, &head, &length), LINUX_ESRCH, "invalid task rejected");
    });
}
