#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"

extern "C"
{
#include <klib/klib.h>
#include <selftests/selftests.h>
}

static void poststart_vfs_test_fail(const char* message, unsigned long long* failures)
{
    (*failures)++;
    kprintf("Arx kernel: poststart_vfs_selftest FAIL: %s\n", message);
}

extern "C" void run_poststart_vfs_selftests(void* logicLayerCaps)
{
    unsigned long long passes = 0;
    unsigned long long fails  = 0;

    kprintf("Arx kernel: poststart_vfs_selftest start\n");

    LogicLayerCaps* logicCaps = static_cast<LogicLayerCaps*>(logicLayerCaps);
    if (logicCaps == nullptr || logicCaps->virtualFileSystem == nullptr)
    {
        poststart_vfs_test_fail("missing virtual file system", &fails);
        goto done;
    }

    VirtualFileSystem* vfs = logicCaps->virtualFileSystem;

    vfs_path_t start = {};
    vfs_path_t resolved = {};
    if (!vfs->ResolvePath(start, "/test.c", &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        poststart_vfs_test_fail("ResolvePath should find mounted initramfs file /test.c", &fails);
    }
    else
    {
        passes++;
    }

    file_t* opened = vfs->Open(start, "/test.c", 0);
    if (opened == nullptr)
    {
        poststart_vfs_test_fail("Open should open mounted initramfs file /test.c", &fails);
        goto done;
    }
    passes++;

    char buffer[16] = {};
    const int64_t readResult = vfs->Read(opened, buffer, sizeof(buffer) - 1);
    if (readResult <= 0)
    {
        poststart_vfs_test_fail("Read should return bytes from mounted initramfs file", &fails);
    }
    else
    {
        passes++;
    }

    if (readResult > 0)
    {
        buffer[(readResult < (int64_t)(sizeof(buffer) - 1)) ? readResult : (int64_t)(sizeof(buffer) - 1)] = '\0';
        if (strncmp(buffer, "#include", 8) != 0)
        {
            poststart_vfs_test_fail("Read should begin with expected initramfs test.c prefix", &fails);
        }
        else
        {
            passes++;
        }
    }

    if (vfs->Close(opened) != 0)
    {
        poststart_vfs_test_fail("Close should succeed for mounted initramfs file", &fails);
    }
    else
    {
        passes++;
    }

done:
    kprintf("Arx kernel: poststart_vfs_selftest summary: pass=%llu fail=%llu\n", passes, fails);
    kprintf("Arx kernel: poststart_vfs_selftest RESULT=%s\n", fails == 0 ? "PASS" : "FAIL");
}
