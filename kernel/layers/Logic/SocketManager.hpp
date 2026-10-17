#pragma once

#include "layers/Logic/VirtualFileSystem.hpp"

#include <klib/spinlock.h>
#include <stddef.h>
#include <stdint.h>

struct ResourceLayerCaps;
struct socket_endpoint_t;
struct socket_ancillary_t;
class EventManager;
class SignalManager;

// Native Linux LP64 wire structures. Never dereference user-provided pointers.
struct socket_address_t
{
    uint16_t family;
    uint8_t  path[108];
};
static_assert(sizeof(socket_address_t) == 110, "Linux sockaddr_un ABI");

struct socket_control_t
{
    uint64_t length;
    int32_t  level;
    int32_t  type;
};
static_assert(sizeof(socket_control_t) == 16, "Linux LP64 cmsghdr ABI");

struct socket_credentials_t
{
    int32_t  pid;
    uint32_t uid;
    uint32_t gid;
};
static_assert(sizeof(socket_credentials_t) == 12, "Linux ucred ABI");

struct socket_message_t
{
    uint64_t name;
    uint32_t nameLength;
    uint32_t padding;
    uint64_t vectors;
    uint64_t vectorCount;
    uint64_t control;
    uint64_t controlLength;
    uint32_t flags;
    uint32_t tailPadding;
};
static_assert(sizeof(socket_message_t) == 56, "Linux LP64 msghdr ABI");
static_assert(offsetof(socket_message_t, vectors) == 16 && offsetof(socket_message_t, flags) == 48, "Linux msghdr offsets");

class SocketManager
{
public:
    SocketManager(ResourceLayerCaps* resources, VirtualFileSystem* vfs, EventManager* events, SignalManager* signals);
    uint64_t Socket(int32_t domain, int32_t type, int32_t protocol);
    uint64_t Socketpair(int32_t domain, int32_t type, int32_t protocol, uint64_t output);
    uint64_t Bind(int32_t fd, uint64_t address, uint32_t length);
    uint64_t Connect(int32_t fd, uint64_t address, uint32_t length);
    uint64_t Listen(int32_t fd, int32_t backlog);
    uint64_t Accept(int32_t fd, uint64_t address, uint64_t length, uint32_t flags);
    uint64_t Sendto(int32_t fd, uint64_t data, uint64_t size, uint32_t flags, uint64_t address, uint32_t length);
    uint64_t Recvfrom(int32_t fd, uint64_t data, uint64_t size, uint32_t flags, uint64_t address, uint64_t length);
    uint64_t Message(int32_t fd, uint64_t message, uint32_t flags, bool send);
    uint64_t Shutdown(int32_t fd, int32_t how);
    uint64_t GetOption(int32_t fd, int32_t level, int32_t option, uint64_t value, uint64_t length);
    uint64_t SetOption(int32_t fd, int32_t level, int32_t option, uint64_t value, uint32_t length);
    uint64_t Name(int32_t fd, uint64_t address, uint64_t length, bool peer);

private:
    static const file_operations_t Operations;
    static void                    Release(file_t* file);
    static bool                    DropReference(file_t* file, bool* last);
    static uint32_t                Poll(file_t* file);
    static uint32_t                ReadinessLocked(socket_endpoint_t* endpoint);
    static int64_t                 Seek(file_t* file, int64_t offset, int whence);
    static int64_t                 Ioctl(file_t* file, uint64_t request, uint64_t argument);
    static uint64_t                Read(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                Write(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                ReadVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static uint64_t                WriteVector(file_t* file, const file_iovec_t* vectors, size_t count);
    file_t*                        RetainDescriptor(int32_t fd, uint64_t* error);
    file_t*                        CreateFile(uint32_t type, uint32_t flags);
    uint64_t                       Install(file_t* file, uint32_t flags);
    void                           Notify();
    void                           RecordReadinessLocked();
    socket_endpoint_t*             FindAddress(const socket_address_t& address, uint32_t length, inode_t* node = nullptr);
    uint64_t                       ResolveAddress(const socket_address_t& address, inode_t** node);
    uint64_t                       ParseControl(const socket_message_t* header, socket_ancillary_t** control);
    uint64_t                       AutobindLocked(socket_endpoint_t* endpoint);
    void                           ChangeControlLocked(socket_ancillary_t* control, bool add);
    bool                           MayQueueRightsLocked(const socket_ancillary_t* control);
    uint64_t Transfer(file_t* file, const file_iovec_t* vectors, size_t count, uint32_t flags, bool send, const socket_address_t* destination = nullptr, uint32_t destinationLength = 0, uint64_t source = 0, uint64_t sourceLength = 0, uint64_t message = 0, const socket_message_t* header = nullptr);
    ResourceLayerCaps* Resources;
    VirtualFileSystem* Vfs;
    EventManager*      Events;
    SignalManager*     Signals;
    spinlock_t         Lock;
    socket_endpoint_t* Endpoints;
    size_t             EndpointCount;
    uint32_t           AutobindSequence = 0;
};
