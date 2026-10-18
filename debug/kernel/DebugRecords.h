#ifndef ARX_DEBUG_RECORDS_H
#define ARX_DEBUG_RECORDS_H

// These macros erase both storage and argument evaluation in release builds.
#if DEBUG

#include <stddef.h>
#include <stdint.h>

#define ARX_DEBUG_ONLY(...) __VA_ARGS__
#define ARX_DEBUG_RUN(...) do { __VA_ARGS__; } while (0)
#define ARX_DEBUG_ADD(counter, amount) \
    arx_debug_counter_add(&(counter), (amount))
#define ARX_DEBUG_STORE(counter, value) \
    arx_debug_counter_store(&(counter), (value))

#define ARX_DEBUG_MAGIC UINT64_C(0x4152584445425547)
#define ARX_DEBUG_VERSION 1
#define ARX_DEBUG_CAPACITY 64
#define ARX_DEBUG_NAME_SIZE 64
#define ARX_DEBUG_TYPE_SIZE 128

typedef struct arx_debug_group
{
    uint64_t activeWriters;
    uint64_t completedUpdates;
} arx_debug_group_t;

typedef enum arx_debug_state
{
    ARX_DEBUG_EMPTY,
    ARX_DEBUG_PUBLISHING,
    ARX_DEBUG_READY,
    ARX_DEBUG_UNAVAILABLE,
    ARX_DEBUG_WITHDRAWN
} arx_debug_state_t;

// Native readers compile against matching headers and implement semanticsId.
// The root's stable lifetime and each count's meaning are part of that contract.
typedef struct arx_debug_view
{
    const char* name;
    const char* rootType;
    const char* semanticsId;
    uint32_t    version;
    const void* root;
    uint64_t    rootSize;
    uint64_t    count;
    uint64_t    capacity;
} arx_debug_view_t;

typedef struct arx_debug_record
{
    uint32_t                 state;
    char                     name[ARX_DEBUG_NAME_SIZE];
    char                     typeName[ARX_DEBUG_TYPE_SIZE];
    const void*              address;
    uint64_t                 size;
    uint32_t                 version;
    const arx_debug_group_t* group;
    const arx_debug_view_t*  views;
    uint32_t                 viewCount;
    const char*              reason;
} arx_debug_record_t;

static inline void arx_debug_counter_add(uint64_t* counter, uint64_t amount)
{
    __atomic_fetch_add(counter, amount, __ATOMIC_RELAXED);
}

static inline void arx_debug_counter_store(uint64_t* counter, uint64_t value)
{
    __atomic_store_n(counter, value, __ATOMIC_RELAXED);
}

static inline uint64_t arx_debug_counter_read(const uint64_t* counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

// Multiwriter markers, not a seqlock. Provider fields still need synchronization.
// Sequential consistency prevents moving payload operations outside the group.
static inline void arx_debug_group_begin(arx_debug_group_t* group)
{
    __atomic_fetch_add(&group->activeWriters, 1, __ATOMIC_SEQ_CST);
}

static inline void arx_debug_group_end(arx_debug_group_t* group)
{
    __atomic_fetch_add(&group->completedUpdates, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_sub(&group->activeWriters, 1, __ATOMIC_SEQ_CST);
}

#else

#define ARX_DEBUG_ONLY(...)
#define ARX_DEBUG_RUN(...) do { } while (0)
#define ARX_DEBUG_ADD(counter, amount) do { } while (0)
#define ARX_DEBUG_STORE(counter, value) do { } while (0)

#endif
#endif
