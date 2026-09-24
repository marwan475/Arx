#pragma once

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
};

struct inode_operations_t
{
	inode_t* (*Lookup)(inode_t* directory, const char* name);
};

struct filesystem_type_t
{
	const char* name;

	bool (*Mount)(filesystem_t* filesystem, void* backingResource, const void* mountOptions);

	void (*Unmount)(filesystem_t* filesystem);
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
};

struct inode_t
{
	uint64_t     inodeNumber;
	inode_type_t type;

	uint64_t size;

	filesystem_t* filesystem;

	const inode_operations_t* inodeOps;
	const file_operations_t*  fileOps;

	void* privateData;
};

struct dentry_t
{
	const char* name;
	char*       ownedName;

	dentry_t* parent;
	inode_t*  inode;

	bool cacheOwnedAllocation;
};

struct mount_t
{
	filesystem_t* filesystem;

	dentry_t* root;

	mount_t*  parentMount;
	dentry_t* mountPoint;

	mount_t* next;
};

struct vfs_path_t
{
	mount_t*  mount;
	dentry_t* dentry;
};

struct file_t
{
	vfs_path_t path;

	inode_t* inode;

	uint64_t offset;

	uint64_t statusFlags;

	const file_operations_t* operations;

	void* privateData;

	spinlock_t refLock;

	uint64_t refCount;
};

struct vfs_namespace_t
{
	mount_t* rootMount;
};

class VirtualFileSystem
{
public:
	explicit VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps);
	~VirtualFileSystem();

	uint32_t HashDentryKey(const dentry_t* parent, const char* name) const;
	bool CacheDentry(dentry_t* dentry);
	dentry_t* FindDentry(const dentry_t* parent, const char* name) const;
	dentry_t* Lookup(dentry_t* parent, const char* name);
	bool SetRootMount(mount_t* rootMount);
	bool RegisterMount(mount_t* mount);
	bool MountRootFileSystem(const char* filesystemType, void* source);
	bool ResolvePath(const vfs_path_t& start, const char* path, vfs_path_t* result);
	file_t* Open(const vfs_path_t& start, const char* path, uint64_t flags);
	int64_t Read(file_t* file, void* buffer, uint64_t count);
	int64_t Write(file_t* file, const void* buffer, uint64_t count);
	int64_t Seek(file_t* file, int64_t offset, int whence);
	int64_t ReadDirectory(file_t* file, directory_entry_t* entry);
	int64_t Ioctl(file_t* file, uint64_t request, uint64_t argument);
	int64_t Mmap(file_t* file, void* address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset);
	bool Retain(file_t* file);
	int64_t Close(file_t* file);

private:
	mount_t* FindChildMount(mount_t* parentMount, dentry_t* mountPoint) const;

	ResourceLayerCaps* ResourceLayerImportCaps;
	mutable spinlock_t DentryCacheLock;
	khashp_t* DentryCache;

	vfs_namespace_t Namespace;
	// TODO: Protect MountListHead with a lock once mounts can mutate concurrently.
	mount_t*        MountListHead;
};
