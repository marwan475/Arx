#include "layers/DispatcherRoutes.hpp"

#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

namespace
{
struct CountedDescriptor
{
    uint32_t references;
    uint32_t releases;
    bool     rejectRetain;
};

static bool retain_descriptor(void*, file_handle_t handle)
{
    CountedDescriptor* descriptor = static_cast<CountedDescriptor*>(handle);

    if (descriptor->rejectRetain)
    {
        return false;
    }

    ++descriptor->references;
    return true;
}

static void release_descriptor(void*, file_handle_t handle)
{
    CountedDescriptor* descriptor = static_cast<CountedDescriptor*>(handle);
    --descriptor->references;
    ++descriptor->releases;
}
} // namespace

void test_ProcessManager(ResourceLayerCaps* resources)
{
    const dispatcher_file_operations_t descriptorOperations(nullptr, retain_descriptor, release_descriptor);

    auto* m = resources ? resources->processManager : nullptr;
    ManagerSuite suite("resource/ProcessManager", m != nullptr);
    suite.Run("resource/ProcessManager/usage_aggregation", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        f.Process->userCpuNanoseconds   = 1999999500;
        f.Process->kernelCpuNanoseconds = 2000001000;
        f.Process->minorFaults          = 7;
        f.Process->childUsage[0]        = 2;
        f.Process->childUsage[1]        = 2;
        f.Process->childUsage[8]        = 3;
        uint64_t usage[18];
        ProcessManager::SnapshotUsage(f.Process, usage);
        t.Equal(usage[0], 4, "child CPU seconds carry from microseconds");
        t.Equal(usage[1], 1, "sub-microsecond CPU precision truncates only at ABI boundary");
        t.Equal(usage[2], 2, "kernel CPU seconds");
        t.Equal(usage[3], 1, "kernel CPU microseconds");
        t.Equal(usage[8], 10, "minor faults include reaped descendants");
        t.Check(usage[9] == 0 && usage[11] == 0 && usage[12] == 0, "memory filesystem does not fabricate disk faults or block I/O");
        ProcessManager::SnapshotUsage(f.Process, usage, false);
        t.Equal(usage[0], 1, "own CPU snapshot excludes descendants");
        t.Equal(usage[8], 7, "own fault snapshot excludes descendants");
        process_t* ancestor = m->AllocateProcess();

        if (t.Check(ancestor != nullptr, "allocate ancestor accounting record"))
        {
            ProcessManager::ReapUsage(ancestor, f.Process);
            ProcessManager::SnapshotUsage(ancestor, usage);
            t.Equal(usage[0], 4, "reaping includes CPU time already collected from grandchildren");
            t.Equal(usage[1], 1, "ancestor preserves normalized CPU microseconds");
            t.Equal(usage[8], 10, "reaping includes descendant faults");
            t.Equal(ancestor->userCpuNanoseconds, 0, "child CPU never becomes parent's own CPU or timer budget");
            t.Check(m->FreeProcess(ancestor), "free ancestor accounting record");
        }
    });

    suite.Run("resource/ProcessManager/private_task_retirement", [&](TestCase& t)
    {
        virt_addr_space_t* space   = resources->virtualMemoryManager->CreateUserAddressSpace();
        process_t*         process = space != nullptr ? m->CreateProcess(space) : nullptr;

        if (!t.Check(process != nullptr, "allocate disposable thread group"))
        {
            resources->virtualMemoryManager->DestroyUserAddressSpace(space);
            return;
        }

        process->ownsAddressSpace = true;
        task_t*           first   = resources->taskManager->AllocateTask();
        task_t*           second  = resources->taskManager->AllocateTask();
        CountedDescriptor file    = {1, 0, false};
        const int64_t     fd      = m->AddFileDescriptor(process, &file, FD_FLAG_NONE, &descriptorOperations);
        bool              ready   = first != nullptr && second != nullptr && fd >= 0;

        if (ready)
        {
            ready                     = m->AddTask(process, first);
            second->descriptorTable   = m->CopyDescriptorTable(process->descriptorTable);
            second->filesystemContext = m->CopyFilesystemContext(process->filesystemContext);
            ready                     = ready && second->descriptorTable != nullptr && second->filesystemContext != nullptr && m->AddTask(process, second);
        }

        t.Check(ready, "attach threads with private descriptor and filesystem contexts");

        if (first != nullptr)
        {
            if (first->process == process)
            {
                m->RemoveTask(process, first);
            }

            t.Check(resources->taskManager->FreeTask(first), "retire original resource owner first");
        }

        if (ready)
        {
            t.Equal(file.references, 1, "private original table closes when its owning task exits");
            t.Equal(file.releases, 1, "surviving private thread does not pin departed owner's table");
            t.Check(second->descriptorTable->entries[fd].file == &file, "private survivor keeps its descriptor usable");
        }

        if (second != nullptr)
        {
            if (second->process == process)
            {
                m->RemoveTask(process, second);
            }

            t.Check(resources->taskManager->FreeTask(second), "retire final resource owner");
        }

        t.Check(m->FreeProcess(process), "reclaim thread group after task retirement");
        t.Equal(file.references, 0, "all file references drain after the last owner exits");
    });
    suite.Run("resource/ProcessManager/resource_reference_ownership", [&](TestCase& t)
    {
        descriptor_table_t* table = m->CreateDescriptorTable();

        if (!t.Check(table != nullptr, "allocate descriptor ownership object"))
        {
            return;
        }

        CountedDescriptor file     = {1, 0, false};
        table->entries[0]          = {&file, FD_FLAG_CLOEXEC, &descriptorOperations};
        descriptor_table_t* copied = m->CopyDescriptorTable(table);

        if (!t.Check(copied != nullptr, "copy retains open file descriptions"))
        {
            m->ReleaseDescriptorTable(table);
            return;
        }

        t.Equal(file.references, 2, "private tables own independent open file references");
        copied->entries[0].flags = FD_FLAG_NONE;
        t.Equal(table->entries[0].flags, FD_FLAG_CLOEXEC, "private descriptor flags stay isolated");
        __atomic_add_fetch(&table->references, 1U, __ATOMIC_RELAXED);
        m->ReleaseDescriptorTable(table);
        t.Equal(file.releases, 0, "shared table survives its first owner exiting");
        m->ReleaseDescriptorTable(copied);
        t.Equal(file.references, 1, "private owner exit preserves the shared table's file");
        m->ReleaseDescriptorTable(table);
        t.Equal(file.references, 0, "last table owner releases open file");
        t.Equal(file.releases, 2, "each table releases exactly one open file reference");
    });

    suite.Run("resource/ProcessManager/resource_copy_rollback", [&](TestCase& t)
    {
        descriptor_table_t* table = m->CreateDescriptorTable();

        if (!t.Check(table != nullptr, "allocate rollback source"))
        {
            return;
        }

        CountedDescriptor first    = {1, 0, false};
        CountedDescriptor second   = {1, 0, true};
        table->entries[0]          = {&first, FD_FLAG_NONE, &descriptorOperations};
        table->entries[1]          = {&second, FD_FLAG_NONE, &descriptorOperations};
        descriptor_table_t* copied = m->CopyDescriptorTable(table);
        t.Check(copied == nullptr, "failed retain rejects the entire table copy");
        t.Equal(first.references, 1, "partial copy rolls back earlier retain");
        t.Equal(second.references, 1, "failed retain never releases unowned reference");
        t.Check(table->entries[0].file == &first && table->entries[1].file == &second, "failed copy preserves source entries");
        m->ReleaseDescriptorTable(copied);
        m->ReleaseDescriptorTable(table);
        t.Equal(first.references, 0, "source cleanup drains first file");
        t.Equal(second.references, 0, "source cleanup drains rejected file");
    });

    suite.Run("resource/ProcessManager/context_copy_isolation", [&](TestCase& t)
    {
        filesystem_context_t*  filesystem = m->CopyFilesystemContext(nullptr);
        signal_dispositions_t* signals    = m->CopySignalDispositions(nullptr);

        if (!t.Check(filesystem != nullptr && signals != nullptr, "allocate filesystem and signal contexts"))
        {
            m->ReleaseFilesystemContext(filesystem);
            m->ReleaseSignalDispositions(signals);
            return;
        }

        filesystem->umask = 0077;
        memcpy(filesystem->cwdPath, "/private", sizeof("/private"));
        signals->actions[9].handler = 1;
        signals->ignored |= 1ULL << 9;
        filesystem_context_t*  filesystemCopy = m->CopyFilesystemContext(filesystem);
        signal_dispositions_t* signalsCopy    = m->CopySignalDispositions(signals);

        if (t.Check(filesystemCopy != nullptr && signalsCopy != nullptr, "copy independent filesystem and signal contexts"))
        {
            filesystem->umask           = 0002;
            filesystem->cwdPath[1]      = 's';
            signals->actions[9].handler = 0;
            signals->ignored &= ~(1ULL << 9);
            t.Equal(filesystemCopy->umask, 0077, "copied umask stays private");
            t.Check(strcmp(filesystemCopy->cwdPath, "/private") == 0, "copied cwd stays private");
            t.Equal(signalsCopy->actions[9].handler, 1, "copied disposition stays private");
            t.Check((signalsCopy->ignored & (1ULL << 9)) != 0, "copied ignored mask agrees with disposition");
            __atomic_add_fetch(&filesystem->references, 1U, __ATOMIC_RELAXED);
            __atomic_add_fetch(&signals->references, 1U, __ATOMIC_RELAXED);
            m->ReleaseFilesystemContext(filesystem);
            m->ReleaseSignalDispositions(signals);
            t.Equal(filesystem->umask, 0002, "filesystem survives first owner release");
            t.Equal(signals->actions[9].handler, 0, "dispositions survive first owner release");
        }

        m->ReleaseFilesystemContext(filesystemCopy);
        m->ReleaseSignalDispositions(signalsCopy);
        m->ReleaseFilesystemContext(filesystem);
        m->ReleaseSignalDispositions(signals);
    });
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

        {
            process_t alias       = {};
            alias.descriptorTable = f.Process->Files();
            FileDescriptorGuard firstOwner(f.Process);
            FileDescriptorGuard secondOwner(&alias);
            file_descriptor_t* entry = m->GetFileDescriptor(&alias, fd);
            t.Check(entry != nullptr && entry->file == &marker, "nested descriptor transactions recognize a table shared across process identities");
        }

        t.Check(f.Close(fd), "remove descriptor");
        {
            FileDescriptorGuard guard(f.Process);
            auto* entry = m->GetFileDescriptor(f.Process, fd);
            t.Check(entry == nullptr, "removed descriptor lookup returns null");
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
