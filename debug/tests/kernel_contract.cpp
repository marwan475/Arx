#include <debug/kernel/DebugManager.hpp>
#include <debug/kernel/stats/FoundationCStats.h>
#include <debug/kernel/stats/FoundationStats.hpp>

#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>

static void CheckRegistry()
{
    DebugManager manager;
    foundation_c_stats_t cStats{};
    FoundationCppStats cppStats{};

    assert(manager.DeclareUnavailable("c", "not initialized") == DebugManager::Result::Ok);
    assert(manager.records[0].state == ARX_DEBUG_UNAVAILABLE);
    assert(manager.Publish("c", "foundation_c_stats_t", &cStats, sizeof(cStats), 1) == DebugManager::Result::Ok);
    assert(manager.Register("cpp", "FoundationCppStats", &cppStats, sizeof(cppStats), 1) == DebugManager::Result::Ok);
    assert(manager.Register("c", "FoundationCppStats", &cppStats, sizeof(cppStats), 1) == DebugManager::Result::Duplicate);
    assert(manager.Withdraw("c", &cppStats) == DebugManager::Result::WrongOwner);
    assert(manager.Withdraw("c", &cStats) == DebugManager::Result::Ok);
    assert(manager.records[0].state == ARX_DEBUG_WITHDRAWN);
    assert(manager.records[0].address == nullptr);
    assert(manager.Publish("c", "foundation_c_stats_t", &cStats, sizeof(cStats), 1) == DebugManager::Result::Duplicate);
    assert(manager.Register("c", "foundation_c_stats_t", &cStats, sizeof(cStats), 1) == DebugManager::Result::Duplicate);

    for (unsigned index = 2; index < ARX_DEBUG_CAPACITY; index++)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "provider-%u", index);
        assert(manager.Register(name, "foundation_c_stats_t", &cStats, sizeof(cStats), 1) == DebugManager::Result::Ok);
    }

    assert(manager.Register("overflow", "foundation_c_stats_t", &cStats, sizeof(cStats), 1) == DebugManager::Result::Full);
    manager.InvalidateAll();

    for (unsigned index = 0; index < ARX_DEBUG_CAPACITY; index++)
    {
        assert(manager.records[index].state == ARX_DEBUG_WITHDRAWN);
        assert(manager.records[index].address == nullptr);
    }
}

static void CheckConcurrentUpdates()
{
    uint64_t counter = 0;
    arx_debug_group_t group{};
    std::atomic<unsigned> started{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> writers;

    for (unsigned index = 0; index < 4; index++)
    {
        writers.emplace_back([&]()
        {
            arx_debug_group_begin(&group);
            started.fetch_add(1);

            while (!release.load())
            {
                std::this_thread::yield();
            }

            for (unsigned iteration = 0; iteration < 100000; iteration++)
            {
                ARX_DEBUG_ADD(counter, 1);
            }

            arx_debug_group_end(&group);
        });
    }

    while (started.load() != 4)
    {
        std::this_thread::yield();
    }

    // An even number of overlapping writers must still report in-progress.
    assert(__atomic_load_n(&group.activeWriters, __ATOMIC_SEQ_CST) == 4);
    assert(arx_debug_counter_read(&group.completedUpdates) == 0);
    release.store(true);

    for (auto& writer : writers)
    {
        writer.join();
    }

    assert(counter == 400000);
    assert(group.activeWriters == 0);
    assert(group.completedUpdates == 4);
}

int main()
{
    CheckRegistry();
    CheckConcurrentUpdates();
    std::puts("PASS registry lifecycle and concurrent counters/groups");
}
