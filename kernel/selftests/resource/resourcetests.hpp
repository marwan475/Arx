#pragma once

struct ResourceLayerCaps;
void run_resource_selftest_group(ResourceLayerCaps* resources);
void test_PhysicalMemoryManager(ResourceLayerCaps* resources);
void test_VirtualMemoryManager(ResourceLayerCaps* resources);
void test_ProcessManager(ResourceLayerCaps* resources);
void test_TaskManager(ResourceLayerCaps* resources);
void test_TimeManager(ResourceLayerCaps* resources);
void test_InitRamFileSystemManager(ResourceLayerCaps* resources);
void test_ResourceFileSystem(ResourceLayerCaps* resources);
