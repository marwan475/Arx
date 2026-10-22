#include "resourcetests.hpp"

#include <selftests/SelfTestManager.hpp>

void test_acpi_host(bool scheduled);
void test_acpi_event_queue(void);

void run_resource_selftest_group(ResourceLayerCaps* resources)
{
    SelfTestManager::Get().GroupBegin("resource");
    test_PhysicalMemoryManager(resources);
    test_DmaManager(resources);
    test_DeviceManager(resources);
    test_AcpiManager(resources);
    test_PciManager(resources);
    test_VirtualMemoryManager(resources);
    test_ProcessManager(resources);
    test_TaskManager(resources);
    test_TimeManager(resources);
    test_InitRamFileSystemManager(resources);
    test_ResourceFileSystem(resources);
    SelfTestManager::Get().GroupEnd("resource");
}

void run_pci_discovery_selftest_group(ResourceLayerCaps* resources)
{
    SelfTestManager::Get().GroupBegin("pci-discovery");
    test_PciManagerPlatform(resources);
    test_AcpiManagerPlatform(resources);
    test_acpi_host(false);
    test_acpi_event_queue();
    test_DmaManagerPlatform(resources);
    SelfTestManager::Get().GroupEnd("pci-discovery");
}
