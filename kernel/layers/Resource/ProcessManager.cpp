#include "layers/Resource/ProcessManager.hpp"

#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <klib/intrusive_list.h>
#include <klib/klib.h>
#include <memory/heap.h>
#include <memory/vmm.h>
#include <platform.h>
}

namespace
{

// Reaped tasks leave a waitable process record. It no longer owns user signal
// dispositions, but state-only transactions may still inspect the record.
static signal_dispositions_t retiredDispositions = {1, 0, SIGNAL_DEFAULT_IGNORED, {}};

static bool push_word_to_process_stack(virt_addr_space_t* addressSpace, uint64_t* writeCursor, uint64_t value)
{
    if (addressSpace == nullptr || writeCursor == nullptr)
    {
        return false;
    }

    if (!copy_to_process_virtual(addressSpace, *writeCursor, &value, sizeof(value)))
    {
        return false;
    }

    *writeCursor += sizeof(uint64_t);
    return true;
}

} // namespace

descriptor_table_t* process_t::Files() const
{
    task_t* task = TaskManager::GetActiveTask();
    return task != nullptr && task->process == this && task->descriptorTable != nullptr ? task->descriptorTable : descriptorTable;
}

uint64_t process_t::DescriptorLimit() const
{
    return __atomic_load_n(&fileLimit.current, __ATOMIC_ACQUIRE);
}

filesystem_context_t* process_t::Fs() const
{
    task_t* task = TaskManager::GetActiveTask();
    return task != nullptr && task->process == this && task->filesystemContext != nullptr ? task->filesystemContext : filesystemContext;
}

signal_dispositions_t* process_t::Signals() const
{
    return SignalDispositionGuard::Current(this);
}

SignalDispositionGuard* SignalDispositionGuard::Active[BOOT_SMP_MAX_CPUS] = {};

SignalDispositionGuard::SignalDispositionGuard(const process_t* process)
    : Process(process), Dispositions(nullptr), Previous(nullptr), Owner(true), Held(true)
{
    cpu_preemption_disable();
    Previous = Active[arch_cpu_id()];
    spinlock_acquire(&process->resourceLock);
    Dispositions = process->signalDispositions != nullptr ? process->signalDispositions : &retiredDispositions;
    __atomic_add_fetch(&Dispositions->references, 1U, __ATOMIC_RELAXED);
    spinlock_release(&process->resourceLock);

    for (SignalDispositionGuard* guard = Previous; guard != nullptr; guard = guard->Previous)
    {
        if (guard->Dispositions == Dispositions)
        {
            Owner = false;
            break;
        }
    }

    if (Owner)
    {
        spinlock_acquire(&Dispositions->lock);
    }

    Active[arch_cpu_id()] = this;
}

SignalDispositionGuard::~SignalDispositionGuard()
{
    Release();
}

void SignalDispositionGuard::Release()
{
    if (!Held)
    {
        return;
    }

    Active[arch_cpu_id()] = Previous;

    if (Owner)
    {
        spinlock_release(&Dispositions->lock);
    }

    Held = false;
    ProcessManager::ReleaseSignalDispositions(Dispositions);
    cpu_preemption_enable();
}

signal_dispositions_t* SignalDispositionGuard::Current(const process_t* process)
{
    for (SignalDispositionGuard* guard = Active[arch_cpu_id()]; guard != nullptr; guard = guard->Previous)
    {
        if (guard->Process == process)
        {
            return guard->Dispositions;
        }
    }

    return process->signalDispositions != nullptr ? process->signalDispositions : &retiredDispositions;
}

uint64_t pending_signals(const process_t* process, const task_t* task)
{
    if (process == nullptr || task == nullptr)
    {
        return 0;
    }

    SignalDispositionGuard guard(process);
    return (__atomic_load_n(&process->signalPending, __ATOMIC_ACQUIRE) | __atomic_load_n(&task->signalPending, __ATOMIC_ACQUIRE)) & ~(__atomic_load_n(&task->signalMask, __ATOMIC_ACQUIRE) & ~SIGNAL_UNBLOCKABLE) & ~__atomic_load_n(&process->Signals()->ignored, __ATOMIC_ACQUIRE);
}

WorkingDirectorySnapshot::WorkingDirectorySnapshot(const process_t* process)
{
    if (process == nullptr)
    {
        memset(path, 0, sizeof(path));
        path[0] = '/';
        return;
    }

    filesystem_context_t* context = process->Fs();
    spinlock_acquire_preempt(&context->lock);
    memcpy(path, context->cwdPath, sizeof(path));
    spinlock_release_preempt(&context->lock);
}

descriptor_table_t* ProcessManager::CreateDescriptorTable()
{
    descriptor_table_t* table = static_cast<descriptor_table_t*>(kzalloc(sizeof(descriptor_table_t)));

    if (table == nullptr)
    {
        return nullptr;
    }

    table->entries = static_cast<file_descriptor_t*>(kzalloc(sizeof(file_descriptor_t) * DEFAULT_FILE_DESCRIPTOR_COUNT));

    if (table->entries == nullptr)
    {
        kfree(table);
        return nullptr;
    }

    table->references = 1;
    table->count      = DEFAULT_FILE_DESCRIPTOR_COUNT;
    return table;
}

descriptor_table_t* ProcessManager::CopyDescriptorTable(descriptor_table_t* source)
{
    if (source == nullptr)
    {
        return nullptr;
    }

    descriptor_table_t* table = static_cast<descriptor_table_t*>(kzalloc(sizeof(descriptor_table_t)));

    if (table == nullptr)
    {
        return nullptr;
    }

    table->references = 1;
    spinlock_acquire_preempt(&source->lock);
    table->count   = source->count;
    table->entries = static_cast<file_descriptor_t*>(kzalloc(sizeof(file_descriptor_t) * table->count));
    bool copied    = table->entries != nullptr;

    for (uint64_t fd = 0; copied && fd < table->count; ++fd)
    {
        const file_descriptor_t& entry = source->entries[fd];

        if (entry.file != nullptr)
        {
            copied = dispatcher_retain_file(entry.operations, entry.file);

            if (copied)
            {
                table->entries[fd] = entry;
            }
        }
    }

    spinlock_release_preempt(&source->lock);

    if (!copied)
    {
        ReleaseDescriptorTable(table);
        return nullptr;
    }

    return table;
}

void ProcessManager::ReleaseDescriptorTable(descriptor_table_t* table)
{
    if (table == nullptr || __atomic_sub_fetch(&table->references, 1U, __ATOMIC_ACQ_REL) != 0)
    {
        return;
    }

    for (uint64_t fd = 0; table->entries != nullptr && fd < table->count; ++fd)
    {
        const file_descriptor_t& entry = table->entries[fd];

        if (entry.file != nullptr && entry.operations != nullptr)
        {
            dispatcher_release_file(entry.operations, entry.file);
        }
    }

    kfree(table->entries);
    kfree(table);
}

filesystem_context_t* ProcessManager::CopyFilesystemContext(filesystem_context_t* source)
{
    filesystem_context_t* context = static_cast<filesystem_context_t*>(kzalloc(sizeof(filesystem_context_t)));

    if (context == nullptr)
    {
        return nullptr;
    }

    context->references = 1;
    context->umask      = 0022U;
    context->cwdPath[0] = '/';

    if (source != nullptr)
    {
        spinlock_acquire_preempt(&source->lock);
        context->umask = __atomic_load_n(&source->umask, __ATOMIC_RELAXED);
        memcpy(context->cwdPath, source->cwdPath, sizeof(context->cwdPath));
        spinlock_release_preempt(&source->lock);
    }

    return context;
}

void ProcessManager::ReleaseFilesystemContext(filesystem_context_t* context)
{
    if (context != nullptr && __atomic_sub_fetch(&context->references, 1U, __ATOMIC_ACQ_REL) == 0)
    {
        kfree(context);
    }
}

signal_dispositions_t* ProcessManager::CopySignalDispositions(signal_dispositions_t* source)
{
    signal_dispositions_t* dispositions = static_cast<signal_dispositions_t*>(kzalloc(sizeof(signal_dispositions_t)));

    if (dispositions == nullptr)
    {
        return nullptr;
    }

    dispositions->references = 1;
    dispositions->ignored    = SIGNAL_DEFAULT_IGNORED;

    if (source != nullptr)
    {
        spinlock_acquire_preempt(&source->lock);
        dispositions->ignored = source->ignored;
        memcpy(dispositions->actions, source->actions, sizeof(dispositions->actions));
        spinlock_release_preempt(&source->lock);
    }

    return dispositions;
}

void ProcessManager::ReleaseSignalDispositions(signal_dispositions_t* dispositions)
{
    if (dispositions != nullptr && __atomic_sub_fetch(&dispositions->references, 1U, __ATOMIC_ACQ_REL) == 0)
    {
        kfree(dispositions);
    }
}

void ProcessManager::ReleaseProcessResources(process_t* process)
{
    SignalDispositionGuard signalGuard(process);
    spinlock_acquire_preempt(&process->resourceLock);
    descriptor_table_t*    table        = process->descriptorTable;
    filesystem_context_t*  filesystem   = process->filesystemContext;
    signal_dispositions_t* dispositions = process->signalDispositions;
    process->descriptorTable            = nullptr;
    process->filesystemContext          = nullptr;
    process->signalDispositions         = nullptr;
    signal_queue_t* queue               = process->signalQueue;
    process->signalQueue                = nullptr;
    __atomic_store_n(&process->signalPending, 0ULL, __ATOMIC_RELEASE);
    spinlock_release_preempt(&process->resourceLock);
    signalGuard.Release();
    kfree(queue);
    ReleaseDescriptorTable(table);
    ReleaseFilesystemContext(filesystem);
    ReleaseSignalDispositions(dispositions);
}

descriptor_table_t* FileDescriptorGuard::Active[BOOT_SMP_MAX_CPUS] = {};

FileDescriptorGuard::FileDescriptorGuard(process_t* process) : Table(process != nullptr ? process->Files() : nullptr), Previous(nullptr), Owner(false)
{
    cpu_preemption_disable();
    const uint8_t cpu = arch_cpu_id();
    Previous          = Active[cpu];
    Owner             = Table != nullptr && Previous != Table;

    if (Owner)
    {
        spinlock_acquire(&Table->lock);
    }

    if (Table != nullptr)
    {
        Active[cpu] = Table;
    }
}

FileDescriptorGuard::~FileDescriptorGuard()
{
    if (Table != nullptr)
    {
        Active[arch_cpu_id()] = Previous;
    }

    if (Owner)
    {
        spinlock_release(&Table->lock);
    }

    cpu_preemption_enable();
}

void ProcessManager::ResetProcessSlot(process_t* process, uint64_t id, bool allocated)
{
    process->allocated            = allocated;
    process->id                   = id;
    process->hasParent            = false;
    process->parentId             = 0;
    process->parentTaskId         = 0;
    process->groupId              = id;
    process->sessionId            = id;
    process->childSequence        = 0;
    process->execSequence         = 0;
    process->waitOwner            = 0;
    process->exitSignal           = 17;
    process->terminationSignal    = 0;
    process->waitable             = false;
    process->waitConsumed         = false;
    process->exitNotified         = false;
    process->stopped              = false;
    process->stopPending          = false;
    process->continuedPending     = false;
    process->stopSignal           = 0;
    process->stopSequence         = 0;
    process->ownsAddressSpace     = false;
    process->taskCursor           = 0;
    process->exited               = false;
    process->reclaiming           = false;
    process->retirementActive     = false;
    process->signalPending        = 0;
    process->signalQueue          = nullptr;
    process->exitRecipient        = nullptr;
    memset(process->signalSenders, 0, sizeof(process->signalSenders));
    process->ownerCpu             = -1;
    process->exitStatus           = 0;
    process->addressSpace         = nullptr;
    process->elfMetadata          = nullptr;
    process->breakBase            = 0;
    process->programBreak         = 0;
    process->memoryUsers          = 0;
    process->lifetimeUsers        = 0;
    process->userCpuNanoseconds   = 0;
    process->kernelCpuNanoseconds = 0;
    process->minorFaults          = 0;
    memset(process->childUsage, 0, sizeof(process->childUsage));
    process->cpuLimit             = {UINT64_MAX, UINT64_MAX};
    process->cpuLimitNotification = 0;
    memset(process->cpuTimerDeadline, 0, sizeof(process->cpuTimerDeadline));
    memset(process->cpuTimerInterval, 0, sizeof(process->cpuTimerInterval));
    process->timerLock            = 0;
    process->realTimerDeadline    = 0;
    process->realTimerInterval    = 0;
    process->realTimerArmed       = false;
    process->realTimerExpired     = false;
    memset(process->timerIntervalSeconds, 0, sizeof(process->timerIntervalSeconds));
    memset(process->timerIntervalMicroseconds, 0, sizeof(process->timerIntervalMicroseconds));
    process->tasks                = nullptr;
    process->descriptorTable      = nullptr;
    process->filesystemContext    = nullptr;
    process->signalDispositions   = nullptr;
    process->resourceLock         = 0;
    process->fileLimit            = {1024, MAX_FILE_DESCRIPTORS};
    process->credentialLock       = 0;
    process->credentials          = {};
    process->supplementaryGroups  = nullptr;
}

ProcessManager::ProcessManager()
{
    static_assert(sizeof(ProcessManager) <= HEAP_MAX_OBJECT_SIZE, "ProcessManager must fit the largest heap object");
    ManagerLock = 0;

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        RunningProcesses[i] = nullptr;
    }

    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        ResetProcessSlot(&Processes[i], i, false);
    }
}

void ProcessManager::LockManager() const
{
    cpu_preemption_disable();
    spinlock_acquire((spinlock_t*) &ManagerLock);
}

void ProcessManager::UnlockManager() const
{
    spinlock_release((spinlock_t*) &ManagerLock);
    cpu_preemption_enable();
}

process_t* ProcessManager::AllocateProcessUnlocked()
{
    for (size_t i = 0; i < MAX_PROCESSES; i++)
    {
        if (!Processes[i].allocated)
        {
            ResetProcessSlot(&Processes[i], i, true);
            return &Processes[i];
        }
    }

    return nullptr;
}

process_t* ProcessManager::AllocateProcess()
{
    descriptor_table_t*    table        = CreateDescriptorTable();
    filesystem_context_t*  filesystem   = CopyFilesystemContext(nullptr);
    signal_dispositions_t* dispositions = CopySignalDispositions(nullptr);

    if (table == nullptr || filesystem == nullptr || dispositions == nullptr)
    {
        ReleaseDescriptorTable(table);
        ReleaseFilesystemContext(filesystem);
        ReleaseSignalDispositions(dispositions);
        return nullptr;
    }

    LockManager();
    process_t* process = AllocateProcessUnlocked();

    if (process != nullptr)
    {
        process->descriptorTable    = table;
        process->filesystemContext  = filesystem;
        process->signalDispositions = dispositions;
    }

    UnlockManager();

    if (process == nullptr)
    {
        ReleaseDescriptorTable(table);
        ReleaseFilesystemContext(filesystem);
        ReleaseSignalDispositions(dispositions);
    }

    return process;
}

process_t* ProcessManager::CreateProcess(virt_addr_space_t* addressSpace, uint64_t preferredId)
{
    if (addressSpace == nullptr)
    {
        return nullptr;
    }

    descriptor_table_t*    table        = CreateDescriptorTable();
    filesystem_context_t*  filesystem   = CopyFilesystemContext(nullptr);
    signal_dispositions_t* dispositions = CopySignalDispositions(nullptr);

    if (table == nullptr || filesystem == nullptr || dispositions == nullptr)
    {
        ReleaseDescriptorTable(table);
        ReleaseFilesystemContext(filesystem);
        ReleaseSignalDispositions(dispositions);
        return nullptr;
    }

    // operation_lock can be held across synchronous TLB shootdown. Never wait
    // for it with ManagerLock held: a target CPU's timer handler can be waiting
    // for ManagerLock with interrupts disabled, unable to acknowledge the IPI.
    spinlock_acquire_preempt(&addressSpace->operation_lock);
    LockManager();

    // Shared roots currently have no per-process ownership model. Do not attach
    // another process once syscall-owned pages exist in this address space.

    for (size_t i = 0; i < MAX_PROCESSES; ++i)
    {
        if (Processes[i].allocated && Processes[i].addressSpace == addressSpace && Processes[i].memoryUsers != 0)
        {
            UnlockManager();
            spinlock_release_preempt(&addressSpace->operation_lock);
            ReleaseDescriptorTable(table);
            ReleaseFilesystemContext(filesystem);
            ReleaseSignalDispositions(dispositions);
            return nullptr;
        }
    }

    bool hasAnonymousPages = false;

    for (virt_region_t* r = addressSpace->user_used_regions; r; r = r->next)
    {
        if (r->backing)
        {
            hasAnonymousPages = true;
            break;
        }
    }

    spinlock_release_preempt(&addressSpace->operation_lock);

    if (hasAnonymousPages)
    {
        UnlockManager();
        ReleaseDescriptorTable(table);
        ReleaseFilesystemContext(filesystem);
        ReleaseSignalDispositions(dispositions);
        return nullptr;
    }

    process_t* process = nullptr;

    if (preferredId < MAX_PROCESSES)
    {
        if (!Processes[preferredId].allocated)
        {
            process = &Processes[preferredId];
            ResetProcessSlot(process, preferredId, true);
        }
    }
    else
    {
        process = AllocateProcessUnlocked();
    }

    if (process == nullptr)
    {
        UnlockManager();
        ReleaseDescriptorTable(table);
        ReleaseFilesystemContext(filesystem);
        ReleaseSignalDispositions(dispositions);
        return nullptr;
    }

    process->descriptorTable    = table;
    process->filesystemContext  = filesystem;
    process->signalDispositions = dispositions;
    process->addressSpace       = addressSpace;
    process->elfMetadata        = nullptr;
    process->exited             = false;
    process->exitStatus         = 0;

    const uint8_t currentCpu = arch_cpu_id();

    if (currentCpu < BOOT_SMP_MAX_CPUS && RunningProcesses[currentCpu] != nullptr)
    {
        process->hasParent = true;
        process->parentId  = RunningProcesses[currentCpu]->id;
        process_t* parent  = RunningProcesses[currentCpu];
        process->groupId   = parent->groupId;
        process->sessionId = parent->sessionId;
        spinlock_acquire(&parent->credentialLock);
        process->credentials         = parent->credentials;
        process->supplementaryGroups = parent->supplementaryGroups;

        if (process->supplementaryGroups != nullptr)
        {
            __atomic_add_fetch(&process->supplementaryGroups->references, 1U, __ATOMIC_RELAXED);
        }

        spinlock_release(&parent->credentialLock);
    }
    else
    {
        process->hasParent = false;
        process->parentId  = 0;
    }

    UnlockManager();
    return process;
}

bool ProcessManager::FreeProcess(process_t* process, bool onlyIfExited)
{
    descriptor_table_t*    tableToFree        = nullptr;
    filesystem_context_t*  filesystemToFree   = nullptr;
    signal_dispositions_t* dispositionsToFree = nullptr;
    process_group_list_t*  groupsToFree       = nullptr;

    LockManager();

    if (process == nullptr)
    {
        UnlockManager();
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        UnlockManager();
        return false;
    }

    if (!process->allocated || (onlyIfExited && !process->exited) || process->memoryUsers || process->lifetimeUsers || IsProcessRunning(process))
    {
        UnlockManager();
        return false;
    }

    for (task_t* task = process->tasks; task != nullptr; task = task->next)
    {
        if (task->allocated)
        {
            UnlockManager();
            return false;
        }
    }

    // Pin the slot and reject new memory operations while teardown runs.
    // Do not retain ManagerLock across page-table operations and IPI waits.
    process->memoryUsers            = 1;
    process->reclaiming             = true;
    const bool wasExited            = process->exited;
    process->exited                 = true;
    virt_addr_space_t* addressSpace = process->addressSpace;
    process_t* exitRecipient = process->exitRecipient;
    process->exitRecipient = nullptr;
    UnlockManager();

    if (exitRecipient != nullptr)
    {
        SignalDispositionGuard guard(exitRecipient);

        if (exitRecipient->signalQueue != nullptr && exitRecipient->signalQueue->reserved != 0)
        {
            --exitRecipient->signalQueue->reserved;
        }

        guard.Release();
        ReleaseProcess(exitRecipient);
    }

    VirtualMemoryManager memory;

    if (process->ownsAddressSpace)
    {
        memory.DestroyUserAddressSpace(addressSpace);
    }
    else if (addressSpace == nullptr || __atomic_load_n(&addressSpace->references, __ATOMIC_ACQUIRE) <= 1)
    {
        memory.ReleaseAnonymousPages(addressSpace);
    }

    LockManager();
    process->memoryUsers = 0;
    process->exited      = wasExited;

    tableToFree        = process->descriptorTable;
    filesystemToFree   = process->filesystemContext;
    dispositionsToFree = process->signalDispositions;

    signal_queue_t* queueToFree = process->signalQueue;
    groupsToFree = process->supplementaryGroups;
    ResetProcessSlot(process, (uint64_t) (process - &Processes[0]), false);

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; i++)
    {
        if (RunningProcesses[i] == process)
        {
            RunningProcesses[i] = nullptr;
        }
    }

    UnlockManager();

    ReleaseDescriptorTable(tableToFree);
    ReleaseFilesystemContext(filesystemToFree);
    ReleaseSignalDispositions(dispositionsToFree);

    ReleaseGroupList(groupsToFree);
    kfree(queueToFree);

    return true;
}

bool ProcessManager::AddTask(process_t* process, task_t* task)
{
    if (process == nullptr || task == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated || process->exited || !task->allocated)
    {
        return false;
    }

    for (task_t* iter = process->tasks; iter != nullptr; iter = iter->next)
    {
        if (iter == task)
        {
            return true;
        }
    }

    if (task->next != nullptr || task->prev != nullptr)
    {
        return false;
    }

    // Explicit task migration (also used by disposable syscall fixtures) joins
    // the destination process's default contexts. Clone tasks arrive unattached
    // with their flag-selected references already prepared.
    if (task->process != nullptr && task->process != process)
    {
        descriptor_table_t*   table      = task->descriptorTable;
        filesystem_context_t* filesystem = task->filesystemContext;
        task->descriptorTable            = nullptr;
        task->filesystemContext          = nullptr;
        ReleaseDescriptorTable(table);
        ReleaseFilesystemContext(filesystem);
    }

    if (task->descriptorTable == nullptr && process->descriptorTable != nullptr)
    {
        task->descriptorTable = process->descriptorTable;
        __atomic_add_fetch(&task->descriptorTable->references, 1U, __ATOMIC_RELAXED);
    }

    if (task->filesystemContext == nullptr && process->filesystemContext != nullptr)
    {
        task->filesystemContext = process->filesystemContext;
        __atomic_add_fetch(&task->filesystemContext->references, 1U, __ATOMIC_RELAXED);
    }

    ILIST_APPEND(process->tasks, task);
    task->process = process;
    return true;
}

void ProcessManager::RemoveTask(process_t* process, task_t* task)
{
    LockManager();

    if (task->prev != nullptr)
    {
        task->prev->next = task->next;
    }
    else if (process->tasks == task)
    {
        process->tasks = task->next;
    }

    if (task->next != nullptr)
    {
        task->next->prev = task->prev;
    }

    task->next = nullptr;
    task->prev = nullptr;
    UnlockManager();
}

int64_t ProcessManager::AddFileDescriptor(process_t* process, file_handle_t file, uint32_t flags, const dispatcher_file_operations_t* operations)
{
    return AddFileDescriptorFrom(process, file, flags, 0, operations);
}

file_descriptor_t* ProcessManager::GetFileDescriptor(process_t* process, uint64_t fd)
{
    if (process == nullptr || process->Files() == nullptr || process->Files()->entries == nullptr || fd >= process->Files()->count)
    {
        return nullptr;
    }

    file_descriptor_t* descriptor = &process->Files()->entries[fd];
    return descriptor->file != nullptr ? descriptor : nullptr;
}

bool ProcessManager::EnsureFileDescriptorCapacity(process_t* process, uint64_t requiredIndex)
{
    FileDescriptorGuard descriptors(process);

    if (process == nullptr || process->Files() == nullptr || requiredIndex >= process->DescriptorLimit())
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated)
    {
        return false;
    }

    if (requiredIndex < process->Files()->count && process->Files()->entries != nullptr)
    {
        return true;
    }

    uint64_t oldCount = process->Files()->count;
    uint64_t newCount = oldCount;

    if (newCount == 0)
    {
        newCount = DEFAULT_FILE_DESCRIPTOR_COUNT;
        oldCount = 0;
    }

    while (requiredIndex >= newCount)
    {
        if (newCount > (UINT64_MAX / 2ULL))
        {
            return false;
        }

        newCount *= 2ULL;
    }

    if (newCount > SIZE_MAX / sizeof(file_descriptor_t))
    {
        return false;
    }

    file_descriptor_t* newTable = (file_descriptor_t*) kmalloc(sizeof(file_descriptor_t) * newCount);

    if (newTable == nullptr)
    {
        return false;
    }

    memset(newTable, 0, sizeof(file_descriptor_t) * newCount);

    if (process->Files()->entries != nullptr && process->Files()->count > 0)
    {
        memcpy(newTable, process->Files()->entries, sizeof(file_descriptor_t) * process->Files()->count);
        kfree(process->Files()->entries);
    }

    process->Files()->entries = newTable;
    process->Files()->count   = newCount;
    return true;
}

int64_t ProcessManager::AddFileDescriptorFrom(process_t* process, file_handle_t file, uint32_t flags, uint64_t minFd, const dispatcher_file_operations_t* operations)
{
    FileDescriptorGuard descriptors(process);

    if (process == nullptr || file == nullptr)
    {
        return -1;
    }

    if (!EnsureFileDescriptorCapacity(process, minFd))
    {
        return minFd >= process->DescriptorLimit() ? (int64_t) LINUX_EMFILE : (int64_t) LINUX_ENOMEM;
    }

    for (uint64_t i = minFd; i < process->Files()->count && i < process->DescriptorLimit(); i++)
    {
        if (process->Files()->entries[i].file == nullptr)
        {
            process->Files()->entries[i].file       = file;
            process->Files()->entries[i].flags      = flags;
            process->Files()->entries[i].operations = operations;
            return (int64_t) i;
        }
    }

    const uint64_t growFrom = process->Files()->count;

    if (growFrom >= process->DescriptorLimit())
    {
        return (int64_t) LINUX_EMFILE;
    }

    if (!EnsureFileDescriptorCapacity(process, growFrom))
    {
        return (int64_t) LINUX_ENOMEM;
    }

    for (uint64_t i = minFd; i < process->Files()->count && i < process->DescriptorLimit(); i++)
    {
        if (process->Files()->entries[i].file == nullptr)
        {
            process->Files()->entries[i].file       = file;
            process->Files()->entries[i].flags      = flags;
            process->Files()->entries[i].operations = operations;
            return (int64_t) i;
        }
    }

    return (int64_t) LINUX_EMFILE;
}

uint32_t ProcessManager::ExchangeUmask(process_t* process, uint32_t mask)
{
    return __atomic_exchange_n(&process->Fs()->umask, mask & 0777U, __ATOMIC_RELAXED);
}

process_t* ProcessManager::AcquireCurrentCredentials()
{
    cpu_preemption_disable();
    process_t* process = GetCurrentProcess();
    // A running process cannot be freed. Keep preemption disabled until the
    // credential lock is released; no user copies or manager-lock calls inside.

    if (process == nullptr || !process->allocated || process->reclaiming)
    {
        cpu_preemption_enable();
        return nullptr;
    }

    spinlock_acquire(&process->credentialLock);
    return process;
}

void ProcessManager::ReleaseCredentials(process_t* process)
{
    spinlock_release(&process->credentialLock);
    cpu_preemption_enable();
}

process_group_list_t* ProcessManager::CreateGroupList(const uint32_t* ids, uint32_t count)
{
    if (count == 0 || count > MAX_SUPPLEMENTARY_GROUPS || ids == nullptr)
    {
        return nullptr;
    }

    // Full Linux-sized lists exceed the slab allocator's maximum object size.
    process_group_list_t* groups = static_cast<process_group_list_t*>(pmm_alloc(sizeof(process_group_list_t) + count * sizeof(uint32_t)));

    if (groups != nullptr)
    {
        groups->references = 1;
        groups->count      = count;
        memcpy(groups->ids, ids, count * sizeof(uint32_t));
    }

    return groups;
}

void ProcessManager::ReleaseGroupList(process_group_list_t* groups)
{
    if (groups != nullptr && __atomic_sub_fetch(&groups->references, 1U, __ATOMIC_ACQ_REL) == 0)
    {
        pmm_free(groups);
    }
}

void ProcessManager::SnapshotUsage(const process_t* process, uint64_t usage[18], bool descendants)
{
    memset(usage, 0, 18 * sizeof(uint64_t));
    const uint64_t user   = __atomic_load_n(&process->userCpuNanoseconds, __ATOMIC_RELAXED) / 1000;
    const uint64_t kernel = __atomic_load_n(&process->kernelCpuNanoseconds, __ATOMIC_RELAXED) / 1000;
    usage[0]             = user / 1000000;
    usage[1]             = user % 1000000;
    usage[2]             = kernel / 1000000;
    usage[3]             = kernel % 1000000;
    usage[8]             = __atomic_load_n(&process->minorFaults, __ATOMIC_RELAXED);
    // No swap or block-device backend exists. Memory-file I/O is not block I/O.

    if (descendants)
    {
        for (size_t index = 0; index < 18; ++index)
        {
            usage[index] += __atomic_load_n(&process->childUsage[index], __ATOMIC_RELAXED);
        }

        usage[0] += usage[1] / 1000000;
        usage[1] %= 1000000;
        usage[2] += usage[3] / 1000000;
        usage[3] %= 1000000;
    }
}

void ProcessManager::ReapUsage(process_t* parent, const process_t* child)
{
    uint64_t usage[18];
    SnapshotUsage(child, usage);

    for (size_t index = 0; index < 18; ++index)
    {
        __atomic_fetch_add(&parent->childUsage[index], usage[index], __ATOMIC_RELAXED);
    }
}

uint64_t ProcessManager::ChangeFileLimit(process_t* process, const process_file_limit_t* proposed, process_file_limit_t* previous)
{
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    // Limits belong to the process, independently of its descriptor tables.
    // Cross-process callers retain this record even after exit releases tables.
    spinlock_acquire_preempt(&process->resourceLock);
    uint64_t result = 0;

    if (proposed != nullptr && proposed->current > proposed->maximum)
    {
        result = LINUX_EINVAL;
    }
    else if (proposed != nullptr && proposed->maximum > process->fileLimit.maximum)
    {
        result = static_cast<uint64_t>(-1); // CAP_SYS_RESOURCE is not implemented.
    }
    else
    {
        if (previous != nullptr)
        {
            *previous = process->fileLimit;
        }

        if (proposed != nullptr)
        {
            process->fileLimit.maximum = proposed->maximum;
            __atomic_store_n(&process->fileLimit.current, proposed->current, __ATOMIC_RELEASE);
        }
    }

    spinlock_release_preempt(&process->resourceLock);
    return result;
}

bool ProcessManager::BuildUserInitialStack(process_t* process, const process_user_stack_layout_t* layout, uint64_t* outUserRsp)
{
    if (process == nullptr || layout == nullptr || outUserRsp == nullptr)
    {
        return false;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return false;
    }

    if (!process->allocated || process->addressSpace == nullptr)
    {
        return false;
    }

    if (layout->stackSize == 0)
    {
        return false;
    }

    if (layout->argc > 0 && layout->argv == nullptr)
    {
        return false;
    }

    if (layout->envc > 0 && layout->envp == nullptr)
    {
        return false;
    }

    if (layout->auxvCount > 0 && layout->auxv == nullptr)
    {
        return false;
    }

    if (add_overflows_u64(layout->stackBase, layout->stackSize))
    {
        return false;
    }

    const uint64_t stackLimit        = layout->stackBase + layout->stackSize;
    uint64_t       stackCursor       = stackLimit;
    uint64_t       metadataWordCount = 0;
    uint64_t       metadataByteCount = 0;
    uint64_t       writeCursor       = 0;

    uint64_t* argvPointers = nullptr;
    uint64_t* envpPointers = nullptr;

    if (layout->argc > 0)
    {
        argvPointers = (uint64_t*) kmalloc(sizeof(uint64_t) * (size_t) layout->argc);

        if (argvPointers == nullptr)
        {
            return false;
        }

        memset(argvPointers, 0, sizeof(uint64_t) * (size_t) layout->argc);
    }

    if (layout->envc > 0)
    {
        envpPointers = (uint64_t*) kmalloc(sizeof(uint64_t) * (size_t) layout->envc);

        if (envpPointers == nullptr)
        {
            if (argvPointers != nullptr)
            {
                kfree(argvPointers);
            }

            return false;
        }

        memset(envpPointers, 0, sizeof(uint64_t) * (size_t) layout->envc);
    }

    for (uint64_t i = layout->argc; i > 0; --i)
    {
        const char* argumentString = layout->argv[i - 1];

        if (argumentString == nullptr)
        {
            goto fail;
        }

        const uint64_t argumentLength = (uint64_t) strlen(argumentString) + 1;

        if (stackCursor < layout->stackBase + argumentLength)
        {
            goto fail;
        }

        stackCursor -= argumentLength;

        if (!copy_to_process_virtual(process->addressSpace, stackCursor, argumentString, argumentLength))
        {
            goto fail;
        }

        argvPointers[i - 1] = stackCursor;
    }

    for (uint64_t i = layout->envc; i > 0; --i)
    {
        const char* environmentString = layout->envp[i - 1];

        if (environmentString == nullptr)
        {
            goto fail;
        }

        const uint64_t environmentLength = (uint64_t) strlen(environmentString) + 1;

        if (stackCursor < layout->stackBase + environmentLength)
        {
            goto fail;
        }

        stackCursor -= environmentLength;

        if (!copy_to_process_virtual(process->addressSpace, stackCursor, environmentString, environmentLength))
        {
            goto fail;
        }

        envpPointers[i - 1] = stackCursor;
    }

    stackCursor = align_down(stackCursor, 16);

    if (add_overflows_u64(layout->auxvCount, layout->argc) || add_overflows_u64(layout->auxvCount, layout->envc))
    {
        goto fail;
    }

    metadataWordCount = 1 + (layout->argc + 1) + (layout->envc + 1) + (layout->auxvCount * 2);
    metadataByteCount = metadataWordCount * sizeof(uint64_t);

    if (stackCursor < layout->stackBase + metadataByteCount)
    {
        goto fail;
    }

    stackCursor = align_down(stackCursor - metadataByteCount, 16);

    if (stackCursor < layout->stackBase)
    {
        goto fail;
    }

    writeCursor = stackCursor;

    if (!push_word_to_process_stack(process->addressSpace, &writeCursor, layout->argc))
    {
        goto fail;
    }

    for (uint64_t i = 0; i < layout->argc; ++i)
    {
        if (!push_word_to_process_stack(process->addressSpace, &writeCursor, argvPointers[i]))
        {
            goto fail;
        }
    }

    if (!push_word_to_process_stack(process->addressSpace, &writeCursor, 0))
    {
        goto fail;
    }

    for (uint64_t i = 0; i < layout->envc; ++i)
    {
        if (!push_word_to_process_stack(process->addressSpace, &writeCursor, envpPointers[i]))
        {
            goto fail;
        }
    }

    if (!push_word_to_process_stack(process->addressSpace, &writeCursor, 0))
    {
        goto fail;
    }

    for (uint64_t i = 0; i < layout->auxvCount; ++i)
    {
        if (!push_word_to_process_stack(process->addressSpace, &writeCursor, layout->auxv[i].type) || !push_word_to_process_stack(process->addressSpace, &writeCursor, layout->auxv[i].value))
        {
            goto fail;
        }
    }

    *outUserRsp = stackCursor;

    if (argvPointers != nullptr)
    {
        kfree(argvPointers);
    }

    if (envpPointers != nullptr)
    {
        kfree(envpPointers);
    }

    return true;

fail:

    if (argvPointers != nullptr)
    {
        kfree(argvPointers);
    }

    if (envpPointers != nullptr)
    {
        kfree(envpPointers);
    }

    return false;
}

bool ProcessManager::ActivateProcessAddressSpace(process_t* process)
{
    return process != nullptr && process->addressSpace != nullptr && SetRunningProcess(arch_cpu_id(), process);
}

task_t* ProcessManager::GetTasks(process_t* process) const
{
    if (process == nullptr)
    {
        return nullptr;
    }

    if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
    {
        return nullptr;
    }

    if (!process->allocated)
    {
        return nullptr;
    }

    return process->tasks;
}

process_t* ProcessManager::GetRunningProcess(uint8_t cpuId) const
{
    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return nullptr;
    }

    return __atomic_load_n(&RunningProcesses[cpuId], __ATOMIC_ACQUIRE);
}

process_t* ProcessManager::GetCurrentProcess() const
{
    return GetRunningProcess(arch_cpu_id());
}

bool ProcessManager::SetRunningProcess(uint8_t cpuId, process_t* process)
{
    bool               shouldSwitch       = false;
    virt_addr_space_t* targetAddressSpace = nullptr;

    if (cpuId >= BOOT_SMP_MAX_CPUS)
    {
        return false;
    }

    LockManager();

    if (process != nullptr)
    {
        if (process < &Processes[0] || process >= &Processes[MAX_PROCESSES])
        {
            UnlockManager();
            return false;
        }

        if (!process->allocated)
        {
            UnlockManager();
            return false;
        }

        if (process->reclaiming)
        {
            UnlockManager();
            return false;
        }

        process->ownerCpu = cpuId;
    }

    // A process may execute distinct tasks on several CPUs. TaskManager owns
    // the exclusive claim on each saved task context; the process manager owns
    // per-CPU address-space activation and teardown lifetime checks.

    __atomic_store_n(&RunningProcesses[cpuId], process, __ATOMIC_RELEASE);

    if (process != nullptr && process->addressSpace != nullptr && cpuId == arch_cpu_id())
    {
        shouldSwitch                       = true;
        targetAddressSpace                 = process->addressSpace;
        platform.cpus[cpuId].address_space = targetAddressSpace;
    }

    if (shouldSwitch)
    {
        vmm_switch_addr_space(targetAddressSpace);
    }

    UnlockManager();
    return true;
}

process_t* ProcessManager::GetProcesses()
{
    return Processes;
}

const process_t* ProcessManager::GetProcesses() const
{
    return Processes;
}

bool ProcessManager::IsProcessRunning(const process_t* process) const
{
    if (process == nullptr)
    {
        return false;
    }

    for (size_t i = 0; i < BOOT_SMP_MAX_CPUS; ++i)
    {
        if (__atomic_load_n(&RunningProcesses[i], __ATOMIC_ACQUIRE) == process)
        {
            return true;
        }
    }

    return false;
}

bool ProcessManager::TryReapExitedProcess(uint64_t processId)
{
    if (processId >= MAX_PROCESSES)
    {
        return false;
    }

    process_t* process = &Processes[processId];

    if (!process->allocated)
    {
        return false;
    }

    if (!process->exited)
    {
        return false;
    }

    if (IsProcessRunning(process))
    {
        return false;
    }

    return FreeProcess(process, true);
}

size_t ProcessManager::GetCapacity() const
{
    return MAX_PROCESSES;
}

process_t* ProcessManager::AcquireProcess(uint64_t id, bool requireRealTimer)
{
    LockManager();
    process_t* process = id < MAX_PROCESSES ? &Processes[id] : nullptr;

    if (process == nullptr || !process->allocated || process->reclaiming)
    {
        process = nullptr;
    }
    else
    {
        // Inactive processes need no timer work. Avoid transient references
        // from every CPU's timer scan interfering with ordinary teardown.
        bool needed = true;

        if (requireRealTimer)
        {
            spinlock_acquire(&process->timerLock);
            needed = process->realTimerArmed;
            spinlock_release(&process->timerLock);
        }

        if (needed)
        {
            ++process->lifetimeUsers;
        }
        else
        {
            process = nullptr;
        }
    }

    UnlockManager();
    return process;
}

void ProcessManager::ReleaseProcess(process_t* process)
{
    LockManager();

    if (process != nullptr && process->lifetimeUsers != 0)
    {
        --process->lifetimeUsers;
    }

    UnlockManager();
}

process_t* ProcessManager::AcquireCurrentMemoryProcess(bool requireExclusive)
{
    LockManager();
    process_t* process = GetCurrentProcess();

    if (process && process->allocated && !process->exited && process->addressSpace)
    {
        for (size_t i = 0; requireExclusive && i < MAX_PROCESSES; ++i)
        {
            if (&Processes[i] != process && Processes[i].allocated && Processes[i].addressSpace == process->addressSpace)
            {
                process = nullptr;
                break;
            }
        }

        if (process)
        {
            process->memoryUsers++;
        }
    }
    else
    {
        process = nullptr;
    }

    UnlockManager();
    return process;
}

void ProcessManager::ReleaseMemoryProcess(process_t* process)
{
    LockManager();

    if (process && process->memoryUsers)
    {
        process->memoryUsers--;
    }

    UnlockManager();
}

void ProcessManager::RetainMemoryProcess(process_t* process)
{
    LockManager();
    ++process->memoryUsers;
    UnlockManager();
}

uint64_t ProcessManager::GetCurrentProgramBreak() const
{
    LockManager();
    process_t*     process = GetCurrentProcess();
    const uint64_t result  = process != nullptr && process->allocated ? process->programBreak : 0;
    UnlockManager();
    return result;
}
