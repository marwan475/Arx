#pragma once
#include <debug/kernel/stats/FileSystemDebugStats.hpp>

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif
#include <klib/khashl/khashp.h>
#ifdef __cplusplus
}

#endif

#include <klib/spinlock.h>

struct ResourceLayerCaps;
class  EventManager;
class  VirtualFileSystem;
class  VirtualMemoryManager;
struct virt_backing_object;
struct virt_addr_space;
struct filesystem_t;
struct filesystem_type_t;
struct file_operations_t;
struct file_t;
struct directory_entry_t;
struct inode_t;

enum inode_type_t
{
    INODE_REGULAR,
    INODE_DIRECTORY,
    INODE_SYMLINK,
    INODE_CHAR_DEVICE,
    INODE_BLOCK_DEVICE,
    INODE_SOCKET,
    INODE_FIFO,
};

struct inode_creation_attributes_t
{
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
};

struct inode_operations_t
{
    inode_t* (*Lookup)(inode_t* directory, const char* name);
    // Callbacks receive kernel memory and return Linux negative errno values.
    // SetMetadata persists all requested fields atomically; the VFS commits
    // cached fields after success. The VFS holds inode->ioLock. Inode identity
    // remains stable until Unmount, including for open, unlinked files.
    int64_t (*SetMetadata)(inode_t* inode, uint32_t mode, uint32_t uid, uint32_t gid);
    int64_t (*Mkdir)(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes);
    int64_t (*Unlink)(inode_t* directory, const char* name, bool directoryEntry);
    int64_t (*Rename)(inode_t* directory, const char* name, inode_t* destination, const char* newName);
    int64_t (*Link)(inode_t* directory, const char* name, inode_t* target);
    int64_t (*Mknod)(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes, uint64_t device);
    int64_t (*Symlink)(inode_t* directory, const char* name, const char* target, const inode_creation_attributes_t* attributes);
    int64_t (*Truncate)(inode_t* inode, uint64_t size);
};

// Source and option strings are copied kernel buffers, valid for the callback.
struct filesystem_mount_request_t
{
    const char* source;
    const char* options;
    uint64_t    flags;
};

struct filesystem_type_t
{
    const char* name;

    int64_t (*Mount)(filesystem_t* filesystem, const filesystem_mount_request_t* request);

    void (*Unmount)(filesystem_t* filesystem);
    int64_t (*Remount)(filesystem_t* filesystem, uint64_t flags, const char* options);
};

struct filesystem_t
{
    const filesystem_type_t* type;

    inode_t* rootInode;

    void* privateData;

    uint64_t flags;
};

struct directory_entry_t
{
    const char*  name;
    uint64_t     inodeNumber;
    inode_type_t type;
};

struct file_iovec_t
{
    uint64_t address;
    uint64_t length;
};
static_assert(sizeof(file_iovec_t) == 16, "Linux LP64 iovec ABI");

struct file_operations_t
{
    int64_t (*Open)(file_t* file);

    int64_t (*Read)(file_t* file, void* buffer, uint64_t count);

    int64_t (*Write)(file_t* file, const void* buffer, uint64_t count);

    int64_t (*Seek)(file_t* file, int64_t offset, int whence);

    int64_t (*ReadDirectory)(file_t* file, directory_entry_t* entry);

    int64_t (*Ioctl)(file_t* file, uint64_t request, uint64_t argument);

    int64_t (*Mmap)(file_t* file, void* address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset);

    void (*Release)(file_t* file);
    // A nonblocking state snapshot. It may run under ReadinessLock: never
    // publish events or mutate observer registrations from this callback.
    uint32_t (*Poll)(file_t* file);
    // Blocking anonymous files own their user-copy/state transaction. These
    // callbacks run with a retained file, outside descriptor and VFS I/O locks.
    uint64_t (*ReadUser)(file_t* file, uint64_t address, uint64_t count);
    uint64_t (*WriteUser)(file_t* file, uint64_t address, uint64_t count);
    uint64_t (*ReadVectorUser)(file_t* file, const file_iovec_t* vectors, size_t count);
    uint64_t (*WriteVectorUser)(file_t* file, const file_iovec_t* vectors, size_t count);
    int64_t (*Truncate)(file_t* file, uint64_t size);
    uint64_t (*Control)(file_t* file, uint32_t command, uint64_t argument);
    bool linuxErrors;
    // Called under inode I/O lock. Returns a borrowed canonical backing and
    // restricts future mprotect permissions (including memfd seal policy).
    // Backend I/O must use these canonical pages while cached. External storage
    // needs a nonblocking positioned writer with object-lifetime context.
    int64_t (*GetBacking)(file_t*, VirtualMemoryManager*, uint32_t, bool, virt_backing_object**, uint32_t*);
    // Optional reference-drop transaction for files with collectable internal
    // references. It must serialize the decrement with its ownership graph.
    bool (*DropReference)(file_t* file, bool* last);
};

struct inode_t
{
    uint64_t     inodeNumber;
    inode_type_t type;

    uint64_t size;
    uint64_t linkCount;
    uint64_t openCount;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    bool     metadataValid;

    filesystem_t* filesystem;

    const inode_operations_t* inodeOps;
    const file_operations_t*  fileOps;

    void* privateData;

    spinlock_t           ioLock;
    uint64_t             namespaceGeneration;
    virt_backing_object* backing;
};

struct dentry_t
{
    const char* name;
    char*       ownedName;

    dentry_t* parent;
    inode_t*  inode;

    bool      cacheOwnedAllocation;
    dentry_t* retiredNext;
    dentry_t* renamedTo;
    bool      unlinked;
};

struct mount_t
{
    filesystem_t* filesystem;

    dentry_t* root;

    mount_t*  parentMount;
    dentry_t* mountPoint;

    mount_t* next;
    uint64_t flags;
    bool     ownedAllocation;
    bool     ownsFilesystem;
};

struct vfs_path_t
{
    mount_t*  mount;
    dentry_t* dentry;
};

struct file_close_observer_t
{
    file_close_observer_t* next;
    void (*Closing)(file_close_observer_t* observer, file_t* file);
    void* context;
};

struct file_t
{
    VirtualFileSystem* owner;
    vfs_path_t         path;

    inode_t* inode;

    uint64_t offset;

    uint64_t statusFlags;

    const file_operations_t* operations;

    void* privateData;

    spinlock_t ioLock;

    spinlock_t refLock;

    uint64_t refCount;

    // Installed while holding a live reference; invoked after the last put,
    // before Release/free, to detach weak readiness registrations.
    void (*closeObserver)(void* context, file_t* file);
    void*                  closeObserverContext;
    file_close_observer_t* readinessObservers;
};

struct vfs_namespace_t
{
    mount_t* rootMount;
};

// Internal retained layouts; callbacks remain opaque to inspection.
struct resource_fs_t;
struct resource_node_t;
struct ResourceLayerFileSystemCaps;

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

struct vfs_symlink_private_t
{
    char* target;
};

struct vfs_overlay_dentry_entry_t
{
    dentry_t*                   dentry;
    vfs_overlay_dentry_entry_t* next;
};

class VirtualFileSystem
{
public:
    ARX_DEBUG_ONLY(mutable VfsDebugStats DebugStats = {};)
    ARX_DEBUG_ONLY(void PublishDebug(DebugManager& manager);)
    // Readiness graph mutation and final-close detachment share this lock.
    // Observers are intrusive and weak. Closing runs under this lock and may
    // detach observers, but cannot close files or call producer/driver code.
    static spinlock_t ReadinessLock;
    static void       ObserveFileLocked(file_t* file, file_close_observer_t* observer);
    static void       UnobserveFileLocked(file_t* file, file_close_observer_t* observer);
    void              NotifyReadiness(file_t* file, uint32_t events);

    explicit VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps);
    void     SetEventManager(EventManager* events);
    void     InitializeAnonymousInode(inode_t* inode, inode_type_t type, uint32_t mode);
    void     NotifyFile(file_t* file, uint32_t mask);
    ~VirtualFileSystem();

    uint32_t  HashDentryKey(const dentry_t* parent, const char* name) const;
    bool      CacheDentry(dentry_t* dentry);
    dentry_t* FindDentry(const dentry_t* parent, const char* name) const;
    dentry_t* Lookup(dentry_t* parent, const char* name);
    bool      SetRootMount(mount_t* rootMount);
    bool      RegisterMount(mount_t* mount);
    // Registered type descriptors and backend inodes live until VFS teardown.
    // Unmount also cleans partially initialized state after a failed Mount.
    bool RegisterFileSystemType(const filesystem_type_t* type);
    bool MountRootFileSystem(const char* filesystemType, void* source);
    bool ResolvePath(const vfs_path_t& start, const char* path, vfs_path_t* result, uint64_t* accessError = nullptr);
    bool           BuildAbsolutePath(const vfs_path_t& path, char* output, size_t capacity) const;
    bool           BuildAbsolutePathFromDentry(const dentry_t* dentry, char* outPath, size_t outPathSize) const;
    bool           JoinPath(const char* base, const char* path, char* outPath, size_t outPathSize) const;
    int64_t StatNoFollow(const vfs_path_t& start, const char* path, inode_t** outInode, vfs_path_t* resolvedPath = nullptr);
    int64_t Symlink(const vfs_path_t& start, const char* target, const char* linkPath);
    int64_t Readlink(const vfs_path_t& start, const char* path, char* buffer, uint64_t bufferSize);
    int64_t Mkdir(const vfs_path_t& start, const char* path, uint32_t mode);
    int64_t Unlink(const vfs_path_t& start, const char* path, bool directory);
    int64_t Rename(const vfs_path_t& oldStart, const char* oldPath, const vfs_path_t& newStart, const char* newPath);
    int64_t Link(const vfs_path_t& oldStart, const char* oldPath, const vfs_path_t& newStart, const char* newPath, bool followSymlink);
    int64_t Mknod(const vfs_path_t& start, const char* path, uint32_t mode, uint64_t device);
    // Creation returns an inode open count owned by the bound endpoint.
    // Lookup returns stable borrowed backend identity and checks write access.
    int64_t        SocketNode(const char* path, bool create, inode_t** inode);
    void           ReleaseSocketNode(inode_t* inode);
    int64_t        TruncatePath(const vfs_path_t& start, const char* path, uint64_t size);
    int64_t        TruncateFile(file_t* file, uint64_t size);
    uint64_t       ChangeMetadata(inode_t* inode, bool ownership, uint32_t first, uint32_t second = 0, mount_t* mount = nullptr, dentry_t* dentry = nullptr);
    uint64_t       Mount(const char* source, const char* target, const char* type, uint64_t flags, const char* options = nullptr);
    file_t*        Open(const vfs_path_t& start, const char* path, uint64_t flags, int64_t* error = nullptr, uint32_t mode = 0644);
    int64_t        Read(file_t* file, void* buffer, uint64_t count);
    int64_t        Write(file_t* file, const void* buffer, uint64_t count);
    int64_t        ReadAt(file_t* file, void* buffer, uint64_t count, uint64_t offset);
    int64_t        WriteAt(file_t* file, const void* buffer, uint64_t count, uint64_t offset);
    int64_t        Seek(file_t* file, int64_t offset, int whence);
    int64_t        ReadDirectory(file_t* file, directory_entry_t* entry);
    int64_t        Ioctl(file_t* file, uint64_t request, uint64_t argument);
    int64_t        Mmap(file_t* file, void* address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset);
    int64_t        AcquireMappingBacking(file_t* file, uint64_t offset, uint64_t length, uint32_t protection, bool shared, virt_backing_object** object, uint32_t* maximumProtection);
    static void    ReleaseMappingBacking(virt_backing_object* object, bool shared, uint32_t maximumProtection);
    bool           Retain(file_t* file);
    uint64_t       CheckExecutable(file_t* file);
    uint32_t       Poll(file_t* file);
    static int64_t Close(file_t* file);
    static void    ReleaseFileHandle(void* file);

private:
    int64_t  Transfer(file_t* file, void* buffer, uint64_t count, bool write, const uint64_t* offset);
    int64_t  ResolveParent(const vfs_path_t& start, const char* path, vfs_path_t* parent, char* name, size_t capacity);
    uint64_t GetCreationAttributes(inode_t* parent, uint32_t mode, bool directory, bool symlink, inode_creation_attributes_t* attributes);
    uint64_t CheckInodeAccess(inode_t* inode, uint32_t requested);
    uint64_t CheckDirectoryMutation(const vfs_path_t& parent, const inode_t* victim = nullptr);
    void     InvalidateDentry(dentry_t* parent, const char* name);
    void     RetireEntryLocked(dentry_t* entry, dentry_t* moved = nullptr);
    bool     BuildBackendPath(const vfs_path_t& start, const char* path, char* output, size_t capacity);
    mount_t*  FindChildMount(mount_t* parentMount, dentry_t* mountPoint) const;
    dentry_t* FindOverlayDentry(const dentry_t* parent, const char* name) const;
    bool      AddOverlayDentry(dentry_t* dentry);

    ResourceLayerCaps*          ResourceLayerImportCaps;
    EventManager*               EventObserver;
    mutable spinlock_t          DentryCacheLock;
    khashp_t*                   DentryCache;
    dentry_t*                   RetiredDentries;
    mutable spinlock_t          OverlayDentryLock;
    vfs_overlay_dentry_entry_t* OverlayDentryHead;

    vfs_namespace_t          Namespace;
    mutable spinlock_t       MountLock;
    spinlock_t               NamespaceMutationLock = {};
    const filesystem_type_t* FileSystemTypes[32];
    mount_t*                 MountListHead;
};
