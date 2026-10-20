#pragma once
#include "ProcessDebugStats.hpp"

#if DEBUG

// Credentials remain inline in process_t; supplementary lists are immutable
// shared objects (null=empty). This record is counters, not another database.
// SET_FS_ID can silently refuse while returning the old ID; count the decision.
struct CredentialDebugStats
{
    arx_debug_group_t group;
    DebugOperationStats identities;
    DebugOperationStats groups;
};

#endif
