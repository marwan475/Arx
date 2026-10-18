#include "Foundation.hpp"
#include "DebugManager.hpp"
#include "stats/FoundationCStats.h"
#include "stats/FoundationStats.hpp"

#if DEBUG

// Controlled examples, not subsystem telemetry. Original pointers stay valid for
// the kernel lifetime, and isolated tests never publish into the live registry.
FoundationCppStats arx_foundation_cpp_stats;
static arx_debug_view_t foundationViews[6];

void DebugFoundationDeclare(DebugManager& manager)
{
    manager.DeclareUnavailable("fixture-c", "C fixture not published yet");
    manager.DeclareUnavailable("fixture-cpp", "C++ fixture not published yet");
}

void DebugFoundationPublishC(DebugManager& manager)
{
    arx_foundation_c_update();
    arx_foundation_c_stats.phase = FOUNDATION_READY;
    arx_foundation_c_stats.usage.samples[0] = 10;
    arx_foundation_c_stats.usage.samples[1] = 20;
    arx_foundation_c_stats.usage.samples[2] = 30;

    manager.Publish("fixture-c", "foundation_c_stats_t", &arx_foundation_c_stats,
                    sizeof(arx_foundation_c_stats), 1);
}

void DebugFoundationPublishCpp(DebugManager& manager)
{
    FoundationCollections& collections = arx_foundation_cpp_stats.collections;
    collections.count = 4;
    collections.head = &collections.nodes[0];
    collections.indexHead = 0;
    collections.label = "controlled foundation fixture";
    collections.buckets[0] = &collections.nodes[0];
    collections.buckets[1] = &collections.nodes[2];

    for (uint32_t index = 0; index < 4; index++)
    {
        collections.nodes[index].value = index + 1;
        collections.nodes[index].next = index < 3 ? &collections.nodes[index + 1] : nullptr;
        collections.nodes[index].nextIndex = index < 3 ? (int32_t) index + 1 : -1;
    }

    const char* names[] = {"array", "list", "indices", "string", "buckets", "graph"};

    for (uint32_t index = 0; index < 6; index++)
    {
        foundationViews[index] = {names[index], "FoundationCollections",
                                  "arx.foundation.collections", 1, &collections,
                                  sizeof(collections), 4, 4};
    }

    manager.Publish("fixture-cpp", "FoundationCppStats", &arx_foundation_cpp_stats,
                    sizeof(arx_foundation_cpp_stats), 1,
                    &arx_foundation_cpp_stats.publication, foundationViews, 6);
}

void DebugFoundationRuntimeUpdate()
{
    arx_debug_group_begin(&arx_foundation_cpp_stats.publication);
    ARX_DEBUG_ADD(arx_foundation_cpp_stats.runtimeCalls, 1);
    arx_foundation_c_update();
    arx_debug_group_end(&arx_foundation_cpp_stats.publication);
}

#endif
