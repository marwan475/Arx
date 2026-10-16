#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/ProcessManager.hpp"

void test_ProcessManager(ResourceLayerCaps* resources)
{
    auto* m = resources ? resources->processManager : nullptr;
    ManagerSuite suite("resource/ProcessManager", m != nullptr);
    suite.Run("resource/ProcessManager/lifecycle_identity", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        t.Check(m->GetCurrentProcess() == f.Process, "current identity matches fixture");
        t.Check(m->GetProcesses() + f.Process->id == f.Process, "process ID resolves to allocated slot");
        t.Check(!m->FreeProcess(f.Process), "running process cannot be freed");
        t.Equal(m->GetCapacity(), ProcessManager::MAX_PROCESSES, "process capacity");
        t.Check(m->CreateProcess(nullptr) == nullptr, "null address space rejected");
    });

    suite.Run("resource/ProcessManager/descriptors", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        int     marker = 0;
        int64_t fd     = m->AddFileDescriptor(f.Process, &marker, FD_FLAG_CLOEXEC);

        if (!t.Check(fd >= 0, "install owned descriptor"))
        {
            return;
        }

        {
            FileDescriptorGuard guard(f.Process);
            auto* entry = m->GetFileDescriptor(f.Process, fd);
            t.Check(entry && entry->file == &marker && entry->flags == FD_FLAG_CLOEXEC, "descriptor lookup and flags");
        }

        t.Check(f.Close(fd), "remove descriptor");
        {
            FileDescriptorGuard guard(f.Process);
            auto* entry = m->GetFileDescriptor(f.Process, fd);
            t.Check(entry && !entry->file, "removed descriptor is empty");
        }

        t.Check(!m->EnsureFileDescriptorCapacity(f.Process, ProcessManager::MAX_FILE_DESCRIPTORS), "capacity bound rejected");
    });

    suite.Run("resource/ProcessManager/task_ownership", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        t.Check(m->GetTasks(f.Process) == f.Task, "attached task is visible");
        t.Check(m->AddTask(f.Process, f.Task), "duplicate attach is idempotent");
        m->RemoveTask(f.Process, f.Task);
        t.Check(m->GetTasks(f.Process) == nullptr, "detach removes list membership");
        t.Check(m->AddTask(f.Process, f.Task), "detached task reattaches");
    });

    suite.Run("resource/ProcessManager/limits_umask", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint32_t old = m->ExchangeUmask(f.Process, 0777);
        t.Equal(m->ExchangeUmask(f.Process, old), 0777, "umask exchange returns prior mask");
        process_file_limit_t invalid = {9, 8}, previous = {};
        t.Equal(m->ChangeFileLimit(f.Process, &invalid, &previous), LINUX_EINVAL, "soft limit cannot exceed hard limit");
    });

    suite.Run("resource/ProcessManager/group_reference", [&](TestCase& t)
    {
        const uint32_t ids[]  = {12, 34};
        auto*          groups = m->CreateGroupList(ids, 2);

        if (t.Check(groups != nullptr, "allocate supplementary group list"))
        {
            t.Equal(groups->count, 2, "group list count");
            t.Equal(groups->ids[1], 34, "group list copies IDs");
            m->ReleaseGroupList(groups);
        }
    });

    suite.Run("resource/ProcessManager/initial_user_stack", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t) || !t.Check(f.Map(), "map initial stack page"))
        {
            return;
        }

        const char*                 arguments[]   = {"unit", "argument"};
        const char*                 environment[] = {"ARX_UNIT=1"};
        process_user_stack_layout_t layout        = {};
        uint64_t                    stack         = 0;

        layout.stackBase = TestFixture::Buffer;
        layout.stackSize = PAGE_SIZE;
        layout.argv      = arguments;
        layout.argc      = 2;
        layout.envp      = environment;
        layout.envc      = 1;

        if (!t.Check(m->BuildUserInitialStack(f.Process, &layout, &stack), "build initial user ABI stack"))
        {
            return;
        }

        t.Check(stack >= TestFixture::Buffer && stack < TestFixture::Buffer + PAGE_SIZE, "stack stays in owned mapping");
        t.Equal(stack % 16, 0, "initial stack alignment");
        uint64_t words[3] = {};

        if (t.Check(safe_read_user(stack, words, sizeof(words)), "read argc and argument pointers"))
        {
            t.Equal(words[0], 2, "argc wire value");
            char   text[16] = {};
            size_t length  = 0;
            t.Check(safe_read_user_string(words[1], text, sizeof(text), &length), "read initial argument");
            t.Check(strcmp(text, "unit") == 0, "argument text copied into user stack");
        }
    });
}
