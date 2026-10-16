#include "logictests.hpp"

#include <selftests/SelfTestManager.hpp>

void run_logic_selftest_group(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    SelfTestManager::Get().GroupBegin("logic");
    test_Scheduler(resources, logic);
    test_VirtualFileSystem(resources, logic);
    test_ElfMapper(resources, logic);
    test_CredentialManager(resources, logic);
    test_EventManager(resources, logic);
    test_SynchronizationManager(resources, logic);
    test_SignalManager(resources, logic);
    test_SocketManager(resources, logic);
    test_InterProcessCommunicationManager(resources, logic);
    test_ProcessController(resources, logic);
    SelfTestManager::Get().GroupEnd("logic");
}
