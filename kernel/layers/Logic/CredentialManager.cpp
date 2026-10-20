#include "layers/Logic/CredentialManager.hpp"

#include "layers/Resource/ResourceLayerFactory.hpp"

#include <klib/klib.h>

namespace
{
constexpr uint64_t CREDENTIAL_EPERM = (uint64_t) -1;

bool identity_contains(const process_identity_t& identity, uint32_t id)
{
    return id == identity.real || id == identity.effective || id == identity.saved;
}

} // namespace

CredentialManager::CredentialManager(ResourceLayerCaps* resourceLayerCaps) : Processes(resourceLayerCaps != nullptr ? resourceLayerCaps->processManager : nullptr)
{
}

uint64_t CredentialManager::GetCredentials(process_credentials_t* credentials)
{
    if (Processes == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* process = Processes->AcquireCurrentCredentials();

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    *credentials = process->credentials;
    Processes->ReleaseCredentials(process);
    return 0;
}

uint64_t CredentialManager::ChangeIdentity(bool group, change_t change, uint32_t real, uint32_t effective, uint32_t saved)
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.identities);)
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (Processes == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (change == SET_ID && real == UINT32_MAX)
    {
        return LINUX_EINVAL;
    }

    process_t* process = Processes->AcquireCurrentCredentials();

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    const bool               privileged = process->credentials.user.effective == 0;
    process_identity_t&      current    = group ? process->credentials.group : process->credentials.user;
    const process_identity_t old        = current;
    process_identity_t       next       = old;
    uint64_t                 result     = 0;

    switch (change)
    {
        case SET_ID:

            if (privileged)
            {
                next.real = next.saved = real;
            }
            else if (real != old.real && real != old.saved)
            {
                result = CREDENTIAL_EPERM;
            }

            next.effective = next.filesystem = real;
            break;
        case SET_RE_ID:

            if (!privileged && ((real != UINT32_MAX && real != old.real && real != old.effective) || (effective != UINT32_MAX && !identity_contains(old, effective))))
            {
                result = CREDENTIAL_EPERM;
            }

            if (real != UINT32_MAX)
            {
                next.real = real;
            }

            if (effective != UINT32_MAX)
            {
                next.effective = effective;
            }

            if (real != UINT32_MAX || (effective != UINT32_MAX && effective != old.real))
            {
                next.saved = next.effective;
            }

            next.filesystem = next.effective;
            break;
        case SET_RES_ID:

            if ((real == UINT32_MAX || real == old.real) && (effective == UINT32_MAX || (effective == old.effective && effective == old.filesystem)) && (saved == UINT32_MAX || saved == old.saved))
            {
                break;
            }

            if (!privileged && ((real != UINT32_MAX && !identity_contains(old, real)) || (effective != UINT32_MAX && !identity_contains(old, effective)) || (saved != UINT32_MAX && !identity_contains(old, saved))))
            {
                result = CREDENTIAL_EPERM;
            }

            if (real != UINT32_MAX)
            {
                next.real = real;
            }

            if (effective != UINT32_MAX)
            {
                next.effective = effective;
            }

            if (saved != UINT32_MAX)
            {
                next.saved = saved;
            }

            next.filesystem = next.effective;
            break;
        case SET_FS_ID:
            result = old.filesystem; // Linux silently refuses disallowed changes.

            if (real != UINT32_MAX && (privileged || identity_contains(old, real) || real == old.filesystem))
            {
                current.filesystem = real;
            }

            ARX_DEBUG_RUN(
                const bool allowed = real == UINT32_MAX || privileged || identity_contains(old, real) || real == old.filesystem;
                debugOperation.Finish(!allowed ? 3 : current.filesystem != old.filesystem ? 0 : 1);
            );
            Processes->ReleaseCredentials(process);
            return result;
        default:
            result = LINUX_EINVAL;
            break;
    }

    if (result == 0)
    {
        current = next;
        ARX_DEBUG_RUN(
            const bool changed = old.real != next.real || old.effective != next.effective
                                 || old.saved != next.saved || old.filesystem != next.filesystem;
            debugOperation.Finish(changed ? 0 : 1);
        );
    }

    Processes->ReleaseCredentials(process);
    return result;
}

uint64_t CredentialManager::GetGroups(uint32_t* groups, uint32_t capacity, uint32_t* count)
{
    if (Processes == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* process = Processes->AcquireCurrentCredentials();

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    const process_group_list_t* list = process->supplementaryGroups;
    *count                           = list != nullptr ? list->count : 0;
    uint64_t result                  = 0;

    if (capacity != 0)
    {
        if (capacity < *count)
        {
            result = LINUX_EINVAL;
        }
        else if (*count != 0)
        {
            memcpy(groups, list->ids, *count * sizeof(uint32_t));
        }
    }

    Processes->ReleaseCredentials(process);
    return result;
}

uint64_t CredentialManager::MaySetGroups()
{
    process_credentials_t credentials;
    uint64_t              result = GetCredentials(&credentials);
    return result != 0 ? result : credentials.user.effective == 0 ? 0 : CREDENTIAL_EPERM;
}

uint64_t CredentialManager::SetGroups(const uint32_t* groups, uint32_t count)
{
    ARX_DEBUG_ONLY(DebugOperation debugOperation(DebugStats.groups);)
    ARX_DEBUG_ONLY(DebugUpdate debugUpdate(DebugStats.group);)

    if (Processes == nullptr)
    {
        return LINUX_ENOSYS;
    }

    if (count > ProcessManager::MAX_SUPPLEMENTARY_GROUPS)
    {
        return LINUX_EINVAL;
    }

    uint64_t result = MaySetGroups();

    if (result != 0)
    {
        return result;
    }

    for (uint32_t i = 0; i < count; ++i)
    {
        if (groups[i] == UINT32_MAX)
        {
            return LINUX_EINVAL;
        }
    }

    process_group_list_t* next = count != 0 ? Processes->CreateGroupList(groups, count) : nullptr;

    if (count != 0 && next == nullptr)
    {
        return LINUX_ENOMEM;
    }

    if (next != nullptr)
    {
        sort_u32(next->ids, count);
    }

    process_t* process = Processes->AcquireCurrentCredentials();

    if (process == nullptr)
    {
        Processes->ReleaseGroupList(next);
        return LINUX_ESRCH;
    }

    process_group_list_t* previous = nullptr;
    // Recheck after allocation; a concurrent credential change cannot bypass policy.

    if (process->credentials.user.effective != 0)
    {
        result = CREDENTIAL_EPERM;
    }
    else
    {
        previous                     = process->supplementaryGroups;
        ARX_DEBUG_ONLY(
            const uint32_t previousCount = previous != nullptr ? previous->count : 0;
            const bool changed = previousCount != count
                || (count != 0 && memcmp(previous->ids, next->ids, count * sizeof(uint32_t)) != 0);
        )
        process->supplementaryGroups = next;
        next                         = nullptr;
        ARX_DEBUG_RUN(debugOperation.Finish(changed ? 0 : 1));
    }

    Processes->ReleaseCredentials(process);
    Processes->ReleaseGroupList(previous);
    Processes->ReleaseGroupList(next);
    return result;
}
