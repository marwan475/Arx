#pragma once

#include "../DebugRecords.h"

#if DEBUG

struct FoundationNode
{
    uint64_t        value;
    FoundationNode* next;
    int32_t         nextIndex;
};

struct FoundationCollections
{
    FoundationNode  nodes[4];
    FoundationNode* head;
    int32_t         indexHead;
    uint64_t        count;
    const char*     label;
    FoundationNode* buckets[2];
};

struct FoundationCppStats
{
    uint64_t              runtimeCalls;
    arx_debug_group_t     publication;
    FoundationCollections collections;
};

#endif
