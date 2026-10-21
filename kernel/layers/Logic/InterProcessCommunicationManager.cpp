#include "layers/Logic/InterProcessCommunicationManager.hpp"
#include "layers/Logic/IpcState.hpp"

#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

#include <klib/klib.h>

namespace
{
constexpr size_t   PIPE_CAPACITY = 65536;
constexpr size_t   PIPE_ATOMIC   = 4096;
constexpr size_t   MEMORY_PAGES  = 256;
constexpr uint32_t DIRECT        = 040000;
constexpr uint32_t NONBLOCK      = 04000;
constexpr uint32_t CLOEXEC       = 02000000;
constexpr uint64_t AGAIN         = (uint64_t) -11;


void record_pipe_readiness_locked(pipe_state_t* pipe)
{
    const uint32_t masks[2] = {
        (pipe->size != 0 ? EVENT_POLL_IN | EVENT_POLL_RDNORM : 0U) | (pipe->writer == nullptr ? EVENT_POLL_HUP : 0U),
        (pipe->capacity - pipe->size >= PIPE_ATOMIC ? EVENT_POLL_OUT | EVENT_POLL_WRNORM : 0U) | (pipe->reader == nullptr ? EVENT_POLL_ERR : 0U),
    };
    for (size_t i = 0; i < 2; ++i)
    {
        pipe->pendingReadiness[i] |= masks[i] & ~pipe->lastReadiness[i];
        pipe->lastReadiness[i] = masks[i];
    }
}

uint8_t pipe_packet(const pipe_state_t* pipe, size_t index)
{
    return (pipe->packet[index / 4] >> ((index % 4) * 2)) & 3;
}

void set_pipe_packet(pipe_state_t* pipe, size_t index, uint8_t flags)
{
    const unsigned shift    = (index % 4) * 2;
    pipe->packet[index / 4] = (pipe->packet[index / 4] & ~(3U << shift)) | (flags << shift);
}


// A mapped store past a partial EOF belongs to the mapping's zero-filled tail,
// not to a later file extension. Clear holes before exposing new file bytes.
void memory_zero_range(memory_file_t* memory, uint64_t start, uint64_t end)
{
    while (start < end)
    {
        const uint64_t amount = end - start < PAGE_SIZE - start % PAGE_SIZE ? end - start : PAGE_SIZE - start % PAGE_SIZE;
        auto*          page   = memory->pages[start / PAGE_SIZE];

        if (page != nullptr)
        {
            memset(static_cast<uint8_t*>(page->pointer) + start % PAGE_SIZE, 0, amount);
        }

        start += amount;
    }
}

uint64_t vector_length(const file_iovec_t* vectors, size_t count, size_t* length)
{
    *length = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (vectors[i].length > INT64_MAX - *length)
        {
            return LINUX_EINVAL;
        }
        *length += vectors[i].length;
    }
    return 0;
}

bool copy_vector(const user_memory_access_t* access, const file_iovec_t* vectors, size_t count, size_t offset, uint8_t* bytes, size_t length, bool write ARX_DEBUG_ONLY(, uint64_t* copiedCounter))
{
    size_t copied = 0;
    for (size_t i = 0; i < count && copied < length; ++i)
    {
        if (offset >= vectors[i].length)
        {
            offset -= vectors[i].length;
            continue;
        }
        const size_t amount = vectors[i].length - offset < length - copied ? vectors[i].length - offset : length - copied;
        if (vectors[i].address > UINT64_MAX - offset || !(write ? safe_read_user_locked(access, vectors[i].address + offset, bytes + copied, amount) : safe_write_user_locked(access, vectors[i].address + offset, bytes + copied, amount)))
        {
            return false;
        }
        ARX_DEBUG_ADD(*copiedCounter, amount);
        copied += amount;
        offset = 0;
    }
    return copied == length;
}

} // namespace

const file_operations_t InterProcessCommunicationManager::PipeOperations = {
        nullptr, nullptr, nullptr, PipeSeek, nullptr, PipeIoctl, nullptr, PipeRelease, PipePoll, PipeRead, PipeWrite, PipeReadVector, PipeWriteVector, nullptr, PipeControl,
};
const file_operations_t InterProcessCommunicationManager::MemoryOperations = {
    nullptr, MemoryRead, MemoryWrite, MemorySeek, nullptr, nullptr, nullptr, MemoryRelease, nullptr, nullptr, nullptr, nullptr, nullptr, MemoryTruncate, MemoryControl, true, MemoryBacking,
};

InterProcessCommunicationManager::InterProcessCommunicationManager(ResourceLayerCaps* resources, VirtualFileSystem* vfs, EventManager* events, SignalManager* signals) : Resources(resources), Vfs(vfs), Events(events), Signals(signals)
{
}

uint64_t InterProcessCommunicationManager::CreatePipe(uint64_t output, uint32_t flags)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.createPipe);)

    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr || Events == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOSYS);
    }

    if ((flags & ~(NONBLOCK | CLOEXEC | DIRECT)) != 0)
    {
        return ARX_COMM_RETURN((flags & ~(NONBLOCK | CLOEXEC | DIRECT | 0200U)) != 0 ? LINUX_EINVAL : LINUX_EOPNOTSUPP);
    }

    pipe_state_t* pipe   = static_cast<pipe_state_t*>(kzalloc(sizeof(pipe_state_t)));
    file_t*       reader = static_cast<file_t*>(kzalloc(sizeof(file_t)));
    file_t*       writer = static_cast<file_t*>(kzalloc(sizeof(file_t)));

    if (pipe == nullptr || reader == nullptr || writer == nullptr)
    {
        kfree(pipe);
        kfree(reader);
        kfree(writer);
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    process_t* process = Resources->processManager->GetCurrentProcess();
    Vfs->InitializeAnonymousInode(&pipe->inode, INODE_FIFO, 0600);
    pipe->reader            = reader;
    pipe->writer            = writer;
    pipe->events            = Events;
    pipe->signals           = Signals;
    pipe->resources         = Resources;
    pipe->vfs               = Vfs;
    pipe->references        = 2;
    ARX_DEBUG_RUN(
        pipe->debugStats = &DebugStats;
        ARX_DEBUG_ADD(DebugStats.livePipes, 1);
        ARX_DEBUG_ADD(DebugStats.pipesCreated, 1);
    );
    pipe->lastReadiness[1]  = EVENT_POLL_OUT | EVENT_POLL_WRNORM;
    pipe->capacity          = PIPE_CAPACITY;
    reader->inode           = writer->inode = &pipe->inode;
    reader->operations      = writer->operations = &PipeOperations;
    reader->privateData     = writer->privateData = pipe;
    reader->refCount        = writer->refCount = 1;
    reader->statusFlags     = flags & (NONBLOCK | DIRECT);
    writer->statusFlags     = 1 | (flags & (NONBLOCK | DIRECT));
    int32_t  descriptors[2] = {-1, -1};
    uint64_t result         = 0;
    {
        FileDescriptorGuard guard(process);

        for (size_t i = 0; i < 2; ++i)
        {
            const int64_t fd = process != nullptr ? Resources->processManager->AddFileDescriptor(process, i == 0 ? reader : writer, flags & CLOEXEC ? FD_FLAG_CLOEXEC : 0, dispatcher_vfs_file_operations()) : (int64_t) LINUX_ESRCH;

            if (fd < 0)
            {
                result = (uint64_t) fd;
                break;
            }

            descriptors[i] = (int32_t) fd;
        }

        if (result == 0 && !safe_write_user(output, descriptors, sizeof(descriptors)))
        {
            result = LINUX_EFAULT;
        }

        if (result != 0)
        {
            for (size_t i = 0; i < 2; ++i)
            {
                if (descriptors[i] >= 0)
                {
                    *Resources->processManager->GetFileDescriptor(process, descriptors[i]) = {};
                }
            }
        }
    }

    if (result != 0)
    {
        Vfs->Close(reader);
        Vfs->Close(writer);
    }

    return ARX_COMM_RETURN(result);
}

void InterProcessCommunicationManager::NotifyPipe(void* state)
{
    pipe_state_t* pipe     = static_cast<pipe_state_t*>(state);
    EventManager* events   = pipe->events;
    file_t*       files[2] = {};
    spinlock_acquire_preempt(&pipe->lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
    files[0] = pipe->reader;
    files[1] = pipe->writer;
    record_pipe_readiness_locked(pipe);
    uint32_t masks[2]         = {pipe->pendingReadiness[0], pipe->pendingReadiness[1]};
    pipe->pendingReadiness[0] = pipe->pendingReadiness[1] = 0;

    for (size_t i = 0; i < 2; ++i)
    {
        if (files[i] != nullptr && (masks[i] == 0 || !pipe->vfs->Retain(files[i])))
        {
            files[i] = nullptr;
        }
    }

    record_pipe_readiness_locked(pipe);
    ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
    spinlock_release_preempt(&pipe->lock);

    for (size_t i = 0; i < 2; ++i)
    {
        if (files[i] != nullptr)
        {
            events->NotifySource(files[i], masks[i]);
            VirtualFileSystem::Close(files[i]);
        }
    }

    events->Notify();
}

void InterProcessCommunicationManager::PipeRelease(file_t* file)
{
    pipe_state_t* pipe = static_cast<pipe_state_t*>(file->privateData);
    spinlock_acquire_preempt(&pipe->lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
    if (pipe->reader == file)
    {
        pipe->reader = nullptr;
    }
    if (pipe->writer == file)
    {
        pipe->writer = nullptr;
    }
    record_pipe_readiness_locked(pipe);
    ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
    spinlock_release_preempt(&pipe->lock);
    NotifyPipe(pipe);
    spinlock_acquire_preempt(&pipe->lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
    const bool empty = --pipe->references == 0;
    record_pipe_readiness_locked(pipe);
    ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
    spinlock_release_preempt(&pipe->lock);
    if (empty)
    {
        ARX_DEBUG_ADD(pipe->debugStats->pipeReleaseDiscardedBytes, pipe->size);
        ARX_DEBUG_ADD(pipe->debugStats->livePipes, UINT64_MAX);
        kfree(pipe);
    }
}

uint32_t InterProcessCommunicationManager::PipePoll(file_t* file)
{
    pipe_state_t* pipe = static_cast<pipe_state_t*>(file->privateData);
    spinlock_acquire_preempt(&pipe->lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
    const uint32_t result
            = (file->statusFlags & 3) == 0 ? (pipe->size != 0 ? EVENT_POLL_IN | EVENT_POLL_RDNORM : 0) | (pipe->writer == nullptr ? EVENT_POLL_HUP : 0) : (pipe->capacity - pipe->size >= PIPE_ATOMIC ? EVENT_POLL_OUT | EVENT_POLL_WRNORM : 0) | (pipe->reader == nullptr ? EVENT_POLL_ERR : 0);
    record_pipe_readiness_locked(pipe);
    ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
    spinlock_release_preempt(&pipe->lock);
    return result;
}

int64_t InterProcessCommunicationManager::PipeSeek(file_t*, int64_t, int)
{
    return -29;
}

int64_t InterProcessCommunicationManager::PipeIoctl(file_t* file, uint64_t request, uint64_t argument)
{
    if (request != 0x541b)
    {
        return (int64_t) LINUX_ENOTTY;
    }
    user_memory_access_t access;
    if (!begin_user_memory(&access))
    {
        return (int64_t) LINUX_ESRCH;
    }
    pipe_state_t* pipe = static_cast<pipe_state_t*>(file->privateData);
    spinlock_acquire_preempt(&pipe->lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
    const int32_t  size   = (int32_t) pipe->size;
    const uint64_t result = safe_write_user_locked(&access, argument, &size, sizeof(size)) ? 0 : LINUX_EFAULT;
    record_pipe_readiness_locked(pipe);
    ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
    spinlock_release_preempt(&pipe->lock);
    end_user_memory(&access);
    return (int64_t) result;
}

uint64_t InterProcessCommunicationManager::PipeControl(file_t* file, uint32_t command, uint64_t argument)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<pipe_state_t*>(file->privateData)->debugStats;
        CommunicationOperation debugOperation(tracking->pipeControl);
    )

    if (command != 1031 && command != 1032)
    {
        return ARX_COMM_RETURN(LINUX_EINVAL);
    }

    pipe_state_t* pipe = static_cast<pipe_state_t*>(file->privateData);

    if (command == 1032)
    {
        spinlock_acquire_preempt(&pipe->lock);
        ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
        const size_t capacity = pipe->capacity;
        record_pipe_readiness_locked(pipe);
        ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
        spinlock_release_preempt(&pipe->lock);
        return ARX_COMM_RETURN(capacity);
    }

    if (argument > PIPE_CAPACITY)
    {
        return ARX_COMM_RETURN((uint64_t) -1);
    }

    size_t capacity = PIPE_ATOMIC;

    while (capacity < argument)
    {
        capacity *= 2;
    }

    uint8_t* staged = static_cast<uint8_t*>(kmalloc(PIPE_CAPACITY * 2));

    if (staged == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    spinlock_acquire_preempt(&pipe->lock);

    ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
    uint64_t result = (uint64_t) -16;

    if (pipe->size <= capacity)
    {
        for (size_t i = 0; i < pipe->size; ++i)
        {
            staged[i]                 = pipe->data[(pipe->head + i) % pipe->capacity];
            staged[PIPE_CAPACITY + i] = pipe_packet(pipe, (pipe->head + i) % pipe->capacity);
        }

        memcpy(pipe->data, staged, pipe->size);

        for (size_t i = 0; i < pipe->size; ++i)
        {
            set_pipe_packet(pipe, i, staged[PIPE_CAPACITY + i]);
        }

        pipe->head     = 0;
        pipe->capacity = capacity;
        result         = capacity;
    }

    record_pipe_readiness_locked(pipe);
    ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
    spinlock_release_preempt(&pipe->lock);
    kfree(staged);
    NotifyPipe(pipe);
    return ARX_COMM_RETURN(result);
}

uint64_t InterProcessCommunicationManager::PipeRead(file_t* file, uint64_t address, uint64_t count)
{
    const file_iovec_t vector = {address, count};
    return PipeTransfer(file, &vector, 1, false);
}
uint64_t InterProcessCommunicationManager::PipeWrite(file_t* file, uint64_t address, uint64_t count)
{
    const file_iovec_t vector = {address, count};
    return PipeTransfer(file, &vector, 1, true);
}
uint64_t InterProcessCommunicationManager::PipeReadVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    return PipeTransfer(file, vectors, count, false);
}
uint64_t InterProcessCommunicationManager::PipeWriteVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    return PipeTransfer(file, vectors, count, true);
}

uint64_t InterProcessCommunicationManager::PipeTransfer(file_t* file, const file_iovec_t* vectors, size_t count, bool write)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<pipe_state_t*>(file->privateData)->debugStats;
        CommunicationOperation debugOperation(write ? tracking->pipeWrite : tracking->pipeRead, true);
    )

    if (((__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & 3) == 0) == write)
    {
        return ARX_COMM_RETURN(LINUX_EBADF);
    }

    size_t   requested;
    uint64_t result = vector_length(vectors, count, &requested);
    ARX_DEBUG_RUN(debugOperation.Request(result == 0 ? requested : 0));

    if (result != 0 || requested == 0)
    {
        return ARX_COMM_RETURN(result);
    }

    pipe_state_t* pipe   = static_cast<pipe_state_t*>(file->privateData);
    uint8_t*      staged = static_cast<uint8_t*>(kmalloc(requested < PIPE_CAPACITY ? requested : PIPE_CAPACITY));

    if (staged == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    size_t transferred = 0;

    for (;;)
    {
        const uint64_t       sequence = pipe->events->Sequence();
        user_memory_access_t access;

        if (!begin_user_memory(&access))
        {
            result = LINUX_ESRCH;
            break;
        }

        spinlock_acquire_preempt(&pipe->lock);

        ARX_DEBUG_RUN(arx_debug_group_begin(&pipe->debugGroup));
        result                    = AGAIN;
        size_t bytes              = write ? pipe->capacity - pipe->size : pipe->size;
        bytes                     = bytes < requested - transferred ? bytes : requested - transferred;
        const bool   packetWrite  = write && (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & DIRECT);
        const size_t packetLength = requested - transferred < PIPE_ATOMIC ? requested - transferred : PIPE_ATOMIC;

        if (packetWrite)
        {
            bytes = pipe->capacity - pipe->size >= packetLength ? packetLength : 0;
        }

        size_t consumed = bytes;

        if (!write && bytes != 0)
        {
            const bool packetRead = (pipe_packet(pipe, pipe->head) & 1) != 0;

            if (packetRead)
            {
                consumed = 0;
                do
                {
                    ++consumed;
                } while (consumed < pipe->size && !(pipe_packet(pipe, (pipe->head + consumed - 1) % pipe->capacity) & 2));
                bytes = bytes < consumed ? bytes : consumed;
            }
            else
            {
                for (size_t i = 1; i < bytes; ++i)
                {
                    if (pipe_packet(pipe, (pipe->head + i) % pipe->capacity) & 1)
                    {
                        bytes = i;
                        break;
                    }
                }

                consumed = bytes;
            }
        }

        if (write && pipe->reader == nullptr)
        {
            result = (uint64_t) -32;
        }
        else if (!write && pipe->size == 0 && pipe->writer == nullptr)
        {
            result = 0;
        }
        else if (bytes != 0 && (!write || packetWrite || requested > PIPE_ATOMIC || bytes == requested))
        {
            if (!write)
            {
                for (size_t i = 0; i < bytes; ++i)
                {
                    staged[i] = pipe->data[(pipe->head + i) % pipe->capacity];
                }
            }

            if (!copy_vector(&access, vectors, count, transferred, staged, bytes, write ARX_DEBUG_ONLY(, write ? &pipe->debugStats->pipeWriteCopiedBytes : &pipe->debugStats->pipeCopiedBytes)))
            {
                result = LINUX_EFAULT;
            }
            else
            {
                if (write)
                {
                    for (size_t i = 0; i < bytes; ++i)
                    {
                        const size_t index = (pipe->head + pipe->size + i) % pipe->capacity;
                        pipe->data[index]  = staged[i];
                        set_pipe_packet(pipe, index, packetWrite ? 1 | (i + 1 == bytes ? 2 : 0) : 0);
                    }

                    pipe->size += bytes;
                    ARX_DEBUG_ADD(pipe->debugStats->pipeEnqueuedBytes, bytes);
                }
                else
                {
                    pipe->head = (pipe->head + consumed) % pipe->capacity;
                    pipe->size -= consumed;
                    ARX_DEBUG_ADD(pipe->debugStats->pipeConsumedBytes, consumed);
                    ARX_DEBUG_ADD(pipe->debugStats->pipeDiscardedBytes, consumed - bytes);
                }

                transferred += bytes;
                result = 0;
            }
        }

        record_pipe_readiness_locked(pipe);
        ARX_DEBUG_RUN(arx_debug_group_end(&pipe->debugGroup));
        spinlock_release_preempt(&pipe->lock);
        end_user_memory(&access);

        if (result == 0)
        {
            NotifyPipe(pipe);
        }

        if (result == (uint64_t) -32 && pipe->signals != nullptr)
        {
            process_t* process = pipe->resources->processManager->GetCurrentProcess();
            task_t*    task    = pipe->resources->taskManager->GetCurrentTask();

            if (process != nullptr && task != nullptr)
            {
                pipe->signals->Send((int32_t) process->id, (int32_t) task->id, 13);
            }
        }

        if (result != AGAIN && (result != 0 || !write || transferred == requested))
        {
            break;
        }

        if (result == AGAIN && (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
        {
            break;
        }

        if (result == AGAIN)
        {
            result = pipe->events->Wait(sequence);

            if (result != 0)
            {
                break;
            }
        }
    }

    kfree(staged);
    return ARX_COMM_RETURN(transferred != 0 || result == 0 ? transferred : result);
}

uint64_t InterProcessCommunicationManager::CreateMemoryFile(uint64_t pointer, uint32_t flags)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.createMemory);)

    if (Resources == nullptr || Resources->processManager == nullptr || Resources->virtualMemoryManager == nullptr || Vfs == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOSYS);
    }

    if ((flags & ~(1U | 2U)) != 0)
    {
        return ARX_COMM_RETURN((flags & 4U) ? LINUX_EOPNOTSUPP : LINUX_EINVAL);
    }

    char   name[250];
    size_t length = 0;

    if (!safe_read_user_string(pointer, name, sizeof(name), &length))
    {
        // Distinguish an overlong readable name from a fault.
        for (size_t i = 0; i < sizeof(name); ++i)
        {
            char byte;

            if (pointer > UINT64_MAX - i || !safe_read_user(pointer + i, &byte, 1))
            {
                return ARX_COMM_RETURN(LINUX_EFAULT);
            }

            if (byte == 0)
            {
                return ARX_COMM_RETURN(LINUX_EFAULT);
            }
        }

        return ARX_COMM_RETURN(LINUX_EINVAL);
    }

    memory_file_t* memory = static_cast<memory_file_t*>(kzalloc(sizeof(memory_file_t)));
    file_t*        file   = static_cast<file_t*>(kzalloc(sizeof(file_t)));

    if (memory == nullptr || file == nullptr)
    {
        kfree(memory);
        kfree(file);
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    Vfs->InitializeAnonymousInode(&memory->inode, INODE_REGULAR, 0777);
    memory->manager       = Resources->virtualMemoryManager;
    memory->inode.backing = memory->manager->CreateBacking(0, true);

    if (memory->inode.backing == nullptr)
    {
        kfree(memory);
        kfree(file);

        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    ARX_DEBUG_RUN(
        memory->debugStats = &DebugStats;
        ARX_DEBUG_ADD(DebugStats.liveMemoryFiles, 1);
        ARX_DEBUG_ADD(DebugStats.memoryFilesCreated, 1);
    );
    memory->inode.backing->volatile_storage = true;
    memory->seals                           = flags & 2 ? 0 : 1;
    file->inode                             = &memory->inode;
    file->operations                        = &MemoryOperations;
    file->privateData                       = memory;
    file->refCount                          = 1;
    file->statusFlags                       = 2;
    FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
    process_t*          process = Resources->processManager->GetCurrentProcess();
    const int64_t       fd      = process != nullptr ? Resources->processManager->AddFileDescriptor(process, file, flags & 1 ? FD_FLAG_CLOEXEC : 0, dispatcher_vfs_file_operations()) : (int64_t) LINUX_ESRCH;
    if (fd < 0)
    {
        Vfs->Close(file);
    }
    return ARX_COMM_RETURN((uint64_t) fd);
}

int64_t InterProcessCommunicationManager::MemoryRead(file_t* file, void* buffer, uint64_t count)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<memory_file_t*>(file->privateData);
        DebugUpdate debugUpdate(tracking->debugGroup);
        CommunicationOperation debugOperation(tracking->debugStats->memoryRead, true, count);
    )

    memory_file_t* memory = static_cast<memory_file_t*>(file->privateData);
    if (file->offset >= memory->inode.size)
    {
        return ARX_COMM_RETURN(0);
    }
    const size_t bytes = count < memory->inode.size - file->offset ? count : memory->inode.size - file->offset;
    for (size_t i = 0; i < bytes; ++i)
    {
        const size_t address             = file->offset + i;
        static_cast<uint8_t*>(buffer)[i] = memory->pages[address / PAGE_SIZE] != nullptr ? static_cast<uint8_t*>(memory->pages[address / PAGE_SIZE]->pointer)[address % PAGE_SIZE] : 0;
    }
    ARX_DEBUG_ADD(memory->debugStats->memoryReadBytes, bytes);
    file->offset += bytes;
    return ARX_COMM_RETURN(bytes);
}

int64_t InterProcessCommunicationManager::MemoryWrite(file_t* file, const void* buffer, uint64_t count)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<memory_file_t*>(file->privateData);
        DebugUpdate debugUpdate(tracking->debugGroup);
        CommunicationOperation debugOperation(tracking->debugStats->memoryWrite, true, count);
    )

    memory_file_t* memory = static_cast<memory_file_t*>(file->privateData);
    if (count == 0)
    {
        return ARX_COMM_RETURN(0);
    }
    if (memory->seals & (8U | 16U))
    {
        return ARX_COMM_RETURN(-1);
    }
    if (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & 02000)
    {
        file->offset = memory->inode.size;
    }
    if (file->offset > MEMORY_PAGES * PAGE_SIZE || count > MEMORY_PAGES * PAGE_SIZE - file->offset)
    {
        return ARX_COMM_RETURN(-27);
    }
    if ((memory->seals & 4) && file->offset + count > memory->inode.size)
    {
        return ARX_COMM_RETURN(-1);
    }

    if (file->offset > memory->inode.size)
    {
        memory_zero_range(memory, memory->inode.size, file->offset);
    }

    size_t copied = 0;

    while (copied < count)
    {
        const size_t address = file->offset + copied;
        const size_t page    = address / PAGE_SIZE;

        if (memory->pages[page] == nullptr)
        {
            memory->pages[page] = memory->manager->GetBackingPage(memory->inode.backing, page * PAGE_SIZE);

            if (memory->pages[page] == nullptr)
            {
                break;
            }
        }

        const size_t bytes = count - copied < PAGE_SIZE - address % PAGE_SIZE ? count - copied : PAGE_SIZE - address % PAGE_SIZE;
        memcpy(static_cast<uint8_t*>(memory->pages[page]->pointer) + address % PAGE_SIZE, static_cast<const uint8_t*>(buffer) + copied, bytes);
        copied += bytes;
    }

    ARX_DEBUG_ADD(memory->debugStats->memoryWriteBytes, copied);
    file->offset += copied;

    if (file->offset > memory->inode.size)
    {
        memory->inode.size = file->offset;
        __atomic_store_n(&memory->inode.backing->size, file->offset, __ATOMIC_RELEASE);
    }

    return ARX_COMM_RETURN(copied != 0 ? (int64_t) copied : (int64_t) LINUX_ENOMEM);
}

int64_t InterProcessCommunicationManager::MemorySeek(file_t* file, int64_t offset, int whence)
{
    const int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t) file->offset : whence == 2 ? (int64_t) file->inode->size : -1;
    if (whence < 0 || whence > 2 || base < 0 || offset < -base || offset > INT64_MAX - base)
    {
        return (int64_t) LINUX_EINVAL;
    }
    file->offset = (uint64_t) (base + offset);
    return (int64_t) file->offset;
}

int64_t InterProcessCommunicationManager::MemoryTruncate(file_t* file, uint64_t size)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<memory_file_t*>(file->privateData);
        DebugUpdate debugUpdate(tracking->debugGroup);
        CommunicationOperation debugOperation(tracking->debugStats->memoryTruncate);
    )

    memory_file_t* memory = static_cast<memory_file_t*>(file->privateData);
    if (size > MEMORY_PAGES * PAGE_SIZE)
    {
        return ARX_COMM_RETURN(-27);
    }
    if ((size < memory->inode.size && (memory->seals & 2)) || (size > memory->inode.size && (memory->seals & 4)))
    {
        return ARX_COMM_RETURN(-1);
    }

    const int64_t status = memory->manager->ResizeVolatileBacking(memory->inode.backing, size);

    if (status != 0)
    {
        return ARX_COMM_RETURN(status);
    }
    if (size < memory->inode.size)
    {
        for (size_t page = (size + PAGE_SIZE - 1) / PAGE_SIZE; page < MEMORY_PAGES; ++page)
        {
            memory->pages[page] = nullptr;
        }
    }
    memory->inode.size = size;
    return ARX_COMM_RETURN(0);
}

uint64_t InterProcessCommunicationManager::MemoryControl(file_t* file, uint32_t command, uint64_t argument)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<memory_file_t*>(file->privateData);
        DebugUpdate debugUpdate(tracking->debugGroup);
        CommunicationOperation debugOperation(tracking->debugStats->memoryControl);
    )

    memory_file_t* memory = static_cast<memory_file_t*>(file->privateData);
    spinlock_acquire_preempt(&memory->inode.ioLock);
    uint64_t result = LINUX_EINVAL;
    if (command == 1034)
    {
        result = memory->seals;
    }
    if (command == 1033)
    {
        result = (argument & ~31ULL) != 0 ? LINUX_EINVAL : (memory->seals & 1) ? (uint64_t) -1 : 0;

        if (result == 0 && (argument & 8) != 0 && __atomic_load_n(&memory->inode.backing->writable_mappings, __ATOMIC_ACQUIRE) != 0)
        {
            result = static_cast<uint64_t>(-16);
        }
        if (result == 0)
        {
            memory->seals |= argument;
        }
    }
    spinlock_release_preempt(&memory->inode.ioLock);
    return ARX_COMM_RETURN(result);
}

void InterProcessCommunicationManager::MemoryRelease(file_t* file)
{
    memory_file_t* memory = static_cast<memory_file_t*>(file->privateData);
    ARX_DEBUG_ADD(memory->debugStats->liveMemoryFiles, UINT64_MAX);
    VirtualMemoryManager::ReleaseBacking(memory->inode.backing);
    kfree(memory);
}

int64_t InterProcessCommunicationManager::MemoryBacking(file_t* file, VirtualMemoryManager*, uint32_t protection, bool shared, virt_backing_object_t** object, uint32_t* maximum)
{
    ARX_DEBUG_ONLY(
        auto* tracking = static_cast<memory_file_t*>(file->privateData);
        DebugUpdate debugUpdate(tracking->debugGroup);
        CommunicationOperation debugOperation(tracking->debugStats->memoryBacking);
    )

    auto* memory = static_cast<memory_file_t*>(file->privateData);

    if (shared && (memory->seals & (8U | 16U)) != 0)
    {
        if ((protection & 2) != 0)
        {
            return ARX_COMM_RETURN(-1);
        }

        *maximum &= ~2U;
    }

    // Populate holes in the same page array used by read/write. Mapping never
    // creates detached copies of memfd storage.
    for (uint64_t offset = 0; offset < memory->inode.size; offset += PAGE_SIZE)
    {
        const size_t index = offset / PAGE_SIZE;

        if (memory->pages[index] == nullptr)
        {
            memory->pages[index] = memory->manager->GetBackingPage(memory->inode.backing, offset);
        }

        if (memory->pages[index] == nullptr)
        {
            return ARX_COMM_RETURN(-12);
        }
    }

    *object = memory->inode.backing;

    return ARX_COMM_RETURN(0);
}
