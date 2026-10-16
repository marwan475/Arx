#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/SignalManager.hpp"

void test_SignalManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->signalManager : nullptr;
    ManagerSuite suite("logic/SignalManager", m != nullptr);
    suite.Run("logic/SignalManager/action_roundtrip", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        signal_action_t ignored = {1, 0, 0, UINT64_MAX}, actual = {};
        t.Equal(m->Action(10, &ignored, nullptr), 0, "set ignored action");
        t.Equal(m->Action(10, nullptr, &actual), 0, "query action");
        t.Equal(actual.handler, 1, "handler round trip");
        t.Equal(actual.mask & SIGNAL_UNBLOCKABLE, 0, "uncatchable signals stripped from action mask");
        t.Equal(m->Action(9, &ignored, nullptr), LINUX_EINVAL, "SIGKILL handler rejected");
        t.Equal(m->Action(0, nullptr, &actual), LINUX_EINVAL, "invalid signal rejected");
    });

    suite.Run("logic/SignalManager/mask_temporary_restore", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint64_t all = UINT64_MAX, previous = 0, actual = 0;
        t.Equal(m->Mask(2, &all, &previous), 0, "replace blocked mask");
        t.Equal(m->Mask(0, nullptr, &actual), 0, "query mask");
        t.Equal(actual, all & ~SIGNAL_UNBLOCKABLE, "uncatchable signals never masked");
        t.Equal(m->BeginTemporaryMask(0), 0, "begin temporary mask");
        m->FinishTemporaryMask(false);
        t.Equal(m->Mask(0, nullptr, &actual), 0, "query restored mask");
        t.Equal(actual, all & ~SIGNAL_UNBLOCKABLE, "temporary mask restores prior state");
    });

    suite.Run("logic/SignalManager/alternate_stack", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        signal_stack_t disabled = {0, 2, 0, 0}, actual = {}, shortStack = {0x40000000, 0, 0, 1};
        t.Equal(m->Stack(&disabled, &actual, 0), 0, "disable alternate stack");
        t.Equal(m->Stack(nullptr, &actual, 0), 0, "query alternate stack");
        t.Equal(actual.flags, 2, "disabled state");
        t.Equal(m->Stack(&shortStack, nullptr, 0), LINUX_ENOMEM, "undersized stack rejected");
    });
}
