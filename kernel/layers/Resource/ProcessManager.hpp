#pragma once

#include <arch/arch.h>
#include <boot/boot.h>
#include "layers/Resource/TaskManager.hpp"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#include <memory/vmm.h>
}
#endif

struct process_t
{
    bool               allocated;
    uint64_t           id;
    virt_addr_space_t* addressSpace;
    task_t*            tasks;
};

class ProcessManager
{
public:
    static constexpr size_t MAX_PROCESSES = 64;

    ProcessManager();

    process_t* AllocateProcess();
    process_t* CreateProcess(virt_addr_space_t* addressSpace);
    bool       FreeProcess(process_t* process);
    bool       AddTask(process_t* process, task_t* task);
    bool       ActivateProcessAddressSpace(process_t* process);
    task_t*    GetTasks(process_t* process) const;

    process_t* GetRunningProcess(uint8_t cpuId) const;
    bool       SetRunningProcess(uint8_t cpuId, process_t* process);
    process_t* GetCurrentProcess() const;

    process_t*       GetProcesses();
    const process_t* GetProcesses() const;

    size_t GetCapacity() const;

private:
    process_t  Processes[MAX_PROCESSES];
    process_t* RunningProcesses[BOOT_SMP_MAX_CPUS];
};
