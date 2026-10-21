#include "layers/Logic/SocketManager.hpp"
#include "layers/Logic/SocketState.hpp"

#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/SignalManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <klib/klib.h>


namespace
{
constexpr uint32_t NONBLOCK         = 04000;
constexpr uint32_t CLOEXEC          = 02000000;
constexpr size_t   MAX_ENDPOINTS    = 128;
constexpr size_t   BUFFER_LIMIT     = 65536;
constexpr size_t   MAX_VECTORS      = 1024;
constexpr uint32_t MSG_PEEK         = 2;
constexpr uint32_t MSG_CTRUNC       = 8;
constexpr uint32_t MSG_CMSG_CLOEXEC = 0x40000000;
constexpr uint32_t MSG_TRUNC        = 0x20;
constexpr uint32_t MSG_DONTWAIT     = 0x40;
constexpr uint32_t MSG_WAITALL      = 0x100;
constexpr uint32_t MSG_NOSIGNAL     = 0x4000;
constexpr int32_t  SOL_SOCKET       = 1;
constexpr int32_t  SCM_RIGHTS       = 1;
constexpr int32_t  SCM_CREDENTIALS  = 2;
constexpr uint64_t EAGAIN           = (uint64_t) -11;
constexpr uint64_t ENOTSOCK         = (uint64_t) -88;
constexpr uint64_t EPIPE            = (uint64_t) -32;
constexpr uint64_t ENOTCONN         = (uint64_t) -107;
constexpr uint64_t ECONNREFUSED     = (uint64_t) -111;


struct socket_delivery_t
{
    uint8_t bytes[1072];
    int32_t descriptors[253];
    file_t* rollback[253];
    size_t  installed;
};

uint64_t validate_socket(int32_t domain, int32_t type, int32_t protocol)
{
    if (domain != 1)
    {
        return (uint64_t) -97; // EAFNOSUPPORT: no network transport is installed.
    }

    if ((type & ~(NONBLOCK | CLOEXEC | 15)) != 0)
    {
        return LINUX_EINVAL;
    }

    if ((type & 15) != 1 && (type & 15) != 2 && (type & 15) != 5)
    {
        return (uint64_t) -94; // ESOCKTNOSUPPORT
    }

    return protocol == 0 ? 0 : (uint64_t) -93; // EPROTONOSUPPORT
}

uint64_t read_address(uint64_t pointer, uint32_t& length, socket_address_t* address, bool autobind = false)
{
    if (length < 2 || length > sizeof(*address))
    {
        return LINUX_EINVAL;
    }

    *address = {};

    if (!safe_read_user(pointer, address, length))
    {
        return LINUX_EFAULT;
    }

    if (address->family != 1)
    {
        return LINUX_EINVAL;
    }

    if (length <= 2)
    {
        return autobind ? 0 : LINUX_EOPNOTSUPP;
    }
    if (address->path[0] != 0)
    {
        size_t bytes = 0;
        while (bytes < length - 2 && address->path[bytes] != 0)
        {
            ++bytes;
        }
        if (bytes == sizeof(address->path))
        {
            return LINUX_ENAMETOOLONG;
        }
        address->path[bytes] = 0;
        length               = (uint32_t) bytes + 3;
    }
    return 0;
}

bool write_address(const user_memory_access_t* access, uint64_t pointer, uint64_t lengthPointer, const socket_address_t& address, uint32_t length)
{
    uint32_t capacity;

    if (!safe_read_user_locked(access, lengthPointer, &capacity, sizeof(capacity)))
    {
        return false;
    }

    const size_t copied = capacity < length ? capacity : length;
    return (copied == 0 || safe_write_user_locked(access, pointer, &address, copied)) && safe_write_user_locked(access, lengthPointer, &length, sizeof(length));
}

void free_control(socket_ancillary_t* control)
{
    while (control != nullptr)
    {
        socket_ancillary_t* next = control->next;
        for (size_t i = 0; i < control->count; ++i)
        {
            VirtualFileSystem::Close(control->files[i]);
        }
        kfree(control);
        control = next;
    }
}

void free_packets(socket_packet_t* packet)
{
    while (packet != nullptr)
    {
        socket_packet_t* next = packet->next;
        free_control(packet->control);
        kfree(packet);
        packet = next;
    }
}

uint64_t vector_size(const file_iovec_t* vectors, size_t count, size_t* size)
{
    *size = 0;

    for (size_t i = 0; i < count; ++i)
    {
        if (vectors[i].length > INT64_MAX - *size)
        {
            return LINUX_EINVAL;
        }

        *size += vectors[i].length;
    }

    return 0;
}

bool copy_vectors(const user_memory_access_t* access, const file_iovec_t* vectors, size_t count, size_t offset, void* data, size_t length, bool send ARX_DEBUG_ONLY(, uint64_t* copiedCounter))
{
    size_t copied = 0;

    for (size_t i = 0; i < count && copied < length; ++i)
    {
        if (offset >= vectors[i].length)
        {
            offset -= vectors[i].length;
            continue;
        }

        const size_t available = vectors[i].length - offset;
        const size_t bytes     = available < length - copied ? available : length - copied;

        if (vectors[i].address > UINT64_MAX - offset)
        {
            return false;
        }

        const uint64_t address = vectors[i].address + offset;
        bool           valid   = send ? safe_read_user_locked(access, address, static_cast<uint8_t*>(data) + copied, bytes) : safe_write_user_locked(access, address, static_cast<uint8_t*>(data) + copied, bytes);

        if (!valid)
        {
            return false;
        }

        ARX_DEBUG_ADD(*copiedCounter, bytes);
        copied += bytes;
        offset = 0;
    }

    return copied == length;
}
} // namespace


const file_operations_t SocketManager::Operations = {
        nullptr, nullptr, nullptr, Seek, nullptr, Ioctl, nullptr, Release, Poll, Read, Write, ReadVector, WriteVector, nullptr, nullptr, false, nullptr, DropReference,
};

SocketManager::SocketManager(ResourceLayerCaps* resources, VirtualFileSystem* vfs, EventManager* events, SignalManager* signals) : Resources(resources), Vfs(vfs), Events(events), Signals(signals), Lock{}, Endpoints(nullptr), EndpointCount(0)
{
}

file_t* SocketManager::RetainDescriptor(int32_t fd, uint64_t* error)
{
    *error = LINUX_ENOSYS;

    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr || Events == nullptr)
    {
        return nullptr;
    }

    *error = LINUX_EBADF;
    FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
    file_descriptor_t*  descriptor = fd >= 0 ? Resources->processManager->GetFileDescriptor(Resources->processManager->GetCurrentProcess(), (uint32_t) fd) : nullptr;

    if (descriptor == nullptr)
    {
        return nullptr;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    *error       = ENOTSOCK;

    if (file->operations != &Operations)
    {
        return nullptr;
    }

    if (!Vfs->Retain(file))
    {
        *error = LINUX_EBADF;
        return nullptr;
    }

    *error = 0;
    return file;
}

file_t* SocketManager::CreateFile(uint32_t type, uint32_t flags)
{
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(kzalloc(sizeof(socket_endpoint_t)));
    file_t*            file     = static_cast<file_t*>(kzalloc(sizeof(file_t)));

    if (endpoint == nullptr || file == nullptr)
    {
        kfree(endpoint);
        kfree(file);
        return nullptr;
    }

    Vfs->InitializeAnonymousInode(&endpoint->inode, INODE_SOCKET, 0777);
    endpoint->manager            = this;
    endpoint->file               = file;
    endpoint->type               = type;
    endpoint->address.family     = endpoint->peerAddress.family = 1;
    endpoint->addressLength      = endpoint->peerAddressLength = 2;
    endpoint->sendLimit          = endpoint->receiveLimit = BUFFER_LIMIT;
    endpoint->peerCredentials[1] = endpoint->peerCredentials[2] = -1;
    process_t* process           = Resources->processManager->GetCurrentProcess();

    if (process != nullptr)
    {
        spinlock_acquire_preempt(&process->credentialLock);
        endpoint->credentials[0] = (int32_t) process->id;
        endpoint->credentials[1] = process->credentials.user.effective;
        endpoint->credentials[2] = process->credentials.group.effective;
        spinlock_release_preempt(&process->credentialLock);
    }
    file->inode             = &endpoint->inode;
    file->operations        = &Operations;
    file->privateData       = endpoint;
    file->statusFlags       = 2 | (flags & NONBLOCK);
    file->refCount          = 1;
    endpoint->lastReadiness = ReadinessLocked(endpoint);
    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));

    if (EndpointCount >= MAX_ENDPOINTS)
    {
        RecordReadinessLocked();
        ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
        spinlock_release_preempt(&Lock);
        kfree(endpoint);
        kfree(file);
        return nullptr;
    }

    endpoint->next = Endpoints;
    Endpoints      = endpoint;
    ++EndpointCount;
    ARX_DEBUG_ADD(DebugStats.endpointsCreated, 1);
    RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);
    return file;
}

uint64_t SocketManager::Install(file_t* file, uint32_t flags)
{
    FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
    process_t*          process = Resources->processManager->GetCurrentProcess();
    return process != nullptr ? (uint64_t) Resources->processManager->AddFileDescriptor(process, file, flags & CLOEXEC ? FD_FLAG_CLOEXEC : 0, dispatcher_vfs_file_operations()) : LINUX_ESRCH;
}

void SocketManager::RecordReadinessLocked()
{
    for (auto* endpoint = Endpoints; endpoint != nullptr; endpoint = endpoint->next)
    {
        const uint32_t ready = ReadinessLocked(endpoint);
        endpoint->pendingReadiness |= ready & ~endpoint->lastReadiness;
        endpoint->lastReadiness = ready;
    }
}

void SocketManager::Notify()
{
    file_t*  files[MAX_ENDPOINTS];
    uint32_t masks[MAX_ENDPOINTS];
    size_t   count = 0;
    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));

    for (socket_endpoint_t* endpoint = Endpoints; endpoint != nullptr; endpoint = endpoint->next)
    {
        const uint32_t edges       = endpoint->pendingReadiness;
        endpoint->pendingReadiness = 0;

        if (edges != 0 && Vfs->Retain(endpoint->file))
        {
            masks[count]   = edges;
            files[count++] = endpoint->file;
        }
    }

    RecordReadinessLocked();

    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);

    for (size_t i = 0; i < count; ++i)
    {
        Events->NotifySource(files[i], masks[i]);
        Vfs->Close(files[i]);
    }

    Events->Notify();
}

uint64_t SocketManager::ResolveAddress(const socket_address_t& address, inode_t** node)
{
    *node = nullptr;
    return address.path[0] != 0 ? (uint64_t) Vfs->SocketNode(reinterpret_cast<const char*>(address.path), false, node) : 0;
}

uint64_t SocketManager::AutobindLocked(socket_endpoint_t* endpoint)
{
    if (endpoint->binding)
    {
        return LINUX_EINVAL;
    }

    static const char digits[] = "0123456789abcdef";

    for (size_t attempt = 0; attempt <= MAX_ENDPOINTS; ++attempt)
    {
        socket_address_t   address = {};
        address.family             = 1;
        uint32_t value             = ++AutobindSequence;

        for (size_t i = 0; i < 5; ++i)
        {
            address.path[5 - i] = digits[value & 15];
            value >>= 4;
        }

        if (FindAddress(address, 8) == nullptr)
        {
            endpoint->address       = address;
            endpoint->addressLength = 8;
            endpoint->bound         = true;
            return 0;
        }
    }

    return (uint64_t) -98;
}

socket_endpoint_t* SocketManager::FindAddress(const socket_address_t& address, uint32_t length, inode_t* node)
{
    for (socket_endpoint_t* endpoint = Endpoints; endpoint != nullptr; endpoint = endpoint->next)
    {
        if (endpoint->bound && (address.path[0] != 0 ? endpoint->node == node : endpoint->node == nullptr && endpoint->addressLength == length && memcmp(&endpoint->address, &address, length) == 0))
        {
            return endpoint;
        }
    }

    return nullptr;
}

uint64_t SocketManager::ParseControl(const socket_message_t* header, socket_ancillary_t** output)
{
    *output                     = nullptr;
    socket_ancillary_t* control = static_cast<socket_ancillary_t*>(kzalloc(sizeof(socket_ancillary_t)));
    if (control == nullptr)
    {
        return LINUX_ENOMEM;
    }

    process_t* process = Resources->processManager->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        kfree(control);
        return LINUX_ESRCH;
    }
    const process_credentials_t identities = process->credentials;
    control->credentials                   = {(int32_t) process->id, identities.user.real, identities.group.real};
    control->senderUid                     = identities.user.effective;
    Resources->processManager->ReleaseCredentials(process);

    uint64_t       result = 0;
    bool           rights = false;
    const uint64_t length = header != nullptr ? header->controlLength : 0;

    if (length > BUFFER_LIMIT || (length != 0 && length < sizeof(socket_control_t)))
    {
        result = LINUX_EINVAL;
    }

    for (uint64_t offset = 0; result == 0 && offset < length;)
    {
        socket_control_t item = {};

        if (length - offset < sizeof(item))
        {
            break; // An incomplete final alignment pad is not another header.
        }

        if (header->control > UINT64_MAX - offset || !safe_read_user(header->control + offset, &item, sizeof(item)))
        {
            result = LINUX_EFAULT;
            break;
        }

        if (item.length < sizeof(item) || item.length > length - offset)
        {
            result = LINUX_EINVAL;
            break;
        }

        const uint64_t payload = item.length - sizeof(item);

        if (item.level != SOL_SOCKET || (item.type != SCM_RIGHTS && item.type != SCM_CREDENTIALS))
        {
            result = LINUX_EOPNOTSUPP;
            break;
        }

        if (header->control + offset > UINT64_MAX - item.length)
        {
            result = LINUX_EFAULT;
            break;
        }

        const uint64_t data = header->control + offset + sizeof(item);

        if (item.type == SCM_RIGHTS)
        {
            if (rights || payload % sizeof(int32_t) != 0 || payload / sizeof(int32_t) > 253)
            {
                result = LINUX_EINVAL;
                break;
            }

            rights = true;
            int32_t descriptors[253];

            if (payload != 0 && !safe_read_user(data, descriptors, payload))
            {
                result = LINUX_EFAULT;
                break;
            }

            {
                FileDescriptorGuard guard(process);

                for (size_t i = 0; i < payload / sizeof(int32_t); ++i)
                {
                    file_descriptor_t* descriptor = descriptors[i] >= 0 ? Resources->processManager->GetFileDescriptor(process, descriptors[i]) : nullptr;

                    if (descriptor == nullptr)
                    {
                        result = LINUX_EBADF;
                        break;
                    }

                    if (descriptor->operations != dispatcher_vfs_file_operations())
                    {
                        result = LINUX_EOPNOTSUPP;
                        break;
                    }

                    file_t* retained = static_cast<file_t*>(descriptor->file);

                    if (retained->operations == &Operations && static_cast<socket_endpoint_t*>(retained->privateData)->manager != this)
                    {
                        result = LINUX_EOPNOTSUPP;
                        break;
                    }

                    if (!Vfs->Retain(retained))
                    {
                        result = LINUX_EBADF;
                        break;
                    }

                    control->files[control->count++] = retained;
                }
            }
        }
        else
        {
            socket_credentials_t supplied = {};

            if (control->explicitCredentials || payload != sizeof(supplied))
            {
                result = LINUX_EINVAL;
                break;
            }

            if (!safe_read_user(data, &supplied, sizeof(supplied)))
            {
                result = LINUX_EFAULT;
                break;
            }

            if (supplied.pid != control->credentials.pid || (supplied.uid != identities.user.real && supplied.uid != identities.user.effective && supplied.uid != identities.user.saved)
                || (supplied.gid != identities.group.real && supplied.gid != identities.group.effective && supplied.gid != identities.group.saved))
            {
                result = (uint64_t) -1;
                break;
            }

            control->credentials         = supplied;
            control->explicitCredentials = true;
        }

        const uint64_t advance = (item.length + 7) & ~7ULL;

        if (advance > length - offset)
        {
            break;
        }

        offset += advance;
    }

    if (result != 0)
    {
        free_control(control);
    }
    else
    {
        *output = control;
    }
    return result;
}

bool SocketManager::MayQueueRightsLocked(const socket_ancillary_t* control)
{
    if (control->count == 0)
    {
        return true;
    }
    size_t         count = control->count;
    const uint64_t limit = Resources->processManager->GetCurrentProcess()->DescriptorLimit();
    for (auto* endpoint = Endpoints; endpoint != nullptr; endpoint = endpoint->next)
    {
        for (auto* packet = endpoint->head; packet != nullptr; packet = packet->next)
        {
            if (packet->control != nullptr && packet->control->senderUid == control->senderUid)
            {
                count += packet->control->count;
            }
        }
    }
    return count <= limit;
}

void SocketManager::ChangeControlLocked(socket_ancillary_t* control, bool add)
{
    ARX_DEBUG_RUN(
        if (control != nullptr)
        {
            if (add)
            {
                ARX_DEBUG_ADD(DebugStats.queuedRights, control->count);
            }
            else
            {
                ARX_DEBUG_ADD(DebugStats.unqueuedRights, control->count);
            }
        }
    );

    for (size_t i = 0; control != nullptr && i < control->count; ++i)
    {
        file_t* file = control->files[i];
        if (file->operations == &Operations)
        {
            auto* target = static_cast<socket_endpoint_t*>(file->privateData);
            if (add)
            {
                ++target->internalReferences;
            }
            else
            {
                --target->internalReferences;
            }
        }
    }
}

bool SocketManager::DropReference(file_t* file, bool* last)
{
    auto*              manager   = static_cast<socket_endpoint_t*>(file->privateData)->manager;
    socket_packet_t*   discarded = nullptr;
    socket_endpoint_t* abandoned = nullptr;
    spinlock_acquire_preempt(&manager->Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&manager->DebugStats.group));
    spinlock_acquire_preempt(&file->refLock);
    if (file->refCount == 0)
    {
        spinlock_release_preempt(&file->refLock);
        manager->RecordReadinessLocked();
        ARX_DEBUG_RUN(arx_debug_group_end(&manager->DebugStats.group));
        spinlock_release_preempt(&manager->Lock);
        return false;
    }
    *last             = --file->refCount == 0;
    const bool rooted = file->refCount > static_cast<socket_endpoint_t*>(file->privateData)->internalReferences;
    spinlock_release_preempt(&file->refLock);
    if (rooted)
    {
        manager->RecordReadinessLocked();
        ARX_DEBUG_RUN(arx_debug_group_end(&manager->DebugStats.group));
        spinlock_release_preempt(&manager->Lock);
        return true;
    }

    // Queue edges and listener-owned accept references are the only internal
    // owners. Count them, then mark every node reachable from external owners.
    // Reference drops serialize with graph changes, so the final external put
    // always observes and reclaims an otherwise unreachable socket cycle.
    socket_endpoint_t* nodes[MAX_ENDPOINTS];
    size_t             incoming[MAX_ENDPOINTS]  = {};
    bool               reachable[MAX_ENDPOINTS] = {};
    size_t             count                    = 0;

    for (auto* node = manager->Endpoints; node != nullptr; node = node->next)
    {
        node->collectionIndex = count;
        nodes[count++]        = node;
    }

    for (size_t i = 0; i < count; ++i)
    {
        for (auto* pending = nodes[i]->pendingHead; pending != nullptr; pending = pending->pendingNext)
        {
            ++incoming[pending->collectionIndex];
        }

        for (auto* packet = nodes[i]->head; packet != nullptr; packet = packet->next)
        {
            for (size_t k = 0; packet->control != nullptr && k < packet->control->count; ++k)
            {
                file_t* retained = packet->control->files[k];

                if (retained->operations == &Operations)
                {
                    auto* target = static_cast<socket_endpoint_t*>(retained->privateData);

                    if (target->manager == manager)
                    {
                        ++incoming[target->collectionIndex];
                    }
                }
            }
        }
    }

    size_t work[MAX_ENDPOINTS];
    size_t queued = 0;

    for (size_t i = 0; i < count; ++i)
    {
        spinlock_acquire_preempt(&nodes[i]->file->refLock);
        reachable[i] = nodes[i]->file->refCount > incoming[i];
        spinlock_release_preempt(&nodes[i]->file->refLock);

        if (reachable[i])
        {
            work[queued++] = i;
        }
    }

    for (size_t cursor = 0; cursor < queued; ++cursor)
    {
        auto* node = nodes[work[cursor]];

        for (auto* pending = node->pendingHead; pending != nullptr; pending = pending->pendingNext)
        {
            const size_t index = pending->collectionIndex;

            if (!reachable[index])
            {
                reachable[index] = true;
                work[queued++]   = index;
            }
        }

        for (auto* packet = node->head; packet != nullptr; packet = packet->next)
        {
            for (size_t k = 0; packet->control != nullptr && k < packet->control->count; ++k)
            {
                file_t* retained = packet->control->files[k];

                if (retained->operations != &Operations)
                {
                    continue;
                }

                auto*        target = static_cast<socket_endpoint_t*>(retained->privateData);
                const size_t index  = target->collectionIndex;

                if (target->manager == manager && !reachable[index])
                {
                    reachable[index] = true;
                    work[queued++]   = index;
                }
            }
        }
    }

    for (size_t i = 0; i < count; ++i)
    {
        if (reachable[i])
        {
            continue;
        }

        auto* node = nodes[i];

        for (auto* packet = node->head; packet != nullptr; packet = packet->next)
        {
            ARX_DEBUG_ADD(manager->DebugStats.discardedRights, packet->control != nullptr ? packet->control->count : 0);
            manager->ChangeControlLocked(packet->control, false);
        }

        if (node->tail != nullptr)
        {
            node->tail->next = discarded;
            discarded        = node->head;
        }

        ARX_DEBUG_ADD(manager->DebugStats.discardedBytes, node->queued);
        ARX_DEBUG_ADD(manager->DebugStats.discardedMessages, node->packetCount);
        node->head   = node->tail = nullptr;
        node->queued = node->packetCount = 0;

        while (node->pendingHead != nullptr)
        {
            auto* pending     = node->pendingHead;
            node->pendingHead = pending->pendingNext;
            --pending->internalReferences;
            pending->pendingNext = abandoned;
            abandoned            = pending;
        }

        node->pendingTail  = nullptr;
        node->pendingCount = 0;
    }

    manager->RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&manager->DebugStats.group));
    spinlock_release_preempt(&manager->Lock);
    free_packets(discarded);

    while (abandoned != nullptr)
    {
        auto* pending = abandoned;
        abandoned     = pending->pendingNext;
        VirtualFileSystem::Close(pending->file);
    }

    return true;
}

void SocketManager::Release(file_t* file)
{
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);
    SocketManager*     manager  = endpoint->manager;
    spinlock_acquire_preempt(&manager->Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&manager->DebugStats.group));
    socket_endpoint_t** link = &manager->Endpoints;

    while (*link != endpoint)
    {
        link = &(*link)->next;
    }

    ARX_DEBUG_ADD(manager->DebugStats.discardedBytes, endpoint->queued);
    ARX_DEBUG_ADD(manager->DebugStats.discardedMessages, endpoint->packetCount);
    *link = endpoint->next;
    --manager->EndpointCount;
    ARX_DEBUG_ADD(manager->DebugStats.endpointsReleased, 1);

    for (socket_endpoint_t* other = manager->Endpoints; other != nullptr; other = other->next)
    {
        if (other->peer == endpoint)
        {
            other->peer = nullptr;
        }
    }

    socket_endpoint_t* pending = endpoint->pendingHead;
    for (auto* incoming = pending; incoming != nullptr; incoming = incoming->pendingNext)
    {
        --incoming->internalReferences;
    }
    for (auto* packet = endpoint->head; packet != nullptr; packet = packet->next)
    {
        ARX_DEBUG_ADD(manager->DebugStats.discardedRights, packet->control != nullptr ? packet->control->count : 0);
        manager->ChangeControlLocked(packet->control, false);
    }
    manager->RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&manager->DebugStats.group));
    spinlock_release_preempt(&manager->Lock);

    while (pending != nullptr)
    {
        socket_endpoint_t* next = pending->pendingNext;
        manager->Vfs->Close(pending->file);
        pending = next;
    }

    free_packets(endpoint->head);
    manager->Vfs->ReleaseSocketNode(endpoint->node);
    kfree(endpoint);
    manager->Notify();
}

uint64_t SocketManager::Socket(int32_t domain, int32_t type, int32_t protocol)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.create);)

    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr || Events == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOSYS);
    }

    uint64_t result = validate_socket(domain, type, protocol);

    if (result != 0)
    {
        return ARX_COMM_RETURN(result);
    }

    file_t* file = CreateFile(type & 15, type);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    result = Install(file, type);

    if ((int64_t) result < 0)
    {
        Vfs->Close(file);
    }

    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Socketpair(int32_t domain, int32_t type, int32_t protocol, uint64_t output)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.pair);)

    if (Resources == nullptr || Resources->processManager == nullptr || Vfs == nullptr || Events == nullptr)
    {
        return ARX_COMM_RETURN(LINUX_ENOSYS);
    }

    uint64_t result = validate_socket(domain, type, protocol);

    if (result != 0)
    {
        return ARX_COMM_RETURN(result);
    }

    file_t* files[2] = {CreateFile(type & 15, type), CreateFile(type & 15, type)};

    if (files[0] == nullptr || files[1] == nullptr)
    {
        Vfs->Close(files[0]);
        Vfs->Close(files[1]);
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
    socket_endpoint_t* first  = static_cast<socket_endpoint_t*>(files[0]->privateData);
    socket_endpoint_t* second = static_cast<socket_endpoint_t*>(files[1]->privateData);
    first->peer               = second;
    second->peer              = first;
    first->connected          = second->connected = true;
    process_t* process        = Resources->processManager->GetCurrentProcess();
    spinlock_acquire(&process->credentialLock);
    first->peerCredentials[0] = second->peerCredentials[0] = (int32_t) process->id;
    first->peerCredentials[1] = second->peerCredentials[1] = process->credentials.user.effective;
    first->peerCredentials[2] = second->peerCredentials[2] = process->credentials.group.effective;
    spinlock_release(&process->credentialLock);
    RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);

    {
        FileDescriptorGuard guard(process);
        int32_t             descriptors[2] = {-1, -1};
        const uint64_t      left           = Install(files[0], type);
        result                             = left;

        if ((int64_t) left >= 0)
        {
            descriptors[0]       = (int32_t) left;
            const uint64_t right = Install(files[1], type);
            result               = right;

            if ((int64_t) right >= 0)
            {
                descriptors[1] = (int32_t) right;
                result         = safe_write_user(output, descriptors, sizeof(descriptors)) ? 0 : LINUX_EFAULT;
            }
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
        Vfs->Close(files[0]);
        Vfs->Close(files[1]);
    }

    Notify();
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Bind(int32_t fd, uint64_t pointer, uint32_t length)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.bind);)

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    socket_address_t address;
    result = read_address(pointer, length, &address, true);

    if (result == 0)
    {
        spinlock_acquire_preempt(&Lock);
        ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
        socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);

        if (endpoint->bound || endpoint->binding)
        {
            result = LINUX_EINVAL;
        }
        else if (length == 2)
        {
            result = AutobindLocked(endpoint);
        }
        else if (address.path[0] == 0 && FindAddress(address, length) != nullptr)
        {
            result = (uint64_t) -98; // EADDRINUSE
        }
        else
        {
            inode_t* node = nullptr;

            if (address.path[0] != 0)
            {
                // Filesystem callbacks and notifications may poll sockets.
                // Reserve this bind, but never enter VFS under the socket lock.
                endpoint->binding = true;
                RecordReadinessLocked();
                ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
                spinlock_release_preempt(&Lock);
                result = (uint64_t) Vfs->SocketNode(reinterpret_cast<const char*>(address.path), true, &node);
                spinlock_acquire_preempt(&Lock);
                ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
                endpoint->binding = false;
            }

            if (result == 0)
            {
                endpoint->node          = node;
                endpoint->address       = address;
                endpoint->addressLength = length;
                endpoint->bound         = true;
            }
        }

        RecordReadinessLocked();

        ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
        spinlock_release_preempt(&Lock);
    }

    Vfs->Close(file);
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Listen(int32_t fd, int32_t backlog)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.listen);)

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);

    if (endpoint->type == 2)
    {
        result = LINUX_EOPNOTSUPP;
    }
    else if (!endpoint->bound || endpoint->connected)
    {
        result = LINUX_EINVAL;
    }
    else
    {
        endpoint->listening = true;
        endpoint->backlog   = (uint32_t) backlog > 128 ? 128 : (uint32_t) backlog;
    }

    RecordReadinessLocked();

    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);
    Vfs->Close(file);
    Notify();
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Connect(int32_t fd, uint64_t pointer, uint32_t length)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.connect);)

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);
    socket_address_t   address  = {};

    if (length < 2 || length > sizeof(address))
    {
        Vfs->Close(file);
        return ARX_COMM_RETURN(LINUX_EINVAL);
    }

    if (!safe_read_user(pointer, &address, length))
    {
        Vfs->Close(file);
        return ARX_COMM_RETURN(LINUX_EFAULT);
    }

    if (address.family == 0 && endpoint->type == 2)
    {
        spinlock_acquire_preempt(&Lock);
        ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
        endpoint->peer               = nullptr;
        endpoint->connected          = false;
        endpoint->peerAddress        = {};
        endpoint->peerAddress.family = 1;
        endpoint->peerAddressLength  = 2;
        RecordReadinessLocked();
        ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
        spinlock_release_preempt(&Lock);
        Vfs->Close(file);
        return ARX_COMM_RETURN(0);
    }

    result        = read_address(pointer, length, &address);
    inode_t* node = nullptr;
    if (result == 0)
    {
        result = ResolveAddress(address, &node);
    }
    file_t* accepted = result == 0 && endpoint->type != 2 ? CreateFile(endpoint->type, 0) : nullptr;

    if (result == 0 && endpoint->type != 2 && accepted == nullptr)
    {
        result = LINUX_ENOMEM;
    }

    while (result == 0)
    {
        const uint64_t sequence = Events->Sequence();
        spinlock_acquire_preempt(&Lock);
        ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
        socket_endpoint_t* target = FindAddress(address, length, node);

        if (endpoint->connected && endpoint->type != 2)
        {
            result = (uint64_t) -106; // EISCONN
        }
        else if (endpoint->listening)
        {
            result = LINUX_EINVAL;
        }
        else if (target == nullptr || (endpoint->type != 2 && !target->listening))
        {
            result = ECONNREFUSED;
        }
        else if (target->type != endpoint->type)
        {
            result = (uint64_t) -91; // EPROTOTYPE
        }
        else if (endpoint->type != 2 && target->pendingCount > target->backlog)
        {
            result = EAGAIN;
        }
        else
        {
            endpoint->peerAddress       = target->address;
            endpoint->peerAddressLength = target->addressLength;
            endpoint->connected         = true;

            if (endpoint->type == 2)
            {
                endpoint->peer = target;
            }
            else
            {
                socket_endpoint_t* incoming = static_cast<socket_endpoint_t*>(accepted->privateData);
                incoming->connected         = true;
                incoming->passCredentials   = target->passCredentials;
                incoming->peer              = endpoint;
                incoming->address           = target->address;
                incoming->addressLength     = target->addressLength;
                incoming->peerAddress       = endpoint->address;
                incoming->peerAddressLength = endpoint->addressLength;
                memcpy(incoming->peerCredentials, endpoint->credentials, sizeof(incoming->peerCredentials));
                memcpy(endpoint->peerCredentials, target->credentials, sizeof(endpoint->peerCredentials));
                endpoint->peer = incoming;

                if (target->pendingTail != nullptr)
                {
                    target->pendingTail->pendingNext = incoming;
                }
                else
                {
                    target->pendingHead = incoming;
                }

                target->pendingTail = incoming;
                ++target->pendingCount;
                ++incoming->internalReferences;
                accepted = nullptr; // The listener owns the incoming file reference.
            }
        }

        RecordReadinessLocked();

        ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
        spinlock_release_preempt(&Lock);

        if (result != EAGAIN || (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
        {
            break;
        }

        result = Events->Wait(sequence);
    }

    Vfs->Close(accepted);
    Vfs->Close(file);
    Notify();
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Accept(int32_t fd, uint64_t address, uint64_t length, uint32_t flags)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.accept);)

    if ((flags & ~(NONBLOCK | CLOEXEC)) != 0)
    {
        return ARX_COMM_RETURN(LINUX_EINVAL);
    }

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);

    for (;;)
    {
        const uint64_t       sequence = Events->Sequence();
        user_memory_access_t access;

        if (!begin_user_memory(&access))
        {
            result = LINUX_ESRCH;
            break;
        }

        spinlock_acquire_preempt(&Lock);
        ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
        socket_endpoint_t* incoming = endpoint->pendingHead;

        if (endpoint->type == 2)
        {
            result = LINUX_EOPNOTSUPP;
        }
        else if (!endpoint->listening)
        {
            result = LINUX_EINVAL;
        }
        else if (incoming == nullptr)
        {
            result = EAGAIN;
        }
        else if (address != 0 && !write_address(&access, address, length, incoming->peerAddress, incoming->peerAddressLength))
        {
            result = LINUX_EFAULT;
        }
        else
        {
            endpoint->pendingHead = incoming->pendingNext;
            incoming->pendingNext = nullptr;

            if (endpoint->pendingHead == nullptr)
            {
                endpoint->pendingTail = nullptr;
            }

            --endpoint->pendingCount;
            --incoming->internalReferences;
            incoming->file->statusFlags = 2 | (flags & NONBLOCK);
            result                      = 0;
        }

        RecordReadinessLocked();

        ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
        spinlock_release_preempt(&Lock);
        end_user_memory(&access);

        if (result == 0)
        {
            result = Install(incoming->file, flags);

            if ((int64_t) result < 0)
            {
                spinlock_acquire_preempt(&Lock);
                ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
                incoming->pendingNext = endpoint->pendingHead;
                endpoint->pendingHead = incoming;

                if (endpoint->pendingTail == nullptr)
                {
                    endpoint->pendingTail = incoming;
                }

                ++endpoint->pendingCount;
                ++incoming->internalReferences;
                RecordReadinessLocked();
                ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
                spinlock_release_preempt(&Lock);
            }

            break;
        }

        if (result != EAGAIN || (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
        {
            break;
        }

        result = Events->Wait(sequence);

        if (result != 0)
        {
            break;
        }
    }

    Vfs->Close(file);
    Notify();
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Name(int32_t fd, uint64_t address, uint64_t length, bool peer)
{
    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return result;
    }

    user_memory_access_t access;

    if (!begin_user_memory(&access))
    {
        Vfs->Close(file);
        return LINUX_ESRCH;
    }

    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);
    result                      = peer && !endpoint->connected ? ENOTCONN : write_address(&access, address, length, peer ? endpoint->peerAddress : endpoint->address, peer ? endpoint->peerAddressLength : endpoint->addressLength) ? 0 : LINUX_EFAULT;
    RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);
    end_user_memory(&access);
    Vfs->Close(file);
    return result;
}

uint32_t SocketManager::Poll(file_t* file)
{
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);
    SocketManager*     manager  = endpoint->manager;
    spinlock_acquire_preempt(&manager->Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&manager->DebugStats.group));
    const uint32_t result = ReadinessLocked(endpoint);
    manager->RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&manager->DebugStats.group));
    spinlock_release_preempt(&manager->Lock);
    return result;
}

uint32_t SocketManager::ReadinessLocked(socket_endpoint_t* endpoint)
{
    uint32_t result = 0;

    if (endpoint->listening)
    {
        result = endpoint->pendingHead != nullptr ? EVENT_POLL_IN : 0;
    }
    else
    {
        const bool eof = endpoint->readShutdown || (endpoint->connected && endpoint->type != 2 && (endpoint->peer == nullptr || endpoint->peer->writeShutdown));

        if (endpoint->head != nullptr || eof)
        {
            result |= EVENT_POLL_IN | EVENT_POLL_RDNORM;
        }

        if (eof)
        {
            result |= 0x2000; // POLLRDHUP
        }

        if (endpoint->type == 2 || (endpoint->peer != nullptr && !endpoint->peer->readShutdown && endpoint->peer->queued < endpoint->peer->receiveLimit && endpoint->peer->queued < endpoint->sendLimit && endpoint->peer->packetCount < 128))
        {
            result |= EVENT_POLL_OUT | EVENT_POLL_WRNORM;
        }

        if (endpoint->connected && endpoint->type != 2 && endpoint->peer == nullptr)
        {
            result |= EVENT_POLL_HUP | EVENT_POLL_OUT;
        }
    }

    return result;
}

int64_t SocketManager::Seek(file_t*, int64_t, int)
{
    return -29; // ESPIPE
}

int64_t SocketManager::Ioctl(file_t* file, uint64_t request, uint64_t argument)
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

    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);
    SocketManager*     manager  = endpoint->manager;
    spinlock_acquire_preempt(&manager->Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&manager->DebugStats.group));
    const int32_t  available = (int32_t) (endpoint->type == 1 ? endpoint->queued : endpoint->head != nullptr ? endpoint->head->size : 0);
    const uint64_t result    = endpoint->listening ? LINUX_EINVAL : safe_write_user_locked(&access, argument, &available, sizeof(available)) ? 0 : LINUX_EFAULT;
    manager->RecordReadinessLocked();
    ARX_DEBUG_RUN(arx_debug_group_end(&manager->DebugStats.group));
    spinlock_release_preempt(&manager->Lock);
    end_user_memory(&access);
    return (int64_t) result;
}

uint64_t SocketManager::Shutdown(int32_t fd, int32_t how)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.shutdown);)

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    socket_packet_t*   discarded = nullptr;
    socket_endpoint_t* pending   = nullptr;
    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);

    if (how < 0 || how > 2)
    {
        result = LINUX_EINVAL;
    }
    else
    {
        result = endpoint->connected || endpoint->listening ? 0 : ENOTCONN;

        if (how == 0 || how == 2)
        {
            endpoint->readShutdown = true;

            if (endpoint->listening)
            {
                endpoint->listening = false;
                pending             = endpoint->pendingHead;
                for (auto* incoming = pending; incoming != nullptr; incoming = incoming->pendingNext)
                {
                    --incoming->internalReferences;
                }
                endpoint->pendingHead  = endpoint->pendingTail = nullptr;
                endpoint->pendingCount = 0;
            }

            discarded      = endpoint->head;
            for (auto* packet = discarded; packet != nullptr; packet = packet->next)
            {
                ARX_DEBUG_ADD(DebugStats.discardedRights, packet->control != nullptr ? packet->control->count : 0);
                ChangeControlLocked(packet->control, false);
            }
            ARX_DEBUG_ADD(DebugStats.discardedBytes, endpoint->queued);
            ARX_DEBUG_ADD(DebugStats.discardedMessages, endpoint->packetCount);
            endpoint->head   = endpoint->tail = nullptr;
            endpoint->queued = endpoint->packetCount = 0;
        }

        if (how == 1 || how == 2)
        {
            endpoint->writeShutdown = true;
        }
    }

    RecordReadinessLocked();

    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);
    free_packets(discarded);

    while (pending != nullptr)
    {
        socket_endpoint_t* next = pending->pendingNext;
        Vfs->Close(pending->file);
        pending = next;
    }

    Vfs->Close(file);
    Notify();
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::GetOption(int32_t fd, int32_t level, int32_t option, uint64_t value, uint64_t length)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.getOption);)

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    uint32_t capacity;

    if (!safe_read_user(length, &capacity, sizeof(capacity)))
    {
        Vfs->Close(file);
        return ARX_COMM_RETURN(LINUX_EFAULT);
    }

    if ((int32_t) capacity < 0)
    {
        Vfs->Close(file);
        return ARX_COMM_RETURN(LINUX_EINVAL);
    }

    int32_t  output[3] = {};
    uint32_t size      = sizeof(int32_t);
    spinlock_acquire_preempt(&Lock);
    ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
    socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);

    if (level != 1)
    {
        result = (uint64_t) -92; // ENOPROTOOPT
    }
    else
    {
        switch (option)
        {
            case 3:
                output[0] = endpoint->type;
                break; // SO_TYPE
            case 4:
                output[0] = 0;
                break; // SO_ERROR
            case 7:
                output[0] = endpoint->sendLimit;
                break;
            case 8:
                output[0] = endpoint->receiveLimit;
                break;
            case 17: // SO_PEERCRED
                memcpy(output, endpoint->peerCredentials, sizeof(output));
                size = sizeof(output);
                break;
            case 16: // SO_PASSCRED
                output[0] = endpoint->passCredentials;
                break;
            case 30:
                output[0] = endpoint->listening;
                break;
            case 38:
                output[0] = 0;
                break; // SO_PROTOCOL
            case 39:
                output[0] = 1;
                break; // SO_DOMAIN
            default:
                result = (uint64_t) -92;
                break;
        }
    }

    RecordReadinessLocked();

    ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
    spinlock_release_preempt(&Lock);

    if (result == 0)
    {
        size   = capacity < size ? capacity : size;
        result = (size == 0 || safe_write_user(value, output, size)) && safe_write_user(length, &size, sizeof(size)) ? 0 : LINUX_EFAULT;
    }

    Vfs->Close(file);
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::SetOption(int32_t fd, int32_t level, int32_t option, uint64_t pointer, uint32_t length)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(DebugStats.setOption);)

    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return ARX_COMM_RETURN(result);
    }

    int32_t value;

    if (level != 1 || (option != 7 && option != 8 && option != 16))
    {
        result = (uint64_t) -92;
    }
    else if (length < sizeof(value))
    {
        result = LINUX_EINVAL;
    }
    else if (!safe_read_user(pointer, &value, sizeof(value)))
    {
        result = LINUX_EFAULT;
    }
    else
    {
        const uint32_t requested = (uint32_t) value > BUFFER_LIMIT / 2 ? BUFFER_LIMIT : (uint32_t) value * 2;
        spinlock_acquire_preempt(&Lock);
        ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
        socket_endpoint_t* endpoint = static_cast<socket_endpoint_t*>(file->privateData);

        if (option == 16)
        {
            if (value != 0 && !endpoint->bound)
            {
                result = AutobindLocked(endpoint);
            }
            if (result == 0)
            {
                endpoint->passCredentials = value != 0;
            }
        }
        else if (option == 7)
        {
            endpoint->sendLimit = requested < 4608 ? 4608 : requested;
        }
        else
        {
            endpoint->receiveLimit = requested < 2304 ? 2304 : requested;
        }

        RecordReadinessLocked();

        ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
        spinlock_release_preempt(&Lock);
    }

    Vfs->Close(file);
    Notify();
    return ARX_COMM_RETURN(result);
}

uint64_t SocketManager::Transfer(file_t* file, const file_iovec_t* vectors, size_t count, uint32_t flags, bool send, const socket_address_t* destination, uint32_t destinationLength, uint64_t source, uint64_t sourceLength, uint64_t message, const socket_message_t* header)
{
    ARX_DEBUG_ONLY(CommunicationOperation debugOperation(send ? DebugStats.send : DebugStats.receive, true);)

    const uint32_t supported = send ? MSG_DONTWAIT | MSG_NOSIGNAL | 0x80U : MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC | MSG_WAITALL | 0x40000000U;

    if ((flags & ~supported) != 0 || ((flags & MSG_PEEK) && (flags & MSG_WAITALL)))
    {
        return ARX_COMM_RETURN(LINUX_EOPNOTSUPP);
    }

    size_t   requested;
    uint64_t result = vector_size(vectors, count, &requested);
    ARX_DEBUG_RUN(debugOperation.Request(result == 0 ? requested : 0));

    if (result != 0)
    {
        return ARX_COMM_RETURN(result);
    }

    socket_endpoint_t* endpoint    = static_cast<socket_endpoint_t*>(file->privateData);
    size_t             transferred = 0;
    const size_t       allocation  = requested < BUFFER_LIMIT ? requested : BUFFER_LIMIT;
    socket_packet_t*   outgoing    = send ? static_cast<socket_packet_t*>(kzalloc(sizeof(socket_packet_t) + allocation)) : nullptr;
    socket_delivery_t* delivery    = !send && message != 0 ? static_cast<socket_delivery_t*>(kzalloc(sizeof(socket_delivery_t))) : nullptr;

    if ((send && outgoing == nullptr) || (!send && message != 0 && delivery == nullptr))
    {
        return ARX_COMM_RETURN(LINUX_ENOMEM);
    }

    inode_t* destinationNode = nullptr;
    if (send)
    {
        result = ParseControl(header, &outgoing->control);
        if (result == 0 && destination != nullptr)
        {
            result = ResolveAddress(*destination, &destinationNode);
        }
        if (result == 0 && endpoint->type == 1 && requested == 0 && (outgoing->control->count != 0 || outgoing->control->explicitCredentials))
        {
            result = LINUX_EINVAL; // Stream ancillary delivery requires a data byte.
        }
        if (result != 0)
        {
            free_packets(outgoing);
            return ARX_COMM_RETURN(result);
        }
    }

    for (;;)
    {
        const uint64_t      sequence         = Events->Sequence();
        bool                changed          = false;
        bool                complete         = false;
        bool                wroteMetadata    = false;
        socket_ancillary_t* discardedControl = nullptr;
        socket_packet_t*    discardedPacket  = nullptr;
        {
            // Descriptor mutation precedes the socket lock, matching close/dup.
            // No descriptor lock may be held across a blocking socket wait.
            FileDescriptorGuard  descriptors(Resources->processManager->GetCurrentProcess());
            user_memory_access_t access;

            if (!begin_user_memory(&access))
            {
                result = LINUX_ESRCH;
                break;
            }

            spinlock_acquire_preempt(&Lock);
            ARX_DEBUG_RUN(arx_debug_group_begin(&DebugStats.group));
            result            = EAGAIN;
            const bool stream = endpoint->type == 1;

            if (endpoint->listening)
            {
                result = send ? EPIPE : LINUX_EINVAL;
            }
            else if (endpoint->type != 2 && !endpoint->connected)
            {
                result = send ? ENOTCONN : LINUX_EINVAL;
            }
            else if (send)
            {
                socket_endpoint_t* target = destination != nullptr ? FindAddress(*destination, destinationLength, destinationNode) : endpoint->peer;

                if (endpoint->writeShutdown)
                {
                    result = EPIPE;
                }
                else if (destination != nullptr && endpoint->type != 2)
                {
                    result = (uint64_t) -106;
                }
                else if (target == nullptr)
                {
                    result = endpoint->type == 2 ? (destination != nullptr || endpoint->connected ? ECONNREFUSED : (uint64_t) -89) : EPIPE;
                }
                else if (target->type != endpoint->type)
                {
                    result = (uint64_t) -91;
                }
                else if (target->readShutdown)
                {
                    result = EPIPE;
                }
                else if (endpoint->type == 2 && target->connected && target->peer != endpoint)
                {
                    result = (uint64_t) -1;
                }
                else if (!stream && (requested > BUFFER_LIMIT || requested > endpoint->sendLimit))
                {
                    result = (uint64_t) -90; // EMSGSIZE: messages are atomic.
                }
                else if (stream && requested == 0)
                {
                    result   = 0;
                    complete = true;
                }
                else
                {
                    const size_t limit = stream && endpoint->sendLimit < target->receiveLimit ? endpoint->sendLimit : target->receiveLimit;
                    const size_t space = target->queued < limit ? limit - target->queued : 0;
                    size_t       bytes = stream && requested > space ? space : requested;
                    bytes              = stream && bytes > endpoint->sendLimit ? endpoint->sendLimit : bytes;
                    bytes              = bytes > allocation ? allocation : bytes;

                    if ((stream ? bytes != 0 : requested <= space) && target->packetCount < 128)
                    {
                        if (!MayQueueRightsLocked(outgoing->control))
                        {
                            result = (uint64_t) -109; // ETOOMANYREFS: bounded per-sender-UID in-flight rights.
                        }
                        else if (!copy_vectors(&access, vectors, count, 0, outgoing->data, bytes, true ARX_DEBUG_ONLY(, &DebugStats.sendCopiedBytes)))
                        {
                            result = LINUX_EFAULT;
                        }
                        else
                        {
                            outgoing->next         = nullptr;
                            outgoing->offset       = 0;
                            outgoing->size         = bytes;
                            outgoing->source       = endpoint->address;
                            outgoing->sourceLength = endpoint->bound ? endpoint->addressLength : 0;
                            ChangeControlLocked(outgoing->control, true);

                            if (target->tail != nullptr)
                            {
                                target->tail->next = outgoing;
                            }
                            else
                            {
                                target->head = outgoing;
                            }

                            target->tail = outgoing;
                            target->queued += bytes;
                            ARX_DEBUG_ADD(DebugStats.enqueuedBytes, bytes);
                            ARX_DEBUG_ADD(DebugStats.enqueuedMessages, 1);
                            ++target->packetCount;
                            outgoing    = nullptr;
                            transferred = bytes;
                            changed     = complete = true;
                            result      = 0;
                        }
                    }
                }
            }
            else
            {
                socket_packet_t* packet = endpoint->head;
                const bool       eof    = endpoint->readShutdown || (endpoint->type != 2 && endpoint->connected && (endpoint->peer == nullptr || endpoint->peer->writeShutdown));

                if (stream && requested == 0)
                {
                    result   = 0;
                    complete = true;
                }
                else if (packet != nullptr)
                {
                    const size_t   available   = packet->size - packet->offset;
                    const size_t   bytes       = available < requested - transferred ? available : requested - transferred;
                    uint32_t       outputFlags = (!stream && bytes < available ? MSG_TRUNC : 0) | (flags & 0x40000000U);
                    const uint32_t nameLength  = stream ? 0 : packet->sourceLength;
                    ARX_DEBUG_ONLY(size_t deliveredRights = 0;)
                    bool           copied      = copy_vectors(&access, vectors, count, transferred, packet->data + packet->offset, bytes, false ARX_DEBUG_ONLY(, &DebugStats.copiedBytes));

                    if (copied && transferred == 0 && source != 0)
                    {
                        copied = write_address(&access, source, sourceLength, packet->source, nameLength);
                    }

                    const bool hasControl = packet->control != nullptr && (packet->control->count != 0 || endpoint->passCredentials);

                    if (copied && message != 0)
                    {
                        const uint32_t reportedName  = header->name != 0 ? nameLength : 0;
                        const uint32_t copiedName    = header->nameLength < nameLength ? header->nameLength : nameLength;
                        uint64_t       controlLength = 0;
                        const uint64_t capacity      = header->control != 0 ? header->controlLength : 0;
                        delivery->installed          = 0;
                        memset(delivery->bytes, 0, sizeof(delivery->bytes));

                        if (packet->control != nullptr && endpoint->passCredentials)
                        {
                            const size_t availableControl = capacity < 32 ? capacity : 32;

                            if (availableControl >= sizeof(socket_control_t))
                            {
                                const size_t           payload = availableControl - sizeof(socket_control_t) < sizeof(socket_credentials_t) ? availableControl - sizeof(socket_control_t) : sizeof(socket_credentials_t);
                                const socket_control_t item    = {sizeof(socket_control_t) + payload, SOL_SOCKET, SCM_CREDENTIALS};
                                memcpy(delivery->bytes, &item, sizeof(item));
                                memcpy(delivery->bytes + sizeof(item), &packet->control->credentials, payload);
                                controlLength = availableControl;
                            }

                            if (availableControl < sizeof(socket_control_t) + sizeof(socket_credentials_t))
                            {
                                outputFlags |= MSG_CTRUNC;
                            }
                        }

                        if (packet->control != nullptr && packet->control->count != 0)
                        {
                            const uint64_t remaining = capacity > controlLength ? capacity - controlLength : 0;
                            size_t         limit     = remaining >= sizeof(socket_control_t) ? (remaining - sizeof(socket_control_t)) / sizeof(int32_t) : 0;
                            limit                    = limit < packet->control->count ? limit : packet->control->count;
                            process_t* process       = Resources->processManager->GetCurrentProcess();

                            for (size_t i = 0; i < limit; ++i)
                            {
                                file_t*       received   = packet->control->files[i];
                                const int64_t descriptor = Resources->processManager->AddFileDescriptor(process, received, flags & MSG_CMSG_CLOEXEC ? FD_FLAG_CLOEXEC : 0, dispatcher_vfs_file_operations());

                                if (descriptor < 0)
                                {
                                    if (descriptor != (int64_t) LINUX_EMFILE)
                                    {
                                        copied = false;
                                        result = (uint64_t) descriptor;
                                    }

                                    break;
                                }

                                Vfs->Retain(received); // Allocation owns a reference, separate from the queued one.
                                delivery->descriptors[delivery->installed] = (int32_t) descriptor;
                                delivery->rollback[delivery->installed++]  = received;
                            }

                            if (delivery->installed < packet->control->count)
                            {
                                outputFlags |= MSG_CTRUNC;
                            }

                            if (delivery->installed != 0)
                            {
                                const uint64_t         size = sizeof(socket_control_t) + delivery->installed * sizeof(int32_t);
                                const socket_control_t item = {size, SOL_SOCKET, SCM_RIGHTS};
                                memcpy(delivery->bytes + controlLength, &item, sizeof(item));
                                memcpy(delivery->bytes + controlLength + sizeof(item), delivery->descriptors, delivery->installed * sizeof(int32_t));
                                controlLength += remaining < ((size + 7) & ~7ULL) ? remaining : ((size + 7) & ~7ULL);
                            }
                        }

                        copied = copied && (header->name == 0 || copiedName == 0 || safe_write_user_locked(&access, header->name, &packet->source, copiedName))
                                 && safe_write_user_locked(&access, message + offsetof(socket_message_t, nameLength), &reportedName, sizeof(reportedName))
                                 && (controlLength == 0 || safe_write_user_locked(&access, header->control, delivery->bytes, controlLength))
                                 && safe_write_user_locked(&access, message + offsetof(socket_message_t, controlLength), &controlLength, sizeof(controlLength))
                                 && safe_write_user_locked(&access, message + offsetof(socket_message_t, flags), &outputFlags, sizeof(outputFlags));

                        if (!copied)
                        {
                            for (size_t i = 0; i < delivery->installed; ++i)
                            {
                                *Resources->processManager->GetFileDescriptor(Resources->processManager->GetCurrentProcess(), delivery->descriptors[i]) = {};
                            }
                        }
                        else
                        {
                            ARX_DEBUG_RUN(deliveredRights = delivery->installed);
                            ARX_DEBUG_ADD(DebugStats.installedRights, delivery->installed);
                            delivery->installed = 0; // Receiver table now owns the references.
                        }
                    }

                    wroteMetadata = copied && message != 0;

                    if (!copied)
                    {
                        result = result != EAGAIN ? result : LINUX_EFAULT;
                    }
                    else
                    {
                        transferred += !stream && (flags & MSG_TRUNC) ? available : bytes;
                        result   = 0;
                        complete = !stream || transferred == requested || (flags & MSG_PEEK) || hasControl;

                        if (!(flags & MSG_PEEK))
                        {
                            ARX_DEBUG_ADD(DebugStats.consumedBytes, stream ? bytes : available);
                            ARX_DEBUG_ADD(DebugStats.discardedBytes, stream ? 0 : available - bytes);
                            endpoint->queued -= stream ? bytes : available;
                            packet->offset += bytes;
                            discardedControl = packet->control;
                            ARX_DEBUG_ADD(DebugStats.discardedRights, discardedControl != nullptr ? discardedControl->count - deliveredRights : 0);
                            ChangeControlLocked(discardedControl, false);
                            packet->control = nullptr;

                            if (!stream || packet->offset == packet->size)
                            {
                                endpoint->head = packet->next;

                                if (endpoint->head == nullptr)
                                {
                                    endpoint->tail = nullptr;
                                }

                                --endpoint->packetCount;
                                ARX_DEBUG_ADD(DebugStats.consumedMessages, 1);
                                packet->next    = nullptr;
                                discardedPacket = packet;
                            }

                            changed = true;
                        }
                    }
                }
                else if (eof)
                {
                    result   = 0;
                    complete = true;
                }
                else if (transferred != 0 && !(flags & MSG_WAITALL))
                {
                    result   = 0;
                    complete = true;
                }
            }

            if (!send && complete && result == 0 && transferred == 0 && message != 0 && !wroteMetadata)
            {
                const uint32_t zero          = 0;
                const uint64_t controlLength = 0;
                const uint32_t outputFlags   = flags & 0x40000000U;
                if (!safe_write_user_locked(&access, message + offsetof(socket_message_t, nameLength), &zero, sizeof(zero)) || !safe_write_user_locked(&access, message + offsetof(socket_message_t, controlLength), &controlLength, sizeof(controlLength))
                    || !safe_write_user_locked(&access, message + offsetof(socket_message_t, flags), &outputFlags, sizeof(outputFlags)))
                {
                    result = LINUX_EFAULT;
                }
            }

            RecordReadinessLocked();

            ARX_DEBUG_RUN(arx_debug_group_end(&DebugStats.group));
            spinlock_release_preempt(&Lock);
            end_user_memory(&access);
        }
        // Releasing a passed socket can reenter the graph collector.
        if (delivery != nullptr)
        {
            for (size_t i = 0; i < delivery->installed; ++i)
            {
                Vfs->Close(delivery->rollback[i]);
            }
            delivery->installed = 0;
        }
        free_control(discardedControl);
        free_packets(discardedPacket);

        if (changed)
        {
            Notify();
        }

        if (complete || (result != 0 && result != EAGAIN))
        {
            break;
        }

        if (result == EAGAIN)
        {
            if ((flags & MSG_DONTWAIT) || (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & NONBLOCK))
            {
                break;
            }

            result = Events->Wait(sequence);

            if (result != 0)
            {
                break;
            }
        }
    }

    free_packets(outgoing);
    kfree(delivery);

    if (send && result == EPIPE && !(flags & MSG_NOSIGNAL) && Signals != nullptr)
    {
        process_t* process = Resources->processManager->GetCurrentProcess();
        task_t*    task    = Resources->taskManager->GetCurrentTask();

        if (process != nullptr && task != nullptr)
        {
            Signals->Send((int32_t) process->id, (int32_t) task->id, 13);
        }
    }

    return ARX_COMM_RETURN(transferred != 0 || result == 0 ? transferred : result);
}

uint64_t SocketManager::Sendto(int32_t fd, uint64_t data, uint64_t size, uint32_t flags, uint64_t address, uint32_t length)
{
    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return result;
    }

    socket_address_t destination;
    result = address != 0 ? read_address(address, length, &destination) : 0;

    if (result == 0)
    {
        const file_iovec_t vector = {data, size};
        result                    = Transfer(file, &vector, 1, flags, true, address != 0 ? &destination : nullptr, length);
    }

    Vfs->Close(file);
    return result;
}

uint64_t SocketManager::Recvfrom(int32_t fd, uint64_t data, uint64_t size, uint32_t flags, uint64_t address, uint64_t length)
{
    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return result;
    }

    const file_iovec_t vector = {data, size};
    result                    = Transfer(file, &vector, 1, flags, false, nullptr, 0, address, length);
    Vfs->Close(file);
    return result;
}

uint64_t SocketManager::Message(int32_t fd, uint64_t message, uint32_t flags, bool send)
{
    uint64_t result;
    file_t*  file = RetainDescriptor(fd, &result);

    if (file == nullptr)
    {
        return result;
    }

    socket_message_t header;
    file_iovec_t*    vectors = nullptr;
    socket_address_t destination;

    if (!safe_read_user(message, &header, sizeof(header)))
    {
        result = LINUX_EFAULT;
    }
    else if (header.vectorCount > MAX_VECTORS)
    {
        result = (uint64_t) -90;
    }
    else
    {
        if (header.vectorCount != 0)
        {
            vectors = static_cast<file_iovec_t*>(kmalloc(header.vectorCount * sizeof(file_iovec_t)));

            if (vectors == nullptr)
            {
                result = LINUX_ENOMEM;
            }
            else if (!safe_read_user(header.vectors, vectors, header.vectorCount * sizeof(file_iovec_t)))
            {
                result = LINUX_EFAULT;
            }
        }

        if (result == 0 && send && header.name != 0)
        {
            result = read_address(header.name, header.nameLength, &destination);
        }

        if (result == 0)
        {
            result = Transfer(file, vectors, header.vectorCount, flags, send, send && header.name != 0 ? &destination : nullptr, header.nameLength, 0, 0, send ? 0 : message, &header);
        }
    }

    kfree(vectors);
    Vfs->Close(file);
    return result;
}

uint64_t SocketManager::Read(file_t* file, uint64_t address, uint64_t count)
{
    const file_iovec_t vector = {address, count};
    return count == 0 ? 0 : ReadVector(file, &vector, 1);
}

uint64_t SocketManager::Write(file_t* file, uint64_t address, uint64_t count)
{
    const file_iovec_t vector = {address, count};
    return WriteVector(file, &vector, 1);
}

uint64_t SocketManager::ReadVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    size_t         size;
    const uint64_t result = vector_size(vectors, count, &size);

    if (result != 0 || size == 0)
    {
        return result;
    }

    return static_cast<socket_endpoint_t*>(file->privateData)->manager->Transfer(file, vectors, count, 0, false);
}

uint64_t SocketManager::WriteVector(file_t* file, const file_iovec_t* vectors, size_t count)
{
    return static_cast<socket_endpoint_t*>(file->privateData)->manager->Transfer(file, vectors, count, 0, true);
}
