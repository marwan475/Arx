#include "layers/Request/CredentialRequestManager.hpp"

#include "layers/Logic/CredentialManager.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <klib/klib.h>

CredentialRequestManager::CredentialRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps) : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

CredentialManager* CredentialRequestManager::GetManager() const
{
    return LogicCaps != nullptr ? LogicCaps->credentialManager : nullptr;
}

uint64_t CredentialRequestManager::GetIdentity(const arch_syscall_frame_t* frame, bool group, bool effective)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    CredentialManager* manager = GetManager();

    if (manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_credentials_t credentials;
    uint64_t              result = manager->GetCredentials(&credentials);

    if (result != 0)
    {
        return result;
    }

    const process_identity_t& identity = group ? credentials.group : credentials.user;
    return effective ? identity.effective : identity.real;
}

uint64_t CredentialRequestManager::GetResIdentity(const arch_syscall_frame_t* frame, bool group)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    CredentialManager* manager = GetManager();

    if (manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_credentials_t credentials;
    uint64_t              result = manager->GetCredentials(&credentials);

    if (result != 0)
    {
        return result;
    }

    const process_identity_t& identity = group ? credentials.group : credentials.user;
    // Linux writes in order and may update earlier outputs before a later fault.
    return safe_write_user(frame->arg0, &identity.real, sizeof(uint32_t)) && safe_write_user(frame->arg1, &identity.effective, sizeof(uint32_t)) && safe_write_user(frame->arg2, &identity.saved, sizeof(uint32_t)) ? 0 : LINUX_EFAULT;
}

uint64_t CredentialRequestManager::ChangeIdentity(const arch_syscall_frame_t* frame, bool group, uint32_t change)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    CredentialManager* manager = GetManager();

    if (manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    return manager->ChangeIdentity(group, static_cast<CredentialManager::change_t>(change), (uint32_t) frame->arg0, (uint32_t) frame->arg1, (uint32_t) frame->arg2);
}

uint64_t CredentialRequestManager::HandleGetuidRequest(const arch_syscall_frame_t* frame)
{
    return GetIdentity(frame, false, false);
}

uint64_t CredentialRequestManager::HandleGetgidRequest(const arch_syscall_frame_t* frame)
{
    return GetIdentity(frame, true, false);
}

uint64_t CredentialRequestManager::HandleGeteuidRequest(const arch_syscall_frame_t* frame)
{
    return GetIdentity(frame, false, true);
}

uint64_t CredentialRequestManager::HandleGetegidRequest(const arch_syscall_frame_t* frame)
{
    return GetIdentity(frame, true, true);
}

uint64_t CredentialRequestManager::HandleGetresuidRequest(const arch_syscall_frame_t* frame)
{
    return GetResIdentity(frame, false);
}

uint64_t CredentialRequestManager::HandleSetuidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, false, CredentialManager::SET_ID);
}

uint64_t CredentialRequestManager::HandleSetreuidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, false, CredentialManager::SET_RE_ID);
}

uint64_t CredentialRequestManager::HandleSetresuidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, false, CredentialManager::SET_RES_ID);
}

uint64_t CredentialRequestManager::HandleSetfsuidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, false, CredentialManager::SET_FS_ID);
}

uint64_t CredentialRequestManager::HandleGetresgidRequest(const arch_syscall_frame_t* frame)
{
    return GetResIdentity(frame, true);
}

uint64_t CredentialRequestManager::HandleSetgidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, true, CredentialManager::SET_ID);
}

uint64_t CredentialRequestManager::HandleSetregidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, true, CredentialManager::SET_RE_ID);
}

uint64_t CredentialRequestManager::HandleSetresgidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, true, CredentialManager::SET_RES_ID);
}

uint64_t CredentialRequestManager::HandleSetfsgidRequest(const arch_syscall_frame_t* frame)
{
    return ChangeIdentity(frame, true, CredentialManager::SET_FS_ID);
}

uint64_t CredentialRequestManager::HandleGetgroupsRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    CredentialManager* manager = GetManager();

    if (manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    const int32_t capacity = (int32_t) frame->arg0;

    if (capacity < 0)
    {
        return LINUX_EINVAL;
    }

    uint32_t count  = 0;
    uint64_t result = manager->GetGroups(nullptr, 0, &count);

    if (result != 0 || capacity == 0 || count == 0)
    {
        return result != 0 ? result : count;
    }

    if ((uint32_t) capacity < count)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->physicalMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    const uint32_t bufferCount = (uint32_t) capacity < ProcessManager::MAX_SUPPLEMENTARY_GROUPS ? (uint32_t) capacity : ProcessManager::MAX_SUPPLEMENTARY_GROUPS;
    uint32_t*      buffer      = static_cast<uint32_t*>(ResourceCaps->physicalMemoryManager->Alloc(bufferCount * sizeof(uint32_t)));

    if (buffer == nullptr)
    {
        return LINUX_ENOMEM;
    }

    result = manager->GetGroups(buffer, bufferCount, &count);

    if (result == 0 && count != 0 && !safe_write_user(frame->arg1, buffer, count * sizeof(uint32_t)))
    {
        result = LINUX_EFAULT;
    }

    ResourceCaps->physicalMemoryManager->Free(buffer);
    return result != 0 ? result : count;
}

uint64_t CredentialRequestManager::HandleSetgroupsRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    CredentialManager* manager = GetManager();

    if (manager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    uint64_t result = manager->MaySetGroups();

    if (result != 0)
    {
        return result;
    }

    const uint32_t count = (uint32_t) frame->arg0;

    if (count > ProcessManager::MAX_SUPPLEMENTARY_GROUPS)
    {
        return LINUX_EINVAL;
    }

    if (count == 0)
    {
        return manager->SetGroups(nullptr, 0);
    }

    if (ResourceCaps == nullptr || ResourceCaps->physicalMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    uint32_t* buffer = static_cast<uint32_t*>(ResourceCaps->physicalMemoryManager->Alloc(count * sizeof(uint32_t)));

    if (buffer == nullptr)
    {
        return LINUX_ENOMEM;
    }

    result = safe_read_user(frame->arg1, buffer, count * sizeof(uint32_t)) ? manager->SetGroups(buffer, count) : LINUX_EFAULT;
    ResourceCaps->physicalMemoryManager->Free(buffer);
    return result;
}
