#include "DebugManager.hpp"

#if DEBUG

extern "C"
{
    uint32_t arx_debug_format_marker = ARX_DEBUG_VERSION;
}

static bool DebugTextFits(const char* text, size_t capacity)
{
    if (text == nullptr || text[0] == '\0')
    {
        return false;
    }

    for (size_t index = 1; index < capacity; index++)
    {
        if (text[index] == '\0')
        {
            return true;
        }
    }

    return false;
}

static void DebugCopyText(char* destination, const char* source)
{
    while (*source != '\0')
    {
        *destination++ = *source++;
    }

    *destination = '\0';
}

static bool DebugSameText(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        left++;
        right++;
    }

    return *left == *right;
}

DebugManager::DebugManager()
    : magic(ARX_DEBUG_MAGIC), formatVersion(ARX_DEBUG_VERSION),
      capacity(ARX_DEBUG_CAPACITY), reserved(0), records{}
{
}

DebugManager::~DebugManager()
{
    InvalidateAll();
}

arx_debug_record_t* DebugManager::Find(const char* name)
{
    if (!DebugTextFits(name, ARX_DEBUG_NAME_SIZE))
    {
        return nullptr;
    }

    for (uint32_t index = 0; index < reserved; index++)
    {
        if (DebugSameText(records[index].name, name))
        {
            return &records[index];
        }
    }

    return nullptr;
}

DebugManager::Result DebugManager::Reserve(const char* name, arx_debug_record_t** record)
{
    if (!DebugTextFits(name, ARX_DEBUG_NAME_SIZE))
    {
        return Result::Invalid;
    }

    if (Find(name) != nullptr)
    {
        return Result::Duplicate;
    }

    if (reserved >= capacity)
    {
        return Result::Full;
    }

    *record = &records[reserved];
    (*record)->state = ARX_DEBUG_PUBLISHING;
    DebugCopyText((*record)->name, name);
    __atomic_store_n(&reserved, reserved + 1, __ATOMIC_RELEASE);

    return Result::Ok;
}

DebugManager::Result DebugManager::Fill(arx_debug_record_t* record,
                                      const char* typeName, const void* address,
                                      uint64_t size, uint32_t version,
                                      const arx_debug_group_t* group,
                                      const arx_debug_view_t* views, uint32_t viewCount)
{
    if (!DebugTextFits(typeName, ARX_DEBUG_TYPE_SIZE) || address == nullptr ||
        size == 0 || version == 0 || viewCount > ARX_DEBUG_CAPACITY ||
        (viewCount != 0 && views == nullptr))
    {
        record->reason = "invalid record metadata";
        __atomic_store_n(&record->state, ARX_DEBUG_UNAVAILABLE, __ATOMIC_RELEASE);
        return Result::Invalid;
    }

    DebugCopyText(record->typeName, typeName);
    record->address   = address;
    record->size      = size;
    record->version   = version;
    record->group     = group;
    record->views     = views;
    record->viewCount = viewCount;
    record->reason    = nullptr;
    __atomic_store_n(&record->state, ARX_DEBUG_READY, __ATOMIC_RELEASE);

    return Result::Ok;
}

DebugManager::Result DebugManager::Register(const char* name, const char* typeName,
                                          const void* address, uint64_t size,
                                          uint32_t version, const arx_debug_group_t* group,
                                          const arx_debug_view_t* views, uint32_t viewCount)
{
    arx_debug_record_t* record = nullptr;
    Result result = Reserve(name, &record);

    if (result != Result::Ok)
    {
        return result;
    }

    return Fill(record, typeName, address, size, version, group, views, viewCount);
}

DebugManager::Result DebugManager::DeclareUnavailable(const char* name, const char* reason)
{
    if (reason == nullptr)
    {
        return Result::Invalid;
    }

    arx_debug_record_t* record = nullptr;
    Result result = Reserve(name, &record);

    if (result != Result::Ok)
    {
        return result;
    }

    record->reason = reason;
    __atomic_store_n(&record->state, ARX_DEBUG_UNAVAILABLE, __ATOMIC_RELEASE);

    return Result::Ok;
}

DebugManager::Result DebugManager::Publish(const char* name, const char* typeName,
                                         const void* address, uint64_t size,
                                         uint32_t version, const arx_debug_group_t* group,
                                         const arx_debug_view_t* views, uint32_t viewCount)
{
    arx_debug_record_t* record = Find(name);

    if (record == nullptr)
    {
        return Result::NotFound;
    }

    if (record->state != ARX_DEBUG_UNAVAILABLE)
    {
        return Result::Duplicate;
    }

    __atomic_store_n(&record->state, ARX_DEBUG_PUBLISHING, __ATOMIC_RELEASE);

    return Fill(record, typeName, address, size, version, group, views, viewCount);
}

DebugManager::Result DebugManager::Withdraw(const char* name, const void* owner)
{
    arx_debug_record_t* record = Find(name);

    if (record == nullptr)
    {
        return Result::NotFound;
    }

    if (record->state != ARX_DEBUG_READY || record->address != owner)
    {
        return Result::WrongOwner;
    }

    __atomic_store_n(&record->state, ARX_DEBUG_WITHDRAWN, __ATOMIC_SEQ_CST);
    record->address = nullptr;
    record->group   = nullptr;
    record->views   = nullptr;

    return Result::Ok;
}

void DebugManager::InvalidateAll()
{
    for (uint32_t index = 0; index < reserved; index++)
    {
        __atomic_store_n(&records[index].state, ARX_DEBUG_WITHDRAWN, __ATOMIC_SEQ_CST);
        records[index].address = nullptr;
        records[index].group   = nullptr;
        records[index].views   = nullptr;
    }
}

#endif
