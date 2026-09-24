#include "layers/Logic/VirtualFileSystem.hpp"
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
	resource_fs_t* backend;
	ResourceLayerFileSystemCaps* caps;
};

struct vfs_resource_inode_private_t
{
	resource_node_t* backendNode;
	ResourceLayerFileSystemCaps* caps;
};

static inode_t* vfs_resource_lookup(inode_t* directory, const char* name);
static int64_t vfs_resource_open(file_t* file);
static int64_t vfs_resource_read(file_t* file, void* buffer, uint64_t count);
static int64_t vfs_resource_write(file_t* file, const void* buffer, uint64_t count);
static int64_t vfs_resource_seek(file_t* file, int64_t offset, int whence);
static void    vfs_resource_release(file_t* file);

static const inode_operations_t g_vfs_resource_inode_ops = {
	vfs_resource_lookup,
};

static const file_operations_t g_vfs_resource_file_ops = {
	vfs_resource_open,
	vfs_resource_read,
	vfs_resource_write,
	vfs_resource_seek,
	nullptr,
	nullptr,
	nullptr,
	vfs_resource_release,
};

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

static inode_t* vfs_wrap_resource_node(filesystem_t* filesystem,
	ResourceLayerFileSystemCaps* caps,
	resource_node_t* backendNode)
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
	inode->inodeNumber = info.inodeNumber;
	inode->type        = vfs_map_resource_node_type(info.type);
	inode->size        = info.size;
	inode->filesystem  = filesystem;
	inode->privateData = inodePrivate;

	inodePrivate->backendNode = backendNode;
	inodePrivate->caps        = caps;

	if (inode->type == INODE_DIRECTORY)
	{
		inode->inodeOps = &g_vfs_resource_inode_ops;
		inode->fileOps  = nullptr;
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
	(void) file;
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
	file->offset = newOffset;
	return (int64_t) newOffset;
}

static void vfs_resource_release(file_t* file)
{
	(void) file;
}

static uint32_t vfs_hash_parent_and_name(const dentry_t* parent, const char* name)
{
	if (name == nullptr)
	{
		return 0;
	}

	uint32_t       hash      = 2166136261U; // FNV-1a
	uintptr_t      parentPtr = (uintptr_t) parent;
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
	spinlock_acquire(&file->refLock);
	if (file->refCount == 0)
	{
		spinlock_release(&file->refLock);
		return false;
	}

	file->refCount++;
	spinlock_release(&file->refLock);
	return true;
}

static bool vfs_file_ref_release(file_t* file, bool* isLastReference)
{
	spinlock_acquire(&file->refLock);
	if (file->refCount == 0)
	{
		spinlock_release(&file->refLock);
		return false;
	}

	file->refCount--;
	*isLastReference = (file->refCount == 0);
	spinlock_release(&file->refLock);
	return true;
}

VirtualFileSystem::VirtualFileSystem(ResourceLayerCaps* resourceLayerCaps)
{
	ResourceLayerImportCaps = resourceLayerCaps;
	DentryCacheLock         = 0;
	DentryCache             = khp_init(sizeof(dentry_key_t), sizeof(dentry_t*), vfs_dentry_cache_hash_fn, vfs_dentry_cache_eq_fn);
	Namespace.rootMount     = nullptr;
	MountListHead           = nullptr;
}

VirtualFileSystem::~VirtualFileSystem()
{
	if (DentryCache != nullptr)
	{
		spinlock_acquire(&DentryCacheLock);
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
		spinlock_release(&DentryCacheLock);

		khp_destroy(DentryCache);
		DentryCache = nullptr;
	}
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

	for (mount_t* it = MountListHead; it != nullptr; it = it->next)
	{
		if (it == mount)
		{
			return true;
		}

		if (it->parentMount == mount->parentMount && it->mountPoint == mount->mountPoint)
		{
			return false;
		}
	}

	mount->next  = MountListHead;
	MountListHead = mount;
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

mount_t* VirtualFileSystem::FindChildMount(mount_t* parentMount, dentry_t* mountPoint) const
{
	for (mount_t* it = MountListHead; it != nullptr; it = it->next)
	{
		if (it->parentMount == parentMount && it->mountPoint == mountPoint)
		{
			return it;
		}
	}

	return nullptr;
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

	spinlock_acquire(&DentryCacheLock);

	dentry_key_t key;
	key.parent = dentry->parent;
	key.name   = dentry->ownedName != nullptr ? dentry->ownedName : duplicatedName;

	int     absent = 0;
	khint_t k      = khp_put(DentryCache, &key, &absent);
	if (k == khp_end(DentryCache))
	{
		spinlock_release(&DentryCacheLock);
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
		spinlock_release(&DentryCacheLock);
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
	spinlock_release(&DentryCacheLock);
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
		return nullptr;
	}

	spinlock_acquire(&DentryCacheLock);

	dentry_key_t key;
	key.parent = parent;
	key.name   = name;

	khint_t k = khp_get(DentryCache, &key);
	if (k == khp_end(DentryCache))
	{
		spinlock_release(&DentryCacheLock);
		return nullptr;
	}

	dentry_t* value = nullptr;
	khp_get_val(DentryCache, k, &value);
	if (value == nullptr)
	{
		spinlock_release(&DentryCacheLock);
		return nullptr;
	}

	spinlock_release(&DentryCacheLock);

	return value;
}

dentry_t* VirtualFileSystem::Lookup(dentry_t* parent, const char* name)
{
	if (parent == nullptr || name == nullptr || parent->inode == nullptr || parent->inode->type != INODE_DIRECTORY)
	{
		return nullptr;
	}

	dentry_t* cached = FindDentry(parent, name);
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

	return dentry;
}

bool VirtualFileSystem::ResolvePath(const vfs_path_t& start, const char* path, vfs_path_t* result)
{
	// TODO: Enforce trailing '/' directory requirement once open/create semantics land.
	// TODO: Add symlink traversal with a max-follow bound.
	if (result == nullptr || path == nullptr)
	{
		return false;
	}

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

		cur.mount = Namespace.rootMount;
		cur.dentry = Namespace.rootMount->root;
	}

	if (cur.mount == nullptr || cur.dentry == nullptr)
	{
		kfree(work);
		return false;
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
		*p = '\0';

		if (strcmp(component, ".") == 0)
		{
			*p = saved;
			continue;
		}

		if (strcmp(component, "..") == 0)
		{
			if (cur.dentry == cur.mount->root)
			{
				if (cur.mount->parentMount != nullptr && cur.mount->mountPoint != nullptr)
				{
					dentry_t* parentOfMountPoint = cur.mount->mountPoint->parent;
					cur.dentry = parentOfMountPoint != nullptr ? parentOfMountPoint : cur.mount->mountPoint;
					cur.mount  = cur.mount->parentMount;
				}
			}
			else if (cur.dentry->parent != nullptr)
			{
				cur.dentry = cur.dentry->parent;
			}
			*p = saved;
			continue;
		}

		dentry_t* next = Lookup(cur.dentry, component);
		*p            = saved;
		if (next == nullptr)
		{
			kfree(work);
			return false;
		}

		mount_t* childMount = FindChildMount(cur.mount, next);
		if (childMount != nullptr && childMount->root != nullptr)
		{
			cur.mount  = childMount;
			cur.dentry = childMount->root;
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

file_t* VirtualFileSystem::Open(const vfs_path_t& start, const char* path, uint64_t flags)
{
	vfs_path_t resolved = {};
	if (!ResolvePath(start, path, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
	{
		return nullptr;
	}

	file_t* file = (file_t*) kmalloc(sizeof(file_t));
	if (file == nullptr)
	{
		return nullptr;
	}

	memset(file, 0, sizeof(file_t));
	file->path       = resolved;
	file->inode      = resolved.dentry->inode;
	file->offset     = 0;
	file->statusFlags = flags;
	file->operations = file->inode->fileOps;
	file->privateData = nullptr;
	file->refLock    = 0;
	file->refCount   = 1;

	if (file->operations != nullptr && file->operations->Open != nullptr)
	{
		if (file->operations->Open(file) < 0)
		{
			if (file->operations->Release != nullptr)
			{
				file->operations->Release(file);
			}
			kfree(file);
			return nullptr;
		}
	}

	return file;
}

int64_t VirtualFileSystem::Read(file_t* file, void* buffer, uint64_t count)
{
	if (file == nullptr || file->operations == nullptr || file->operations->Read == nullptr)
	{
		return -1;
	}

	return file->operations->Read(file, buffer, count);
}

int64_t VirtualFileSystem::Write(file_t* file, const void* buffer, uint64_t count)
{
	if (file == nullptr || file->operations == nullptr || file->operations->Write == nullptr)
	{
		return -1;
	}

	return file->operations->Write(file, buffer, count);
}

int64_t VirtualFileSystem::Seek(file_t* file, int64_t offset, int whence)
{
	if (file == nullptr || file->operations == nullptr || file->operations->Seek == nullptr)
	{
		return -1;
	}

	return file->operations->Seek(file, offset, whence);
}

int64_t VirtualFileSystem::ReadDirectory(file_t* file, directory_entry_t* entry)
{
	if (file == nullptr || file->operations == nullptr || file->operations->ReadDirectory == nullptr)
	{
		return -1;
	}

	return file->operations->ReadDirectory(file, entry);
}

int64_t VirtualFileSystem::Ioctl(file_t* file, uint64_t request, uint64_t argument)
{
	if (file == nullptr || file->operations == nullptr || file->operations->Ioctl == nullptr)
	{
		return -1;
	}

	return file->operations->Ioctl(file, request, argument);
}

int64_t VirtualFileSystem::Mmap(file_t* file, void* address, uint64_t length, uint64_t prot, uint64_t flags, uint64_t offset)
{
	if (file == nullptr || file->operations == nullptr || file->operations->Mmap == nullptr)
	{
		return -1;
	}

	return file->operations->Mmap(file, address, length, prot, flags, offset);
}

bool VirtualFileSystem::Retain(file_t* file)
{
	if (file == nullptr)
	{
		return false;
	}

	return vfs_file_ref_acquire(file);
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

	if (file->operations != nullptr && file->operations->Release != nullptr)
	{
		file->operations->Release(file);
	}

	kfree(file);
	return 0;
}
