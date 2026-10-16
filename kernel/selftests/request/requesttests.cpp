#include "requesttests.hpp"
#include <selftests/SelfTestManager.hpp>

void run_request_selftest_group(void* requestLayerCaps)
{
    SelfTestManager& manager = SelfTestManager::Get();
    manager.GroupBegin("request");
    launch_syscall_selftests(requestLayerCaps);
    manager.GroupEnd("request");
}
