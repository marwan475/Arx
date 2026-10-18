#pragma once

#include "DebugRecords.h"

#if DEBUG

// Lifecycle mutations are serialized by bootstrap/quiescent composition wiring.
// Counter updates never access this registry or acquire a framework lock.
class DebugManager
{
public:
    enum class Result
    {
        Ok,
        Duplicate,
        Full,
        Invalid,
        NotFound,
        WrongOwner
    };

    DebugManager();
    ~DebugManager();

    Result Register(const char* name, const char* typeName, const void* address,
                    uint64_t size, uint32_t version,
                    const arx_debug_group_t* group = nullptr,
                    const arx_debug_view_t* views = nullptr, uint32_t viewCount = 0);
    Result DeclareUnavailable(const char* name, const char* reason);
    Result Publish(const char* name, const char* typeName, const void* address,
                   uint64_t size, uint32_t version,
                   const arx_debug_group_t* group = nullptr,
                   const arx_debug_view_t* views = nullptr, uint32_t viewCount = 0);
    Result Withdraw(const char* name, const void* owner);
    void InvalidateAll();

    // Public, versioned discovery data; GDB reads fields, never target methods.
    uint64_t           magic;
    uint32_t           formatVersion;
    uint32_t           capacity;
    uint32_t           reserved;
    arx_debug_record_t records[ARX_DEBUG_CAPACITY];

private:
    arx_debug_record_t* Find(const char* name);
    Result Reserve(const char* name, arx_debug_record_t** record);
    Result Fill(arx_debug_record_t* record, const char* typeName,
                const void* address, uint64_t size, uint32_t version,
                const arx_debug_group_t* group, const arx_debug_view_t* views,
                uint32_t viewCount);
};

#endif
