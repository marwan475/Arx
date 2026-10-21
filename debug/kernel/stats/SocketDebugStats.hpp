#pragma once
#include "CommunicationDebug.hpp"

#if DEBUG
class SocketManager;
struct socket_endpoint_t;

// arx.sockets/1: count=0 capacity=128. *head is the manager-owned endpoint list,
// next until null, max128. *count is the stored live endpoint count, not an array
// length. Verify manager, file->operations, privateData and embedded inode links.
// SocketState.hpp supplies exact layouts. pendingHead/pendingNext FIFO owns file
// refs/internalReferences. peer is WEAK; datagram asymmetry is legitimate.
// Pending count can be backlog+1 under current admission policy. Message head/
// next FIFO has <=128 packets per endpoint, offset<=size, queued=sum(size-offset).
// control/next chains retain <=253 files per node, bounded by global budgets.
// Addresses use explicit length (2..110), raw abstract bytes preserve NULs;
// sourceLength=0 on messages means unnamed. Only UNIX types1,2,5 are supported.
// Persistent owning graphs exclude transfer/rollback/detached references. Neither
// refCount nor internalReferences nor this view proves complete ownership/GC.
// All bytes in stats are provider-level normal-path committed transitions. PEEK
// can copy/install rights without dequeue. TRUNC return can exceed copied bytes.
// Queue discard counters include shutdown/collector/final-release paths.
struct SocketDebugStats
{
    SocketManager* source;
    const ProcessDebugStats* processes;
    socket_endpoint_t* const* head;
    const size_t* count;
    const void* operations;
    uint64_t capacity;
    arx_debug_group_t group;
    CommunicationOperationStats create;
    CommunicationOperationStats pair;
    CommunicationOperationStats bind;
    CommunicationOperationStats connect;
    CommunicationOperationStats listen;
    CommunicationOperationStats accept;
    CommunicationOperationStats shutdown;
    CommunicationOperationStats getOption;
    CommunicationOperationStats setOption;
    CommunicationOperationStats send;
    CommunicationOperationStats receive;
    uint64_t endpointsCreated;
    uint64_t endpointsReleased;
    uint64_t enqueuedBytes;
    uint64_t copiedBytes; // Completed receive payload fragments before metadata success.
    uint64_t sendCopiedBytes; // Completed send payload fragments, possibly never enqueued.
    uint64_t consumedBytes;
    uint64_t discardedBytes;
    uint64_t enqueuedMessages;
    uint64_t consumedMessages;
    uint64_t discardedMessages;
    uint64_t queuedRights;
    uint64_t unqueuedRights;
    uint64_t installedRights;
    uint64_t discardedRights;
};
#endif
