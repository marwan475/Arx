#pragma once

#include <arch/selftest.hpp>

// Preserve the exact expectation in diagnostics for compact ABI contracts.
#define REQUEST_EXPECT(expression) ::request_selftest::expect((expression), #expression)

#include <selftests/ManagerSuite.hpp>

#include "layers/Dispatcher.hpp"
#include "layers/Logic/CredentialManager.hpp"
#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/ProcessController.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Logic/SocketManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <klib/klib.h>
#include <platform.h>
#include <selftests/selftests.h>
}

extern "C" uint64_t dispatcher_dispatch_syscall(const arch_syscall_frame_t* frame);

namespace request_selftest
{
// Kernel buffers receive real temporary user mappings, including nested iovecs.
struct request_user_fixture_t
{
    static constexpr size_t   MAX_PAGES = 128;
    static constexpr uint64_t USER_BASE = 0x600000000ULL;
    virt_addr_space_t*        space;
    request_user_fixture_t*   previous;
    virt_addr_t               pages[MAX_PAGES];
    size_t                    count;
    bool                      valid;

    request_user_fixture_t();
    ~request_user_fixture_t();
    uint64_t Address(const void* buffer, size_t length);

    request_user_fixture_t(const request_user_fixture_t&) = delete;

    request_user_fixture_t& operator=(const request_user_fixture_t&) = delete;
};

uint64_t request_user_address(const void* buffer, size_t length);
VirtualMemoryManager* fixture_vmm();

uint64_t syscall_call(uint64_t number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0, uint64_t arg4 = 0, uint64_t arg5 = 0);

uint64_t fixture_map(uint64_t pages = 3, uint64_t address = 0, uint64_t flags = 0x22);
virt_region_t* fixture_region(process_t* process, uint64_t address);
uint8_t* fixture_bytes(process_t* process, uint64_t address);
void fixture_store(process_t* process, uint64_t address, const void* source, size_t length);
void fixture_load(process_t* process, uint64_t address, void* destination, size_t length);

bool expect(bool condition, const char* expectation);

bool equal(uint64_t actual, uint64_t expected, const char* expectation);

void require_cleanup(bool condition, const char* expectation);

bool cleanup(bool condition, const char* expectation);

uint64_t coordination_deadline();

// Supports scheduling with a real caller; TestFixture intentionally cannot.
class ProcessFixture
{
public:
    ProcessFixture();
    ~ProcessFixture();
    ProcessFixture(const ProcessFixture&) = delete;

    ProcessFixture& operator=(const ProcessFixture&) = delete;

    process_t* process = nullptr;
    bool       ready   = false;

private:
    ProcessManager*       processes = nullptr;
    VirtualMemoryManager* memory    = nullptr;
    process_t*            previous  = nullptr;
    virt_addr_space_t*    space     = nullptr;
};

template <typename Function>
bool with_process(Function test)
{
    ProcessFixture fixture;

    return fixture.ready && test(fixture.process);
}

size_t descriptor_count(process_t* process);
size_t pages_in_use();
void detach_fixture_peer(ResourceLayerCaps* resources, process_t* process, task_t* peer);

// Serial BSP-only reporting context. A syscall is a subtest; only manager
// contracts consume the common framework's retained case slots.
class SuiteContext
{
public:
    SuiteContext(TestCase& test, const char* manager);
    ~SuiteContext();
    SuiteContext(const SuiteContext&) = delete;

    SuiteContext& operator=(const SuiteContext&) = delete;
    void Begin(const char* syscall);
    void Finish(bool passed);
    void Skip(const char* reason);

    TestCase&   test;
    const char* manager;
    const char* syscall;
    unsigned    passed = 0, failed = 0, skipped = 0;
    bool        current_failed = false;
};

extern SuiteContext* active_suite;

struct syscall_case_t
{
    uint64_t                    syscallNumber;
    const arch_syscall_frame_t* frame;
};

template <bool (*Validate)(const syscall_case_t*)>
bool isolated(const syscall_case_t* context)
{
    return with_process([&](process_t*)
    {
        // Switching processes also switches page tables. Buffer aliases must
        // belong to this disposable address space and die before it does.
        request_user_fixture_t buffers;
        const bool             passed = Validate(context);

        return expect(buffers.valid, "isolated syscall buffers use valid user mappings") && passed;
    });
}

enum class syscall_coverage_t
{
    Tested,
    Unimplemented,
    Unsupported
};

template <typename ManagerT>
struct syscall_test_t
{
    const char* name;

    uint64_t (ManagerT::*method)(const arch_syscall_frame_t*);

    uint64_t syscallNumber;

    bool (*validate)(const syscall_case_t*);

    syscall_coverage_t coverage = syscall_coverage_t::Tested;
    const char*        reason   = nullptr;
};

template <typename ManagerT, size_t N>
void run_syscalls(ManagerT* manager, const syscall_test_t<ManagerT> (&tests)[N])
{
    if (!expect(manager != nullptr, "request manager is available"))
    {
        return;
    }

    for (const auto& entry : tests)
    {
        active_suite->Begin(entry.name);

        arch_syscall_frame_t frame = {};

        frame.syscall_number = entry.syscallNumber;

        if (entry.coverage != syscall_coverage_t::Tested)
        {
            if (!expect(entry.reason != nullptr && entry.reason[0] != '\0', "coverage gap has an explicit reason"))
            {
                active_suite->Finish(false);

                continue;
            }

            if (entry.coverage == syscall_coverage_t::Unsupported)
            {
                active_suite->Skip(entry.reason);

                continue;
            }
        }

        bool passed = false;
        {
            request_user_fixture_t userFixture;
            const syscall_case_t   context = {entry.syscallNumber, &frame};

            passed = expect(entry.validate != nullptr, "implemented syscall has a behavioral validator");

            if (passed)
            {
                passed = expect(entry.validate(&context), "syscall behavioral contract");
            }

            passed &= expect(userFixture.valid, "syscall buffers use valid user mappings");
        } // Alias cleanup completes before reporting or starting another syscall.

        if (passed && entry.coverage == syscall_coverage_t::Unimplemented)
        {
            active_suite->Skip(entry.reason);
        }
        else
        {
            active_suite->Finish(passed);
        }
    }
}

struct syscall_peer_context_t
{
    Scheduler* scheduler;
    uint64_t   number;
    uint64_t   args[6];
    uint64_t   result;
};

void syscall_peer_entry(void* argument);

template <typename Wait>
static bool with_syscall_peer(process_t* process, syscall_peer_context_t* context, Wait wait)
{
    auto           dispatcher           = static_cast<Dispatcher*>(platform.dispacher);
    auto           resources            = dispatcher->GetResourceLayerCaps();
    auto           table                = process->descriptorTable;
    auto           filesystem           = process->filesystemContext;
    const uint32_t tableReferences      = table->references;
    const uint32_t filesystemReferences = filesystem->references;

    context->scheduler = dispatcher->GetLogicLayerCaps()->scheduler;
    context->result    = UINT64_MAX;

    task_t* peer = resources->taskManager->CreateKernelTask(syscall_peer_entry, context);

    // The fixture owns cleanup; keep the idle reaper from recycling this slot.
    const bool retained = resources->taskManager->RetainForDispatch(peer);
    const bool added    = retained && resources->processManager->AddTask(process, peer);
    const bool ready    = added && context->scheduler->EnqueueProcess(arch_cpu_id(), process->id);

    bool ok = expect(ready, "syscall runnable peer fixture");

    if (ready)
    {
        ok &= wait();
    }

    if (added)
    {
        detach_fixture_peer(resources, process, peer);
    }

    if (peer != nullptr)
    {
        if (retained)
        {
            resources->taskManager->ReleaseDispatchClaim(peer);
        }

        ok &= cleanup(resources->taskManager->FreeTask(peer), "syscall peer cleanup");
    }

    ok &= cleanup(process->descriptorTable == table && process->filesystemContext == filesystem && table->references == tableReferences &&
                      filesystem->references == filesystemReferences,
        "syscall peer cleanup preserves fixture contexts and releases peer references");

    return ok;
}

void test_user_copy_fixture();
void test_process_manager(RequestLayerCaps* caps);
void test_scheduler_manager(RequestLayerCaps* caps);
void test_time_manager(RequestLayerCaps* caps);
void test_memory_manager(RequestLayerCaps* caps);
void test_vfs_manager(RequestLayerCaps* caps);
void test_event_manager(RequestLayerCaps* caps);
void test_sync_manager(RequestLayerCaps* caps);
void test_signal_manager(RequestLayerCaps* caps);
void test_credential_manager(RequestLayerCaps* caps);
void test_system_manager(RequestLayerCaps* caps);
void test_socket_manager(RequestLayerCaps* caps);

} // namespace request_selftest
