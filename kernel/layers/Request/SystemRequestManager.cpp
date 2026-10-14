#include "layers/Request/SystemRequestManager.hpp"

#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"
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
};
static_assert(sizeof(linux_utsname_t) == 390, "Linux utsname ABI");
static_assert(sizeof(linux_sysinfo_t) == 112 && offsetof(linux_sysinfo_t, totalhigh) == 88 && offsetof(linux_sysinfo_t, mem_unit) == 104, "Linux LP64 sysinfo ABI");

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

SystemRequestManager::SystemRequestManager(ResourceLayerCaps* resourceLayerCaps) : ResourceCaps(resourceLayerCaps)
{
}

uint64_t SystemRequestManager::HandleArch_prctlRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

#if defined(__x86_64__)
    const int32_t operation = (int32_t) frame->arg0;

    if (operation < 0x1001 || operation > 0x1004)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    const bool gs = operation == 0x1001 || operation == 0x1004;

    if (operation == 0x1001 || operation == 0x1002)
    {
        // The kernel uses four-level paging; reject noncanonical and kernel bases.

        if (frame->arg1 >= (1ULL << 47))
            return (uint64_t) -1; // EPERM
        return ResourceCaps->taskManager->SetCurrentTlsBase(gs, frame->arg1) ? 0 : LINUX_ESRCH;
    }

    uint64_t value;

    if (!ResourceCaps->taskManager->GetCurrentTlsBase(gs, &value))
    {
        return LINUX_ESRCH;
    }

    return safe_write_user(frame->arg1, &value, sizeof(value)) ? 0 : LINUX_EFAULT;
#else
    return LINUX_ENOSYS;
#endif
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

    return ResourceCaps->processManager->ExchangeUmask(currentProcess, (uint32_t) frame->arg0);
}

uint64_t SystemRequestManager::HandlePrlimit64Request(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    return HandleFileLimitRequest((int32_t) frame->arg0, (uint32_t) frame->arg1, frame->arg2, frame->arg3);
}

uint64_t SystemRequestManager::HandleGetrlimitRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (frame->arg1 == 0)
    {
        return LINUX_EFAULT;
    }

    return HandleFileLimitRequest(0, (uint32_t) frame->arg0, 0, frame->arg1);
}

uint64_t SystemRequestManager::HandleSetrlimitRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (frame->arg1 == 0)
    {
        return LINUX_EFAULT;
    }

    return HandleFileLimitRequest(0, (uint32_t) frame->arg0, frame->arg1, 0);
}

uint64_t SystemRequestManager::HandleFileLimitRequest(int32_t pid, uint32_t resource, uint64_t proposed, uint64_t previous)
{
    if (resource >= 16)
    {
        return LINUX_EINVAL;
    }

    if (resource != 7) // RLIMIT_NOFILE is the only enforced resource.
        return LINUX_EOPNOTSUPP;

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    ProcessManager*     manager = ResourceCaps->processManager;
    FileDescriptorGuard guard(manager->GetCurrentProcess());
    process_t*          process = manager->GetCurrentProcess();

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (pid < 0)
    {
        return LINUX_ESRCH;
    }

    if (pid != 0 && (uint64_t) pid != process->id)
    {
        return LINUX_EOPNOTSUPP;
    }

    process_file_limit_t next = {}, old = {};

    if (proposed != 0 && !safe_read_user(proposed, &next, sizeof(next)))
    {
        return LINUX_EFAULT;
    }

    // Linux commits the new limit before copying the previous limit out.
    // Snapshot input first so new/old pointers may alias.
    const uint64_t result = manager->ChangeFileLimit(process, proposed != 0 ? &next : nullptr, &old);

    if (result != 0)
    {
        return result;
    }

    if (previous != 0 && !safe_write_user(previous, &old, sizeof(old)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t SystemRequestManager::HandlePrctlRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    const int32_t operation = (int32_t) frame->arg0;

    if (operation != 15 && operation != 16) // PR_SET_NAME, PR_GET_NAME
        return LINUX_EOPNOTSUPP;

    if (ResourceCaps == nullptr || ResourceCaps->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    char name[16] = {};

    if (operation == 16)
    {
        if (!ResourceCaps->taskManager->GetCurrentName(name))
        {
            return LINUX_ESRCH;
        }

        return safe_write_user(frame->arg1, name, sizeof(name)) ? 0 : LINUX_EFAULT;
    }

    // Read only through the first NUL or 15 bytes: a valid short string at
    // the end of a page must not require the following page to be mapped.

    for (size_t i = 0; i < 15; ++i)
    {
        if (frame->arg1 > UINT64_MAX - i || !safe_read_user(frame->arg1 + i, &name[i], 1))
        {
            return LINUX_EFAULT;
        }

        if (name[i] == '\0')
        {
            break;
        }
    }

    return ResourceCaps->taskManager->SetCurrentName(name) ? 0 : LINUX_ESRCH;
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

    if (!safe_write_user(userUts, &utsData, sizeof(utsData)))
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

    if (ResourceCaps->timeManager == nullptr || ResourceCaps->taskManager == nullptr || ResourceCaps->physicalMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    time_timespec_t elapsed     = {};
    const uint64_t  clockResult = ResourceCaps->timeManager->ReadClock(1, &elapsed);

    if (clockResult != 0)
    {
        return clockResult;
    }

    uint64_t totalMemoryBytes = 0;
    uint64_t freeMemoryBytes  = 0;

    ResourceCaps->physicalMemoryManager->GetMemoryInfo(&totalMemoryBytes, &freeMemoryBytes);

    linux_sysinfo_t sysinfoData = {};
    memset(&sysinfoData, 0, sizeof(sysinfoData));
    sysinfoData.uptime    = elapsed.seconds + (elapsed.nanoseconds != 0);
    sysinfoData.totalram  = totalMemoryBytes;
    sysinfoData.freeram   = freeMemoryBytes;
    sysinfoData.sharedram = 0;
    sysinfoData.bufferram = 0;
    sysinfoData.totalswap = 0;
    sysinfoData.freeswap  = 0;
    sysinfoData.procs     = (uint16_t) ResourceCaps->taskManager->CountTasks();
    sysinfoData.totalhigh = 0;
    sysinfoData.freehigh  = 0;
    sysinfoData.mem_unit  = 1;

    if (!safe_write_user(userSysinfo, &sysinfoData, sizeof(sysinfoData)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t SystemRequestManager::HandleGetrandomRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    const uint32_t flags = (uint32_t) frame->arg2;

    if ((flags & ~7U) != 0 || (flags & 6U) == 6U)
    {
        return LINUX_EINVAL;
    }

    // No entropy source or CSPRNG exists. Never return predictable bytes.
    return LINUX_ENOSYS;
}
