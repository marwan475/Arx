#include "resourcetests.hpp"

#include <selftests/SelfTestManager.hpp>

void run_resource_selftest_group(ResourceLayerCaps* resources)
{
    SelfTestManager::Get().GroupBegin("resource");
    test_PhysicalMemoryManager(resources);
    test_DmaManager(resources);
    test_VirtualMemoryManager(resources);
    test_ProcessManager(resources);
    test_TaskManager(resources);
    test_TimeManager(resources);
    test_InitRamFileSystemManager(resources);
    test_ResourceFileSystem(resources);
    SelfTestManager::Get().GroupEnd("resource");
}
