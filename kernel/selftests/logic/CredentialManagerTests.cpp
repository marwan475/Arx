#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/CredentialManager.hpp"

void test_CredentialManager(ResourceLayerCaps* resources, LogicLayerCaps* logic)
{
    auto* m = logic ? logic->credentialManager : nullptr;
    ManagerSuite suite("logic/CredentialManager", m != nullptr);
    suite.Run("logic/CredentialManager/identity_query_change", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        process_credentials_t original = {}, current = {};
        t.Equal(m->GetCredentials(&original), 0, "read owned credentials");
        t.Equal(m->ChangeIdentity(false, CredentialManager::SET_RES_ID, UINT32_MAX, UINT32_MAX, UINT32_MAX), 0, "no-op identity change");
        t.Equal(m->GetCredentials(&current), 0, "read after no-op");
        t.Check(memcmp(&original, &current, sizeof(current)) == 0, "no-op preserves credentials");
        t.Equal(m->ChangeIdentity(false, CredentialManager::SET_ID, UINT32_MAX), LINUX_EINVAL, "invalid uid rejected");
    });

    suite.Run("logic/CredentialManager/groups", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        uint32_t groups[] = {13, 27}, count = 0, actual[2] = {};

        if (!t.Equal(m->MaySetGroups(), 0, "disposable root identity may set groups"))
        {
            return;
        }

        t.Equal(m->SetGroups(groups, 2), 0, "set supplementary groups");
        t.Equal(m->GetGroups(actual, 2, &count), 0, "read supplementary groups");
        t.Equal(count, 2, "supplementary group count");
        t.Check(actual[0] == 13 && actual[1] == 27, "supplementary group values");
        t.Equal(m->GetGroups(actual, 1, &count), LINUX_EINVAL, "short group buffer rejected");
    });

    suite.Run("logic/CredentialManager/unprivileged_rejection", [&](TestCase& t)
    {
        TestFixture f(resources);

        if (!f.Valid(t))
        {
            return;
        }

        if (!t.Equal(m->ChangeIdentity(false, CredentialManager::SET_ID, 1234), 0, "drop fixture privilege"))
        {
            return;
        }

        t.Equal(m->ChangeIdentity(false, CredentialManager::SET_ID, 0), (uint64_t) -1, "unprivileged identity cannot regain root");
        process_credentials_t current = {};
        m->GetCredentials(&current);
        t.Equal(current.user.effective, 1234, "rejected change preserves identity");
    });
}
