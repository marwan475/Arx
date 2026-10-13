#pragma once

#include <stdint.h>

class ProcessManager;
class VirtualMemoryManager;
struct process_t;

class ProcessMemoryManager
{
public:
    ProcessMemoryManager(ProcessManager* processManager, VirtualMemoryManager* virtualMemoryManager);

    uint64_t MapProcessMemory(uint64_t address, uint64_t length, uint32_t protection, uint32_t flags, int fd, uint64_t offset);
    uint64_t UnmapProcessMemory(uint64_t address, uint64_t length);
    uint64_t ProtectProcessMemory(uint64_t address, uint64_t length, uint32_t protection);
    uint64_t QueryProcessMemoryResidency(uint64_t address, uint64_t length, uint64_t vector);
    uint64_t AdviseProcessMemory(uint64_t address, uint64_t length, int advice);
    uint64_t ResizeProcessBreak(uint64_t address);

private:
    process_t* BeginMemoryOperation();
    uint64_t EndMemoryOperation(process_t* process, uint64_t result);

    ProcessManager*        ProcessManagerImport;
    VirtualMemoryManager* VirtualMemoryManagerImport;
};
