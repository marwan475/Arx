#include "layers/Request/SystemRequestManager.hpp"

#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <boot/boot.h>
#include <klib/klib.h>
#include <platform.h>
}

namespace
{
struct linux_utsname_t
{
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct linux_sysinfo_t
{
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    char     _f[0];
};

static void copy_uts_field(char* destination, size_t destinationSize, const char* source)
{
    if (destination == nullptr || destinationSize == 0)
    {
        return;
    }

    memset(destination, 0, destinationSize);
    if (source == nullptr)
    {
        return;
    }

    size_t sourceLength = strlen(source);
    if (sourceLength >= destinationSize)
    {
        sourceLength = destinationSize - 1;
    }

    memcpy(destination, source, sourceLength);
}
} // namespace

SystemRequestManager::SystemRequestManager(ResourceLayerCaps* resourceLayerCaps)
    : ResourceCaps(resourceLayerCaps)
{
}

uint64_t SystemRequestManager::HandleArch_prctlRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SystemRequestManager::HandleUmaskRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint32_t newMask = (uint32_t) frame->arg0 & 0777U;
    const uint32_t oldMask = currentProcess->umask;
    currentProcess->umask  = newMask;
    return (uint64_t) oldMask;
}

uint64_t SystemRequestManager::HandlePrlimit64Request(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SystemRequestManager::HandleGetrlimitRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SystemRequestManager::HandleSetrlimitRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SystemRequestManager::HandlePrctlRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}

uint64_t SystemRequestManager::HandleUnameRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uintptr_t userUts = (uintptr_t) frame->arg0;
    if (userUts == 0)
    {
        return LINUX_EFAULT;
    }

    linux_utsname_t utsData = {};
    copy_uts_field(utsData.sysname, sizeof(utsData.sysname), "Arx");
    copy_uts_field(utsData.nodename, sizeof(utsData.nodename), "arx-kernel");
    copy_uts_field(utsData.release, sizeof(utsData.release), "0.1.0");
    copy_uts_field(utsData.version, sizeof(utsData.version), "Arx");
    copy_uts_field(utsData.machine, sizeof(utsData.machine), platform.arch == ARCH_AARCH64 ? "aarch64" : "x86_64");
    copy_uts_field(utsData.domainname, sizeof(utsData.domainname), "localdomain");

    if (!ResourceCaps->virtualMemoryManager->CopyToUser(userUts, &utsData, sizeof(utsData), currentProcess->addressSpace))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t SystemRequestManager::HandleSysinfoRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();
    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uintptr_t userSysinfo = (uintptr_t) frame->arg0;
    if (userSysinfo == 0)
    {
        return LINUX_EFAULT;
    }

    uint64_t totalMemoryBytes = 0;
    uint64_t freeMemoryBytes  = 0;

    for (size_t node = 0; node < platform.numa_node_count; ++node)
    {
        totalMemoryBytes += (uint64_t) platform.numa_nodes[node].zone.total_memory;
        freeMemoryBytes += (uint64_t) platform.numa_nodes[node].zone.free_pages * PAGE_SIZE;
    }

    linux_sysinfo_t sysinfoData = {};
    sysinfoData.uptime          = 0;
    sysinfoData.totalram        = totalMemoryBytes;
    sysinfoData.freeram         = freeMemoryBytes;
    sysinfoData.sharedram       = 0;
    sysinfoData.bufferram       = 0;
    sysinfoData.totalswap       = 0;
    sysinfoData.freeswap        = 0;
    sysinfoData.procs           = (uint16_t) platform.cpu_count;
    sysinfoData.totalhigh       = 0;
    sysinfoData.freehigh        = 0;
    sysinfoData.mem_unit        = 1;

    if (!ResourceCaps->virtualMemoryManager->CopyToUser(userSysinfo, &sysinfoData, sizeof(sysinfoData), currentProcess->addressSpace))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t SystemRequestManager::HandleGetrandomRequest(const arch_syscall_frame_t* frame)
{
    (void) frame;
    return (uint64_t) -38;
}
