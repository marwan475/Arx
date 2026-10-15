#include "layers/Logic/ProcessController.hpp"

#include "layers/Logic/ElfMapper.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>
#include <platform.h>

namespace
{
constexpr uint64_t CLONE_VM             = 0x100;
constexpr uint64_t CLONE_FS             = 0x200;
constexpr uint64_t CLONE_FILES          = 0x400;
constexpr uint64_t CLONE_SIGHAND        = 0x800;
constexpr uint64_t CLONE_VFORK          = 0x4000;
constexpr uint64_t CLONE_PARENT         = 0x8000;
constexpr uint64_t CLONE_THREAD         = 0x10000;
constexpr uint64_t CLONE_SYSVSEM        = 0x40000;
constexpr uint64_t CLONE_SETTLS         = 0x80000;
constexpr uint64_t CLONE_PARENT_SETTID  = 0x100000;
constexpr uint64_t CLONE_CHILD_CLEARTID = 0x200000;
constexpr uint64_t CLONE_DETACHED       = 0x400000;
constexpr uint64_t CLONE_CHILD_SETTID   = 0x1000000;
constexpr uint64_t CLONE_SUPPORTED     = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VFORK | CLONE_PARENT | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_DETACHED | CLONE_CHILD_SETTID;

class LifecycleGuard
{
public:
    explicit LifecycleGuard(spinlock_t* lock) : Lock(lock)
    {
        spinlock_acquire_preempt(Lock);
    }

    ~LifecycleGuard()
    {
        spinlock_release_preempt(Lock);
    }

private:
    spinlock_t* Lock;
};

static void close_descriptors(process_t* process, VirtualFileSystem* files, bool closeOnExec)
{
    FileDescriptorGuard guard(process);

    for (uint64_t fd = 0; fd < process->fileDescriptorCount; ++fd)
    {
        file_descriptor_t& descriptor = process->fileDescriptors[fd];

        if (descriptor.file != nullptr && (!closeOnExec || (descriptor.flags & FD_FLAG_CLOEXEC) != 0))
        {
            file_t* file = static_cast<file_t*>(descriptor.file);
            descriptor   = {};
            (void) files->Close(file);
        }
    }
}

static bool writable_word(uint64_t address)
{
    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        return false;
    }

    const bool result = safe_user_memory_accessible_locked(&access, address, sizeof(uint32_t), true);
    end_user_memory(&access);
    return result;
}
} // namespace

ProcessController::ProcessController(ResourceLayerCaps* resources, LogicLayerCaps* logic) : Resources(resources), Logic(logic), Lock(0)
{
}

uint64_t ProcessController::Clone(const arch_syscall_frame_t* frame, const process_clone_arguments_t& arguments)
{
#if defined(__x86_64__)
    if (frame == nullptr || (arguments.flags & ~CLONE_SUPPORTED) != 0 || arguments.pidfd != 0 || arguments.setTidSize != 0 || arguments.cgroup != 0)
    {
        return LINUX_EOPNOTSUPP;
    }

    const uint64_t flags  = arguments.flags;
    const bool     thread = (flags & CLONE_THREAD) != 0;

    if (arguments.exitSignal > 64 || ((flags & CLONE_SIGHAND) != 0 && (flags & CLONE_VM) == 0) || (thread && ((flags & CLONE_SIGHAND) == 0 || arguments.exitSignal != 0)) || ((flags & CLONE_VFORK) != 0 && thread))
    {
        return LINUX_EINVAL;
    }

    // Per-thread private descriptor/FS contexts and cross-process shared signal
    // handlers need separate ownership objects; reject rather than mis-share.
    if ((thread && (flags & (CLONE_FS | CLONE_FILES)) != (CLONE_FS | CLONE_FILES)) || (!thread && (flags & (CLONE_FS | CLONE_FILES | CLONE_SIGHAND)) != 0))
    {
        return LINUX_EOPNOTSUPP;
    }

    if ((frame->user_cs & 3) != 3 || frame->user_rip > CANONICAL_USER_END || frame->user_rsp > CANONICAL_USER_END || arguments.stack > CANONICAL_USER_END || ((flags & CLONE_SETTLS) != 0 && arguments.tls > CANONICAL_USER_END) || (thread && arguments.stack == 0))
    {
        return LINUX_EINVAL;
    }

    ProcessManager* processes = Resources->processManager;
    TaskManager*    tasks     = Resources->taskManager;
    process_t*      parent    = processes->GetCurrentProcess();
    task_t*         caller    = tasks->GetCurrentTask();

    if (parent == nullptr || caller == nullptr)
    {
        return LINUX_ESRCH;
    }

    Logic->scheduler->ReapExitedProcesses();

    if (parent->addressSpace == nullptr || parent->addressSpace->type != VIRT_ADDR_USER || ((flags & CLONE_PARENT) != 0 && !parent->hasParent))
    {
        return LINUX_EINVAL;
    }

    if (((flags & CLONE_PARENT_SETTID) != 0 && !writable_word(arguments.parentTid)) || ((flags & CLONE_CHILD_SETTID) != 0 && !writable_word(arguments.childTid)))
    {
        return LINUX_EFAULT;
    }

    arch_syscall_frame_t childFrame = *frame;

    if (arguments.stack != 0)
    {
        childFrame.user_rsp = arguments.stack;
    }

    task_t* task = nullptr;

    for (uint64_t id = thread ? ProcessManager::MAX_PROCESSES : 1; id < (thread ? TaskManager::MAX_TASKS : ProcessManager::MAX_PROCESSES); ++id)
    {
        if ((!thread && processes->GetProcesses()[id].allocated) || tasks->GetTasks()[id].allocated)
        {
            continue;
        }

        task = tasks->CreateUserCloneTask(&childFrame, id);

        if (task != nullptr)
        {
            break;
        }
    }

    if (task == nullptr)
    {
        return LINUX_EAGAIN;
    }

    process_t* child = parent;

    if (!thread)
    {
        VirtualMemoryManager* memory = Resources->virtualMemoryManager;
        virt_addr_space_t* empty      = memory->CreateUserAddressSpace();
        child                         = empty != nullptr ? processes->CreateProcess(empty, task->id) : nullptr;

        if (child == nullptr)
        {
            memory->DestroyUserAddressSpace(empty);
            tasks->FreeTask(task);
            return LINUX_EAGAIN;
        }

        virt_addr_space_t* space = nullptr;

        if ((flags & CLONE_VM) != 0)
        {
            space = parent->addressSpace;
            __atomic_add_fetch(&space->references, 1U, __ATOMIC_ACQ_REL);
        }
        else
        {
            space = memory->CloneUserAddressSpace(parent->addressSpace);
        }

        if (space == nullptr)
        {
            tasks->FreeTask(task);
            processes->FreeProcess(child);
            memory->DestroyUserAddressSpace(empty);
            return LINUX_ENOMEM;
        }

        child->addressSpace     = space;
        child->ownsAddressSpace = true;
        memory->DestroyUserAddressSpace(empty);
        child->breakBase        = parent->breakBase;
        child->programBreak     = parent->programBreak;
        child->umask            = parent->umask;
        child->fileLimit        = parent->fileLimit;
        child->exitSignal       = static_cast<uint32_t>(arguments.exitSignal);
        child->parentTaskId     = (flags & CLONE_PARENT) != 0 ? parent->parentTaskId : caller->id;
        memcpy(child->cwdPath, parent->cwdPath, sizeof(child->cwdPath));

        if ((flags & CLONE_PARENT) != 0)
        {
            child->hasParent = parent->hasParent;
            child->parentId  = parent->parentId;
        }

        {
            LifecycleGuard guard(&parent->signalLock);
            memcpy(child->signalActions, parent->signalActions, sizeof(child->signalActions));
            child->signalIgnored = parent->signalIgnored;
        }

        bool copied = true;

        {
            FileDescriptorGuard guard(parent);

            for (uint64_t fd = 0; fd < parent->fileDescriptorCount; ++fd)
            {
                const file_descriptor_t& descriptor = parent->fileDescriptors[fd];

                if (descriptor.file == nullptr)
                {
                    continue;
                }

                if (!processes->EnsureFileDescriptorCapacity(child, fd) || !Logic->virtualFileSystem->Retain(static_cast<file_t*>(descriptor.file)))
                {
                    copied = false;
                    break;
                }

                child->fileDescriptors[fd] = descriptor;
            }
        }

        if (!copied)
        {
            tasks->FreeTask(task);
            close_descriptors(child, Logic->virtualFileSystem, false);
            processes->FreeProcess(child);
            return LINUX_ENOMEM;
        }
    }

    task->signalMask  = caller->signalMask;
    task->signalStack = (flags & CLONE_VM) != 0 && (flags & CLONE_VFORK) == 0 ? signal_stack_t{0, 2, 0, 0} : caller->signalStack;
    memcpy(task->name, caller->name, sizeof(task->name));

    if ((flags & CLONE_SETTLS) != 0)
    {
        task->taskContext.fsBase = arguments.tls;
    }

    task->setChildTid = (flags & CLONE_CHILD_SETTID) != 0 ? arguments.childTid : 0;
    Logic->synchronizationManager->ConfigureChildTid(task, child, (flags & CLONE_CHILD_CLEARTID) != 0 ? arguments.childTid : 0);
    const uint32_t id = static_cast<uint32_t>(task->id);

    if ((flags & CLONE_PARENT_SETTID) != 0 && !safe_write_user(arguments.parentTid, &id, sizeof(id)))
    {
        tasks->FreeTask(task);

        if (!thread)
        {
            close_descriptors(child, Logic->virtualFileSystem, false);
            processes->FreeProcess(child);
        }

        return LINUX_EFAULT;
    }

    child->waitable = !thread || child->waitable;

    if (!Logic->scheduler->PublishTask(child, task))
    {
        tasks->FreeTask(task);

        if (!thread)
        {
            close_descriptors(child, Logic->virtualFileSystem, false);
            processes->FreeProcess(child);
        }

        return LINUX_EAGAIN;
    }

    if ((flags & CLONE_VFORK) != 0)
    {
        // Retain the record while the child replaces or releases its shared VM.
        processes->AcquireProcess(child->id);
        const uint64_t sequence = __atomic_load_n(&child->execSequence, __ATOMIC_ACQUIRE);

        while (!__atomic_load_n(&child->exited, __ATOMIC_ACQUIRE) && __atomic_load_n(&child->execSequence, __ATOMIC_ACQUIRE) == sequence)
        {
            (void) Logic->scheduler->WaitForEvent(&child->execSequence, sequence, UINT64_MAX, true);
        }

        processes->ReleaseProcess(child);
    }

    return id;
#else
    (void) frame;
    (void) arguments;
    return LINUX_ENOSYS;
#endif
}

uint64_t ProcessController::SetTidAddress(uint64_t address)
{
    task_t* task = Resources->taskManager->GetCurrentTask();

    if (task == nullptr)
    {
        return LINUX_ESRCH;
    }

    Logic->synchronizationManager->ConfigureChildTid(task, Resources->processManager->GetCurrentProcess(), address);
    return task->id;
}

uint64_t ProcessController::Group(uint32_t operation, int32_t pid, int32_t group)
{
    LifecycleGuard guard(&Lock);
    ProcessManager* manager = Resources->processManager;
    process_t*     caller  = manager->GetCurrentProcess();

    if (caller == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (pid < 0 || (operation == 1 && group < 0))
    {
        return operation == 1 ? LINUX_EINVAL : LINUX_ESRCH;
    }

    const uint64_t id = pid == 0 ? caller->id : static_cast<uint32_t>(pid);
    process_t* target = id < manager->GetCapacity() ? &manager->GetProcesses()[id] : nullptr;

    if (target == nullptr || !target->allocated)
    {
        return LINUX_ESRCH;
    }

    if (operation == 0 || operation == 2)
    {
        return operation == 0 ? target->groupId : target->sessionId;
    }

    if (operation == 3)
    {
        for (size_t index = 0; index < manager->GetCapacity(); ++index)
        {
            const process_t& member = manager->GetProcesses()[index];

            if (member.allocated && member.groupId == caller->id)
            {
                return static_cast<uint64_t>(-1);
            }
        }

        caller->sessionId = caller->id;
        caller->groupId   = caller->id;
        return caller->id;
    }

    if (target != caller && (!target->hasParent || target->parentId != caller->id))
    {
        return LINUX_ESRCH;
    }

    if (target != caller && target->execSequence != 0)
    {
        return static_cast<uint64_t>(-13);
    }

    if (target->sessionId != caller->sessionId || target->sessionId == target->id)
    {
        return static_cast<uint64_t>(-1);
    }

    const uint64_t desired = group == 0 ? target->id : static_cast<uint32_t>(group);
    bool exists            = desired == target->id;

    for (size_t index = 0; !exists && index < manager->GetCapacity(); ++index)
    {
        const process_t& member = manager->GetProcesses()[index];
        exists = member.allocated && member.groupId == desired && member.sessionId == caller->sessionId;
    }

    if (!exists)
    {
        return static_cast<uint64_t>(-1);
    }

    target->groupId = desired;
    return 0;
}

uint64_t ProcessController::Exit(int32_t status, bool group)
{
    ProcessManager* processes = Resources->processManager;
    TaskManager*    tasks     = Resources->taskManager;
    process_t*      process   = processes->GetCurrentProcess();
    task_t*         task      = tasks->GetCurrentTask();

    if (process == nullptr || task == nullptr)
    {
        return LINUX_ESRCH;
    }

    // Kernel selftests exercise the entry point without abandoning the harness.
    if (!task->isUserTask)
    {
        process->exitStatus = status;
        __atomic_store_n(&process->exited, true, __ATOMIC_RELEASE);
        return 0;
    }

    bool last = true;

    for (task_t* sibling = process->tasks; sibling != nullptr; sibling = sibling->next)
    {
        if (sibling != task && sibling->allocated && !sibling->dying)
        {
            last = false;
        }
    }

    tasks->CleanupCurrentTask();

    if (last || group)
    {
        close_descriptors(process, Logic->virtualFileSystem, false);
        (void) Logic->scheduler->KillProcess(process->id, status & 255);
    }
    else
    {
        tasks->MarkDying(task);
    }

    Logic->scheduler->ExitCurrentTask();
}

uint64_t ProcessController::Wait(int64_t selector, uint64_t status, uint32_t options, uint64_t usage, bool waitid, uint32_t type)
{
    if (!waitid && selector == INT32_MIN)
    {
        return LINUX_ESRCH;
    }

    constexpr uint32_t NOHANG = 1;
    constexpr uint32_t EXITED = 4;
    constexpr uint32_t NOWAIT = 0x1000000;
    constexpr uint32_t WALL   = 0x40000000;
    constexpr uint32_t WCLONE = 0x80000000;

    if ((options & ~(NOHANG | 2U | EXITED | 8U | NOWAIT | WALL | WCLONE | 0x20000000U)) != 0 || (!waitid && (options & (EXITED | NOWAIT)) != 0) || (waitid && ((options & (EXITED | 2U | 8U)) == 0 || type > 2)))
    {
        return LINUX_EINVAL;
    }

    ProcessManager* manager = Resources->processManager;
    process_t*     parent  = manager->GetCurrentProcess();
    task_t*        task    = Resources->taskManager->GetCurrentTask();

    if (parent == nullptr || task == nullptr)
    {
        return LINUX_ESRCH;
    }

    for (;;)
    {
        const uint64_t sequence = __atomic_load_n(&parent->childSequence, __ATOMIC_ACQUIRE);
        process_t* selected    = nullptr;
        int event              = 0;
        int stopSignal         = 0;
        uint64_t stopSequence  = 0;
        bool matching          = false;

        {
            LifecycleGuard guard(&Lock);

            for (size_t index = 0; index < manager->GetCapacity(); ++index)
            {
                process_t* child = &manager->GetProcesses()[index];

                if (!child->allocated || !child->waitable || child->waitConsumed || !child->hasParent || child->parentId != parent->id || ((options & 0x20000000U) != 0 && child->parentTaskId != task->id))
                {
                    continue;
                }

                const bool matches = waitid ? type == 0 || (type == 1 && child->id == static_cast<uint64_t>(selector)) || (type == 2 && child->groupId == (selector == 0 ? parent->groupId : static_cast<uint64_t>(selector))) : selector == -1 || (selector > 0 && child->id == static_cast<uint64_t>(selector)) || (selector == 0 && child->groupId == parent->groupId) || (selector < -1 && child->groupId == static_cast<uint64_t>(-selector));

                if (!matches || ((options & WALL) == 0 && ((child->exitSignal != 17) != ((options & WCLONE) != 0))))
                {
                    continue;
                }

                matching = true;
                uint64_t unclaimed = 0;
                const bool exited = __atomic_load_n(&child->exited, __ATOMIC_ACQUIRE) && (!waitid || (options & EXITED) != 0);
                const bool stopped = !child->exited && child->stopPending && (options & 2) != 0;
                const bool continued = !child->exited && child->continuedPending && (options & 8) != 0;

                if ((exited || stopped || continued) && __atomic_compare_exchange_n(&child->waitOwner, &unclaimed, task->id + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                {
                    selected = manager->AcquireProcess(child->id);
                    event = exited ? 1 : stopped ? 2 : 3;
                    stopSignal = child->stopSignal;
                    stopSequence = child->stopSequence;
                    break;
                }
            }
        }

        if (selected != nullptr)
        {
            const uint64_t pid = selected->id;
            bool copied        = true;

            if (waitid)
            {
                struct
                {
                    int32_t signal;
                    int32_t error;
                    int32_t code;
                    int32_t padding;
                    int32_t pid;
                    uint32_t uid;
                    int32_t status;
                    uint32_t padding2;
                    uint64_t times[2];
                    uint8_t reserved[80];
                } info = {};
                static_assert(sizeof(info) == 128, "Linux x86-64 siginfo ABI");
                info.signal = 17;
                info.code   = event == 2 ? 5 : event == 3 ? 6 : selected->terminationSignal != 0 ? 2 : 1;
                info.pid    = static_cast<int32_t>(pid);
                info.uid    = selected->credentials.user.real;
                info.status = event == 2 ? stopSignal : event == 3 ? 18 : selected->terminationSignal != 0 ? selected->terminationSignal : selected->exitStatus & 255;
                copied      = status == 0 || safe_write_user(status, &info, sizeof(info));
            }
            else
            {
                const int32_t value = event == 2 ? (stopSignal << 8) | 127 : event == 3 ? 0xffff : selected->terminationSignal != 0 ? selected->terminationSignal & 127 : (selected->exitStatus & 255) << 8;
                copied              = status == 0 || safe_write_user(status, &value, sizeof(value));
            }

            if (copied && usage != 0)
            {
                const uint64_t values[18] = {};
                copied = safe_write_user(usage, values, sizeof(values));
            }

            if (copied && (options & NOWAIT) == 0)
            {
                if (event == 1)
                {
                    __atomic_store_n(&selected->waitConsumed, true, __ATOMIC_RELEASE);
                }
                else
                {
                    LifecycleGuard guard(&selected->signalLock);

                    if (selected->stopSequence == stopSequence)
                    {
                        selected->stopPending = event == 2 ? false : selected->stopPending;
                        selected->continuedPending = event == 3 ? false : selected->continuedPending;
                    }
                }
            }

            __atomic_store_n(&selected->waitOwner, 0ULL, __ATOMIC_RELEASE);
            manager->ReleaseProcess(selected);
            __atomic_add_fetch(&parent->childSequence, 1ULL, __ATOMIC_RELEASE);
            Logic->scheduler->WakeEventWaiters(&parent->childSequence);
            return copied ? waitid ? 0 : pid : LINUX_EFAULT;
        }

        if (!matching)
        {
            return static_cast<uint64_t>(-10);
        }

        if ((options & NOHANG) != 0)
        {
            if (waitid && status != 0)
            {
                const uint8_t empty[128] = {};

                if (!safe_write_user(status, empty, sizeof(empty)))
                {
                    return LINUX_EFAULT;
                }
            }

            return 0;
        }

        const uint64_t result = Logic->scheduler->WaitForEvent(&parent->childSequence, sequence);

        if (result != 0)
        {
            return result;
        }
    }
}

namespace
{
constexpr size_t EXEC_ARGUMENT_BYTES = 2 * 1024 * 1024;
constexpr size_t EXEC_STRING_BYTES   = 128 * 1024;
constexpr size_t EXEC_POINTERS       = EXEC_ARGUMENT_BYTES / sizeof(uint64_t);

struct ExecArguments
{
    char*  bytes;
    char** argv;
    char** envp;
    size_t used;
    size_t argc;
    size_t envc;

    ExecArguments() : bytes(static_cast<char*>(vmalloc(EXEC_ARGUMENT_BYTES))), argv(static_cast<char**>(vmalloc(EXEC_ARGUMENT_BYTES))), envp(static_cast<char**>(vmalloc(EXEC_ARGUMENT_BYTES))), used(0), argc(0), envc(0)
    {
    }

    ~ExecArguments()
    {
        if (bytes != nullptr)
        {
            vfree(bytes);
        }

        if (argv != nullptr)
        {
            vfree(argv);
        }

        if (envp != nullptr)
        {
            vfree(envp);
        }
    }

    uint64_t CopyVector(uint64_t vector, bool environment)
    {
        if (vector == 0)
        {
            return 0;
        }

        size_t& count  = environment ? envc : argc;
        char**  output = environment ? envp : argv;

        for (size_t index = 0; argc + envc + 4 < EXEC_POINTERS; ++index)
        {
            uint64_t address;

            if (vector > CANONICAL_USER_END - index * sizeof(uint64_t) || !safe_read_user(vector + index * sizeof(uint64_t), &address, sizeof(address)))
            {
                return LINUX_EFAULT;
            }

            if (address == 0)
            {
                return 0;
            }

            output[count] = bytes + used;
            size_t length = 0;

            for (; length < EXEC_STRING_BYTES; ++length)
            {
                if (used + (argc + envc + 4) * sizeof(uint64_t) >= EXEC_ARGUMENT_BYTES)
                {
                    return static_cast<uint64_t>(-7);
                }

                if (address > CANONICAL_USER_END - length || !safe_read_user(address + length, bytes + used, 1))
                {
                    return LINUX_EFAULT;
                }

                if (bytes[used++] == '\0')
                {
                    break;
                }
            }

            if (length == EXEC_STRING_BYTES)
            {
                return static_cast<uint64_t>(-7);
            }

            ++count;
        }

        return static_cast<uint64_t>(-7);
    }

    char* Store(const char* string)
    {
        const size_t size = strlen(string) + 1;

        if (size > EXEC_ARGUMENT_BYTES - used || used + size + (argc + envc + 8) * sizeof(uint64_t) >= EXEC_ARGUMENT_BYTES)
        {
            return nullptr;
        }

        char* result = bytes + used;
        memcpy(result, string, size);
        used += size;
        return result;
    }
};

struct ExecImage
{
    ResourceLayerCaps* resources;
    process_t*        process;
    elf_metadata_t    executable;
    elf_metadata_t    interpreter;

    explicit ExecImage(ResourceLayerCaps* caps) : resources(caps), process(nullptr), executable{}, interpreter{}
    {
    }

    ~ExecImage()
    {
        if (process != nullptr)
        {
            resources->processManager->FreeProcess(process);
        }

        kfree(executable.segments);
        kfree(interpreter.segments);
    }
};
} // namespace

uint64_t ProcessController::Execute(arch_syscall_frame_t* frame)
{
#if defined(__x86_64__)
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    ProcessManager* processes = Resources->processManager;
    TaskManager*    tasks     = Resources->taskManager;
    process_t*      current   = processes->GetCurrentProcess();
    task_t*         caller    = tasks->GetCurrentTask();

    if (current == nullptr || caller == nullptr)
    {
        return LINUX_ESRCH;
    }

    char requested[256];

    if (!safe_read_user_string(frame->arg0, requested, sizeof(requested), nullptr))
    {
        return LINUX_EFAULT;
    }

    if (requested[0] == '\0')
    {
        return LINUX_ENOENT;
    }

    if (!caller->isUserTask || (frame->user_cs & 3) != 3)
    {
        return LINUX_EINVAL;
    }

    ExecArguments arguments;

    if (arguments.bytes == nullptr || arguments.argv == nullptr || arguments.envp == nullptr)
    {
        return LINUX_ENOMEM;
    }

    uint64_t result = arguments.CopyVector(frame->arg1, false);

    if (result == 0)
    {
        result = arguments.CopyVector(frame->arg2, true);
    }

    if (result != 0)
    {
        return result;
    }

    VirtualFileSystem* files = Logic->virtualFileSystem;
    char path[256];

    if (!files->JoinPath(current->cwdPath, requested, path, sizeof(path)))
    {
        return static_cast<uint64_t>(-36);
    }

    ExecImage image(Resources);
    virt_addr_space_t* space = Resources->virtualMemoryManager->CreateUserAddressSpace();
    image.process           = space != nullptr ? processes->CreateProcess(space) : nullptr;

    if (image.process == nullptr)
    {
        Resources->virtualMemoryManager->DestroyUserAddressSpace(space);
        return LINUX_ENOMEM;
    }

    image.process->ownsAddressSpace = true;
    uint64_t entry                 = 0;
    uint64_t interpreterBase       = 0;
    uint32_t executableMode        = 0;
    uint32_t executableUid         = 0;
    uint32_t executableGid         = 0;
    bool allowSetId                = false;
    bool loaded                    = false;

    for (unsigned depth = 0; depth < 5; ++depth)
    {
        int64_t error = 0;
        file_t* file  = files->Open({}, path, 0, &error);

        if (file == nullptr)
        {
            return error != 0 ? static_cast<uint64_t>(error) : LINUX_ENOENT;
        }

        if (file->inode == nullptr || file->inode->type != INODE_REGULAR || (file->path.mount != nullptr && (file->path.mount->flags & 8) != 0))
        {
            files->Close(file);
            return LINUX_EACCES;
        }

        result = files->CheckExecutable(file);

        if (result != 0)
        {
            files->Close(file);
            return result;
        }

        char header[256] = {};
        const int64_t headerSize = files->ReadAt(file, header, sizeof(header) - 1, 0);

        if (headerSize >= 2 && header[0] == '#' && header[1] == '!')
        {
            files->Close(file);
            char* end = header + 2;

            while (*end != '\0' && *end != '\n')
            {
                ++end;
            }

            if (*end == '\0' && headerSize == static_cast<int64_t>(sizeof(header) - 1))
            {
                return static_cast<uint64_t>(-8);
            }

            *end = '\0';

            while (end > header + 2 && (end[-1] == ' ' || end[-1] == '\t'))
            {
                *--end = '\0';
            }

            char* interpreter = header + 2;

            while (*interpreter == ' ' || *interpreter == '\t')
            {
                ++interpreter;
            }

            char* option = interpreter;

            while (*option != '\0' && *option != ' ' && *option != '\t')
            {
                ++option;
            }

            if (*option != '\0')
            {
                *option++ = '\0';
            }

            while (*option == ' ' || *option == '\t')
            {
                ++option;
            }

            if (*interpreter == '\0')
            {
                return static_cast<uint64_t>(-8);
            }

            char* savedPath        = arguments.Store(path);
            char* savedInterpreter = arguments.Store(interpreter);
            char* savedOption      = *option != '\0' ? arguments.Store(option) : nullptr;

            if (savedPath == nullptr || savedInterpreter == nullptr || (*option != '\0' && savedOption == nullptr))
            {
                return static_cast<uint64_t>(-7);
            }

            const size_t prefix = savedOption != nullptr ? 3 : 2;
            const size_t tail   = arguments.argc != 0 ? arguments.argc - 1 : 0;

            if (tail + prefix >= EXEC_POINTERS)
            {
                return static_cast<uint64_t>(-7);
            }

            for (size_t index = tail; index > 0; --index)
            {
                arguments.argv[prefix + index - 1] = arguments.argv[index];
            }

            arguments.argv[0]          = savedInterpreter;
            arguments.argv[prefix - 1] = savedPath;

            if (savedOption != nullptr)
            {
                arguments.argv[1] = savedOption;
            }

            arguments.argc = tail + prefix;

            if (!files->JoinPath(current->cwdPath, savedInterpreter, path, sizeof(path)))
            {
                return static_cast<uint64_t>(-36);
            }

            if (depth == 4)
            {
                return static_cast<uint64_t>(-40);
            }

            continue;
        }

        executableMode = file->inode->mode;
        executableUid  = file->inode->uid;
        executableGid  = file->inode->gid;
        allowSetId     = file->path.mount == nullptr || (file->path.mount->flags & 2) == 0;
        loaded         = Logic->elfMapper->ReadElf(file, &image.executable) && Logic->elfMapper->LoadExecutable(image.process, file, &image.executable);
        files->Close(file);
        break;
    }

    if (!loaded)
    {
        return static_cast<uint64_t>(-8);
    }

    entry = image.executable.entryPoint;
    const uint64_t breakBase = image.process->breakBase;

    if (image.executable.interpreter[0] != '\0')
    {
        int64_t error = 0;
        file_t* file = files->Open({}, image.executable.interpreter, 0, &error);

        if (file == nullptr)
        {
            return error != 0 ? static_cast<uint64_t>(error) : LINUX_ENOENT;
        }

        result = files->CheckExecutable(file);
        bool valid = result == 0 && Logic->elfMapper->ReadElf(file, &image.interpreter);
        image.interpreter.loadBias = 0x700000000000;
        valid = valid && image.interpreter.interpreter[0] == '\0' && Logic->elfMapper->LoadExecutable(image.process, file, &image.interpreter);
        files->Close(file);

        if (!valid)
        {
            return result != 0 ? result : static_cast<uint64_t>(-80);
        }

        interpreterBase = image.interpreter.loadBias;
        entry           = image.interpreter.entryPoint;
    }

    constexpr uint64_t STACK_TOP  = 0x7ffffff00000;
    constexpr uint64_t STACK_SIZE = 8 * 1024 * 1024;

    for (uint64_t address = STACK_TOP - STACK_SIZE; address < STACK_TOP; address += PAGE_SIZE)
    {
        if (!Resources->virtualMemoryManager->AllocateAnonymousPage(space, address, image.executable.executableStack ? 7 : 3, false))
        {
            return LINUX_ENOMEM;
        }
    }

    uint64_t seed[2] = {};
    const bool randomAvailable = arch_user_random_seed(seed);
    const uint64_t randomAddress = STACK_TOP - 16;
    const uint64_t nameAddress = randomAddress - strlen(requested) - 1;
    if (!copy_to_process_virtual(space, randomAddress, seed, sizeof(seed)) || !copy_to_process_virtual(space, nameAddress, requested, strlen(requested) + 1))
    {
        return LINUX_ENOMEM;
    }

    process_credentials_t credentials = current->credentials;

    if (allowSetId && (executableMode & 04000) != 0)
    {
        credentials.user.effective = executableUid;
    }

    if (allowSetId && (executableMode & 02010) == 02010)
    {
        credentials.group.effective = executableGid;
    }

    credentials.user.saved       = credentials.user.effective;
    credentials.user.filesystem  = credentials.user.effective;
    credentials.group.saved      = credentials.group.effective;
    credentials.group.filesystem = credentials.group.effective;
    const uint64_t secure = credentials.user.real != credentials.user.effective || credentials.group.real != credentials.group.effective;
    process_user_auxv_entry_t auxiliary[20] = {
        {3, image.executable.programHeaderAddress},
        {4, image.executable.programHeaderEntrySize},
        {5, image.executable.programHeaderCount},
        {6, PAGE_SIZE},
        {7, interpreterBase},
        {8, 0},
        {9, image.executable.entryPoint},
        {11, credentials.user.real},
        {12, credentials.user.effective},
        {13, credentials.group.real},
        {14, credentials.group.effective},
        {17, 100},
        {23, secure},
        {31, nameAddress},
    };
    size_t auxiliaryCount = 14;

    if (randomAvailable)
    {
        auxiliary[auxiliaryCount++] = {25, randomAddress};
    }

    auxiliary[auxiliaryCount++] = {0, 0};
    process_user_stack_layout_t layout = {};
    layout.stackBase = STACK_TOP - STACK_SIZE;
    layout.stackSize = nameAddress - layout.stackBase;
    layout.argv      = arguments.argv;
    layout.argc      = arguments.argc;
    layout.envp      = arguments.envp;
    layout.envc      = arguments.envc;
    layout.auxv      = auxiliary;
    layout.auxvCount = auxiliaryCount;
    uint64_t stackPointer = 0;

    if (!processes->BuildUserInitialStack(image.process, &layout, &stackPointer))
    {
        return static_cast<uint64_t>(-7);
    }

    // Everything that can reject the executable has completed. Retire sibling
    // contexts on their old VM before publishing the replacement address space.
    for (task_t* sibling = current->tasks; sibling != nullptr; sibling = sibling->next)
    {
        if (sibling != caller)
        {
            tasks->MarkDying(sibling);
        }
    }

    for (;;)
    {
        const uint64_t sequence = __atomic_load_n(&current->childSequence, __ATOMIC_ACQUIRE);
        Logic->scheduler->WakeSleepingTasks(arch_cpu_id());
        Logic->scheduler->ReapExitedProcesses();
        bool siblings = false;

        for (task_t* sibling = current->tasks; sibling != nullptr; sibling = sibling->next)
        {
            siblings = siblings || sibling != caller;
        }

        if (!siblings)
        {
            break;
        }

        (void) Logic->scheduler->WaitForEvent(&current->childSequence, sequence, UINT64_MAX, true);
    }

    tasks->CleanupCurrentTask();
    close_descriptors(current, files, true);
    virt_addr_space_t* oldSpace = current->addressSpace;
    const bool oldOwned        = current->ownsAddressSpace;
    current->addressSpace          = space;
    current->ownsAddressSpace      = true;
    image.process->addressSpace    = nullptr;
    image.process->ownsAddressSpace = false;
    current->breakBase             = breakBase;
    current->programBreak          = breakBase;
    current->elfMetadata           = nullptr;
    current->credentials           = credentials;
    current->exitSignal            = 17;

    {
        LifecycleGuard guard(&current->signalLock);

        for (size_t index = 0; index < 32; ++index)
        {
            if (current->signalActions[index].handler != 1)
            {
                current->signalActions[index] = {};
            }

            const uint64_t bit = 1ULL << index;

            if (current->signalActions[index].handler == 1 || (current->signalActions[index].handler == 0 && (SIGNAL_DEFAULT_IGNORED & bit) != 0))
            {
                current->signalIgnored |= bit;
            }
            else
            {
                current->signalIgnored &= ~bit;
            }
        }
    }

    caller->signalStack = {0, 2, 0, 0};
    caller->signalDepth = 0;
    caller->signalRestoreMask = false;
    caller->setChildTid = 0;
    tasks->SetCurrentIdentity(current->id);

    for (size_t index = 0; index < processes->GetCapacity(); ++index)
    {
        process_t* child = processes->AcquireProcess(index);

        if (child != nullptr)
        {
            if (child->hasParent && child->parentId == current->id)
            {
                child->parentTaskId = current->id;
            }

            processes->ReleaseProcess(child);
        }
    }

    memset(caller->name, 0, sizeof(caller->name));
    const char* name = path;

    for (const char* cursor = path; *cursor != '\0'; ++cursor)
    {
        if (*cursor == '/')
        {
            name = cursor + 1;
        }
    }

    memcpy(caller->name, name, strlen(name) < 15 ? strlen(name) : 15);
    tasks->SetCurrentTlsBase(false, 0);
    tasks->SetCurrentTlsBase(true, 0);
    processes->ActivateProcessAddressSpace(current);

    if (oldOwned)
    {
        Resources->virtualMemoryManager->DestroyUserAddressSpace(oldSpace);
    }

    const uint64_t floating = frame->fxstate;
    memset(frame, 0, sizeof(*frame));
    frame->fxstate     = floating;
    frame->user_rip    = entry;
    frame->user_rsp    = stackPointer;
    frame->user_cs     = 0x1b;
    frame->user_ss     = 0x23;
    frame->user_rflags = 0x202;

    if (floating != 0)
    {
        memset(reinterpret_cast<void*>(floating), 0, 512);
        *reinterpret_cast<uint16_t*>(floating) = 0x37f;
        *reinterpret_cast<uint32_t*>(floating + 24) = 0x1f80;
    }

    __atomic_add_fetch(&current->execSequence, 1ULL, __ATOMIC_RELEASE);
    Logic->scheduler->WakeEventWaiters(&current->execSequence);
    return 0;
#else
    (void) frame;
    return LINUX_ENOSYS;
#endif
}
