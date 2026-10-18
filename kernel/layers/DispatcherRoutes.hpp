#pragma once

#include "layers/DispatcherBridge.h"

class MemoryRequestManager;

// Provider definitions belong at the routing seam. Resource and Platform only
// see the opaque handles in DispatcherBridge.h and cannot invoke the providers.
struct dispatcher_task_cleanup
{
    constexpr dispatcher_task_cleanup(void* context, void (*cleanup)(void*, task_t*)) : Context(context), Cleanup(cleanup)
    {
    }

private:
    friend void dispatcher_cleanup_task(const dispatcher_task_cleanup_t* route, task_t* task);

    void* const Context;
    void (*const Cleanup)(void*, task_t*);
};

struct dispatcher_file_operations
{
    constexpr dispatcher_file_operations(void* context, bool (*retain)(void*, void*), void (*release)(void*, void*)) : Context(context), Retain(retain), Release(release)
    {
    }

private:
    friend bool dispatcher_retain_file(const dispatcher_file_operations_t* route, void* file);
    friend void dispatcher_release_file(const dispatcher_file_operations_t* route, void* file);

    void* const Context;
    bool (*const Retain)(void*, void*);
    void (*const Release)(void*, void*);
};

struct dispatcher_backing_operations
{
    constexpr dispatcher_backing_operations(int64_t (*write)(void*, uint64_t, const void*, uint64_t), void (*release)(void*)) : Write(write), Release(release)
    {
    }

private:
    friend class MemoryRequestManager;
    friend bool dispatcher_backing_can_write(const dispatcher_backing_operations_t* route);

    int64_t (*const Write)(void*, uint64_t, const void*, uint64_t);
    void (*const Release)(void*);
};
