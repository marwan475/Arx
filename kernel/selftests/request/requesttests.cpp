#include "requesttests.hpp"
#include "RequestTestSupport.hpp"

using namespace request_selftest;

void run_request_selftest_group(void* requestLayerCaps)
{
    SelfTestManager::Get().GroupBegin("request");

    launch_syscall_selftests(requestLayerCaps);

    SelfTestManager::Get().GroupEnd("request");
}

extern "C" void launch_syscall_selftests(void* requestLayerCaps)
{
    auto caps = static_cast<RequestLayerCaps*>(requestLayerCaps);

    if (caps == nullptr)
    {
        TestCase test("request/capabilities");

        test.Check(false, "request layer capabilities missing");

        return;
    }

    {
        TestCase     test("request/user_copy");
        SuiteContext context(test, "user_copy");

        test_user_copy_fixture();
    }

    {
        ManagerSuite suite("request/process", caps->processRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/process/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "process");

            test_process_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/scheduler", caps->schedulerRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/scheduler/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "scheduler");

            test_scheduler_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/time", caps->timeRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/time/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "time");

            test_time_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/memory", caps->memoryRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/memory/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "memory");

            test_memory_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/vfs", caps->vfsRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/vfs/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "vfs");

            test_vfs_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/event", caps->eventRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/event/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "event");

            test_event_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/sync", caps->syncRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/sync/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "sync");

            test_sync_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/signal", caps->signalRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/signal/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "signal");

            test_signal_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/credential", caps->credentialRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/credential/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "credential");

            test_credential_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/system", caps->systemRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/system/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "system");

            test_system_manager(caps);
        });
    }

    {
        ManagerSuite suite("request/socket", caps->socketRequestManager != nullptr, ManagerSuite::CapabilityReport::InContract);

        suite.Run("request/socket/syscalls", [&](TestCase& test)
        {
            SuiteContext context(test, "socket");

            test_socket_manager(caps);
        });
    }
}
