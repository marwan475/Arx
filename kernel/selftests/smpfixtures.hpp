#pragma once

#include "layers/Resource/ProcessManager.hpp"

// FreeProcess temporarily pins a shared root with memoryUsers while reclaiming
// a peer fixture. CreateProcess may reject that root during the pin. Preserve
// the shared-root stress test, but allow bounded retries for this contention.
static inline process_t* smp_create_test_process(ProcessManager* manager, virt_addr_space_t* space)
{
    for (unsigned attempt = 0; attempt < 1024; ++attempt)
    {
        process_t* process = manager->CreateProcess(space);
        if (process != nullptr)
            return process;
        arch_pause();
    }
    return nullptr;
}
