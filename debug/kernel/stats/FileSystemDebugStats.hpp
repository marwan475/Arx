#pragma once
#include "ProcessDebugStats.hpp"

#if DEBUG

class InitRamFileSystemManager;
class ResourceFileSystem;
class VirtualFileSystem;
class DebugManager;
struct initramfs_archive_t;
struct mount_t;
struct dentry_t;
struct vfs_overlay_dentry_entry_t;
struct vfs_namespace_t;

// arx.filesystem.archive/1. Borrowed owner-lifetime array of 256 entries.
// count points at current length; ready=1 means parsing reached the trailer.
// Stored paths are not canonicalized: leading ./ then / were removed by loader.
// A directory entry named '.' is shown as an archive root marker; backend
// Lookup rejects dot names, so it is not a child or a root-attribute override.
// Raw mode describes archive type; Resource Lookup exposes directory or regular.
// Size/data are current mutable parsed state. No payload read is implied.
struct InitRamDebugStats
{
    InitRamFileSystemManager* source;
    const initramfs_archive_t* records;
    const size_t* count;
    uint64_t capacity;
    uint64_t ready;
    arx_debug_group_t group;
    DebugOperationStats load;
};

struct ResourceFileSystemDebugStats
{
    ResourceFileSystem* source;
    InitRamFileSystemManager* archives;
    arx_debug_group_t group;
    DebugOperationStats mount;
    DebugOperationStats lookup;
    DebugOperationStats enumerate;
    DebugOperationStats read;
    DebugOperationStats write;
    DebugOperationStats mutation;
    uint64_t readBytes;
    uint64_t writtenBytes;
};

// arx.filesystem.namespace/1. Roots point at owner fields, not copied lists.
// Native interpretation: overlay(parent,name) precedes lazy hash cache, then
// supported backend Lookup. CPIO data comes from each verified resource node's
// fs->initRamManager, never an assumed global archive. Exact entries precede
// inferred directories. Raw archive kinds differ from effective backend kinds.
// Visible identity is (mount,dentry/path), not inode number. Cross child mounts
// by (parentMount,mountPoint), first in list order; bind roots can be interior.
// Skip retired/unlinked/forwarding entries as independent visible children.
// Follow forwarding and ancestry only under bounds/cycle checks. Cache is NOT
// a namespace catalogue; unknown callback backends always remain incomplete.
// khashp occupied bitmap indexes buckets, stride=key_len+val_len, not count.
// Each scanned bucket consumes work, including holes. All objects are ephemeral.
// Locks/groups indicate transitional state, not an atomic snapshot. Operations
// count nested API calls independently; totals are unsigned modular lifetime
// observations. No callback, target getter, lock or payload access is permitted.
struct VfsDebugStats
{
    VirtualFileSystem* source;
    const vfs_namespace_t* namespaceRoot;
    mount_t* const* mounts;
    const void* cacheRoot; // khashp_t* field address (anonymous C typedef).
    dentry_t* const* retired;
    vfs_overlay_dentry_entry_t* const* overlays;
    const void* resourceInodeOperations;
    const void* resourceFileOperations;
    const void* symlinkFileOperations;
    const void* resourceCaps; // Nonnull only after matching native backend thunk identities.
    const void* mountLock;
    const void* cacheLock;
    const void* overlayLock;
    const void* mutationLock;
    arx_debug_group_t group;
    uint64_t registeredMounts;
    uint64_t overlayEntries;
    uint64_t retiredEntries;
    DebugOperationStats mount;
    DebugOperationStats root;
    DebugOperationStats lookup;
    DebugOperationStats cache;
    DebugOperationStats mutation;
};

enum class FileSystemDebugResult
{
    BooleanOrPointer,
    SignedStatus,
    LinuxResult
};

class FileSystemDebugOperation
{
public:
    FileSystemDebugOperation(DebugOperationStats& stats, FileSystemDebugResult kind, uint64_t* bytes = nullptr)
        : Operation(stats), Kind(kind), Bytes(bytes)
    {
    }

    template<typename T> T Result(T value)
    {
        const uint64_t raw = (uint64_t) value;
        bool success = false;

        if (Kind == FileSystemDebugResult::BooleanOrPointer)
        {
            success = raw != 0;
        }
        else if (Kind == FileSystemDebugResult::SignedStatus)
        {
            success = (int64_t) raw >= 0;
        }
        else
        {
            success = raw < (uint64_t) -4095;
        }

        if (success && Bytes != nullptr)
        {
            ARX_DEBUG_ADD(*Bytes, raw);
        }

        Operation.Finish(success ? 0 : 2);
        return value;
    }

private:
    DebugOperation Operation;
    FileSystemDebugResult Kind;
    uint64_t* Bytes;
};

#define ARX_FILESYSTEM_SCOPE(stats, kind, bytes) \
    FileSystemDebugOperation debugFileSystem(stats, FileSystemDebugResult::kind, bytes); \
    DebugUpdate debugFileSystemUpdate(DebugStats.group)
#define ARX_FILESYSTEM_RETURN(value) debugFileSystem.Result(value)

#else
#define ARX_FILESYSTEM_SCOPE(stats, kind, bytes) do { } while (0)
#define ARX_FILESYSTEM_RETURN(value) (value)
#endif
