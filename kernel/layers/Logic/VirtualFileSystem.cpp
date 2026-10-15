#include "layers/Logic/VirtualFileSystem.hpp"

#include "layers/Logic/EventManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

extern "C"
{
#include <klib/khashl/khashp.h>
}

#include <klib/klib.h>

struct dentry_key_t
{
    const dentry_t* parent;
    const char*     name;
};

struct vfs_resource_filesystem_private_t
{
    resource_fs_t*               backend;
    ResourceLayerFileSystemCaps* caps;
};

struct vfs_resource_inode_private_t
{
    resource_node_t*             backendNode;
    ResourceLayerFileSystemCaps* caps;
};

static bool     vfs_readonly(const vfs_path_t& path);
static inode_t* vfs_resource_lookup(inode_t* directory, const char* name);
static int64_t  vfs_resource_open(file_t* file);
static int64_t  vfs_resource_read(file_t* file, void* buffer, uint64_t count);
static int64_t  vfs_resource_write(file_t* file, const void* buffer, uint64_t count);
static int64_t  vfs_resource_seek(file_t* file, int64_t offset, int whence);
static int64_t  vfs_resource_readdir(file_t* file, directory_entry_t* entry);
static void     vfs_resource_release(file_t* file);

static int64_t vfs_symlink_read(file_t* file, void* buffer, uint64_t count);

static const inode_operations_t g_vfs_resource_inode_ops = {
        vfs_resource_lookup,
};

static const file_operations_t g_vfs_resource_file_ops = {
        vfs_resource_open, vfs_resource_read, vfs_resource_write, vfs_resource_seek, vfs_resource_readdir, nullptr, nullptr, vfs_resource_release,
};

static const file_operations_t g_vfs_symlink_file_ops = {
        nullptr, vfs_symlink_read, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

struct vfs_symlink_private_t
{
    char* target;
};

struct vfs_resource_file_private_t
{
    char direntName[256];
};

struct vfs_overlay_dentry_entry_t
{
    dentry_t*                   dentry;
    vfs_overlay_dentry_entry_t* next;
};

constexpr int64_t VFS_ERR_NOENT  = -2;
constexpr int64_t VFS_ERR_EXIST  = -17;
constexpr int64_t VFS_ERR_NOTDIR = -20;
constexpr int64_t VFS_ERR_INVAL  = -22;
constexpr int64_t VFS_ERR_NOMEM  = -12;
constexpr int64_t VFS_ERR_LOOP   = -40;
constexpr int64_t VFS_ERR_NOSYS  = -38;

static inode_type_t vfs_map_resource_node_type(resource_node_type_t type)
{
    switch (type)
    {
        case RESOURCE_NODE_DIRECTORY:
            return INODE_DIRECTORY;
        case RESOURCE_NODE_SYMLINK:
            return INODE_SYMLINK;
        case RESOURCE_NODE_CHAR_DEVICE:
            return INODE_CHAR_DEVICE;
        case RESOURCE_NODE_BLOCK_DEVICE:
            return INODE_BLOCK_DEVICE;
        case RESOURCE_NODE_REGULAR:
        default:
            return INODE_REGULAR;
    }
}

static inode_t* vfs_wrap_resource_node(filesystem_t* filesystem, ResourceLayerFileSystemCaps* caps, resource_node_t* backendNode)
{
    if (filesystem == nullptr || caps == nullptr || backendNode == nullptr || caps->GetNodeInfo == nullptr)
    {
        return nullptr;
    }

    resource_node_info_t info = {};

    if (!caps->GetNodeInfo(caps, backendNode, &info))
    {
        return nullptr;
    }

    vfs_resource_inode_private_t* inodePrivate = (vfs_resource_inode_private_t*) kmalloc(sizeof(vfs_resource_inode_private_t));

    if (inodePrivate == nullptr)
    {
        return nullptr;
    }

    inode_t* inode = (inode_t*) kmalloc(sizeof(inode_t));

    if (inode == nullptr)
    {
        kfree(inodePrivate);
        return nullptr;
    }

    memset(inode, 0, sizeof(inode_t));
    inode->inodeNumber   = info.inodeNumber;
    inode->type          = vfs_map_resource_node_type(info.type);
    inode->size          = info.size;
    inode->mode          = info.mode;
    inode->uid           = info.uid;
    inode->gid           = info.gid;
    inode->metadataValid = true;
    inode->linkCount     = 1;
    inode->filesystem    = filesystem;
    inode->privateData   = inodePrivate;
    inode->ioLock        = 0;

    inodePrivate->backendNode = backendNode;
    inodePrivate->caps        = caps;

    if (inode->type == INODE_DIRECTORY)
    {
        inode->inodeOps = &g_vfs_resource_inode_ops;
        inode->fileOps  = &g_vfs_resource_file_ops;
    }
    else
    {
        inode->inodeOps = nullptr;
        inode->fileOps  = &g_vfs_resource_file_ops;
    }

    return inode;
}

static inode_t* vfs_resource_lookup(inode_t* directory, const char* name)
{
    if (directory == nullptr || name == nullptr || directory->privateData == nullptr)
    {
        return nullptr;
    }

    vfs_resource_inode_private_t* directoryPrivate = (vfs_resource_inode_private_t*) directory->privateData;

    if (directoryPrivate->caps == nullptr || directoryPrivate->caps->Lookup == nullptr)
    {
        return nullptr;
    }

    resource_node_t* child = directoryPrivate->caps->Lookup(directoryPrivate->caps, directoryPrivate->backendNode, name);

    if (child == nullptr)
    {
        return nullptr;
    }

    return vfs_wrap_resource_node(directory->filesystem, directoryPrivate->caps, child);
}

static int64_t vfs_resource_open(file_t* file)
{
    if (file == nullptr)
    {
        return -1;
    }

    vfs_resource_file_private_t* filePrivate = (vfs_resource_file_private_t*) kmalloc(sizeof(vfs_resource_file_private_t));

    if (filePrivate == nullptr)
    {
        return -1;
    }

    memset(filePrivate, 0, sizeof(*filePrivate));
    file->privateData = filePrivate;
    return 0;
}

static int64_t vfs_resource_read(file_t* file, void* buffer, uint64_t count)
{
    if (file == nullptr || file->inode == nullptr || file->inode->privateData == nullptr)
    {
        return -1;
    }

    vfs_resource_inode_private_t* inodePrivate = (vfs_resource_inode_private_t*) file->inode->privateData;

    if (inodePrivate->caps == nullptr || inodePrivate->caps->Read == nullptr)
    {
        return -1;
    }

    int64_t readResult = inodePrivate->caps->Read(inodePrivate->caps, inodePrivate->backendNode, file->offset, buffer, count);

    if (readResult > 0)
    {
        file->offset += (uint64_t) readResult;
    }

    return readResult;
}

static int64_t vfs_resource_write(file_t* file, const void* buffer, uint64_t count)
{
    if (file == nullptr || file->inode == nullptr || file->inode->privateData == nullptr)
    {
        return -1;
    }

    vfs_resource_inode_private_t* inodePrivate = (vfs_resource_inode_private_t*) file->inode->privateData;

    if (inodePrivate->caps == nullptr || inodePrivate->caps->Write == nullptr)
    {
        return -1;
    }

    int64_t writeResult = inodePrivate->caps->Write(inodePrivate->caps, inodePrivate->backendNode, file->offset, buffer, count);

    if (writeResult > 0)
    {
        file->offset += (uint64_t) writeResult;
    }

    return writeResult;
}

static int64_t vfs_resource_seek(file_t* file, int64_t offset, int whence)
{
    if (file == nullptr)
    {
        return -1;
    }

    uint64_t base = 0;

    if (whence == 0)
    {
        base = 0;
    }
    else if (whence == 1)
    {
        base = file->offset;
    }
    else if (whence == 2)
    {
        if (file->inode == nullptr)
        {
            return -1;
        }

        base = file->inode->size;
    }
    else
    {
        return -1;
    }

    if (offset < 0 && (uint64_t) (-offset) > base)
    {
        return -1;
    }

    uint64_t newOffset = offset < 0 ? (base - (uint64_t) (-offset)) : (base + (uint64_t) offset);
    file->offset       = newOffset;
    return (int64_t) newOffset;
}

static int64_t vfs_resource_readdir(file_t* file, directory_entry_t* entry)
{
    if (file == nullptr || file->inode == nullptr || file->inode->privateData == nullptr || entry == nullptr)
    {
        return -1;
    }

    if (file->inode->type != INODE_DIRECTORY)
    {
        return -1;
    }

    vfs_resource_file_private_t* filePrivate = (vfs_resource_file_private_t*) file->privateData;

    if (filePrivate == nullptr)
    {
        return -1;
    }

    vfs_resource_inode_private_t* inodePrivate = (vfs_resource_inode_private_t*) file->inode->privateData;

    if (inodePrivate->caps == nullptr || inodePrivate->caps->ReadDirectory == nullptr)
    {
        return -1;
    }

    resource_directory_entry_t resourceEntry = {};
    uint64_t                   cursor        = file->offset;
    const int64_t              result        = inodePrivate->caps->ReadDirectory(inodePrivate->caps, inodePrivate->backendNode, &cursor, &resourceEntry);

    if (result <= 0)
    {
        return result;
    }

    memcpy(filePrivate->direntName, resourceEntry.name, sizeof(filePrivate->direntName));

    entry->name        = filePrivate->direntName;
    entry->inodeNumber = resourceEntry.inodeNumber;

    switch (resourceEntry.type)
    {
        case RESOURCE_NODE_DIRECTORY:
            entry->type = INODE_DIRECTORY;
            break;
        case RESOURCE_NODE_SYMLINK:
            entry->type = INODE_SYMLINK;
            break;
        case RESOURCE_NODE_CHAR_DEVICE:
            entry->type = INODE_CHAR_DEVICE;
            break;
        case RESOURCE_NODE_BLOCK_DEVICE:
            entry->type = INODE_BLOCK_DEVICE;
            break;
        case RESOURCE_NODE_REGULAR:
        default:
            entry->type = INODE_REGULAR;
            break;
    }

    file->offset = cursor;
    return 1;
}

static int64_t vfs_symlink_read(file_t* file, void* buffer, uint64_t count)
{
    if (file == nullptr || file->inode == nullptr || file->inode->privateData == nullptr || buffer == nullptr)
    {
        return -1;
    }

    vfs_symlink_private_t* symlink = (vfs_symlink_private_t*) file->inode->privateData;

    if (symlink->target == nullptr)
    {
        return -1;
    }

    const uint64_t targetLen = (uint64_t) strlen(symlink->target);

    if (file->offset >= targetLen)
    {
        return 0;
    }

    const uint64_t remaining = targetLen - file->offset;
    const uint64_t toRead    = count < remaining ? count : remaining;
    memcpy(buffer, symlink->target + file->offset, (size_t) toRead);
    file->offset += toRead;
    return (int64_t) toRead;
}

static void vfs_resource_release(file_t* file)
{
    if (file == nullptr)
    {
        return;
    }

    if (file->privateData != nullptr)
    {
        kfree(file->privateData);
        file->privateData = nullptr;
    }
}

static uint32_t vfs_hash_parent_and_name(const dentry_t* parent, const char* name)
{
    if (name == nullptr)
    {
        return 0;
    }

    uint32_t       hash       = 2166136261U; // FNV-1a
    uintptr_t      parentPtr  = (uintptr_t) parent;
    const uint8_t* parentData = (const uint8_t*) &parentPtr;

    for (size_t i = 0; i < sizeof(parentPtr); ++i)
    {
        hash ^= parentData[i];
        hash *= 16777619U;
    }

    for (const uint8_t* p = (const uint8_t*) name; *p != 0; ++p)
    {
        hash ^= *p;
        hash *= 16777619U;
    }

    return hash;
}

static khint_t vfs_dentry_cache_hash_fn(const void* key, uint32_t key_len)
{
    (void) key_len;

    const dentry_key_t* k = (const dentry_key_t*) key;
    return vfs_hash_parent_and_name(k->parent, k->name);
}

static int vfs_dentry_cache_eq_fn(const void* key1, const void* key2, uint32_t key_len)
{
    (void) key_len;

    const dentry_key_t* a = (const dentry_key_t*) key1;
    const dentry_key_t* b = (const dentry_key_t*) key2;

    if (a->parent != b->parent)
    {
        return 0;
    }

    if (a->name == nullptr || b->name == nullptr)
    {
        return a->name == b->name;
    }

    return strcmp(a->name, b->name) == 0;
}

static bool vfs_file_ref_acquire(file_t* file)
{
    spinlock_acquire_preempt(&file->refLock);

    if (file->refCount == 0)
    {
        spinlock_release_preempt(&file->refLock);
        return false;
    }

    file->refCount++;
    spinlock_release_preempt(&file->refLock);
    return true;
}

static bool vfs_file_ref_release(file_t* file, bool* isLastReference)
{
    spinlock_acquire_preempt(&file->refLock);

    if (file->refCount == 0)
    {
        spinlock_release_preempt(&file->refLock);
        return false;
    }

    file->refCount--;
    *isLastReference = (file->refCount == 0);
    spinlock_release_preempt(&file->refLock);
    return true;
}

static bool vfs_file_io_begin(file_t* file)
{
    if (file == nullptr || file->inode == nullptr)
    {
        return false;
    }

    spinlock_acquire_preempt(&file->ioLock);
    spinlock_acquire_preempt(&file->inode->ioLock);
    return true;
}

static bool vfs_read_symlink_target(dentry_t* dentry, char* outTarget, size_t outTargetSize)
{
    if (dentry == nullptr || dentry->inode == nullptr || outTarget == nullptr || outTargetSize < 2)
    {
        return false;
    }

    if (dentry->inode->type != INODE_SYMLINK)
    {
        return false;
    }

    if (dentry->inode->fileOps == nullptr || dentry->inode->fileOps->Read == nullptr)
    {
        return false;
    }

    file_t linkFile = {};
    linkFile.inode  = dentry->inode;
    linkFile.offset = 0;

    const int64_t readResult = dentry->inode->fileOps->Read(&linkFile, outTarget, outTargetSize - 1);

    if (readResult <= 0)
    {
        return false;
    }

    const size_t used = (size_t) readResult;

    if (used >= outTargetSize)
    {
        return false;
    }

    outTarget[used] = '\0';
    return true;
}

static bool vfs_split_parent_and_leaf(const char* path, char* parentOut, size_t parentOutSize, char* leafOut, size_t leafOutSize)
{
    if (path == nullptr || parentOut == nullptr || leafOut == nullptr || parentOutSize == 0 || leafOutSize == 0)
    {
        return false;
    }

    const size_t originalLen = strlen(path);

    if (originalLen == 0)
    {
        return false;
    }

    size_t len = originalLen;

    while (len > 1 && path[len - 1] == '/')
    {
        len--;
    }

    if (len == 1 && path[0] == '/')
    {
        return false;
    }

    size_t lastSlash = SIZE_MAX;

    for (size_t i = 0; i < len; ++i)
    {
        if (path[i] == '/')
        {
            lastSlash = i;
        }
    }

    if (lastSlash == SIZE_MAX)
    {
        if (parentOutSize < 2 || len + 1 > leafOutSize)
        {
            return false;
        }

        parentOut[0] = '.';
        parentOut[1] = '\0';
        memcpy(leafOut, path, len);
        leafOut[len] = '\0';
        return true;
    }

    if (lastSlash == 0)
    {
        if (parentOutSize < 2)
        {
            return false;
        }

        parentOut[0] = '/';
        parentOut[1] = '\0';
    }
    else
    {
        if (lastSlash + 1 > parentOutSize)
        {
            return false;
        }

        memcpy(parentOut, path, lastSlash);
        parentOut[lastSlash] = '\0';
    }

    const size_t leafLen = len - (lastSlash + 1);

    if (leafLen == 0 || leafLen + 1 > leafOutSize)
    {
        return false;
    }

    memcpy(leafOut, path + lastSlash + 1, leafLen);
    leafOut[leafLen] = '\0';
    return true;
}

static void vfs_file_io_end(file_t* file)
{
    spinlock_release_preempt(&file->inode->ioLock);
    spinlock_release_preempt(&file->ioLock);
}

VirtualFileSystem::VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps)
{
    ResourceLayerImportCaps = resourceLayerCaps;
    EventObserver           = nullptr;
    DentryCacheLock         = 0;
    RetiredDentries         = nullptr;
    DentryCache             = khp_init(sizeof(dentry_key_t), sizeof(dentry_t*), vfs_dentry_cache_hash_fn, vfs_dentry_cache_eq_fn);
    OverlayDentryLock       = 0;
    OverlayDentryHead       = nullptr;
    Namespace.rootMount     = nullptr;
    MountListHead           = nullptr;
    MountLock               = 0;
    memset(FileSystemTypes, 0, sizeof(FileSystemTypes));
}

VirtualFileSystem::~VirtualFileSystem()
{
    if (DentryCache != nullptr)
    {
        spinlock_acquire_preempt(&DentryCacheLock);
        khint_t k;
        khp_foreach(DentryCache, k)
        {
            dentry_t* dentry = nullptr;
            khp_get_val(DentryCache, k, &dentry);

            if (dentry == nullptr)
            {
                continue;
            }

            if (dentry->ownedName != nullptr)
            {
                kfree(dentry->ownedName);
                dentry->ownedName = nullptr;
            }

            dentry->name = nullptr;

            if (dentry->cacheOwnedAllocation)
            {
                kfree(dentry);
            }
        }

        spinlock_release_preempt(&DentryCacheLock);

        khp_destroy(DentryCache);
        DentryCache = nullptr;
    }

    while (RetiredDentries != nullptr)
    {
        dentry_t* dentry = RetiredDentries;
        RetiredDentries  = dentry->retiredNext;
        kfree(dentry->ownedName);
        if (dentry->cacheOwnedAllocation)
        {
            kfree(dentry);
        }
    }

    spinlock_acquire_preempt(&OverlayDentryLock);
    vfs_overlay_dentry_entry_t* overlay = OverlayDentryHead;
    OverlayDentryHead                   = nullptr;
    spinlock_release_preempt(&OverlayDentryLock);

    while (overlay != nullptr)
    {
        vfs_overlay_dentry_entry_t* next = overlay->next;
        dentry_t*                   d    = overlay->dentry;

        if (d != nullptr)
        {
            if (d->ownedName != nullptr)
            {
                kfree(d->ownedName);
                d->ownedName = nullptr;
            }

            if (d->inode != nullptr)
            {
                vfs_symlink_private_t* symlinkPrivate = (vfs_symlink_private_t*) d->inode->privateData;

                if (symlinkPrivate != nullptr)
                {
                    if (symlinkPrivate->target != nullptr)
                    {
                        kfree(symlinkPrivate->target);
                    }

                    kfree(symlinkPrivate);
                }

                kfree(d->inode);
            }

            kfree(d);
        }

        kfree(overlay);
        overlay = next;
    }
    for (mount_t* mount = MountListHead; mount != nullptr;)
    {
        mount_t* next = mount->next;
        if (mount->ownedAllocation)
        {
            if (mount->ownsFilesystem)
            {
                mount->filesystem->type->Unmount(mount->filesystem);
                kfree(mount->filesystem);
                kfree(mount->root);
            }
            kfree(mount);
        }
        mount = next;
    }
}

dentry_t* VirtualFileSystem::FindOverlayDentry(const dentry_t* parent, const char* name) const
{
    if (name == nullptr)
    {
        return nullptr;
    }

    spinlock_acquire_preempt(&OverlayDentryLock);

    for (vfs_overlay_dentry_entry_t* entry = OverlayDentryHead; entry != nullptr; entry = entry->next)
    {
        dentry_t* dentry = entry->dentry;

        if (dentry == nullptr || dentry->name == nullptr)
        {
            continue;
        }

        if (dentry->parent == parent && strcmp(dentry->name, name) == 0)
        {
            spinlock_release_preempt(&OverlayDentryLock);
            return dentry;
        }
    }

    spinlock_release_preempt(&OverlayDentryLock);
    return nullptr;
}

bool VirtualFileSystem::AddOverlayDentry(dentry_t* dentry)
{
    if (dentry == nullptr || dentry->name == nullptr)
    {
        return false;
    }

    vfs_overlay_dentry_entry_t* entry = (vfs_overlay_dentry_entry_t*) kmalloc(sizeof(vfs_overlay_dentry_entry_t));

    if (entry == nullptr)
    {
        return false;
    }

    spinlock_acquire_preempt(&OverlayDentryLock);

    for (vfs_overlay_dentry_entry_t* it = OverlayDentryHead; it != nullptr; it = it->next)
    {
        dentry_t* existing = it->dentry;

        if (existing == nullptr || existing->name == nullptr)
        {
            continue;
        }

        if (existing->parent == dentry->parent && strcmp(existing->name, dentry->name) == 0)
        {
            spinlock_release_preempt(&OverlayDentryLock);
            kfree(entry);
            return false;
        }
    }

    entry->dentry     = dentry;
    entry->next       = OverlayDentryHead;
    OverlayDentryHead = entry;
    spinlock_release_preempt(&OverlayDentryLock);
    return true;
}

uint32_t VirtualFileSystem::HashDentryKey(const dentry_t* parent, const char* name) const
{
    return vfs_hash_parent_and_name(parent, name);
}

bool VirtualFileSystem::SetRootMount(mount_t* rootMount)
{
    if (rootMount == nullptr || rootMount->root == nullptr)
    {
        return false;
    }

    if (rootMount->parentMount != nullptr || rootMount->mountPoint != nullptr)
    {
        return false;
    }

    if (!RegisterMount(rootMount))
    {
        return false;
    }

    Namespace.rootMount = rootMount;
    return true;
}

bool VirtualFileSystem::RegisterMount(mount_t* mount)
{
    if (mount == nullptr || mount->root == nullptr)
    {
        return false;
    }

    spinlock_acquire_preempt(&MountLock);
    for (mount_t* it = MountListHead; it != nullptr; it = it->next)
    {
        if (it == mount)
        {
            spinlock_release_preempt(&MountLock);
            return true;
        }

        if (it->parentMount == mount->parentMount && it->mountPoint == mount->mountPoint)
        {
            spinlock_release_preempt(&MountLock);
            return false;
        }
    }

    mount->next   = MountListHead;
    MountListHead = mount;
    spinlock_release_preempt(&MountLock);
    return true;
}

bool VirtualFileSystem::MountRootFileSystem(const char* filesystemType, void* source)
{
    if (filesystemType == nullptr || ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->fileSystemCaps == nullptr)
    {
        return false;
    }

    if (Namespace.rootMount != nullptr)
    {
        return true;
    }

    ResourceLayerFileSystemCaps* caps = ResourceLayerImportCaps->fileSystemCaps;

    if (caps->MountFilesystem == nullptr || caps->GetRootNode == nullptr)
    {
        return false;
    }

    resource_fs_t* backendFs = caps->MountFilesystem(caps, filesystemType, source);

    if (backendFs == nullptr)
    {
        return false;
    }

    resource_node_t* backendRoot = caps->GetRootNode(caps, backendFs);

    if (backendRoot == nullptr)
    {
        return false;
    }

    vfs_resource_filesystem_private_t* fsPrivate = (vfs_resource_filesystem_private_t*) kmalloc(sizeof(vfs_resource_filesystem_private_t));

    if (fsPrivate == nullptr)
    {
        return false;
    }

    filesystem_t* filesystem = (filesystem_t*) kmalloc(sizeof(filesystem_t));

    if (filesystem == nullptr)
    {
        kfree(fsPrivate);
        return false;
    }

    memset(fsPrivate, 0, sizeof(vfs_resource_filesystem_private_t));
    fsPrivate->backend = backendFs;
    fsPrivate->caps    = caps;

    memset(filesystem, 0, sizeof(filesystem_t));
    filesystem->privateData = fsPrivate;

    inode_t* rootInode = vfs_wrap_resource_node(filesystem, caps, backendRoot);

    if (rootInode == nullptr)
    {
        kfree(filesystem);
        kfree(fsPrivate);
        return false;
    }

    filesystem->rootInode = rootInode;

    dentry_t* rootDentry = (dentry_t*) kmalloc(sizeof(dentry_t));

    if (rootDentry == nullptr)
    {
        kfree(rootInode->privateData);
        kfree(rootInode);
        kfree(filesystem);
        kfree(fsPrivate);
        return false;
    }

    memset(rootDentry, 0, sizeof(dentry_t));
    rootDentry->name                 = "/";
    rootDentry->ownedName            = nullptr;
    rootDentry->parent               = nullptr;
    rootDentry->inode                = rootInode;
    rootDentry->cacheOwnedAllocation = true;

    if (!CacheDentry(rootDentry))
    {
        if (rootDentry->ownedName != nullptr)
        {
            kfree(rootDentry->ownedName);
        }

        kfree(rootDentry);
        kfree(rootInode->privateData);
        kfree(rootInode);
        kfree(filesystem);
        kfree(fsPrivate);
        return false;
    }

    mount_t* rootMount = (mount_t*) kmalloc(sizeof(mount_t));

    if (rootMount == nullptr)
    {
        return false;
    }

    memset(rootMount, 0, sizeof(mount_t));
    rootMount->filesystem  = filesystem;
    rootMount->root        = rootDentry;
    rootMount->parentMount = nullptr;
    rootMount->mountPoint  = nullptr;
    rootMount->next        = nullptr;

    if (!SetRootMount(rootMount))
    {
        kfree(rootMount);
        return false;
    }

    return true;
}

bool VirtualFileSystem::RegisterFileSystemType(const filesystem_type_t* type)
{
    if (type == nullptr || type->name == nullptr || type->name[0] == 0 || type->Mount == nullptr || type->Unmount == nullptr || strcmp(type->name, "cpio") == 0)
    {
        return false;
    }
    spinlock_acquire_preempt(&MountLock);
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < 32; ++i)
    {
        if (FileSystemTypes[i] != nullptr && strcmp(FileSystemTypes[i]->name, type->name) == 0)
        {
            spinlock_release_preempt(&MountLock);
            return false;
        }
        if (FileSystemTypes[i] == nullptr && slot == SIZE_MAX)
        {
            slot = i;
        }
    }
    if (slot != SIZE_MAX)
    {
        FileSystemTypes[slot] = type;
    }
    spinlock_release_preempt(&MountLock);
    return slot != SIZE_MAX;
}

mount_t* VirtualFileSystem::FindChildMount(mount_t* parentMount, dentry_t* mountPoint) const
{
    spinlock_acquire_preempt(&MountLock);
    mount_t* result = nullptr;
    for (mount_t* it = MountListHead; it != nullptr; it = it->next)
    {
        if (it->parentMount == parentMount && it->mountPoint == mountPoint)
        {
            result = it;
            break;
        }
    }
    spinlock_release_preempt(&MountLock);
    return result;
}

bool VirtualFileSystem::CacheDentry(dentry_t* dentry)
{
    if (DentryCache == nullptr || dentry == nullptr || dentry->name == nullptr)
    {
        return false;
    }

    char* duplicatedName = nullptr;

    if (dentry->ownedName == nullptr)
    {
        duplicatedName = kstrdup(dentry->name);

        if (duplicatedName == nullptr)
        {
            return false;
        }
    }

    spinlock_acquire_preempt(&DentryCacheLock);

    dentry_key_t key;
    key.parent = dentry->parent;
    key.name   = dentry->ownedName != nullptr ? dentry->ownedName : duplicatedName;

    int     absent = 0;
    khint_t k      = khp_put(DentryCache, &key, &absent);

    if (k == khp_end(DentryCache))
    {
        spinlock_release_preempt(&DentryCacheLock);

        if (duplicatedName != nullptr)
        {
            kfree(duplicatedName);
        }

        return false;
    }

    if (!absent)
    {
        dentry_t* existing = nullptr;
        khp_get_val(DentryCache, k, &existing);
        spinlock_release_preempt(&DentryCacheLock);

        if (duplicatedName != nullptr)
        {
            kfree(duplicatedName);
        }

        return existing == dentry;
    }

    if (dentry->ownedName == nullptr)
    {
        dentry->ownedName = duplicatedName;
        dentry->name      = dentry->ownedName;
        duplicatedName    = nullptr;
    }

    khp_set_val(DentryCache, k, &dentry);
    spinlock_release_preempt(&DentryCacheLock);

    if (duplicatedName != nullptr)
    {
        kfree(duplicatedName);
    }

    return true;
}

dentry_t* VirtualFileSystem::FindDentry(const dentry_t* parent, const char* name) const
{
    if (DentryCache == nullptr || name == nullptr)
    {
        return FindOverlayDentry(parent, name);
    }

    dentry_t* overlay = FindOverlayDentry(parent, name);

    if (overlay != nullptr)
    {
        return overlay;
    }

    spinlock_acquire_preempt(&DentryCacheLock);

    // khashp's generic get/put equality handling is inconsistent in this tree,
    // so match (parent,name) explicitly while holding the cache lock.
    khint_t k;
    khp_foreach(DentryCache, k)
    {
        dentry_key_t existingKey = {};
        khp_get_key(DentryCache, k, &existingKey);

        if (existingKey.parent != parent)
        {
            continue;
        }

        if (existingKey.name == nullptr || strcmp(existingKey.name, name) != 0)
        {
            continue;
        }

        dentry_t* value = nullptr;
        khp_get_val(DentryCache, k, &value);
        spinlock_release_preempt(&DentryCacheLock);
        return value;
    }

    spinlock_release_preempt(&DentryCacheLock);
    return nullptr;
}

dentry_t* VirtualFileSystem::Lookup(dentry_t* parent, const char* name)
{
    if (parent == nullptr || name == nullptr || parent->inode == nullptr || parent->inode->type != INODE_DIRECTORY)
    {
        return nullptr;
    }

    const uint64_t generation = __atomic_load_n(&parent->inode->namespaceGeneration, __ATOMIC_ACQUIRE);
    dentry_t*      cached     = FindDentry(parent, name);

    if (cached != nullptr)
    {
        return cached;
    }

    inode_t* directory = parent->inode;

    if (directory->inodeOps == nullptr || directory->inodeOps->Lookup == nullptr)
    {
        return nullptr;
    }

    inode_t* childInode = directory->inodeOps->Lookup(directory, name);

    if (childInode == nullptr)
    {
        return nullptr;
    }

    dentry_t* dentry = (dentry_t*) kmalloc(sizeof(dentry_t));

    if (dentry == nullptr)
    {
        return nullptr;
    }

    memset(dentry, 0, sizeof(dentry_t));
    dentry->name                 = name;
    dentry->parent               = parent;
    dentry->inode                = childInode;
    dentry->cacheOwnedAllocation = true;

    if (!CacheDentry(dentry))
    {
        if (dentry->ownedName != nullptr)
        {
            kfree(dentry->ownedName);
            dentry->ownedName = nullptr;
            dentry->name      = nullptr;
        }

        kfree(dentry);
        return FindDentry(parent, name);
    }

    if (__atomic_load_n(&parent->inode->namespaceGeneration, __ATOMIC_ACQUIRE) != generation)
    {
        InvalidateDentry(parent, name);
        return nullptr;
    }
    return dentry;
}

bool VirtualFileSystem::ResolvePath(const vfs_path_t& start, const char* path, vfs_path_t* result)
{
    // TODO: Enforce trailing '/' directory requirement once open/create semantics land.

    if (result == nullptr || path == nullptr)
    {
        return false;
    }

    constexpr size_t VFS_MAX_SYMLINK_FOLLOWS = 16;
    size_t           symlinkFollows          = 0;

    const size_t pathLen = strlen(path);

    if (pathLen == 0)
    {
        *result = start;
        return true;
    }

    char* work = kstrdup(path);

    if (work == nullptr)
    {
        return false;
    }

    vfs_path_t cur = start;

    if (work[0] == '/')
    {
        if (Namespace.rootMount == nullptr || Namespace.rootMount->root == nullptr)
        {
            kfree(work);
            return false;
        }

        cur.mount  = Namespace.rootMount;
        cur.dentry = Namespace.rootMount->root;
    }

    if (cur.mount == nullptr || cur.dentry == nullptr)
    {
        kfree(work);
        return false;
    }

    for (mount_t* child = FindChildMount(cur.mount, cur.dentry); child != nullptr; child = FindChildMount(cur.mount, cur.dentry))
    {
        cur.mount  = child;
        cur.dentry = child->root;
    }
    char* p = work;

    while (*p != '\0')
    {
        while (*p == '/')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        char* component = p;

        while (*p != '\0' && *p != '/')
        {
            p++;
        }

        char saved = *p;
        *p         = '\0';

        if (strcmp(component, ".") == 0)
        {
            *p = saved;
            continue;
        }

        if (strcmp(component, "..") == 0)
        {
            spinlock_acquire_preempt(&MountLock);
            if (cur.dentry == cur.mount->root)
            {
                if (cur.mount->parentMount != nullptr && cur.mount->mountPoint != nullptr)
                {
                    dentry_t* parentOfMountPoint = cur.mount->mountPoint->parent;
                    cur.dentry                   = parentOfMountPoint != nullptr ? parentOfMountPoint : cur.mount->mountPoint;
                    cur.mount                    = cur.mount->parentMount;
                }
            }
            else if (cur.dentry->parent != nullptr)
            {
                cur.dentry = cur.dentry->parent;
            }

            spinlock_release_preempt(&MountLock);
            *p = saved;
            continue;
        }

        const vfs_path_t beforeLookup = cur;

        dentry_t* next = Lookup(cur.dentry, component);
        *p             = saved;

        if (next == nullptr)
        {
            kfree(work);
            return false;
        }

        if (next->inode != nullptr && next->inode->type == INODE_SYMLINK)
        {
            if (symlinkFollows >= VFS_MAX_SYMLINK_FOLLOWS)
            {
                kfree(work);
                return false;
            }

            char linkTarget[512] = {};

            if (!vfs_read_symlink_target(next, linkTarget, sizeof(linkTarget)))
            {
                kfree(work);
                return false;
            }

            const char*  remainder = (saved == '\0') ? "" : p;
            const size_t targetLen = strlen(linkTarget);
            const size_t remainLen = strlen(remainder);

            char* replacement = (char*) kmalloc(targetLen + remainLen + 1);

            if (replacement == nullptr)
            {
                kfree(work);
                return false;
            }

            memcpy(replacement, linkTarget, targetLen);
            memcpy(replacement + targetLen, remainder, remainLen + 1);

            if (linkTarget[0] == '/')
            {
                if (Namespace.rootMount == nullptr || Namespace.rootMount->root == nullptr)
                {
                    kfree(replacement);
                    kfree(work);
                    return false;
                }

                cur.mount  = Namespace.rootMount;
                cur.dentry = Namespace.rootMount->root;
            }
            else
            {
                cur = beforeLookup;
            }

            kfree(work);
            work = replacement;
            p    = work;
            symlinkFollows++;
            continue;
        }

        mount_t* childMount = FindChildMount(cur.mount, next);

        if (childMount != nullptr && childMount->root != nullptr)
        {
            cur.mount  = childMount;
            cur.dentry = childMount->root;
            while ((childMount = FindChildMount(cur.mount, cur.dentry)) != nullptr)
            {
                cur.mount  = childMount;
                cur.dentry = childMount->root;
            }
        }
        else
        {
            cur.dentry = next;
        }
    }

    kfree(work);
    *result = cur;
    return true;
}

bool VirtualFileSystem::BuildAbsolutePath(const vfs_path_t& path, char* output, size_t capacity) const
{
    if (output == nullptr || capacity < 2 || path.dentry == nullptr || path.mount == nullptr)
    {
        return false;
    }
    const char* components[256];
    size_t      count = 0, length = 1;
    vfs_path_t  current = path;
    spinlock_acquire_preempt(&MountLock);
    while (current.dentry != nullptr)
    {
        if (current.dentry == current.mount->root && current.mount->parentMount != nullptr)
        {
            current.dentry = current.mount->mountPoint;
            current.mount  = current.mount->parentMount;
            continue;
        }
        if (current.dentry->parent == nullptr)
        {
            break;
        }
        if (count == 256 || current.dentry->name == nullptr)
        {
            spinlock_release_preempt(&MountLock);
            return false;
        }
        components[count++] = current.dentry->name;
        length += strlen(current.dentry->name) + 1;
        current.dentry = current.dentry->parent;
    }
    if (length + (count == 0 ? 1 : 0) > capacity)
    {
        spinlock_release_preempt(&MountLock);
        return false;
    }
    size_t used    = 0;
    output[used++] = '/';
    while (count != 0)
    {
        const char*  name  = components[--count];
        const size_t bytes = strlen(name);
        memcpy(output + used, name, bytes);
        used += bytes;
        if (count != 0)
        {
            output[used++] = '/';
        }
    }
    output[used] = 0;
    spinlock_release_preempt(&MountLock);
    return true;
}

bool VirtualFileSystem::BuildAbsolutePathFromDentry(const dentry_t* dentry, char* outPath, size_t outPathSize) const
{
    if (dentry == nullptr || outPath == nullptr || outPathSize == 0)
    {
        return false;
    }

    if (dentry->parent == nullptr)
    {
        if (outPathSize < 2)
        {
            return false;
        }

        outPath[0] = '/';
        outPath[1] = '\0';
        return true;
    }

    const dentry_t* nodes[64] = {};
    size_t          depth     = 0;

    const dentry_t* current = dentry;

    while (current != nullptr && current->parent != nullptr)
    {
        if (depth >= (sizeof(nodes) / sizeof(nodes[0])))
        {
            return false;
        }

        nodes[depth++] = current;
        current        = current->parent;
    }

    size_t writePos = 0;

    if (writePos >= outPathSize)
    {
        return false;
    }

    outPath[writePos++] = '/';

    for (size_t i = depth; i > 0; --i)
    {
        const char* component = nodes[i - 1]->name;

        if (component == nullptr)
        {
            return false;
        }

        const size_t componentLen = strlen(component);

        if (writePos + componentLen + 1 >= outPathSize)
        {
            return false;
        }

        memcpy(outPath + writePos, component, componentLen);
        writePos += componentLen;

        if (i > 1)
        {
            outPath[writePos++] = '/';
        }
    }

    outPath[writePos] = '\0';
    return true;
}

bool VirtualFileSystem::JoinPath(const char* base, const char* path, char* outPath, size_t outPathSize) const
{
    if (base == nullptr || path == nullptr || outPath == nullptr || outPathSize == 0)
    {
        return false;
    }

    if (path[0] == '/')
    {
        const size_t pathLen = strlen(path);

        if (pathLen + 1 > outPathSize)
        {
            return false;
        }

        memcpy(outPath, path, pathLen + 1);
        return true;
    }

    size_t baseLen = strlen(base);

    if (baseLen == 0)
    {
        base    = "/";
        baseLen = 1;
    }

    const size_t pathLen = strlen(path);

    if (base[baseLen - 1] == '/')
    {
        if (baseLen + pathLen + 1 > outPathSize)
        {
            return false;
        }

        memcpy(outPath, base, baseLen);
        memcpy(outPath + baseLen, path, pathLen + 1);
        return true;
    }

    if (baseLen + 1 + pathLen + 1 > outPathSize)
    {
        return false;
    }

    memcpy(outPath, base, baseLen);
    outPath[baseLen] = '/';
    memcpy(outPath + baseLen + 1, path, pathLen + 1);
    return true;
}

int64_t VirtualFileSystem::StatNoFollow(const vfs_path_t& start, const char* path, inode_t** outInode, vfs_path_t* resolvedPath)
{
    if (path == nullptr || outInode == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    *outInode = nullptr;

    if (strcmp(path, "/") == 0)
    {
        vfs_path_t resolvedRoot = {};

        if (!ResolvePath({}, "/", &resolvedRoot) || resolvedRoot.dentry == nullptr || resolvedRoot.dentry->inode == nullptr)
        {
            return VFS_ERR_NOENT;
        }

        *outInode = resolvedRoot.dentry->inode;
        if (resolvedPath != nullptr)
        {
            *resolvedPath = resolvedRoot;
        }
        return 0;
    }

    char parentPath[512] = {};
    char leafName[256]   = {};

    if (!vfs_split_parent_and_leaf(path, parentPath, sizeof(parentPath), leafName, sizeof(leafName)))
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t parentStart = start;

    if (parentPath[0] == '/')
    {
        parentStart = {};
    }

    vfs_path_t resolvedParent = {};

    if (!ResolvePath(parentStart, parentPath, &resolvedParent) || resolvedParent.dentry == nullptr || resolvedParent.dentry->inode == nullptr)
    {
        return VFS_ERR_NOENT;
    }

    dentry_t* targetDentry = nullptr;

    if (strcmp(leafName, "/") == 0)
    {
        targetDentry = resolvedParent.dentry;
    }
    else
    {
        targetDentry = Lookup(resolvedParent.dentry, leafName);
    }

    if (targetDentry == nullptr || targetDentry->inode == nullptr)
    {
        return VFS_ERR_NOENT;
    }

    vfs_path_t target = {resolvedParent.mount, targetDentry};
    while (mount_t* child = FindChildMount(target.mount, target.dentry))
    {
        target.mount  = child;
        target.dentry = child->root;
    }
    *outInode = target.dentry->inode;
    if (resolvedPath != nullptr)
    {
        *resolvedPath = target;
    }
    return 0;
}

int64_t VirtualFileSystem::Symlink(const vfs_path_t& start, const char* target, const char* linkPath)
{
    if (target == nullptr || linkPath == nullptr || target[0] == '\0' || linkPath[0] == '\0')
    {
        return VFS_ERR_INVAL;
    }

    char parentPath[512] = {};
    char leafName[256]   = {};

    if (!vfs_split_parent_and_leaf(linkPath, parentPath, sizeof(parentPath), leafName, sizeof(leafName)))
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t parentStart = start;

    if (parentPath[0] == '/')
    {
        parentStart = {};
    }

    vfs_path_t parentResolved = {};

    if (!ResolvePath(parentStart, parentPath, &parentResolved) || parentResolved.dentry == nullptr || parentResolved.dentry->inode == nullptr)
    {
        return VFS_ERR_NOENT;
    }

    if (parentResolved.dentry->inode->type != INODE_DIRECTORY)
    {
        return VFS_ERR_NOTDIR;
    }

    inode_t* parentInode = parentResolved.dentry->inode;
    if (parentInode->filesystem != nullptr && parentInode->filesystem->type != nullptr)
    {
        const uint64_t permission = CheckDirectoryMutation(parentResolved);
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        inode_creation_attributes_t attributes      = {};
        const uint64_t              attributeResult = GetCreationAttributes(parentInode, 0777, false, true, &attributes);
        if (attributeResult != 0)
        {
            return (int64_t) attributeResult;
        }
        const int64_t result = parentInode->inodeOps != nullptr && parentInode->inodeOps->Symlink != nullptr ? parentInode->inodeOps->Symlink(parentInode, leafName, target, &attributes) : -95;
        if (result == 0 && EventObserver != nullptr)
        {
            dentry_t* child = Lookup(parentResolved.dentry, leafName);
            EventObserver->NotifyVfs(child != nullptr ? child->inode : nullptr, 0x100, parentInode, leafName);
        }
        return result;
    }

    vfs_symlink_private_t* symlinkPrivate = (vfs_symlink_private_t*) kmalloc(sizeof(vfs_symlink_private_t));

    if (symlinkPrivate == nullptr)
    {
        return VFS_ERR_NOMEM;
    }

    symlinkPrivate->target = kstrdup(target);

    if (symlinkPrivate->target == nullptr)
    {
        kfree(symlinkPrivate);
        return VFS_ERR_NOMEM;
    }

    inode_t* inode = (inode_t*) kmalloc(sizeof(inode_t));

    if (inode == nullptr)
    {
        kfree(symlinkPrivate->target);
        kfree(symlinkPrivate);
        return VFS_ERR_NOMEM;
    }

    memset(inode, 0, sizeof(inode_t));
    inode->inodeNumber = vfs_hash_parent_and_name(parentResolved.dentry, leafName) ^ 0x53594d4cU;
    inode->type        = INODE_SYMLINK;
    inode->size        = (uint64_t) strlen(target);
    inode->filesystem  = parentResolved.dentry->inode->filesystem;
    inode->inodeOps    = nullptr;
    inode->fileOps     = &g_vfs_symlink_file_ops;
    inode->privateData = symlinkPrivate;
    inode->ioLock      = 0;

    dentry_t* newDentry = (dentry_t*) kmalloc(sizeof(dentry_t));

    if (newDentry == nullptr)
    {
        kfree(inode);
        kfree(symlinkPrivate->target);
        kfree(symlinkPrivate);
        return VFS_ERR_NOMEM;
    }

    memset(newDentry, 0, sizeof(dentry_t));
    newDentry->name                 = leafName;
    newDentry->parent               = parentResolved.dentry;
    newDentry->inode                = inode;
    newDentry->cacheOwnedAllocation = true;

    newDentry->ownedName = kstrdup(leafName);

    if (newDentry->ownedName == nullptr)
    {
        kfree(newDentry);
        kfree(inode);
        kfree(symlinkPrivate->target);
        kfree(symlinkPrivate);
        return VFS_ERR_NOMEM;
    }

    newDentry->name = newDentry->ownedName;

    if (!AddOverlayDentry(newDentry))
    {
        kfree(newDentry->ownedName);
        kfree(newDentry);
        kfree(inode);
        kfree(symlinkPrivate->target);
        kfree(symlinkPrivate);
        return VFS_ERR_EXIST;
    }

    if (EventObserver != nullptr)
    {
        EventObserver->NotifyVfs(inode, 0x100, parentResolved.dentry->inode, leafName);
    }
    return 0;
}

int64_t VirtualFileSystem::Readlink(const vfs_path_t& start, const char* path, char* buffer, uint64_t bufferSize)
{
    if (path == nullptr || buffer == nullptr || bufferSize == 0)
    {
        return VFS_ERR_INVAL;
    }

    char parentPath[512] = {};
    char leafName[256]   = {};

    if (!vfs_split_parent_and_leaf(path, parentPath, sizeof(parentPath), leafName, sizeof(leafName)))
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t parentStart = start;

    if (parentPath[0] == '/')
    {
        parentStart = {};
    }

    vfs_path_t parentResolved = {};

    if (!ResolvePath(parentStart, parentPath, &parentResolved) || parentResolved.dentry == nullptr || parentResolved.dentry->inode == nullptr)
    {
        return VFS_ERR_NOENT;
    }

    dentry_t* targetDentry = Lookup(parentResolved.dentry, leafName);

    if (targetDentry == nullptr || targetDentry->inode == nullptr)
    {
        return VFS_ERR_NOENT;
    }

    if (targetDentry->inode->type != INODE_SYMLINK)
    {
        return VFS_ERR_INVAL;
    }

    char linkTarget[512] = {};

    if (!vfs_read_symlink_target(targetDentry, linkTarget, sizeof(linkTarget)))
    {
        return VFS_ERR_NOENT;
    }

    const uint64_t targetLen = (uint64_t) strlen(linkTarget);
    const uint64_t toCopy    = bufferSize < targetLen ? bufferSize : targetLen;
    memcpy(buffer, linkTarget, (size_t) toCopy);
    return (int64_t) toCopy;
}

int64_t VirtualFileSystem::ResolveParent(const vfs_path_t& start, const char* path, vfs_path_t* parent, char* name, size_t capacity)
{
    char directory[512] = {};
    if (!vfs_split_parent_and_leaf(path, directory, sizeof(directory), name, capacity) || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
        return VFS_ERR_INVAL;
    }
    if (!ResolvePath(start, directory, parent) || parent->dentry == nullptr || parent->dentry->inode == nullptr)
    {
        return VFS_ERR_NOENT;
    }
    return parent->dentry->inode->type == INODE_DIRECTORY ? 0 : VFS_ERR_NOTDIR;
}

uint64_t VirtualFileSystem::GetCreationAttributes(inode_t* parent, uint32_t mode, bool directory, bool symlink, inode_creation_attributes_t* attributes)
{
    ProcessManager* manager = ResourceLayerImportCaps->processManager;
    process_t*      process = manager->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }
    spinlock_acquire_preempt(&parent->ioLock);
    attributes->uid  = process->credentials.user.filesystem;
    attributes->gid  = (parent->mode & 02000) ? parent->gid : process->credentials.group.filesystem;
    attributes->mode = symlink ? 0777 : (mode & ~0777U) | (mode & 0777U & ~__atomic_load_n(&process->umask, __ATOMIC_RELAXED));
    if (directory && (parent->mode & 02000))
    {
        attributes->mode |= 02000;
    }
    spinlock_release_preempt(&parent->ioLock);
    manager->ReleaseCredentials(process);
    return 0;
}

uint64_t VirtualFileSystem::CheckInodeAccess(inode_t* inode, uint32_t requested)
{
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    ProcessManager* manager = ResourceLayerImportCaps->processManager;
    process_t*      process = manager->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }
    spinlock_acquire_preempt(&inode->ioLock);
    const uint32_t caller = process->credentials.user.filesystem;
    bool           member = process->credentials.group.filesystem == inode->gid;
    for (uint32_t i = 0; process->supplementaryGroups != nullptr && i < process->supplementaryGroups->count; ++i)
    {
        member |= process->supplementaryGroups->ids[i] == inode->gid;
    }
    const uint32_t mode       = inode->metadataValid ? inode->mode : inode->type == INODE_DIRECTORY ? 0755 : 0644;
    const uint32_t allowed    = (mode >> (caller == inode->uid ? 6 : member ? 3 : 0)) & 7;
    const bool     authorized = caller == 0 ? (requested & 1) == 0 || inode->type == INODE_DIRECTORY || (mode & 0111) != 0 : (allowed & requested) == requested;
    spinlock_release_preempt(&inode->ioLock);
    manager->ReleaseCredentials(process);
    return authorized ? 0 : LINUX_EACCES;
}

uint64_t VirtualFileSystem::CheckExecutable(file_t* file)
{
    if (file == nullptr || file->inode == nullptr || file->inode->type != INODE_REGULAR || (file->path.mount != nullptr && (file->path.mount->flags & 8) != 0))
    {
        return LINUX_EACCES;
    }

    return CheckInodeAccess(file->inode, 1);
}

uint64_t VirtualFileSystem::CheckDirectoryMutation(const vfs_path_t& parent, const inode_t* victim)
{
    if (vfs_readonly(parent))
    {
        return (uint64_t) -30;
    }
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    ProcessManager* manager = ResourceLayerImportCaps->processManager;
    process_t*      process = manager->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }
    inode_t* inode = parent.dentry->inode;
    spinlock_acquire_preempt(&inode->ioLock);
    const uint32_t caller = process->credentials.user.filesystem;
    bool           member = process->credentials.group.filesystem == inode->gid;
    for (uint32_t i = 0; process->supplementaryGroups != nullptr && i < process->supplementaryGroups->count; ++i)
    {
        member |= process->supplementaryGroups->ids[i] == inode->gid;
    }
    const uint32_t mode    = inode->metadataValid ? inode->mode : 0755;
    const uint32_t allowed = (mode >> (caller == inode->uid ? 6 : member ? 3 : 0)) & 7;
    uint64_t       result  = caller != 0 && (allowed & 3) != 3 ? LINUX_EACCES : 0;
    if (result == 0 && victim != nullptr && (mode & 01000) && caller != 0 && caller != inode->uid && caller != victim->uid)
    {
        result = (uint64_t) -1;
    }
    spinlock_release_preempt(&inode->ioLock);
    manager->ReleaseCredentials(process);
    return result;
}

void VirtualFileSystem::InvalidateDentry(dentry_t* parent, const char* name)
{
    __atomic_add_fetch(&parent->inode->namespaceGeneration, 1ULL, __ATOMIC_RELEASE);
    if (DentryCache == nullptr)
    {
        return;
    }
    spinlock_acquire_preempt(&DentryCacheLock);
    khint_t index;
    khp_foreach(DentryCache, index)
    {
        dentry_key_t key = {};
        khp_get_key(DentryCache, index, &key);
        if (key.parent != parent || key.name == nullptr || strcmp(key.name, name) != 0)
        {
            continue;
        }
        dentry_t* dentry = nullptr;
        khp_get_val(DentryCache, index, &dentry);
        khp_del(DentryCache, index);
        dentry->retiredNext = RetiredDentries;
        RetiredDentries     = dentry;
        break;
    }
    spinlock_release_preempt(&DentryCacheLock);
}

bool VirtualFileSystem::BuildBackendPath(const vfs_path_t& start, const char* path, char* output, size_t capacity)
{
    char       directory[512] = {}, name[256] = {}, base[512] = {};
    vfs_path_t parent = {};
    // Resolve mount crossings before constructing a path for the legacy boot
    // backend. Native filesystems receive parent inodes, never global paths.
    if (!vfs_split_parent_and_leaf(path, directory, sizeof(directory), name, sizeof(name)) || !ResolvePath(start, directory, &parent) || parent.dentry == nullptr)
    {
        return false;
    }
    return BuildAbsolutePathFromDentry(parent.dentry, base, sizeof(base)) && JoinPath(base, name, output, capacity);
}

static bool vfs_readonly(const vfs_path_t& path)
{
    return (path.mount != nullptr && (__atomic_load_n(&path.mount->flags, __ATOMIC_RELAXED) & 1)) || (path.dentry != nullptr && path.dentry->inode != nullptr && path.dentry->inode->filesystem != nullptr && (__atomic_load_n(&path.dentry->inode->filesystem->flags, __ATOMIC_RELAXED) & 1));
}

int64_t VirtualFileSystem::Mkdir(const vfs_path_t& start, const char* path, uint32_t mode)
{
    if (path == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t    parent    = {};
    char          name[256] = {};
    const int64_t resolved  = ResolveParent(start, path, &parent, name, sizeof(name));
    if (resolved != 0)
    {
        return resolved;
    }
    inode_t* node = parent.dentry->inode;
    if (node->filesystem != nullptr && node->filesystem->type != nullptr)
    {
        const uint64_t permission = CheckDirectoryMutation(parent);
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        inode_creation_attributes_t attributes      = {};
        const uint64_t              attributeResult = GetCreationAttributes(node, mode, true, false, &attributes);
        if (attributeResult != 0)
        {
            return (int64_t) attributeResult;
        }
        const int64_t result = node->inodeOps != nullptr && node->inodeOps->Mkdir != nullptr ? node->inodeOps->Mkdir(parent.dentry->inode, name, &attributes) : -95;
        if (result == 0)
        {
            dentry_t* child = Lookup(parent.dentry, name);
            if (EventObserver != nullptr)
            {
                EventObserver->NotifyVfs(child != nullptr ? child->inode : nullptr, 0x100, node, name);
            }
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Mkdir == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char effectivePath[512] = {};

    if (!BuildBackendPath(start, path, effectivePath, sizeof(effectivePath)))
    {
        return VFS_ERR_INVAL;
    }

    return caps->Mkdir(caps, effectivePath, mode);
}

int64_t VirtualFileSystem::Unlink(const vfs_path_t& start, const char* path, bool directory)
{
    if (path == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    if (strcmp(path, "/") == 0)
    {
        return -16;
    }
    vfs_path_t    parent    = {};
    char          name[256] = {};
    const int64_t resolved  = ResolveParent(start, path, &parent, name, sizeof(name));
    if (resolved != 0)
    {
        return resolved;
    }
    inode_t* node = parent.dentry->inode;
    if (node->filesystem != nullptr && node->filesystem->type != nullptr)
    {
        const uint64_t permission = CheckDirectoryMutation(parent);
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        dentry_t* target = Lookup(parent.dentry, name);
        if (target != nullptr && FindChildMount(parent.mount, target) != nullptr)
        {
            return -16;
        }
        const uint64_t stickyPermission = target != nullptr ? CheckDirectoryMutation(parent, target->inode) : 0;
        if (stickyPermission != 0)
        {
            return (int64_t) stickyPermission;
        }
        const int64_t result = node->inodeOps != nullptr && node->inodeOps->Unlink != nullptr ? node->inodeOps->Unlink(parent.dentry->inode, name, directory) : -95;
        if (result == 0)
        {
            if (EventObserver != nullptr)
            {
                EventObserver->NotifyVfs(target != nullptr ? target->inode : nullptr, 0x200, node, name);
                if (target != nullptr)
                {
                    EventObserver->NotifyVfs(target->inode, 4, nullptr, nullptr);
                }
                if (target != nullptr && target->inode->linkCount == 0 && __atomic_load_n(&target->inode->openCount, __ATOMIC_ACQUIRE) == 0)
                {
                    EventObserver->NotifyVfs(target->inode, 0x400, nullptr, nullptr);
                }
            }
            InvalidateDentry(parent.dentry, name);
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Unlink == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char effectivePath[512] = {};

    if (!BuildBackendPath(start, path, effectivePath, sizeof(effectivePath)))
    {
        return VFS_ERR_INVAL;
    }

    return caps->Unlink(caps, effectivePath, directory);
}

int64_t VirtualFileSystem::Rename(const vfs_path_t& oldStart, const char* oldPath, const vfs_path_t& newStart, const char* newPath)
{
    if (oldPath == nullptr || newPath == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t oldParent = {}, newParent = {};
    char       oldName[256] = {}, newName[256] = {};
    int64_t    status = ResolveParent(oldStart, oldPath, &oldParent, oldName, sizeof(oldName));
    if (status == 0)
    {
        status = ResolveParent(newStart, newPath, &newParent, newName, sizeof(newName));
    }
    if (status != 0)
    {
        return status;
    }
    inode_t* oldDirectory = oldParent.dentry->inode;
    inode_t* newDirectory = newParent.dentry->inode;
    if (oldParent.mount != newParent.mount || oldDirectory->filesystem != newDirectory->filesystem)
    {
        return -18;
    }
    if (oldDirectory->filesystem != nullptr && oldDirectory->filesystem->type != nullptr)
    {
        if (vfs_readonly(oldParent) || vfs_readonly(newParent))
        {
            return -30;
        }
        dentry_t* target = Lookup(oldParent.dentry, oldName);
        if (target == nullptr)
        {
            return -2;
        }
        dentry_t* replacement = Lookup(newParent.dentry, newName);
        if (FindChildMount(oldParent.mount, target) != nullptr || (replacement != nullptr && FindChildMount(newParent.mount, replacement) != nullptr))
        {
            return -16;
        }
        if (oldDirectory == newDirectory && strcmp(oldName, newName) == 0)
        {
            return 0;
        }
        uint64_t permission = CheckDirectoryMutation(oldParent, target->inode);
        if (permission == 0)
        {
            permission = CheckDirectoryMutation(newParent, replacement != nullptr ? replacement->inode : nullptr);
        }
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        const int64_t result = oldDirectory->inodeOps != nullptr && oldDirectory->inodeOps->Rename != nullptr ? oldDirectory->inodeOps->Rename(oldDirectory, oldName, newDirectory, newName) : -95;
        if (result == 0)
        {
            if (EventObserver != nullptr)
            {
                static uint32_t nextCookie = 0;
                uint32_t        cookie;
                do
                {
                    cookie = __atomic_add_fetch(&nextCookie, 1U, __ATOMIC_RELAXED);
                } while (cookie == 0);
                EventObserver->NotifyVfs(target->inode, 0x40, oldDirectory, oldName, cookie);
                EventObserver->NotifyVfs(target->inode, 0x80, newDirectory, newName, cookie);
                EventObserver->NotifyVfs(target->inode, 0x800, nullptr, nullptr);
            }
            InvalidateDentry(oldParent.dentry, oldName);
            InvalidateDentry(newParent.dentry, newName);
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Rename == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char effectiveOldPath[512] = {};

    if (!BuildBackendPath(oldStart, oldPath, effectiveOldPath, sizeof(effectiveOldPath)))
    {
        return VFS_ERR_INVAL;
    }

    char effectiveNewPath[512] = {};

    if (!BuildBackendPath(newStart, newPath, effectiveNewPath, sizeof(effectiveNewPath)))
    {
        return VFS_ERR_INVAL;
    }

    return caps->Rename(caps, effectiveOldPath, effectiveNewPath);
}

int64_t VirtualFileSystem::Link(const vfs_path_t& oldStart, const char* oldPath, const vfs_path_t& newStart, const char* newPath, bool followSymlink)
{
    if (oldPath == nullptr || newPath == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t oldParent = {}, newParent = {};
    char       oldName[256] = {}, newName[256] = {};
    int64_t    status = ResolveParent(oldStart, oldPath, &oldParent, oldName, sizeof(oldName));
    if (status == 0)
    {
        status = ResolveParent(newStart, newPath, &newParent, newName, sizeof(newName));
    }
    if (status != 0)
    {
        return status;
    }
    inode_t* oldDirectory = oldParent.dentry->inode;
    inode_t* newDirectory = newParent.dentry->inode;
    if (oldParent.mount != newParent.mount || oldDirectory->filesystem != newDirectory->filesystem)
    {
        return -18;
    }
    if (oldDirectory->filesystem != nullptr && oldDirectory->filesystem->type != nullptr)
    {
        if (vfs_readonly(oldParent) || vfs_readonly(newParent))
        {
            return -30;
        }
        dentry_t* target = Lookup(oldParent.dentry, oldName);
        if (target == nullptr)
        {
            return -2;
        }
        inode_t*   targetInode = target->inode;
        vfs_path_t followed    = {};
        if (followSymlink && targetInode->type == INODE_SYMLINK)
        {
            if (!ResolvePath(oldStart, oldPath, &followed) || followed.dentry == nullptr)
            {
                return -2;
            }
            if (followed.mount != newParent.mount)
            {
                return -18;
            }
            targetInode = followed.dentry->inode;
        }
        if (targetInode->type == INODE_DIRECTORY)
        {
            return -1;
        }
        const uint64_t permission = CheckDirectoryMutation(newParent);
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        const int64_t result = newDirectory->inodeOps != nullptr && newDirectory->inodeOps->Link != nullptr ? newDirectory->inodeOps->Link(newDirectory, newName, targetInode) : -95;
        if (result == 0 && EventObserver != nullptr)
        {
            EventObserver->NotifyVfs(targetInode, 0x100, newDirectory, newName);
            EventObserver->NotifyVfs(targetInode, 4, nullptr, nullptr);
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Link == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char effectiveOldPath[512] = {};

    if (!BuildBackendPath(oldStart, oldPath, effectiveOldPath, sizeof(effectiveOldPath)))
    {
        return VFS_ERR_INVAL;
    }

    char effectiveNewPath[512] = {};

    if (!BuildBackendPath(newStart, newPath, effectiveNewPath, sizeof(effectiveNewPath)))
    {
        return VFS_ERR_INVAL;
    }

    return caps->Link(caps, effectiveOldPath, effectiveNewPath, followSymlink);
}

int64_t VirtualFileSystem::Mknod(const vfs_path_t& start, const char* path, uint32_t mode, uint64_t device)
{
    if (path == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t    parent    = {};
    char          name[256] = {};
    const int64_t resolved  = ResolveParent(start, path, &parent, name, sizeof(name));
    if (resolved != 0)
    {
        return resolved;
    }
    inode_t* node = parent.dentry->inode;
    if (node->filesystem != nullptr && node->filesystem->type != nullptr)
    {
        const uint64_t permission = CheckDirectoryMutation(parent);
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        inode_creation_attributes_t attributes      = {};
        const uint64_t              attributeResult = GetCreationAttributes(node, mode, false, false, &attributes);
        if (attributeResult != 0)
        {
            return (int64_t) attributeResult;
        }
        const int64_t result = node->inodeOps != nullptr && node->inodeOps->Mknod != nullptr ? node->inodeOps->Mknod(parent.dentry->inode, name, &attributes, device) : -95;
        if (result == 0)
        {
            dentry_t* child = Lookup(parent.dentry, name);
            if (EventObserver != nullptr)
            {
                EventObserver->NotifyVfs(child != nullptr ? child->inode : nullptr, 0x100, node, name);
            }
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Mknod == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char effectivePath[512] = {};

    if (!BuildBackendPath(start, path, effectivePath, sizeof(effectivePath)))
    {
        return VFS_ERR_INVAL;
    }

    return caps->Mknod(caps, effectivePath, mode, device);
}

int64_t VirtualFileSystem::TruncatePath(const vfs_path_t& start, const char* path, uint64_t size)
{
    if (path == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    vfs_path_t pathResult = {};
    if (!ResolvePath(start, path, &pathResult) || pathResult.dentry == nullptr)
    {
        return -2;
    }
    inode_t* inode = pathResult.dentry->inode;
    if (inode->filesystem != nullptr && inode->filesystem->type != nullptr)
    {
        if (vfs_readonly(pathResult))
        {
            return -30;
        }
        const uint64_t permission = CheckInodeAccess(inode, 2);
        if (permission != 0)
        {
            return (int64_t) permission;
        }
        spinlock_acquire_preempt(&inode->ioLock);
        const int64_t result = inode->inodeOps != nullptr && inode->inodeOps->Truncate != nullptr ? inode->inodeOps->Truncate(inode, size) : -95;
        if (result == 0)
        {
            inode->size = size;
        }
        spinlock_release_preempt(&inode->ioLock);
        if (result == 0 && EventObserver != nullptr)
        {
            EventObserver->NotifyVfs(inode, 2, pathResult.dentry->parent != nullptr ? pathResult.dentry->parent->inode : nullptr, pathResult.dentry->name);
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Truncate == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char effectivePath[512] = {};

    if (!BuildBackendPath(start, path, effectivePath, sizeof(effectivePath)))
    {
        return VFS_ERR_INVAL;
    }

    const int64_t result = caps->Truncate(caps, effectivePath, size);

    if (result == 0)
    {
        vfs_path_t resolved = {};

        if (ResolvePath(start, path, &resolved) && resolved.dentry != nullptr && resolved.dentry->inode != nullptr)
        {
            resolved.dentry->inode->size = size;
        }
    }

    return result;
}

int64_t VirtualFileSystem::TruncateFile(file_t* file, uint64_t size)
{
    if (file != nullptr && ((file->path.mount != nullptr && (__atomic_load_n(&file->path.mount->flags, __ATOMIC_RELAXED) & 1)) || (file->inode != nullptr && file->inode->filesystem != nullptr && (__atomic_load_n(&file->inode->filesystem->flags, __ATOMIC_RELAXED) & 1))))
    {
        return -30;
    }
    if (file != nullptr && file->operations != nullptr && file->operations->Truncate != nullptr)
    {
        if (!vfs_file_io_begin(file))
        {
            return VFS_ERR_INVAL;
        }
        const int64_t result = file->operations->Truncate(file, size);
        vfs_file_io_end(file);
        return result;
    }

    if (file == nullptr || file->path.dentry == nullptr)
    {
        return VFS_ERR_INVAL;
    }

    if (file->inode != nullptr && file->inode->filesystem != nullptr && file->inode->filesystem->type != nullptr)
    {
        if (!vfs_file_io_begin(file))
        {
            return -22;
        }
        const int64_t result = file->inode->inodeOps != nullptr && file->inode->inodeOps->Truncate != nullptr ? file->inode->inodeOps->Truncate(file->inode, size) : -95;
        if (result == 0)
        {
            file->inode->size = size;
        }
        vfs_file_io_end(file);
        if (result == 0)
        {
            NotifyFile(file, 2);
        }
        return result;
    }

    ResourceLayerFileSystemCaps* caps = (ResourceLayerImportCaps != nullptr) ? ResourceLayerImportCaps->fileSystemCaps : nullptr;

    if (caps == nullptr || caps->Truncate == nullptr)
    {
        return VFS_ERR_NOSYS;
    }

    char absolutePath[512] = {};

    if (!BuildAbsolutePathFromDentry(file->path.dentry, absolutePath, sizeof(absolutePath)))
    {
        return VFS_ERR_INVAL;
    }

    const int64_t result = caps->Truncate(caps, absolutePath, size);

    if (result == 0)
    {
        if (file->inode != nullptr)
        {
            file->inode->size = size;
        }
    }

    return result;
}

file_t* VirtualFileSystem::Open(const vfs_path_t& start, const char* path, uint64_t flags, int64_t* error, uint32_t mode)
{
    auto fail = [&](int64_t result) -> file_t*
    {
        if (error != nullptr)
        {
            *error = result;
        }
        return nullptr;
    };
    if (path == nullptr || path[0] == 0)
    {
        return fail(-2);
    }
    vfs_path_t resolved = {};
    bool       created  = false;
    bool       exists   = ResolvePath(start, path, &resolved) && resolved.dentry != nullptr && resolved.dentry->inode != nullptr;
    if (!exists && (flags & 0100))
    {
        const int64_t result = Mknod(start, path, (mode & 07777) | 0100000, 0);
        if (result != 0 && (result != -17 || (flags & 0200)))
        {
            return fail(result);
        }
        created = result == 0;
        if (!ResolvePath(start, path, &resolved) || resolved.dentry == nullptr)
        {
            return fail(-2);
        }
    }
    else if (!exists)
    {
        return fail(-2);
    }
    const bool native = resolved.dentry->inode->filesystem != nullptr && resolved.dentry->inode->filesystem->type != nullptr;
    if (native)
    {
        if (exists && (flags & (0100 | 0200)) == (0100 | 0200))
        {
            return fail(-17);
        }
        if ((flags & 0200000) && resolved.dentry->inode->type != INODE_DIRECTORY)
        {
            return fail(-20);
        }
        if ((flags & (3 | 01000)) != 0 && vfs_readonly(resolved))
        {
            return fail(-30);
        }
        if ((flags & 3) != 0 && resolved.dentry->inode->type == INODE_DIRECTORY)
        {
            return fail(-21);
        }
        const uint32_t access     = ((flags & 3) != 1 ? 4 : 0) | ((flags & 3) != 0 || (flags & 01000) ? 2 : 0);
        const uint64_t permission = created ? 0 : CheckInodeAccess(resolved.dentry->inode, access);
        if (permission != 0)
        {
            return fail((int64_t) permission);
        }
    }

    file_t* file = (file_t*) kmalloc(sizeof(file_t));

    if (file == nullptr)
    {
        return fail(-12);
    }

    memset(file, 0, sizeof(file_t));
    file->owner       = this;
    file->path        = resolved;
    file->inode       = resolved.dentry->inode;
    file->offset      = 0;
    file->statusFlags = native ? flags & ~(0100ULL | 0200ULL | 0400ULL | 01000ULL | 02000000ULL) : flags;
    file->operations  = file->inode->fileOps;
    file->privateData = nullptr;
    file->ioLock      = 0;
    file->refLock     = 0;
    file->refCount    = 1;

    if (file->operations != nullptr && file->operations->Open != nullptr)
    {
        const int64_t result = file->operations->Open(file);
        if (result < 0)
        {
            if (file->operations->Release != nullptr)
            {
                file->operations->Release(file);
            }

            kfree(file);
            return fail(native ? result : result == -1 ? -5 : result);
        }
    }

    if (native && (flags & 01000) && file->inode->type == INODE_REGULAR)
    {
        const int64_t result = TruncateFile(file, 0);
        if (result != 0)
        {
            file->owner = nullptr;
            Close(file);
            return fail(result);
        }
    }
    if (error != nullptr)
    {
        *error = 0;
    }
    if (native)
    {
        __atomic_add_fetch(&file->inode->openCount, 1ULL, __ATOMIC_ACQ_REL);
    }
    NotifyFile(file, 0x20);
    return file;
}

int64_t VirtualFileSystem::Read(file_t* file, void* buffer, uint64_t count)
{
    return Transfer(file, buffer, count, false, nullptr);
}

int64_t VirtualFileSystem::Write(file_t* file, const void* buffer, uint64_t count)
{
    return Transfer(file, const_cast<void*>(buffer), count, true, nullptr);
}

int64_t VirtualFileSystem::ReadAt(file_t* file, void* buffer, uint64_t count, uint64_t offset)
{
    return Transfer(file, buffer, count, false, &offset);
}

int64_t VirtualFileSystem::WriteAt(file_t* file, const void* buffer, uint64_t count, uint64_t offset)
{
    return Transfer(file, const_cast<void*>(buffer), count, true, &offset);
}

int64_t VirtualFileSystem::Transfer(file_t* file, void* buffer, uint64_t count, bool write, const uint64_t* offset)
{
    if (file != nullptr && file->operations != nullptr && (file->operations->ReadUser != nullptr || file->operations->WriteUser != nullptr))
        return offset != nullptr ? -29 : -95; // positioned/vector/kernel-buffer I/O not implemented for these files

    if (file == nullptr || file->operations == nullptr || (write ? file->operations->Write == nullptr : file->operations->Read == nullptr))
    {
        return -1;
    }

    if (write && ((file->path.mount != nullptr && (__atomic_load_n(&file->path.mount->flags, __ATOMIC_RELAXED) & 1)) || (file->inode != nullptr && file->inode->filesystem != nullptr && (__atomic_load_n(&file->inode->filesystem->flags, __ATOMIC_RELAXED) & 1))))
    {
        return -30;
    }
    if (!vfs_file_io_begin(file))
    {
        return -1;
    }

    const uint64_t savedOffset = file->offset;
    int64_t        result      = 0;

    if (offset != nullptr)
    {
        result = *offset > INT64_MAX || file->operations->Seek == nullptr ? -29 : file->operations->Seek(file, (int64_t) *offset, 0);
    }

    if (offset == nullptr || result >= 0)
    {
        if (write && file->inode->type == INODE_REGULAR && file->inode->filesystem != nullptr && file->inode->filesystem->type != nullptr && (__atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED) & 02000))
        {
            file->offset = file->inode->size;
        }
        result = write ? file->operations->Write(file, buffer, count) : file->operations->Read(file, buffer, count);
    }

    // The entire save/seek/transfer/restore transaction owns both I/O locks.

    if (offset != nullptr)
    {
        file->offset = savedOffset;
    }

    vfs_file_io_end(file);
    if (result > 0)
    {
        NotifyFile(file, write ? 2 : 1);
    }
    return result;
}

int64_t VirtualFileSystem::Seek(file_t* file, int64_t offset, int whence)
{
    if (file == nullptr || file->operations == nullptr || file->operations->Seek == nullptr)
    {
        return -1;
    }

    if (!vfs_file_io_begin(file))
    {
        return -1;
    }

    const int64_t result = file->operations->Seek(file, offset, whence);
    vfs_file_io_end(file);
    return result;
}

int64_t VirtualFileSystem::ReadDirectory(file_t* file, directory_entry_t* entry)
{
    if (file == nullptr || file->operations == nullptr || file->operations->ReadDirectory == nullptr)
    {
        return -1;
    }

    if (!vfs_file_io_begin(file))
    {
        return -1;
    }

    const int64_t result = file->operations->ReadDirectory(file, entry);
    vfs_file_io_end(file);
    return result;
}

int64_t VirtualFileSystem::Ioctl(file_t* file, uint64_t request, uint64_t argument)
{
    if (file == nullptr || file->operations == nullptr || file->operations->Ioctl == nullptr)
    {
        return -1;
    }

    if (!vfs_file_io_begin(file))
    {
        return -1;
    }

    const int64_t result = file->operations->Ioctl(file, request, argument);
    vfs_file_io_end(file);
    return result;
}

int64_t VirtualFileSystem::Mmap(file_t* file, void* address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset)
{
    if (file == nullptr || file->operations == nullptr || file->operations->Mmap == nullptr)
    {
        return -1;
    }

    if (!vfs_file_io_begin(file))
    {
        return -1;
    }

    const int64_t result = file->operations->Mmap(file, address, length, prot, flags, offset);
    vfs_file_io_end(file);
    return result;
}

bool VirtualFileSystem::Retain(file_t* file)
{
    if (file == nullptr)
    {
        return false;
    }

    return vfs_file_ref_acquire(file);
}

uint32_t VirtualFileSystem::Poll(file_t* file)
{
    if (file == nullptr)
        return 0x20; // POLLNVAL

    if (file->operations != nullptr && file->operations->Poll != nullptr)
    {
        return file->operations->Poll(file);
    }

    // Linux's default readiness for synchronous files, including EOF.
    return 0x145; // POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM
}

int64_t VirtualFileSystem::Close(file_t* file)
{
    if (file == nullptr)
    {
        return -1;
    }

    bool isLastReference = false;

    if (!vfs_file_ref_release(file, &isLastReference))
    {
        return -1;
    }

    if (!isLastReference)
    {
        return 0;
    }

    if (file->owner != nullptr)
    {
        file->owner->NotifyFile(file, (file->statusFlags & 3) == 0 ? 0x10 : 8);
        if (file->inode != nullptr && file->inode->filesystem != nullptr && file->inode->filesystem->type != nullptr && __atomic_sub_fetch(&file->inode->openCount, 1ULL, __ATOMIC_ACQ_REL) == 0 && file->inode->linkCount == 0 && file->owner->EventObserver != nullptr)
        {
            file->owner->EventObserver->NotifyVfs(file->inode, 0x400, nullptr, nullptr);
        }
    }

    if (file->closeObserver != nullptr)
    {
        file->closeObserver(file->closeObserverContext, file);
    }

    if (file->operations != nullptr && file->operations->Release != nullptr)
    {
        file->operations->Release(file);
    }

    kfree(file);
    return 0;
}

void VirtualFileSystem::ReleaseFileHandle(void* file)
{
    (void) Close(static_cast<file_t*>(file));
}

uint64_t VirtualFileSystem::ChangeMetadata(inode_t* inode, bool ownership, uint32_t first, uint32_t second, mount_t* mount, dentry_t* dentry)
{
    if (inode == nullptr)
    {
        return LINUX_ENOENT;
    }
    if ((inode->filesystem != nullptr && (__atomic_load_n(&inode->filesystem->flags, __ATOMIC_RELAXED) & 1)) || (mount != nullptr && (__atomic_load_n(&mount->flags, __ATOMIC_RELAXED) & 1)))
    {
        return (uint64_t) -30;
    }
    if (inode->filesystem != nullptr && inode->filesystem->type == nullptr && (inode->inodeOps == nullptr || inode->inodeOps->SetMetadata == nullptr))
    {
        return (uint64_t) -30;
    }
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    ProcessManager* processes = ResourceLayerImportCaps->processManager;
    process_t*      process   = processes->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }
    const uint32_t caller = process->credentials.user.filesystem;
    bool           member = second == process->credentials.group.filesystem;
    for (uint32_t i = 0; process->supplementaryGroups != nullptr && i < process->supplementaryGroups->count; ++i)
    {
        member |= second == process->supplementaryGroups->ids[i];
    }
    const uint32_t group = process->credentials.group.filesystem;
    spinlock_acquire_preempt(&inode->ioLock);
    uint64_t result = 0;
    if (caller != 0 && (caller != inode->uid || (ownership && ((first != UINT32_MAX && first != inode->uid) || (second != UINT32_MAX && second != inode->gid && !member)))))
    {
        result = (uint64_t) -1;
    }
    else if (!ownership && inode->type == INODE_SYMLINK)
    {
        result = LINUX_EOPNOTSUPP;
    }
    else
    {
        const uint32_t previousMode  = inode->mode;
        const uint32_t previousUid   = inode->uid;
        const uint32_t previousGid   = inode->gid;
        const bool     previousValid = inode->metadataValid;
        if (!inode->metadataValid)
        {
            inode->mode          = inode->type == INODE_DIRECTORY ? 0755 : inode->type == INODE_SOCKET || inode->type == INODE_SYMLINK ? 0777 : 0644;
            inode->metadataValid = true;
        }
        if (ownership)
        {
            if (first != UINT32_MAX)
            {
                inode->uid = first;
            }
            if (second != UINT32_MAX)
            {
                inode->gid = second;
            }
            if (inode->type == INODE_REGULAR && (first != UINT32_MAX || second != UINT32_MAX))
            {
                inode->mode &= ~06000U;
            }
        }
        else
        {
            inode->mode      = first & 07777;
            bool groupMember = group == inode->gid;
            for (uint32_t i = 0; process->supplementaryGroups != nullptr && i < process->supplementaryGroups->count; ++i)
            {
                groupMember |= process->supplementaryGroups->ids[i] == inode->gid;
            }
            if (caller != 0 && !groupMember)
            {
                inode->mode &= ~02000U;
            }
        }
        if (inode->filesystem != nullptr)
        {
            const uint32_t mode = inode->mode, uid = inode->uid, gid = inode->gid;
            inode->mode          = previousMode;
            inode->uid           = previousUid;
            inode->gid           = previousGid;
            inode->metadataValid = previousValid;
            result               = inode->inodeOps != nullptr && inode->inodeOps->SetMetadata != nullptr ? (uint64_t) inode->inodeOps->SetMetadata(inode, mode, uid, gid) : LINUX_EOPNOTSUPP;
            if (result == 0)
            {
                inode->mode          = mode;
                inode->uid           = uid;
                inode->gid           = gid;
                inode->metadataValid = true;
            }
        }
    }
    spinlock_release_preempt(&inode->ioLock);
    processes->ReleaseCredentials(process);
    if (result == 0 && EventObserver != nullptr)
    {
        EventObserver->NotifyVfs(inode, 4, dentry != nullptr && dentry->parent != nullptr ? dentry->parent->inode : nullptr, dentry != nullptr ? dentry->name : nullptr);
    }
    return result;
}

uint64_t VirtualFileSystem::Mount(const char* source, const char* target, const char* type, uint64_t flags, const char* options)
{
    constexpr uint64_t BIND = 4096, MOVE = 8192, REMOUNT = 32, RECURSIVE = 16384;
    if ((flags & 0xffff0000ULL) == 0xc0ed0000ULL)
    {
        flags &= ~0xffff0000ULL;
    }
    if (target == nullptr || (flags & ~0x03ffffffULL) != 0)
    {
        return LINUX_EINVAL;
    }
    vfs_path_t resolved = {};
    if (!ResolvePath({}, target, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }
    if (flags & ((15ULL << 17) | RECURSIVE))
    {
        return LINUX_EOPNOTSUPP;
    }
    if (flags & REMOUNT)
    {
        if (flags & MOVE || resolved.dentry != resolved.mount->root)
        {
            return LINUX_EINVAL;
        }
        if (flags & BIND)
        {
            __atomic_store_n(&resolved.mount->flags, flags & ~(BIND | REMOUNT), __ATOMIC_RELAXED);
            return 0;
        }
        filesystem_t* fs = resolved.mount->filesystem;
        if (fs == nullptr || fs->type == nullptr || fs->type->Remount == nullptr)
        {
            return LINUX_EOPNOTSUPP;
        }
        const int64_t result = fs->type->Remount(fs, flags & ~REMOUNT, options);
        if (result == 0)
        {
            __atomic_store_n(&fs->flags, flags & ~REMOUNT, __ATOMIC_RELAXED);
            __atomic_store_n(&resolved.mount->flags, flags & ~REMOUNT, __ATOMIC_RELAXED);
        }
        return (uint64_t) result;
    }
    if (flags & MOVE)
    {
        if (flags != MOVE || source == nullptr || source[0] == 0)
        {
            return LINUX_EINVAL;
        }
        vfs_path_t origin = {};
        if (!ResolvePath({}, source, &origin))
        {
            return LINUX_ENOENT;
        }
        spinlock_acquire_preempt(&MountLock);
        bool invalid = origin.dentry != origin.mount->root || origin.mount->parentMount == nullptr;
        for (mount_t* ancestor = resolved.mount; ancestor != nullptr; ancestor = ancestor->parentMount)
        {
            invalid |= ancestor == origin.mount;
        }
        if (!invalid && resolved.dentry->inode->type == origin.dentry->inode->type)
        {
            origin.mount->parentMount = resolved.mount;
            origin.mount->mountPoint  = resolved.dentry;
        }
        else
        {
            invalid = true;
        }
        spinlock_release_preempt(&MountLock);
        return invalid ? LINUX_EINVAL : 0;
    }
    mount_t* mount = static_cast<mount_t*>(kzalloc(sizeof(mount_t)));
    if (mount == nullptr)
    {
        return LINUX_ENOMEM;
    }
    uint64_t result = 0;
    if (flags & BIND)
    {
        vfs_path_t origin = {};
        if (source == nullptr || source[0] == 0)
        {
            result = LINUX_EINVAL;
        }
        else if (!ResolvePath({}, source, &origin))
        {
            result = LINUX_ENOENT;
        }
        else if ((origin.dentry->inode->type == INODE_DIRECTORY) != (resolved.dentry->inode->type == INODE_DIRECTORY))
        {
            result = (uint64_t) -20;
        }
        else
        {
            mount->filesystem = origin.mount->filesystem;
            mount->root       = origin.dentry;
            mount->flags      = __atomic_load_n(&origin.mount->flags, __ATOMIC_RELAXED);
        }
    }
    else
    {
        const filesystem_type_t* backend = nullptr;
        spinlock_acquire_preempt(&MountLock);
        for (size_t i = 0; type != nullptr && i < 32; ++i)
        {
            if (FileSystemTypes[i] != nullptr && strcmp(FileSystemTypes[i]->name, type) == 0)
            {
                backend = FileSystemTypes[i];
                break;
            }
        }
        spinlock_release_preempt(&MountLock);
        if (backend == nullptr)
        {
            result = type != nullptr && strcmp(type, "cpio") == 0 ? LINUX_EOPNOTSUPP : (uint64_t) -19;
        }
        else if (resolved.dentry->inode->type != INODE_DIRECTORY)
        {
            result = (uint64_t) -20;
        }
        else
        {
            filesystem_t* fs   = static_cast<filesystem_t*>(kzalloc(sizeof(filesystem_t)));
            dentry_t*     root = static_cast<dentry_t*>(kzalloc(sizeof(dentry_t)));
            if (fs == nullptr || root == nullptr)
            {
                kfree(fs);
                kfree(root);
                result = LINUX_ENOMEM;
            }
            else
            {
                fs->type                           = backend;
                fs->flags                          = flags;
                filesystem_mount_request_t request = {source, options, flags};
                result                             = (uint64_t) backend->Mount(fs, &request);
                if (result == 0 && (fs->rootInode == nullptr || fs->rootInode->type != INODE_DIRECTORY))
                {
                    result = LINUX_EINVAL;
                }
                if (result != 0)
                {
                    backend->Unmount(fs);
                    kfree(fs);
                    kfree(root);
                }
                else
                {
                    root->name                = "/";
                    root->inode               = fs->rootInode;
                    fs->rootInode->filesystem = fs;
                    mount->root               = root;
                    mount->filesystem         = fs;
                    mount->ownsFilesystem     = true;
                    mount->flags              = flags;
                }
            }
        }
    }
    if (result == 0)
    {
        mount->parentMount     = resolved.mount;
        mount->mountPoint      = resolved.dentry;
        mount->ownedAllocation = true;
        if (RegisterMount(mount))
        {
            return 0;
        }
        result = (uint64_t) -16;
    }
    if ((flags & BIND) == 0 && mount->filesystem != nullptr)
    {
        mount->filesystem->type->Unmount(mount->filesystem);
        kfree(mount->filesystem);
        kfree(mount->root);
    }
    kfree(mount);
    return result;
}

void VirtualFileSystem::SetEventManager(EventManager* events)
{
    EventObserver = events;
}

void VirtualFileSystem::NotifyFile(file_t* file, uint32_t mask)
{
    if (EventObserver == nullptr || file == nullptr || file->inode == nullptr)
    {
        return;
    }
    dentry_t* dentry = file->path.dentry;
    EventObserver->NotifyVfs(file->inode, mask, dentry != nullptr && dentry->parent != nullptr ? dentry->parent->inode : nullptr, dentry != nullptr ? dentry->name : nullptr);
}

void VirtualFileSystem::InitializeAnonymousInode(inode_t* inode, inode_type_t type, uint32_t mode)
{
    static uint64_t nextInode = 0x100000;
    inode->inodeNumber        = __atomic_add_fetch(&nextInode, 1ULL, __ATOMIC_RELAXED);
    inode->type               = type;
    inode->mode               = mode;
    inode->linkCount          = 1;
    inode->metadataValid      = true;
    if (ResourceLayerImportCaps == nullptr || ResourceLayerImportCaps->processManager == nullptr)
    {
        return;
    }
    process_t* process = ResourceLayerImportCaps->processManager->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        return;
    }
    inode->uid = process->credentials.user.filesystem;
    inode->gid = process->credentials.group.filesystem;
    ResourceLayerImportCaps->processManager->ReleaseCredentials(process);
}
