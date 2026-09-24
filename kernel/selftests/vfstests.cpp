#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

extern "C"
{
#include <klib/klib.h>
#include <selftests/selftests.h>
}

static void vfs_test_log_fail(const char* message, unsigned long long* failures)
{
    (*failures)++;
    kprintf("Arx kernel: vfs_selftest FAIL: %s\n", message);
}

extern "C" void run_vfs_selftests(void* logicLayerCaps)
{
    unsigned long long passes = 0;
    unsigned long long fails  = 0;

    kprintf("Arx kernel: vfs_selftest start\n");

    LogicLayerCaps* logicCaps = static_cast<LogicLayerCaps*>(logicLayerCaps);
    if (logicCaps == nullptr || logicCaps->virtualFileSystem == nullptr)
    {
        vfs_test_log_fail("missing virtual file system", &fails);
        goto done;
    }

    VirtualFileSystem* vfs = logicCaps->virtualFileSystem;

    dentry_t rootDentry  = {};
    dentry_t childDentry = {};

    dentry_key_t keys[2] = {
        {&rootDentry, (char*) "root"},
        {&childDentry, (char*) "child"},
    };

    if (!vfs->PutDentryKey(&keys[0]) || !vfs->PutDentryKey(&keys[1]))
    {
        vfs_test_log_fail("PutDentryKey failed", &fails);
    }
    else
    {
        passes++;
    }

    const uint32_t hashA1 = vfs->HashDentryKey(&keys[0]);
    const uint32_t hashA2 = vfs->HashDentryKey(&keys[0]);
    if (hashA1 != hashA2)
    {
        vfs_test_log_fail("HashDentryKey is not deterministic", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->HashDentryKey(nullptr) != 0)
    {
        vfs_test_log_fail("HashDentryKey(nullptr) should return 0", &fails);
    }
    else
    {
        passes++;
    }

    const dentry_key_t* child = vfs->GetDentryKeyByName("child");
    if (child == nullptr || child->dentry != &childDentry)
    {
        vfs_test_log_fail("GetDentryKeyByName failed to find existing key", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->GetDentryKeyByName("missing") != nullptr)
    {
        vfs_test_log_fail("GetDentryKeyByName should return nullptr for missing key", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->GetDentryKeyByName(nullptr) != nullptr)
    {
        vfs_test_log_fail("GetDentryKeyByName should reject null name", &fails);
    }
    else
    {
        passes++;
    }

done:
    kprintf("Arx kernel: vfs_selftest summary: pass=%llu fail=%llu\n", passes, fails);
    kprintf("Arx kernel: vfs_selftest RESULT=%s\n", fails == 0 ? "PASS" : "FAIL");
    if (fails == 0)
    {
        KDEBUG("vfs_selftest passed with %llu checks\n", passes);
    }
    else
    {
        KDEBUG("vfs_selftest failed with %llu checks\n", fails);
    }
}
