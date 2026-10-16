#pragma once

struct ResourceLayerCaps;

struct LogicLayerCaps;
void run_logic_selftest_group(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_Scheduler(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_VirtualFileSystem(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_ElfMapper(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_CredentialManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_EventManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_SynchronizationManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_SignalManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_SocketManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_InterProcessCommunicationManager(ResourceLayerCaps* resources, LogicLayerCaps* logic);
void test_ProcessController(ResourceLayerCaps* resources, LogicLayerCaps* logic);
