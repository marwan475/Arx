#pragma once

#include "TestCase.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <platform.h>

// A synchronous, disposable identity. Never schedule this task or sleep while
// holding this fixture: the executing stack still belongs to its caller.
// The isolated user root keeps test mappings out of the live kernel root.
class TestFixture
{
public:
    explicit TestFixture(ResourceLayerCaps* resources)
        : R(resources), Process(nullptr), Task(nullptr), Space(nullptr), PreviousProcess(nullptr), PreviousTask(nullptr), PreviousSpace(nullptr), Ready(false)
    {
        cpu_preemption_disable();
        Cpu           = arch_cpu_id();
        PreviousSpace = platform.cpus[Cpu].address_space;

        if (!R || !R->processManager || !R->taskManager || !R->virtualMemoryManager)
        {
            return;
        }

        PreviousProcess = R->processManager->GetCurrentProcess();
        PreviousTask    = R->taskManager->GetCurrentTask();
        Space           = R->virtualMemoryManager->CreateUserAddressSpace();

        if (!Space)
        {
            return;
        }

        Process = R->processManager->CreateProcess(Space);
        Task    = R->taskManager->AllocateTask();

        if (!Process || !Task)
        {
            return;
        }

        if (!R->processManager->AddTask(Process, Task))
        {
            return;
        }

        if (!R->processManager->SetRunningProcess(Cpu, Process))
        {
            return;
        }

        if (!R->taskManager->SetRunningTask(Cpu, Task))
        {
            return;
        }

        Ready = true;
    }

    ~TestFixture()
    {
        if (R && R->processManager && R->taskManager && R->virtualMemoryManager)
        {
            if (!R->taskManager->SetRunningTask(Cpu, PreviousTask) || !R->processManager->SetRunningProcess(Cpu, PreviousProcess))
            {
                SelfTestManager::Get().AbortUnsafe("cannot restore fixture identity");
            }

            if (PreviousSpace)
            {
                R->virtualMemoryManager->SwitchAddressSpace(PreviousSpace);
            }

            if (Task)
            {
                if (Process && Task->process == Process)
                {
                    R->processManager->RemoveTask(Process, Task);
                }

                if (!R->taskManager->FreeTask(Task))
                {
                    SelfTestManager::Get().AbortUnsafe("cannot reclaim fixture task");
                }
            }

            if (Process && !R->processManager->FreeProcess(Process))
            {
                SelfTestManager::Get().AbortUnsafe("cannot reclaim fixture process");
            }

            if (Space)
            {
                R->virtualMemoryManager->DestroyUserAddressSpace(Space);
            }
        }

        cpu_preemption_enable();
    }

    TestFixture(const TestFixture&) = delete;
    TestFixture& operator=(const TestFixture&) = delete;
    bool Valid(TestCase& test) const
    {
        if (!R || !R->processManager || !R->taskManager || !R->virtualMemoryManager)
        {
            test.Block("process, task, or address-space fixture dependency unavailable");
            return false;
        }

        if (!Ready)
        {
            test.Check(false, "allocate disposable process, task, and address space");
        }

        return Ready;
    }

    bool Map(uint64_t address = Buffer)
    {
        spinlock_acquire_preempt(&Space->operation_lock);
        bool ok = R->virtualMemoryManager->AllocateAnonymousPage(Space, address, 3, false);
        spinlock_release_preempt(&Space->operation_lock);
        return ok;
    }

    bool Close(int64_t fd)
    {
        file_descriptor_t saved = {};
        {
            FileDescriptorGuard guard(Process);
            file_descriptor_t* entry = R->processManager->GetFileDescriptor(Process, fd);

            if (!entry || !entry->file)
            {
                return false;
            }

            saved  = *entry;
            *entry = {};
        }

        if (saved.release)
        {
            saved.release(saved.file);
        }

        return true;
    }

    static constexpr uint64_t Buffer = 0x40000000;
    ResourceLayerCaps*        R;
    process_t*                Process;
    task_t*                   Task;
    virt_addr_space_t*        Space;

private:
    process_t*         PreviousProcess;
    task_t*            PreviousTask;
    virt_addr_space_t* PreviousSpace;
    uint8_t            Cpu;
    bool               Ready;
};
