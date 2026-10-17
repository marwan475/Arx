#include "layers/Request/MemoryRequestManager.hpp"

#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Logic/ProcessController.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"

#include <klib/klib.h>

extern "C"
{
#include <memory/vmm.h>
}

namespace
{
static bool valid_mapping_length(uint64_t length)
{
    return length != 0 && length <= align_down(CANONICAL_USER_END, PAGE_SIZE);
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

    if ((frame->arg3 & (0x10 | 0x100000)) != 0 && ((frame->arg0 & 4095) != 0 || !range_within_u64(frame->arg0, frame->arg1, CANONICAL_USER_BASE, CANONICAL_USER_END)))
    {
        return LINUX_EINVAL;
    }

    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->MapProcessMemory(frame->arg0, frame->arg1, frame->arg2, frame->arg3, static_cast<int>(frame->arg4), frame->arg5);
}

uint64_t MemoryRequestManager::HandleMunmapRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0 || !valid_mapping_length(frame->arg1) || !range_within_u64(frame->arg0, frame->arg1, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return LINUX_EINVAL;
    }

    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->UnmapProcessMemory(frame->arg0, frame->arg1);
}

uint64_t MemoryRequestManager::HandleMprotectRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0 || (frame->arg2 & ~7ULL) != 0 || !range_within_u64(frame->arg0, frame->arg1, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return LINUX_EINVAL;
    }

    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->ProtectProcessMemory(frame->arg0, frame->arg1, frame->arg2);
}

uint64_t MemoryRequestManager::HandleMincoreRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0)
    {
        return LINUX_EINVAL;
    }

    if (!range_within_u64(frame->arg0, frame->arg1, CANONICAL_USER_BASE, CANONICAL_USER_END))
    {
        return static_cast<uint64_t>(-12);
    }

    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->QueryProcessMemoryResidency(frame->arg0, frame->arg1, frame->arg2);
}

uint64_t MemoryRequestManager::HandleMadviseRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr || (frame->arg0 & 4095) != 0 || !range_within_u64(frame->arg0, frame->arg1, CANONICAL_USER_BASE, CANONICAL_USER_END) || frame->arg2 > 4)
    {
        // MADV_FREE needs lazy discard and dirty-page tracking.
        return LINUX_EINVAL;
    }

    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return LogicCaps->processController->AdviseProcessMemory(frame->arg0, frame->arg1, frame->arg2);
}

uint64_t MemoryRequestManager::HandleBrkRequest(const arch_syscall_frame_t* frame)
{
    if (LogicCaps == nullptr || LogicCaps->processController == nullptr)
    {
        return 0;
    }

    return LogicCaps->processController->ResizeProcessBreak(frame != nullptr ? frame->arg0 : 0);
}
