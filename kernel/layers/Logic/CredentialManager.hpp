#pragma once

#include "layers/Resource/ProcessManager.hpp"

struct ResourceLayerCaps;

class CredentialManager
{
public:
    enum change_t
    {
        SET_ID,
        SET_RE_ID,
        SET_RES_ID,
        SET_FS_ID
    };

    explicit CredentialManager(ResourceLayerCaps* resourceLayerCaps);
    uint64_t GetCredentials(process_credentials_t* credentials);
    uint64_t ChangeIdentity(bool group, change_t change, uint32_t real, uint32_t effective = UINT32_MAX, uint32_t saved = UINT32_MAX);
    uint64_t GetGroups(uint32_t* groups, uint32_t capacity, uint32_t* count);
    uint64_t MaySetGroups();
    uint64_t SetGroups(const uint32_t* groups, uint32_t count);

private:
    ProcessManager* Processes;
};
