#pragma once

#include "VirtualFileSystem.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"
#include <debug/kernel/stats/IpcDebugStats.hpp>

class EventManager;
class SignalManager;
struct ResourceLayerCaps;

// Provider-owned runtime layout, also used by exact-build native readers.
struct pipe_state_t
{
    inode_t            inode;
    ARX_DEBUG_ONLY(
        IpcDebugStats* debugStats;
        arx_debug_group_t debugGroup;
    )
    spinlock_t         lock;
    file_t*            reader;
    file_t*            writer;
    EventManager*      events;
    SignalManager*     signals;
    ResourceLayerCaps* resources;
    VirtualFileSystem* vfs;
    size_t             references;
    uint32_t           lastReadiness[2];
    uint32_t           pendingReadiness[2];
    size_t             capacity;
    size_t             head;
    size_t             size;
    uint8_t            data[65536];
    // Packet bits travel with bytes, so changing O_DIRECT affects future writes.
    uint8_t packet[65536 / 4];
};

struct memory_file_t
{
    inode_t               inode;
    ARX_DEBUG_ONLY(
        IpcDebugStats* debugStats;
        arx_debug_group_t debugGroup;
    )
    uint32_t              seals;
    virt_shared_page_t*   pages[256];
    VirtualMemoryManager* manager;
};

