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

static inode_t g_lookup_child_inode;
static inode_t g_lookup_dir_inode;
static inode_t g_root_inode;
static inode_t g_mounted_root_inode;
static int     g_lookup_invocations = 0;
static int     g_file_open_invocations = 0;
static int     g_file_read_invocations = 0;
static int     g_file_release_invocations = 0;

static int64_t vfs_file_open_stub(file_t* file)
{
    if (file == nullptr)
    {
        return -1;
    }

    g_file_open_invocations++;
    return 0;
}

static int64_t vfs_file_read_stub(file_t* file, void* buffer, uint64_t count)
{
    if (file == nullptr || buffer == nullptr)
    {
        return -1;
    }

    g_file_read_invocations++;

    char* out = static_cast<char*>(buffer);
    if (count > 0)
    {
        out[0] = 'X';
    }

    return count > 0 ? 1 : 0;
}

static void vfs_file_release_stub(file_t* file)
{
    (void)file;
    g_file_release_invocations++;
}

static const file_operations_t g_file_ops = {
    vfs_file_open_stub,
    vfs_file_read_stub,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    vfs_file_release_stub,
};

static inode_t* vfs_lookup_stub(inode_t* directory, const char* name)
{
    if (name == nullptr)
    {
        return nullptr;
    }

    if (directory == &g_root_inode)
    {
        if (strcmp(name, "from-backend") == 0)
        {
            g_lookup_invocations++;
            return &g_lookup_child_inode;
        }

        if (strcmp(name, "dir") == 0)
        {
            g_lookup_invocations++;
            return &g_lookup_dir_inode;
        }

        return nullptr;
    }

    if (directory == &g_lookup_dir_inode)
    {
        if (strcmp(name, "file") == 0)
        {
            g_lookup_invocations++;
            return &g_lookup_child_inode;
        }

        return nullptr;
    }

    if (directory == &g_mounted_root_inode)
    {
        if (strcmp(name, "inside") == 0)
        {
            g_lookup_invocations++;
            return &g_lookup_child_inode;
        }

        return nullptr;
    }

    return nullptr;
}

static const inode_operations_t g_lookup_ops = {
    vfs_lookup_stub,
};

extern "C" void run_vfs_selftests(void* logicLayerCaps)
{
    unsigned long long passes = 0;
    unsigned long long fails  = 0;
    VirtualFileSystem  localVfs(nullptr);
    VirtualFileSystem* vfs    = &localVfs;

    kprintf("Arx kernel: vfs_selftest start\n");

    LogicLayerCaps* logicCaps = static_cast<LogicLayerCaps*>(logicLayerCaps);
    if (logicCaps == nullptr)
    {
        vfs_test_log_fail("missing virtual file system", &fails);
        goto done;
    }

    {

    static dentry_t root = {};
    root.name     = "/";
    root.ownedName = nullptr;
    root.parent   = nullptr;
    root.inode    = nullptr;
    root.cacheOwnedAllocation = false;

    static dentry_t binDir = {};
    binDir.name     = "bin";
    binDir.ownedName = nullptr;
    binDir.parent   = &root;
    binDir.inode    = nullptr;
    binDir.cacheOwnedAllocation = false;

    static dentry_t homeDir = {};
    homeDir.name     = "home";
    homeDir.ownedName = nullptr;
    homeDir.parent   = &root;
    homeDir.inode    = nullptr;
    homeDir.cacheOwnedAllocation = false;

    static dentry_t binTest = {};
    binTest.name     = "test";
    binTest.ownedName = nullptr;
    binTest.parent   = &binDir;
    binTest.inode    = nullptr;
    binTest.cacheOwnedAllocation = false;

    static dentry_t homeTest = {};
    homeTest.name     = "test";
    homeTest.ownedName = nullptr;
    homeTest.parent   = &homeDir;
    homeTest.inode    = nullptr;
    homeTest.cacheOwnedAllocation = false;

    static dentry_t duplicateBinTest = {};
    duplicateBinTest.name     = "test";
    duplicateBinTest.ownedName = nullptr;
    duplicateBinTest.parent   = &binDir;
    duplicateBinTest.inode    = nullptr;
    duplicateBinTest.cacheOwnedAllocation = false;

    g_root_inode.inodeNumber = 1;
    g_root_inode.type = INODE_DIRECTORY;
    g_root_inode.size = 0;
    g_root_inode.filesystem = nullptr;
    g_root_inode.inodeOps = &g_lookup_ops;
    g_root_inode.fileOps = nullptr;
    g_root_inode.privateData = nullptr;

    root.inode = &g_root_inode;

    g_lookup_dir_inode.inodeNumber = 3;
    g_lookup_dir_inode.type = INODE_DIRECTORY;
    g_lookup_dir_inode.size = 0;
    g_lookup_dir_inode.filesystem = nullptr;
    g_lookup_dir_inode.inodeOps = &g_lookup_ops;
    g_lookup_dir_inode.fileOps = nullptr;
    g_lookup_dir_inode.privateData = nullptr;

    g_lookup_child_inode.inodeNumber = 2;
    g_lookup_child_inode.type = INODE_REGULAR;
    g_lookup_child_inode.size = 123;
    g_lookup_child_inode.filesystem = nullptr;
    g_lookup_child_inode.inodeOps = nullptr;
    g_lookup_child_inode.fileOps = &g_file_ops;
    g_lookup_child_inode.privateData = nullptr;

    g_mounted_root_inode.inodeNumber = 4;
    g_mounted_root_inode.type = INODE_DIRECTORY;
    g_mounted_root_inode.size = 0;
    g_mounted_root_inode.filesystem = nullptr;
    g_mounted_root_inode.inodeOps = &g_lookup_ops;
    g_mounted_root_inode.fileOps = nullptr;
    g_mounted_root_inode.privateData = nullptr;

    static dentry_t mntDentry = {};
    mntDentry.name = "mnt";
    mntDentry.ownedName = nullptr;
    mntDentry.parent = &root;
    mntDentry.inode = &g_lookup_dir_inode;
    mntDentry.cacheOwnedAllocation = false;

    static dentry_t mountedRootDentry = {};
    mountedRootDentry.name = "/";
    mountedRootDentry.ownedName = nullptr;
    mountedRootDentry.parent = nullptr;
    mountedRootDentry.inode = &g_mounted_root_inode;
    mountedRootDentry.cacheOwnedAllocation = false;

    static mount_t rootMount = {};
    rootMount.filesystem = nullptr;
    rootMount.root = &root;
    rootMount.parentMount = nullptr;
    rootMount.mountPoint = nullptr;
    rootMount.next = nullptr;

    static mount_t childMount = {};
    childMount.filesystem = nullptr;
    childMount.root = &mountedRootDentry;
    childMount.parentMount = &rootMount;
    childMount.mountPoint = &mntDentry;
    childMount.next = nullptr;

    if (!vfs->SetRootMount(&rootMount) || !vfs->RegisterMount(&childMount))
    {
        vfs_test_log_fail("mount registration failed", &fails);
        goto done;
    }

    if (!vfs->CacheDentry(&mntDentry))
    {
        vfs_test_log_fail("CacheDentry failed for mountpoint", &fails);
        goto done;
    }
    g_lookup_invocations = 0;

    if (!vfs->CacheDentry(&binTest) || !vfs->CacheDentry(&homeTest))
    {
        vfs_test_log_fail("CacheDentry failed", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->CacheDentry(&duplicateBinTest))
    {
        vfs_test_log_fail("CacheDentry should reject duplicate (parent,name) for different dentry", &fails);
    }
    else
    {
        passes++;
    }

    const uint32_t hashA1 = vfs->HashDentryKey(binTest.parent, binTest.name);
    const uint32_t hashA2 = vfs->HashDentryKey(binTest.parent, binTest.name);
    if (hashA1 != hashA2)
    {
        vfs_test_log_fail("HashDentryKey is not deterministic", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->HashDentryKey(nullptr, nullptr) != 0)
    {
        vfs_test_log_fail("HashDentryKey(nullptr, nullptr) should return 0", &fails);
    }
    else
    {
        passes++;
    }

    dentry_t* foundBinTest = vfs->FindDentry(&binDir, "test");
    if (foundBinTest != &binTest)
    {
        vfs_test_log_fail("FindDentry failed to find /bin/test", &fails);
    }
    else
    {
        passes++;
    }

    dentry_t* foundHomeTest = vfs->FindDentry(&homeDir, "test");
    if (foundHomeTest != &homeTest)
    {
        vfs_test_log_fail("FindDentry failed to disambiguate same name across parents", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->FindDentry(&binDir, "missing") != nullptr)
    {
        vfs_test_log_fail("FindDentry should return nullptr for missing key", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->FindDentry(nullptr, nullptr) != nullptr)
    {
        vfs_test_log_fail("FindDentry should reject null name", &fails);
    }
    else
    {
        passes++;
    }

    dentry_t* lookupMissResolved = vfs->Lookup(&root, "from-backend");
    if (lookupMissResolved == nullptr)
    {
        vfs_test_log_fail("Lookup should resolve cache miss through inode Lookup", &fails);
    }
    else
    {
        passes++;
    }

    if (lookupMissResolved == nullptr || lookupMissResolved->inode != &g_lookup_child_inode)
    {
        vfs_test_log_fail("Lookup should wire returned inode into new dentry", &fails);
    }
    else
    {
        passes++;
    }

    if (g_lookup_invocations != 1)
    {
        vfs_test_log_fail("Lookup backend should be invoked exactly once on first miss", &fails);
    }
    else
    {
        passes++;
    }

    dentry_t* lookupHit = vfs->Lookup(&root, "from-backend");
    if (lookupHit != lookupMissResolved)
    {
        vfs_test_log_fail("Lookup should return cached dentry on second query", &fails);
    }
    else
    {
        passes++;
    }

    if (g_lookup_invocations != 1)
    {
        vfs_test_log_fail("Lookup cache hit should not call backend again", &fails);
    }
    else
    {
        passes++;
    }

    vfs_path_t startPath = {};
    startPath.mount = &rootMount;
    startPath.dentry = &root;

    vfs_path_t resolvedAbsolute = {};
    if (!vfs->ResolvePath(startPath, "/dir/file", &resolvedAbsolute) || resolvedAbsolute.dentry == nullptr)
    {
        vfs_test_log_fail("ResolvePath should resolve absolute paths", &fails);
    }
    else
    {
        passes++;
    }

    if (resolvedAbsolute.dentry == nullptr || resolvedAbsolute.dentry->inode != &g_lookup_child_inode)
    {
        vfs_test_log_fail("ResolvePath should end at backend inode for /dir/file", &fails);
    }
    else
    {
        passes++;
    }

    const int invocationsAfterAbsolute = g_lookup_invocations;
    vfs_path_t resolvedRelative = {};
    if (!vfs->ResolvePath(startPath, "dir/file", &resolvedRelative) || resolvedRelative.dentry != resolvedAbsolute.dentry)
    {
        vfs_test_log_fail("ResolvePath relative and absolute should converge on same cached dentry", &fails);
    }
    else
    {
        passes++;
    }

    if (g_lookup_invocations != invocationsAfterAbsolute)
    {
        vfs_test_log_fail("ResolvePath cache hit should not call backend again", &fails);
    }
    else
    {
        passes++;
    }

    vfs_path_t missingPath = {};
    if (vfs->ResolvePath(startPath, "/missing/path", &missingPath))
    {
        vfs_test_log_fail("ResolvePath should fail on missing path components", &fails);
    }
    else
    {
        passes++;
    }

    vfs_path_t mountedPath = {};
    if (!vfs->ResolvePath(startPath, "/mnt/inside", &mountedPath) || mountedPath.mount != &childMount || mountedPath.dentry == nullptr)
    {
        vfs_test_log_fail("ResolvePath should descend into mounted filesystem root", &fails);
    }
    else
    {
        passes++;
    }

    vfs_path_t backToParent = {};
    vfs_path_t mountedStart = {};
    mountedStart.mount = &childMount;
    mountedStart.dentry = &mountedRootDentry;
    if (!vfs->ResolvePath(mountedStart, "..", &backToParent) || backToParent.mount != &rootMount || backToParent.dentry != &root)
    {
        vfs_test_log_fail("ResolvePath '..' should cross back to parent mount namespace", &fails);
    }
    else
    {
        passes++;
    }

    g_file_open_invocations = 0;
    g_file_read_invocations = 0;
    g_file_release_invocations = 0;
    file_t* opened = vfs->Open(startPath, "/dir/file", 0x1234);
    if (opened == nullptr)
    {
        vfs_test_log_fail("Open should create file object for resolvable path", &fails);
    }
    else
    {
        passes++;
    }

    if (opened == nullptr || opened->inode != &g_lookup_child_inode || opened->operations != &g_file_ops || opened->statusFlags != 0x1234)
    {
        vfs_test_log_fail("Open should populate inode/ops/flags from resolved path", &fails);
    }
    else
    {
        passes++;
    }

    if (opened == nullptr || opened->refCount != 1)
    {
        vfs_test_log_fail("Open should initialize file refCount to 1", &fails);
    }
    else
    {
        passes++;
    }

    if (g_file_open_invocations != 1)
    {
        vfs_test_log_fail("Open should call file operations Open exactly once", &fails);
    }
    else
    {
        passes++;
    }

    char readBuffer[2] = {0, 0};
    if (opened == nullptr || vfs->Read(opened, readBuffer, 1) != 1)
    {
        vfs_test_log_fail("Read should dispatch to file operations Read", &fails);
    }
    else
    {
        passes++;
    }

    if (readBuffer[0] != 'X' || g_file_read_invocations != 1)
    {
        vfs_test_log_fail("Read dispatch should update buffer and invocation count", &fails);
    }
    else
    {
        passes++;
    }

    if (opened != nullptr && !vfs->Retain(opened))
    {
        vfs_test_log_fail("Retain should increment file refCount for shared descriptions", &fails);
    }
    else
    {
        passes++;
    }

    if (opened != nullptr && opened->refCount != 2)
    {
        vfs_test_log_fail("Retain should move file refCount from 1 to 2", &fails);
    }
    else
    {
        passes++;
    }

    if (opened != nullptr && vfs->Close(opened) != 0)
    {
        vfs_test_log_fail("Close should drop one reference", &fails);
    }
    else
    {
        passes++;
    }

    if (g_file_release_invocations != 0)
    {
        vfs_test_log_fail("Close should not release while file references remain", &fails);
    }
    else
    {
        passes++;
    }

    if (opened != nullptr && vfs->Close(opened) != 0)
    {
        vfs_test_log_fail("Final Close should release and free file object", &fails);
    }
    else
    {
        passes++;
    }

    if (g_file_release_invocations != 1)
    {
        vfs_test_log_fail("Final Close should call Release exactly once", &fails);
    }
    else
    {
        passes++;
    }

    if (vfs->Read(nullptr, readBuffer, 1) >= 0)
    {
        vfs_test_log_fail("Read should fail on null file", &fails);
    }
    else
    {
        passes++;
    }

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
