#include "layers/Request/MemoryRequestManager.hpp"

#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/ProcessMemoryManager.hpp"
#include "layers/Request/RequestLayerFactory.hpp"

namespace
{
static bool valid_user_range(uint64_t address, uint64_t length)
{
    return address <= 0x7fffffffffffULL && length <= 0x800000000000ULL - address;
}

static bool valid_mapping_length(uint64_t length)
{
    return length != 0 && length <= 0x7ffffffff000ULL;
}
} // namespace

uint64_t MemoryRequestManager::HandleMmapRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || !valid_mapping_length(frame->arg1) || (frame->arg2 & ~7ULL) != 0 || (frame->arg5 & 4095) != 0)
    {
        return LINUX_EINVAL;
    }

    const uint64_t supportedFlags = 0x100000 | 0x10 | 0x20 | 3;
    if ((frame->arg3 & ~supportedFlags) != 0 || (frame->arg3 & 3) == 0)
    {
        return LINUX_EINVAL;
    }

    if ((frame->arg3 & (0x10 | 0x100000)) != 0 && ((frame->arg0 & 4095) != 0 || !valid_user_range(frame->arg0, frame->arg1)))
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return ResourceCaps->processMemoryManager->MapProcessMemory(frame->arg0, frame->arg1, frame->arg2, frame->arg3, static_cast<int>(frame->arg4), frame->arg5);
}

uint64_t MemoryRequestManager::HandleMunmapRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0 || !valid_mapping_length(frame->arg1) || !valid_user_range(frame->arg0, frame->arg1))
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return ResourceCaps->processMemoryManager->UnmapProcessMemory(frame->arg0, frame->arg1);
}

uint64_t MemoryRequestManager::HandleMprotectRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0 || (frame->arg2 & ~7ULL) != 0 || !valid_user_range(frame->arg0, frame->arg1))
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return ResourceCaps->processMemoryManager->ProtectProcessMemory(frame->arg0, frame->arg1, frame->arg2);
}

uint64_t MemoryRequestManager::HandleMincoreRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0)
    {
        return LINUX_EINVAL;
    }

    if (!valid_user_range(frame->arg0, frame->arg1))
    {
        return static_cast<uint64_t>(-12);
    }

    if (ResourceCaps == nullptr || ResourceCaps->processMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return ResourceCaps->processMemoryManager->QueryProcessMemoryResidency(frame->arg0, frame->arg1, frame->arg2);
}

uint64_t MemoryRequestManager::HandleMadviseRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0 || !valid_user_range(frame->arg0, frame->arg1) || frame->arg2 > 4)
    {
        // MADV_FREE needs lazy discard and dirty-page tracking.
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return ResourceCaps->processMemoryManager->AdviseProcessMemory(frame->arg0, frame->arg1, frame->arg2);
}

uint64_t MemoryRequestManager::HandleBrkRequest(const arch_syscall_frame_t* frame)
{
    if (ResourceCaps == nullptr || ResourceCaps->processMemoryManager == nullptr)
    {
        return 0;
    }

    return ResourceCaps->processMemoryManager->ResizeProcessBreak(frame != nullptr ? frame->arg0 : 0);
}
