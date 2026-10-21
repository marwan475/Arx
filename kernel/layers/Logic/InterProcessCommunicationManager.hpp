#pragma once

#include <debug/kernel/stats/IpcDebugStats.hpp>

#include "layers/Logic/VirtualFileSystem.hpp"

struct ResourceLayerCaps;
class EventManager;
class SignalManager;

class InterProcessCommunicationManager
{
public:
    ARX_DEBUG_ONLY(IpcDebugStats DebugStats{};)
    ARX_DEBUG_ONLY(void PublishDebug(DebugManager& manager, const ProcessDebugStats* processes);)

    ARX_DEBUG_ONLY(
        static const void* DebugPipeOperations()
        {
            return &PipeOperations;
        }

        static const void* DebugMemoryOperations()
        {
            return &MemoryOperations;
        }
    )
    InterProcessCommunicationManager(ResourceLayerCaps* resources, VirtualFileSystem* vfs, EventManager* events, SignalManager* signals);
    uint64_t CreatePipe(uint64_t output, uint32_t flags);
    uint64_t CreateMemoryFile(uint64_t name, uint32_t flags);

private:
    static const file_operations_t PipeOperations;
    static const file_operations_t MemoryOperations;
    static uint64_t                PipeRead(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                PipeWrite(file_t* file, uint64_t address, uint64_t count);
    static uint64_t                PipeReadVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static uint64_t                PipeWriteVector(file_t* file, const file_iovec_t* vectors, size_t count);
    static uint64_t                PipeTransfer(file_t* file, const file_iovec_t* vectors, size_t count, bool write);
    static uint32_t                PipePoll(file_t* file);
    static int64_t                 PipeSeek(file_t* file, int64_t offset, int whence);
    static int64_t                 PipeIoctl(file_t* file, uint64_t request, uint64_t argument);
    static uint64_t                PipeControl(file_t* file, uint32_t command, uint64_t argument);
    static void                    PipeRelease(file_t* file);
    static void                    NotifyPipe(void* state);
    static int64_t                 MemoryRead(file_t* file, void* buffer, uint64_t count);
    static int64_t                 MemoryWrite(file_t* file, const void* buffer, uint64_t count);
    static int64_t                 MemorySeek(file_t* file, int64_t offset, int whence);
    static int64_t                 MemoryTruncate(file_t* file, uint64_t size);
    static int64_t                 MemoryBacking(file_t*, VirtualMemoryManager*, uint32_t, bool, virt_backing_object**, uint32_t*);
    static uint64_t                MemoryControl(file_t* file, uint32_t command, uint64_t argument);
    static void                    MemoryRelease(file_t* file);
    ResourceLayerCaps*             Resources;
    VirtualFileSystem*             Vfs;
    EventManager*                  Events;
    SignalManager*                 Signals;
};
