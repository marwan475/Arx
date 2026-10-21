#pragma once
#include "CommunicationDebug.hpp"

#if DEBUG
class InterProcessCommunicationManager;
struct SocketDebugStats;

// arx.ipc/1: count=0 capacity=0 means descriptor/ancillary reachability, not a
// catalogue. Verify ProcessDebugStats.vfsRoute before file_t interpretation and
// pipeOperations/memoryOperations before privateData casts to IpcState.hpp types.
// Native readers enumerate allocated process/task slots (including overrides),
// deduplicate descriptor tables/files but retain owner edges. Borrowed identities
// use dependency-owned generations. Socket message rights extend reachable roots
// with bounded graphs, not every transient/retained file. No historical IDs.
// pipe rings: logical i -> (head+i)%capacity, capacity in [4096,65536], size<=cap.
// packet has 2 bits per physical byte: 1=packet byte, 2=end. NEVER copy an entire
// pipe_state_t by default: it embeds 64KiB payload. Use member reads for metadata.
// memory_file_t has 256 page slots, holes are zero, inode.backing may outlive file.
// Mappings correlate virt_region_t.object/backing_offset via known process spaces.
// Stored readiness is last/pending transitions, not a target Poll invocation.
// livePipes/liveMemoryFiles count object allocations reaching initialization,
// including objects whose descriptor installation later fails, until final free.
struct IpcDebugStats
{
    InterProcessCommunicationManager* source;
    const ProcessDebugStats* processes;
    const SocketDebugStats* sockets;
    const void* pipeOperations;
    const void* memoryOperations;
    arx_debug_group_t group;
    CommunicationOperationStats createPipe;
    CommunicationOperationStats createMemory;
    CommunicationOperationStats pipeRead;
    CommunicationOperationStats pipeWrite;
    CommunicationOperationStats pipeControl;
    CommunicationOperationStats memoryRead;
    CommunicationOperationStats memoryWrite;
    CommunicationOperationStats memoryControl;
    CommunicationOperationStats memoryTruncate;
    CommunicationOperationStats memoryBacking;
    uint64_t livePipes;
    uint64_t liveMemoryFiles;
    uint64_t pipesCreated;
    uint64_t memoryFilesCreated;
    uint64_t pipeEnqueuedBytes;
    uint64_t pipeCopiedBytes; // Completed read-to-user fragments, even if later work fails.
    uint64_t pipeWriteCopiedBytes; // Completed user-to-staging fragments, including failed later enqueue.
    uint64_t pipeConsumedBytes;
    uint64_t pipeDiscardedBytes;
    uint64_t pipeReleaseDiscardedBytes;
    uint64_t memoryReadBytes;
    uint64_t memoryWriteBytes;
};
#endif
