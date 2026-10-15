#include "layers/Request/VfsRequestManager.hpp"

#include "layers/Logic/InterProcessCommunicationManager.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

using linux_timespec_t = time_timespec_t;

struct linux_stat_t
{
    uint64_t         st_dev;
    uint64_t         st_ino;
    uint64_t         st_nlink;
    uint32_t         st_mode;
    uint32_t         st_uid;
    uint32_t         st_gid;
    int32_t          pad0;
    uint64_t         st_rdev;
    int64_t          st_size;
    int64_t          st_blksize;
    int64_t          st_blocks;
    linux_timespec_t st_atim;
    linux_timespec_t st_mtim;
    linux_timespec_t st_ctim;
    int64_t          reserved[3];
};

struct linux_dirent64_header_t
{
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
} __attribute__((packed));

struct linux_statfs_t
{
    int64_t  f_type;
    int64_t  f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid_val[2];
    int64_t  f_namelen;
    int64_t  f_frsize;
    int64_t  f_flags;
    int64_t  f_spare[4];
};

constexpr uint32_t LINUX_S_IFREG = 0100000U;
constexpr uint32_t LINUX_S_IFDIR = 0040000U;
constexpr uint32_t LINUX_S_IFLNK = 0120000U;

constexpr uint64_t LINUX_O_WRONLY  = 01ULL;
constexpr uint64_t LINUX_O_CREAT   = 0100ULL;
constexpr uint64_t LINUX_O_TRUNC   = 01000ULL;
constexpr uint64_t LINUX_O_CLOEXEC = 02000000ULL;

constexpr int64_t LINUX_AT_SYMLINK_NOFOLLOW = 0x100;
constexpr int64_t LINUX_AT_REMOVEDIR        = 0x200;
constexpr int64_t LINUX_AT_SYMLINK_FOLLOW   = 0x400;
constexpr int64_t LINUX_AT_EMPTY_PATH       = 0x1000;
constexpr int64_t LINUX_AT_EACCESS          = 0x200;

constexpr int64_t LINUX_F_OK = 0;
constexpr int64_t LINUX_X_OK = 1;
constexpr int64_t LINUX_W_OK = 2;
constexpr int64_t LINUX_R_OK = 4;

constexpr uint8_t LINUX_DT_UNKNOWN = 0;
constexpr uint8_t LINUX_DT_CHR     = 2;
constexpr uint8_t LINUX_DT_DIR     = 4;
constexpr uint8_t LINUX_DT_BLK     = 6;
constexpr uint8_t LINUX_DT_REG     = 8;
constexpr uint8_t LINUX_DT_LNK     = 10;

constexpr uint64_t LINUX_F_DUPFD         = 0;
constexpr uint64_t LINUX_F_GETFD         = 1;
constexpr uint64_t LINUX_F_SETFD         = 2;
constexpr uint64_t LINUX_F_GETFL         = 3;
constexpr uint64_t LINUX_F_SETFL         = 4;
constexpr uint64_t LINUX_F_DUPFD_CLOEXEC = 1030;
constexpr uint64_t LINUX_FD_CLOEXEC      = 1;

constexpr uint64_t LINUX_CLOSE_RANGE_CLOEXEC = 1ULL << 2;
constexpr uint64_t LINUX_CLOSE_RANGE_UNSHARE = 1ULL << 1;

constexpr int64_t  LINUX_TMPFS_MAGIC  = 0x01021994;
constexpr uint64_t LINUX_STATFS_BSIZE = 4096;

static uint32_t linux_mode_for_inode(const inode_t* inode)
{
    if (inode == nullptr)
    {
        return 0;
    }

    const uint32_t permissions = inode->metadataValid ? inode->mode & 07777U : inode->type == INODE_DIRECTORY ? 0755U : inode->type == INODE_SYMLINK || inode->type == INODE_SOCKET ? 0777U : 0644U;
    switch (inode->type)
    {
        case INODE_FIFO:
            return 0010000U | permissions;
        case INODE_SOCKET:
            return 0140000U | permissions;
        case INODE_DIRECTORY:
            return LINUX_S_IFDIR | permissions;
        case INODE_SYMLINK:
            return LINUX_S_IFLNK | permissions;
        default:
            return LINUX_S_IFREG | permissions;
    }
}

static void fill_linux_stat_from_inode(const inode_t* inode, linux_stat_t* statBuffer)
{
    if (inode == nullptr || statBuffer == nullptr)
    {
        return;
    }

    spinlock_acquire_preempt((spinlock_t*) &inode->ioLock);
    linux_stat_t statData = {};
    statData.st_ino       = inode->inodeNumber;
    statData.st_dev       = 1;
    statData.st_nlink     = 1;
    statData.st_mode      = linux_mode_for_inode(inode);
    statData.st_uid       = inode->uid;
    statData.st_gid       = inode->gid;
    statData.st_rdev      = 0;
    statData.st_size      = (int64_t) inode->size;
    statData.st_blksize   = 4096;
    statData.st_blocks    = (int64_t) ((inode->size + 511ULL) / 512ULL);

    *statBuffer = statData;
    spinlock_release_preempt((spinlock_t*) &inode->ioLock);
}

static uint8_t linux_dirent_type_for_inode_type(inode_type_t type)
{
    switch (type)
    {
        case INODE_DIRECTORY:
            return LINUX_DT_DIR;
        case INODE_SYMLINK:
            return LINUX_DT_LNK;
        case INODE_CHAR_DEVICE:
            return LINUX_DT_CHR;
        case INODE_BLOCK_DEVICE:
            return LINUX_DT_BLK;
        case INODE_SOCKET:
            return 12;
        case INODE_FIFO:
            return 1;
        case INODE_REGULAR:
            return LINUX_DT_REG;
        default:
            return LINUX_DT_UNKNOWN;
    }
}

static uint16_t align_dirent_record_length(uint16_t length)
{
    return (uint16_t) ((length + 7U) & ~7U);
}

static bool inode_allows_access_mode(const inode_t* inode, int64_t mode)
{
    if (inode == nullptr)
    {
        return false;
    }

    if (mode == LINUX_F_OK)
    {
        return true;
    }

    const uint32_t inodeMode = linux_mode_for_inode(inode) & 0777U;
    const uint32_t ownerBits = (inodeMode >> 6) & 0x7U;

    if ((mode & LINUX_R_OK) != 0 && (ownerBits & 4U) == 0)
    {
        return false;
    }

    if ((mode & LINUX_W_OK) != 0 && (ownerBits & 2U) == 0)
    {
        return false;
    }

    if ((mode & LINUX_X_OK) != 0 && (ownerBits & 1U) == 0)
    {
        return false;
    }

    return true;
}

static void fill_linux_statfs_from_inode(const inode_t* inode, linux_statfs_t* statfsBuffer)
{
    if (inode == nullptr || statfsBuffer == nullptr)
    {
        return;
    }

    linux_statfs_t statfsData = {};
    statfsData.f_type         = LINUX_TMPFS_MAGIC;
    statfsData.f_bsize        = (int64_t) LINUX_STATFS_BSIZE;
    statfsData.f_frsize       = (int64_t) LINUX_STATFS_BSIZE;
    statfsData.f_namelen      = 255;
    statfsData.f_flags        = 0;

    uint64_t blocks = (inode->size + (LINUX_STATFS_BSIZE - 1ULL)) / LINUX_STATFS_BSIZE;

    if (blocks == 0)
    {
        blocks = 1;
    }

    statfsData.f_blocks = blocks;
    statfsData.f_bfree  = 0;
    statfsData.f_bavail = 0;
    statfsData.f_files  = 0;
    statfsData.f_ffree  = 0;

    *statfsBuffer = statfsData;
}

static bool is_obviously_invalid_user_pointer(const void* pointer)
{
    const uintptr_t address = (uintptr_t) pointer;
    return address == 0 || address < 0x1000ULL;
}

static bool copy_user_path(VirtualMemoryManager* virtualMemoryManager, process_t* process, uintptr_t userPathAddress, char* pathBuffer, size_t pathBufferSize)
{
    if (virtualMemoryManager == nullptr || process == nullptr || pathBuffer == nullptr || pathBufferSize == 0)
    {
        return false;
    }

    if (is_obviously_invalid_user_pointer(reinterpret_cast<const void*>(userPathAddress)))
    {
        return false;
    }

    return safe_read_user_string(userPathAddress, pathBuffer, pathBufferSize, nullptr);
}

static bool resolve_relative_base_for_dirfd(process_t* currentProcess, int64_t dirfd, vfs_path_t* outStart, bool* outNonDirectoryDirfd)
{
    if (currentProcess == nullptr || outStart == nullptr || outNonDirectoryDirfd == nullptr)
    {
        return false;
    }

    *outNonDirectoryDirfd = false;

    if (dirfd == LINUX_AT_FDCWD)
    {
        *outStart = {};
        return true;
    }

    if (dirfd < 0)
    {
        return false;
    }

    const uint64_t     fd         = (uint64_t) dirfd;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return false;
    }

    file_t* baseFile = static_cast<file_t*>(descriptor->file);

    if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
    {
        *outNonDirectoryDirfd = true;
        return true;
    }

    *outStart = baseFile->path;
    return true;
}

static bool resolve_open_start_path(VirtualFileSystem* virtualFileSystem, process_t* currentProcess, const arch_syscall_frame_t* frame, const char* path, vfs_path_t* outStart, bool* outNonDirectoryDirfd)
{
    if (virtualFileSystem == nullptr || currentProcess == nullptr || frame == nullptr || path == nullptr || outStart == nullptr || outNonDirectoryDirfd == nullptr)
    {
        return false;
    }

    *outNonDirectoryDirfd = false;

    if (path[0] == '/')
    {
        *outStart = {};
        return true;
    }

    if ((int64_t) frame->arg0 == LINUX_AT_FDCWD)
    {
        vfs_path_t start = {};

        if (!virtualFileSystem->ResolvePath(start, "/", &start))
        {
            return false;
        }

        *outStart = start;
        return true;
    }

    return resolve_relative_base_for_dirfd(currentProcess, (int64_t) frame->arg0, outStart, outNonDirectoryDirfd);
}

static bool expand_cwd_path(VirtualFileSystem* vfs, process_t* process, int64_t dirfd, const char* path, char* scratch, size_t capacity, const char** effectivePath)
{
    *effectivePath = path;

    if (path[0] != '/' && dirfd == LINUX_AT_FDCWD)
    {
        if (!vfs->JoinPath(process->cwdPath, path, scratch, capacity))
        {
            return false;
        }

        *effectivePath = scratch;
    }

    return true;
}

static uint64_t prepare_request_path(VirtualFileSystem* vfs, process_t* process, int64_t dirfd, const char* path, char* cwdPath, size_t capacity, const char** effectivePath, vfs_path_t* start)
{
    if (!expand_cwd_path(vfs, process, dirfd, path, cwdPath, capacity, effectivePath))
    {
        return LINUX_ENAMETOOLONG;
    }

    if ((*effectivePath)[0] != '/')
    {
        bool nonDirectoryBase = false;

        if (!resolve_relative_base_for_dirfd(process, dirfd, start, &nonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (nonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    return 0;
}

struct linux_iovec_t
{
    void*    iov_base;
    uint64_t iov_len;
};

constexpr int64_t LINUX_IOV_MAX = 1024;

static uint64_t normalize_vfs_result(int64_t result);

constexpr uint64_t LINUX_IO_CHUNK_SIZE = 1024;

static uint64_t perform_user_buffer_io(VirtualMemoryManager* virtualMemoryManager, virt_addr_space_t* addressSpace, VirtualFileSystem* virtualFileSystem, file_t* file, uintptr_t userBufferAddress, uint64_t count, bool isWrite, const uint64_t* positionedOffset = nullptr)
{
    if (virtualMemoryManager == nullptr || addressSpace == nullptr || virtualFileSystem == nullptr || file == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (count == 0)
    {
        return 0;
    }

    if (is_obviously_invalid_user_pointer(reinterpret_cast<const void*>(userBufferAddress)))
    {
        return LINUX_EFAULT;
    }

    char     kernelBuffer[LINUX_IO_CHUNK_SIZE] = {};
    uint64_t totalTransferred                  = 0;

    while (totalTransferred < count)
    {
        const uint64_t  remaining        = count - totalTransferred;
        const uint64_t  chunkSize        = remaining > LINUX_IO_CHUNK_SIZE ? LINUX_IO_CHUNK_SIZE : remaining;
        const uintptr_t userChunkAddress = userBufferAddress + totalTransferred;

        if (isWrite)
        {
            if (!safe_read_user(userChunkAddress, kernelBuffer, (size_t) chunkSize))
            {
                return (totalTransferred > 0) ? totalTransferred : LINUX_EFAULT;
            }
        }

        int64_t ioResult;

        if (positionedOffset != nullptr)
        {
            if (*positionedOffset > INT64_MAX || totalTransferred > INT64_MAX - *positionedOffset)
            {
                return totalTransferred > 0 ? totalTransferred : LINUX_EINVAL;
            }

            const uint64_t offset = *positionedOffset + totalTransferred;
            ioResult              = isWrite ? virtualFileSystem->WriteAt(file, kernelBuffer, chunkSize, offset) : virtualFileSystem->ReadAt(file, kernelBuffer, chunkSize, offset);
        }
        else
            ioResult = isWrite ? virtualFileSystem->Write(file, kernelBuffer, chunkSize) : virtualFileSystem->Read(file, kernelBuffer, chunkSize);

        if (ioResult < 0)
        {
            return (totalTransferred > 0) ? totalTransferred : (file->operations != nullptr && file->operations->linuxErrors ? (uint64_t) ioResult : normalize_vfs_result(ioResult));
        }

        if (ioResult == 0)
        {
            break;
        }

        const uint64_t transferredThisChunk = (uint64_t) ioResult;

        if (!isWrite)
        {
            if (!safe_write_user(userChunkAddress, kernelBuffer, (size_t) transferredThisChunk))
            {
                return (totalTransferred > 0) ? totalTransferred : LINUX_EFAULT;
            }
        }

        if (transferredThisChunk > (uint64_t) INT64_MAX - totalTransferred)
        {
            return (totalTransferred > 0) ? totalTransferred : LINUX_EINVAL;
        }

        totalTransferred += transferredThisChunk;

        if (transferredThisChunk < chunkSize)
        {
            break;
        }
    }

    return totalTransferred;
}

static uint64_t perform_positioned_user_buffer_io(VirtualMemoryManager* virtualMemoryManager, virt_addr_space_t* addressSpace, VirtualFileSystem* virtualFileSystem, file_t* file, uintptr_t userBufferAddress, uint64_t count, bool isWrite, uint64_t positionedOffset)
{
    if (virtualMemoryManager == nullptr || addressSpace == nullptr || virtualFileSystem == nullptr || file == nullptr)
    {
        return LINUX_EINVAL;
    }

    return perform_user_buffer_io(virtualMemoryManager, addressSpace, virtualFileSystem, file, userBufferAddress, count, isWrite, &positionedOffset);
}

static uint64_t perform_user_vector_io(VirtualMemoryManager* virtualMemoryManager, process_t* process, VirtualFileSystem* virtualFileSystem, file_t* file, uintptr_t userIovAddress, int64_t iovCount, bool isWrite, bool positioned, uint64_t positionedOffset)
{
    if (virtualMemoryManager == nullptr || process == nullptr || process->addressSpace == nullptr || virtualFileSystem == nullptr || file == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (iovCount == 0)
    {
        return 0;
    }

    linux_iovec_t iov[LINUX_IOV_MAX] = {};
    const size_t  iovBytes           = (size_t) iovCount * sizeof(linux_iovec_t);

    if (!safe_read_user(userIovAddress, iov, iovBytes))
    {
        return LINUX_EFAULT;
    }

    uint64_t runningOffset = positionedOffset;
    uint64_t total         = 0;

    for (int64_t i = 0; i < iovCount; ++i)
    {
        const uint64_t len = iov[i].iov_len;

        if (len == 0)
        {
            continue;
        }

        if (is_obviously_invalid_user_pointer(iov[i].iov_base))
        {
            return (total > 0) ? total : LINUX_EFAULT;
        }

        const uint64_t ioResult = perform_user_buffer_io(virtualMemoryManager, process->addressSpace, virtualFileSystem, file, (uintptr_t) iov[i].iov_base, len, isWrite, positioned ? &runningOffset : nullptr);

        if ((int64_t) ioResult < 0)
        {
            return (total > 0) ? total : ioResult;
        }

        if (ioResult > (uint64_t) INT64_MAX - total)
        {
            return (total > 0) ? total : LINUX_EINVAL;
        }

        total += ioResult;

        if (positioned)
        {
            runningOffset += ioResult;
        }

        if (ioResult < len)
        {
            break;
        }
    }

    return total;
}

static uint64_t normalize_vfs_result(int64_t result)
{
    if (result >= 0)
    {
        return (uint64_t) result;
    }

    if (result == -1)
    {
        return LINUX_EIO;
    }

    if (result >= -4095)
    {
        return (uint64_t) result;
    }

    return LINUX_EIO;
}

static uint64_t duplicate_from_fd(ProcessManager* processManager, VirtualFileSystem* vfs, process_t* process, file_descriptor_t* descriptor, uint32_t flags, uint64_t minFd)
{
    file_t* file = static_cast<file_t*>(descriptor->file);

    if (!vfs->Retain(file))
    {
        return LINUX_EIO;
    }

    const int64_t fd = processManager->AddFileDescriptorFrom(process, descriptor->file, flags, minFd, descriptor->release);

    if (fd < 0)
    {
        (void) vfs->Close(file);
        return (uint64_t) fd;
    }

    return (uint64_t) fd;
}

static uint64_t duplicate_to_fd(ProcessManager* processManager, VirtualFileSystem* virtualFileSystem, process_t* currentProcess, uint64_t oldFd, uint64_t newFd, uint32_t newFlags)
{
    if (currentProcess->fileDescriptors == nullptr || oldFd >= currentProcess->fileDescriptorCount || currentProcess->fileDescriptors[oldFd].file == nullptr)
    {
        return LINUX_EBADF;
    }

    if (oldFd == newFd)
    {
        return newFd;
    }

    if (newFd >= currentProcess->fileLimit.current)
    {
        return LINUX_EBADF;
    }

    if (!processManager->EnsureFileDescriptorCapacity(currentProcess, newFd))
    {
        return LINUX_ENOMEM;
    }

    if (currentProcess->fileDescriptors[newFd].file != nullptr)
    {
        file_t* existing                             = static_cast<file_t*>(currentProcess->fileDescriptors[newFd].file);
        currentProcess->fileDescriptors[newFd].file  = nullptr;
        currentProcess->fileDescriptors[newFd].flags = FD_FLAG_NONE;
        (void) virtualFileSystem->Close(existing);
    }

    file_t* oldFile = static_cast<file_t*>(currentProcess->fileDescriptors[oldFd].file);

    if (!virtualFileSystem->Retain(oldFile))
    {
        return LINUX_EIO;
    }

    currentProcess->fileDescriptors[newFd].file    = currentProcess->fileDescriptors[oldFd].file;
    currentProcess->fileDescriptors[newFd].flags   = newFlags;
    currentProcess->fileDescriptors[newFd].release = currentProcess->fileDescriptors[oldFd].release;
    return newFd;
}

VfsRequestManager::VfsRequestManager(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps) : ResourceCaps(resourceLayerCaps), LogicCaps(logicLayerCaps)
{
}

uint64_t VfsRequestManager::HandleOpenatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    char path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    const int64_t dirfd                                                = (int64_t) frame->arg0;
    char          cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char*   effectivePath                                        = path;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, dirfd, path, cwdResolvedPath, sizeof(cwdResolvedPath), &effectivePath))
    {
        return LINUX_ENAMETOOLONG;
    }

    vfs_path_t start            = {};
    bool       nonDirectoryBase = false;

    if (!resolve_open_start_path(LogicCaps->virtualFileSystem, currentProcess, frame, effectivePath, &start, &nonDirectoryBase))
    {
        return LINUX_EBADF;
    }

    if (nonDirectoryBase)
    {
        return LINUX_ENOTDIR;
    }

    file_t* file = LogicCaps->virtualFileSystem->Open(start, effectivePath, frame->arg2);

    if (file == nullptr)
    {
        return LINUX_ENOENT;
    }

    int64_t fd = ResourceCaps->processManager->AddFileDescriptor(currentProcess, static_cast<file_handle_t>(file), FD_FLAG_NONE, VirtualFileSystem::ReleaseFileHandle);

    if (fd < 0)
    {
        (void) LogicCaps->virtualFileSystem->Close(file);
        return (uint64_t) fd;
    }

    return (uint64_t) fd;
}

uint64_t VfsRequestManager::HandleOpenRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t openatFrame = *frame;
    openatFrame.arg1                 = frame->arg0;
    openatFrame.arg2                 = frame->arg1;
    openatFrame.arg3                 = frame->arg2;
    openatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;

    return HandleOpenatRequest(&openatFrame);
}

uint64_t VfsRequestManager::HandleCreatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t openatFrame = {};
    openatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    openatFrame.arg1                 = frame->arg0;
    openatFrame.arg2                 = LINUX_O_WRONLY | LINUX_O_CREAT | LINUX_O_TRUNC;
    openatFrame.arg3                 = frame->arg1;

    return HandleOpenatRequest(&openatFrame);
}

uint64_t VfsRequestManager::HandleCloseRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t* file      = static_cast<file_t*>(descriptor->file);
    descriptor->file  = nullptr;
    descriptor->flags = FD_FLAG_NONE;

    int64_t closeResult = LogicCaps->virtualFileSystem->Close(file);
    return normalize_vfs_result(closeResult);
}

uint64_t VfsRequestManager::HandleClose_rangeRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    uint64_t first = frame->arg0;
    uint64_t last  = frame->arg1;
    uint64_t flags = frame->arg2;

    if (first > last)
    {
        return LINUX_EINVAL;
    }

    if ((flags & ~(LINUX_CLOSE_RANGE_CLOEXEC | LINUX_CLOSE_RANGE_UNSHARE)) != 0)
    {
        return LINUX_EINVAL;
    }

    if (currentProcess->fileDescriptors == nullptr || currentProcess->fileDescriptorCount == 0)
    {
        return 0;
    }

    if (last >= currentProcess->fileDescriptorCount)
    {
        last = currentProcess->fileDescriptorCount - 1;
    }

    for (uint64_t fd = first; fd <= last; ++fd)
    {
        file_descriptor_t* descriptor = &currentProcess->fileDescriptors[fd];

        if (descriptor->file == nullptr)
        {
            continue;
        }

        if ((flags & LINUX_CLOSE_RANGE_CLOEXEC) != 0)
        {
            descriptor->flags |= FD_FLAG_CLOEXEC;
            continue;
        }

        file_t* file      = static_cast<file_t*>(descriptor->file);
        descriptor->file  = nullptr;
        descriptor->flags = FD_FLAG_NONE;
        (void) LogicCaps->virtualFileSystem->Close(file);
    }

    return 0;
}

uint64_t VfsRequestManager::HandleMkdiratRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t  dirfd                                     = (int64_t) frame->arg0;
    char           path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const uint32_t mode                                      = (uint32_t) frame->arg2;

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    char           cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char*    effectivePath                                        = nullptr;
    vfs_path_t     start                                                = {};
    const uint64_t pathResult                                           = prepare_request_path(LogicCaps->virtualFileSystem, currentProcess, dirfd, path, cwdResolvedPath, sizeof(cwdResolvedPath), &effectivePath, &start);

    if (pathResult != 0)
    {
        return pathResult;
    }

    return normalize_vfs_result(LogicCaps->virtualFileSystem->Mkdir(start, effectivePath, mode));
}

uint64_t VfsRequestManager::HandleMkdirRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t mkdiratFrame = {};
    mkdiratFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    mkdiratFrame.arg1                 = frame->arg0;
    mkdiratFrame.arg2                 = frame->arg1;
    return HandleMkdiratRequest(&mkdiratFrame);
}

static uint64_t perform_anonymous_user_io(ResourceLayerCaps* resources, LogicLayerCaps* logic, const arch_syscall_frame_t* frame, bool write, bool* handled)
{
    *handled = false;

    if (frame == nullptr || resources == nullptr || resources->processManager == nullptr || logic == nullptr || logic->virtualFileSystem == nullptr)
    {
        return 0;
    }

    file_t* file                                      = nullptr;
    uint64_t (*callback)(file_t*, uint64_t, uint64_t) = nullptr;
    {
        FileDescriptorGuard guard(resources->processManager->GetCurrentProcess());
        file_descriptor_t*  descriptor = resources->processManager->GetFileDescriptor(resources->processManager->GetCurrentProcess(), frame->arg0);

        if (descriptor == nullptr)
        {
            return 0;
        }

        file = static_cast<file_t*>(descriptor->file);

        if (file->operations == nullptr)
        {
            return 0;
        }

        callback = write ? file->operations->WriteUser : file->operations->ReadUser;

        if (callback == nullptr)
        {
            return 0;
        }

        *handled = true;

        if (!logic->virtualFileSystem->Retain(file))
        {
            return LINUX_EBADF;
        }
    }

    const uint64_t result = callback(file, frame->arg1, frame->arg2);
    logic->virtualFileSystem->Close(file);
    return result;
}

uint64_t VfsRequestManager::HandleReadRequest(const arch_syscall_frame_t* frame)
{
    bool           handled;
    const uint64_t anonymousResult = perform_anonymous_user_io(ResourceCaps, LogicCaps, frame, false, &handled);

    if (handled)
    {
        return anonymousResult;
    }

    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t count = frame->arg2;

    if (count == 0)
    {
        return 0;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_user_buffer_io(ResourceCaps->virtualMemoryManager, currentProcess->addressSpace, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, count, false);
}

uint64_t VfsRequestManager::HandleWriteRequest(const arch_syscall_frame_t* frame)
{
    bool           handled;
    const uint64_t anonymousResult = perform_anonymous_user_io(ResourceCaps, LogicCaps, frame, true, &handled);

    if (handled)
    {
        return anonymousResult;
    }

    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t count = frame->arg2;

    if (count == 0)
    {
        return 0;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_user_buffer_io(ResourceCaps->virtualMemoryManager, currentProcess->addressSpace, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, count, true);
}

uint64_t VfsRequestManager::HandlePread64Request(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t count = frame->arg2;

    if (count == 0)
    {
        return 0;
    }

    const int64_t offset = (int64_t) frame->arg3;

    if (offset < 0)
    {
        return LINUX_EINVAL;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_positioned_user_buffer_io(ResourceCaps->virtualMemoryManager, currentProcess->addressSpace, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, count, false, (uint64_t) offset);
}

uint64_t VfsRequestManager::HandlePwrite64Request(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t count = frame->arg2;

    if (count == 0)
    {
        return 0;
    }

    const int64_t offset = (int64_t) frame->arg3;

    if (offset < 0)
    {
        return LINUX_EINVAL;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_positioned_user_buffer_io(ResourceCaps->virtualMemoryManager, currentProcess->addressSpace, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, count, true, (uint64_t) offset);
}

static uint64_t perform_anonymous_vector_io(ResourceLayerCaps* resources, LogicLayerCaps* logic, const arch_syscall_frame_t* frame, bool write, bool* handled)
{
    *handled = false;

    if (frame == nullptr || resources == nullptr || resources->processManager == nullptr || logic == nullptr || logic->virtualFileSystem == nullptr)
    {
        return 0;
    }

    file_t* file = nullptr;
    uint64_t (*callback)(file_t*, const file_iovec_t*, size_t) = nullptr;
    {
        FileDescriptorGuard guard(resources->processManager->GetCurrentProcess());
        file_descriptor_t*  descriptor = resources->processManager->GetFileDescriptor(resources->processManager->GetCurrentProcess(), frame->arg0);

        if (descriptor == nullptr)
        {
            return 0;
        }

        file = static_cast<file_t*>(descriptor->file);

        if (file->operations == nullptr)
        {
            return 0;
        }

        callback = write ? file->operations->WriteVectorUser : file->operations->ReadVectorUser;

        if (callback == nullptr)
        {
            return 0;
        }

        *handled = true;

        if (!logic->virtualFileSystem->Retain(file))
        {
            return LINUX_EBADF;
        }
    }

    file_iovec_t* vectors = nullptr;
    const uint64_t result = [&]() -> uint64_t
    {
        const int64_t count = (int64_t) frame->arg2;

        if (count < 0 || count > LINUX_IOV_MAX)
        {
            return LINUX_EINVAL;
        }

        if (count == 0)
        {
            return 0;
        }

        const size_t bytes = (size_t) count * sizeof(file_iovec_t);
        vectors            = static_cast<file_iovec_t*>(vmalloc(bytes));

        if (vectors == nullptr)
        {
            return LINUX_ENOMEM;
        }

        if (!safe_read_user(frame->arg1, vectors, bytes))
        {
            return LINUX_EFAULT;
        }

        constexpr uint64_t maximum = 0x7ffff000;
        uint64_t           total   = 0;

        for (int64_t i = 0; i < count; ++i)
        {
            if (vectors[i].length > INT64_MAX)
            {
                return LINUX_EINVAL;
            }

            if (vectors[i].length != 0 && !range_within_u64(vectors[i].address, vectors[i].length, CANONICAL_USER_BASE, CANONICAL_USER_END))
            {
                return LINUX_EFAULT;
            }

            if (vectors[i].length > maximum - total)
            {
                vectors[i].length = maximum - total;
            }

            total += vectors[i].length;
        }

        return total != 0 ? callback(file, vectors, (size_t) count) : 0;
    }();
    vfree(vectors);
    logic->virtualFileSystem->Close(file);
    return result;
}

uint64_t VfsRequestManager::HandleReadvRequest(const arch_syscall_frame_t* frame)
{
    bool           handled;
    const uint64_t anonymousResult = perform_anonymous_vector_io(ResourceCaps, LogicCaps, frame, false, &handled);

    if (handled)
    {
        return anonymousResult;
    }

    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const int64_t iovCount = (int64_t) frame->arg2;

    if (iovCount < 0 || iovCount > LINUX_IOV_MAX)
    {
        return LINUX_EINVAL;
    }

    if (iovCount == 0)
    {
        return 0;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_user_vector_io(ResourceCaps->virtualMemoryManager, currentProcess, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, iovCount, false, false, 0);
}

uint64_t VfsRequestManager::HandleWritevRequest(const arch_syscall_frame_t* frame)
{
    bool           handled;
    const uint64_t anonymousResult = perform_anonymous_vector_io(ResourceCaps, LogicCaps, frame, true, &handled);

    if (handled)
    {
        return anonymousResult;
    }

    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const int64_t iovCount = (int64_t) frame->arg2;

    if (iovCount < 0 || iovCount > LINUX_IOV_MAX)
    {
        return LINUX_EINVAL;
    }

    if (iovCount == 0)
    {
        return 0;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_user_vector_io(ResourceCaps->virtualMemoryManager, currentProcess, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, iovCount, true, false, 0);
}

uint64_t VfsRequestManager::HandlePreadvRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const int64_t iovCount = (int64_t) frame->arg2;

    if (iovCount < 0 || iovCount > LINUX_IOV_MAX)
    {
        return LINUX_EINVAL;
    }

    if (iovCount == 0)
    {
        return 0;
    }

    const int64_t offset = (int64_t) frame->arg3;

    if (offset < 0)
    {
        return LINUX_EINVAL;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_user_vector_io(ResourceCaps->virtualMemoryManager, currentProcess, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, iovCount, false, true, (uint64_t) offset);
}

uint64_t VfsRequestManager::HandlePwritevRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const int64_t iovCount = (int64_t) frame->arg2;

    if (iovCount < 0 || iovCount > LINUX_IOV_MAX)
    {
        return LINUX_EINVAL;
    }

    if (iovCount == 0)
    {
        return 0;
    }

    const int64_t offset = (int64_t) frame->arg3;

    if (offset < 0)
    {
        return LINUX_EINVAL;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return perform_user_vector_io(ResourceCaps->virtualMemoryManager, currentProcess, LogicCaps->virtualFileSystem, file, (uintptr_t) frame->arg1, iovCount, true, true, (uint64_t) offset);
}

uint64_t VfsRequestManager::HandleLseekRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const int whence = (int) frame->arg2;

    if (whence < 0 || whence > 2)
    {
        return LINUX_EINVAL;
    }

    file_t* file   = static_cast<file_t*>(descriptor->file);
    int64_t offset = (int64_t) frame->arg1;
    int64_t result = LogicCaps->virtualFileSystem->Seek(file, offset, whence);
    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandleGetcwdRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uintptr_t bufferAddress = (uintptr_t) frame->arg0;
    const uint64_t  bufferSize    = frame->arg1;

    if (is_obviously_invalid_user_pointer(reinterpret_cast<const void*>(bufferAddress)))
    {
        return LINUX_EFAULT;
    }

    if (bufferSize == 0)
    {
        return LINUX_EINVAL;
    }

    const size_t cwdLength = strlen(currentProcess->cwdPath) + 1;

    if (cwdLength > bufferSize)
    {
        return LINUX_ERANGE;
    }

    if (!safe_write_user(bufferAddress, currentProcess->cwdPath, cwdLength))
    {
        return LINUX_EFAULT;
    }

    return (uint64_t) cwdLength;
}

uint64_t VfsRequestManager::HandleChdirRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    char path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg0, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    char resolvedPathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!LogicCaps->virtualFileSystem->JoinPath(currentProcess->cwdPath, path, resolvedPathBuffer, sizeof(resolvedPathBuffer)))
    {
        return LINUX_ENAMETOOLONG;
    }

    vfs_path_t start    = {};
    vfs_path_t resolved = {};

    if (!LogicCaps->virtualFileSystem->ResolvePath(start, resolvedPathBuffer, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }

    if (resolved.dentry->inode->type != INODE_DIRECTORY)
    {
        return LINUX_ENOTDIR;
    }

    if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(resolved.dentry, currentProcess->cwdPath, sizeof(currentProcess->cwdPath)))
    {
        return LINUX_ENAMETOOLONG;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleFchdirRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);

    if (file->inode == nullptr || file->inode->type != INODE_DIRECTORY)
    {
        return LINUX_ENOTDIR;
    }

    if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(file->path.dentry, currentProcess->cwdPath, sizeof(currentProcess->cwdPath)))
    {
        return LINUX_ENAMETOOLONG;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleGetdents64Request(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const uintptr_t userBufferAddress = (uintptr_t) frame->arg1;

    if (is_obviously_invalid_user_pointer(reinterpret_cast<const void*>(userBufferAddress)))
    {
        return LINUX_EFAULT;
    }

    const uint64_t userBufferSize = frame->arg2;

    if (userBufferSize == 0)
    {
        return 0;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);

    if (file->inode == nullptr || file->inode->type != INODE_DIRECTORY)
    {
        return LINUX_ENOTDIR;
    }

    uint64_t bytesWritten = 0;

    while (bytesWritten < userBufferSize)
    {
        directory_entry_t entry      = {};
        const int64_t     readResult = LogicCaps->virtualFileSystem->ReadDirectory(file, &entry);

        if (readResult < 0)
        {
            return (bytesWritten > 0) ? bytesWritten : normalize_vfs_result(readResult);
        }

        if (readResult == 0)
        {
            break;
        }

        if (entry.name == nullptr)
        {
            continue;
        }

        const uint64_t nameLength = (uint64_t) strlen(entry.name);

        if (nameLength > (uint64_t) UINT16_MAX)
        {
            return (bytesWritten > 0) ? bytesWritten : LINUX_EIO;
        }

        const uint16_t baseRecordLength = (uint16_t) (sizeof(linux_dirent64_header_t) + nameLength + 1U);
        const uint16_t recordLength     = align_dirent_record_length(baseRecordLength);

        if ((uint64_t) recordLength > (userBufferSize - bytesWritten))
        {
            break;
        }

        linux_dirent64_header_t header = {};
        header.d_ino                   = entry.inodeNumber;
        header.d_off                   = (int64_t) file->offset;
        header.d_reclen                = recordLength;
        header.d_type                  = linux_dirent_type_for_inode_type(entry.type);

        if (!safe_write_user(userBufferAddress + bytesWritten, &header, sizeof(header)))
        {
            return (bytesWritten > 0) ? bytesWritten : LINUX_EFAULT;
        }

        if (!safe_write_user(userBufferAddress + bytesWritten + sizeof(linux_dirent64_header_t), entry.name, (size_t) nameLength + 1U))
        {
            return (bytesWritten > 0) ? bytesWritten : LINUX_EFAULT;
        }

        if (recordLength > baseRecordLength)
        {
            const uint16_t paddingLength  = (uint16_t) (recordLength - baseRecordLength);
            uint8_t        padding[8]     = {0};
            uint64_t       writtenPadding = 0;

            while (writtenPadding < paddingLength)
            {
                const uint64_t remainingPadding = paddingLength - writtenPadding;
                const uint64_t paddingChunk     = remainingPadding > sizeof(padding) ? sizeof(padding) : remainingPadding;

                if (!safe_write_user(userBufferAddress + bytesWritten + baseRecordLength + writtenPadding, padding, (size_t) paddingChunk))
                {
                    return (bytesWritten > 0) ? bytesWritten : LINUX_EFAULT;
                }

                writtenPadding += paddingChunk;
            }
        }

        bytesWritten += recordLength;
    }

    return bytesWritten;
}

uint64_t VfsRequestManager::HandleUnlinkatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t dirfd                                     = (int64_t) frame->arg0;
    char          path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const int64_t flags                                     = (int64_t) frame->arg2;

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    if ((flags & ~LINUX_AT_REMOVEDIR) != 0)
    {
        return LINUX_EINVAL;
    }

    const bool removeDirectory = (flags & LINUX_AT_REMOVEDIR) != 0;

    char           cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char*    effectivePath                                        = nullptr;
    vfs_path_t     start                                                = {};
    const uint64_t pathResult                                           = prepare_request_path(LogicCaps->virtualFileSystem, currentProcess, dirfd, path, cwdResolvedPath, sizeof(cwdResolvedPath), &effectivePath, &start);

    if (pathResult != 0)
    {
        return pathResult;
    }

    return normalize_vfs_result(LogicCaps->virtualFileSystem->Unlink(start, effectivePath, removeDirectory));
}

uint64_t VfsRequestManager::HandleUnlinkRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t unlinkatFrame = {};
    unlinkatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    unlinkatFrame.arg1                 = frame->arg0;
    unlinkatFrame.arg2                 = 0;
    return HandleUnlinkatRequest(&unlinkatFrame);
}

uint64_t VfsRequestManager::HandleRmdirRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t unlinkatFrame = {};
    unlinkatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    unlinkatFrame.arg1                 = frame->arg0;
    unlinkatFrame.arg2                 = (uint64_t) LINUX_AT_REMOVEDIR;
    return HandleUnlinkatRequest(&unlinkatFrame);
}

uint64_t VfsRequestManager::HandleFcntlRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const uint64_t cmd = frame->arg1;
    const uint64_t arg = frame->arg2;

    if (cmd == LINUX_F_GETFD)
    {
        return (descriptor->flags & FD_FLAG_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    }

    if (cmd == LINUX_F_SETFD)
    {
        if ((arg & ~LINUX_FD_CLOEXEC) != 0)
        {
            return LINUX_EINVAL;
        }

        if ((arg & LINUX_FD_CLOEXEC) != 0)
        {
            descriptor->flags |= FD_FLAG_CLOEXEC;
        }
        else
        {
            descriptor->flags &= ~FD_FLAG_CLOEXEC;
        }

        return 0;
    }

    if (cmd == LINUX_F_DUPFD || cmd == LINUX_F_DUPFD_CLOEXEC)
    {
        const uint64_t minFd = arg;

        if ((int64_t) minFd < 0 || minFd >= currentProcess->fileLimit.current)
        {
            return LINUX_EINVAL;
        }

        uint32_t newFlags = FD_FLAG_NONE;

        if (cmd == LINUX_F_DUPFD_CLOEXEC)
        {
            newFlags = FD_FLAG_CLOEXEC;
        }

        return duplicate_from_fd(ResourceCaps->processManager, LogicCaps->virtualFileSystem, currentProcess, descriptor, newFlags, minFd);
    }

    if (cmd == LINUX_F_GETFL)
    {
        file_t* file = static_cast<file_t*>(descriptor->file);

        if (file == nullptr)
        {
            return LINUX_EBADF;
        }

        return __atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED);
    }

    if (cmd == LINUX_F_SETFL)
    {
        file_t* file = static_cast<file_t*>(descriptor->file);

        if (file == nullptr)
        {
            return LINUX_EBADF;
        }

        if (file->operations != nullptr && file->operations->ReadUser != nullptr)
        {
            // Anonymous stream/event files support changing O_NONBLOCK.
            // Access mode is fixed at creation and cannot be changed by SETFL.

            if ((arg & ~(3ULL | 04000ULL)) != 0)
            {
                return LINUX_EOPNOTSUPP;
            }

            const uint64_t flags = __atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED);
            __atomic_store_n(&file->statusFlags, (flags & ~04000ULL) | (arg & 04000ULL), __ATOMIC_RELAXED);
        }
        else if (file->operations != nullptr && file->operations->linuxErrors)
        {
            if ((arg & ~(3ULL | 04000ULL | 02000ULL)) != 0)
            {
                return LINUX_EOPNOTSUPP;
            }
            const uint64_t previous = __atomic_load_n(&file->statusFlags, __ATOMIC_RELAXED);
            __atomic_store_n(&file->statusFlags, (previous & 3) | (arg & (04000ULL | 02000ULL)), __ATOMIC_RELAXED);
        }
        else
            __atomic_store_n(&file->statusFlags, arg, __ATOMIC_RELAXED);
        return 0;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    return file->operations != nullptr && file->operations->Control != nullptr ? file->operations->Control(file, (uint32_t) cmd, arg) : LINUX_EINVAL;
}

uint64_t VfsRequestManager::HandleDupRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     oldFd      = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, oldFd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    return duplicate_from_fd(ResourceCaps->processManager, LogicCaps->virtualFileSystem, currentProcess, descriptor, FD_FLAG_NONE, 0);
}

uint64_t VfsRequestManager::HandleDup2Request(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t oldFd = frame->arg0;
    const uint64_t newFd = frame->arg1;

    return duplicate_to_fd(ResourceCaps->processManager, LogicCaps->virtualFileSystem, currentProcess, oldFd, newFd, FD_FLAG_NONE);
}

uint64_t VfsRequestManager::HandleDup3Request(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t oldFd = frame->arg0;
    const uint64_t newFd = frame->arg1;
    const uint64_t flags = frame->arg2;

    if ((flags & ~LINUX_O_CLOEXEC) != 0)
    {
        return LINUX_EINVAL;
    }

    if (oldFd == newFd)
    {
        return LINUX_EINVAL;
    }

    uint32_t newFdFlags = FD_FLAG_NONE;

    if ((flags & LINUX_O_CLOEXEC) != 0)
    {
        newFdFlags = FD_FLAG_CLOEXEC;
    }

    return duplicate_to_fd(ResourceCaps->processManager, LogicCaps->virtualFileSystem, currentProcess, oldFd, newFd, newFdFlags);
}

uint64_t VfsRequestManager::HandleNewfstatatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t flags = (int64_t) frame->arg3;

    if ((flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0)
    {
        return LINUX_EINVAL;
    }

    const int64_t dirfd                                     = (int64_t) frame->arg0;
    char          path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    if (path[0] == '\0')
    {
        if ((flags & LINUX_AT_EMPTY_PATH) == 0)
        {
            return LINUX_ENOENT;
        }

        if (dirfd < 0)
        {
            return LINUX_EBADF;
        }

        const uint64_t     fd         = (uint64_t) dirfd;
        file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

        if (descriptor == nullptr)
        {
            return LINUX_EBADF;
        }

        file_t* file = static_cast<file_t*>(descriptor->file);

        if (file->inode == nullptr)
        {
            return LINUX_EIO;
        }

        linux_stat_t statData = {};
        fill_linux_stat_from_inode(file->inode, &statData);

        if (!safe_write_user((uintptr_t) frame->arg2, &statData, sizeof(statData)))
        {
            return LINUX_EFAULT;
        }

        return 0;
    }

    vfs_path_t  start                                                = {};
    bool        nonDirectoryBase                                     = false;
    char        cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath                                        = path;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, dirfd, path, cwdResolvedPath, sizeof(cwdResolvedPath), &effectivePath))
    {
        return LINUX_ENAMETOOLONG;
    }

    if ((flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0)
    {
        inode_t*      inode      = nullptr;
        const int64_t statResult = LogicCaps->virtualFileSystem->StatNoFollow(start, effectivePath, &inode);

        if (statResult < 0 || inode == nullptr)
        {
            return normalize_vfs_result(statResult);
        }

        linux_stat_t statData = {};
        fill_linux_stat_from_inode(inode, &statData);

        if (!safe_write_user((uintptr_t) frame->arg2, &statData, sizeof(statData)))
        {
            return LINUX_EFAULT;
        }

        return 0;
    }

    if (effectivePath[0] == '/')
    {
        start = {};
    }
    else
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, dirfd, &start, &nonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (nonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    vfs_path_t resolved = {};

    if (!LogicCaps->virtualFileSystem->ResolvePath(start, effectivePath, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }

    linux_stat_t statData = {};
    fill_linux_stat_from_inode(resolved.dentry->inode, &statData);

    if (!safe_write_user((uintptr_t) frame->arg2, &statData, sizeof(statData)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleStatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t statatFrame = {};
    statatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    statatFrame.arg1                 = frame->arg0;
    statatFrame.arg2                 = frame->arg1;
    statatFrame.arg3                 = 0;
    return HandleNewfstatatRequest(&statatFrame);
}

uint64_t VfsRequestManager::HandleFstatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);

    if (file->inode == nullptr)
    {
        return LINUX_EIO;
    }

    linux_stat_t statData = {};
    fill_linux_stat_from_inode(file->inode, &statData);

    if (!safe_write_user((uintptr_t) frame->arg1, &statData, sizeof(statData)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleLstatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t statatFrame = {};
    statatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    statatFrame.arg1                 = frame->arg0;
    statatFrame.arg2                 = frame->arg1;
    statatFrame.arg3                 = (uint64_t) LINUX_AT_SYMLINK_NOFOLLOW;
    return HandleNewfstatatRequest(&statatFrame);
}

uint64_t VfsRequestManager::HandleRenameatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t olddirfd                                     = (int64_t) frame->arg0;
    char          oldpath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const int64_t newdirfd                                     = (int64_t) frame->arg2;
    char          newpath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, oldpath, sizeof(oldpath)) || !copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg3, newpath, sizeof(newpath)))
    {
        return LINUX_EFAULT;
    }

    char        oldCwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectiveOldPath                                        = oldpath;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, olddirfd, oldpath, oldCwdResolvedPath, sizeof(oldCwdResolvedPath), &effectiveOldPath))
    {
        return LINUX_ENAMETOOLONG;
    }

    char        newCwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectiveNewPath                                        = newpath;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, newdirfd, newpath, newCwdResolvedPath, sizeof(newCwdResolvedPath), &effectiveNewPath))
    {
        return LINUX_ENAMETOOLONG;
    }

    vfs_path_t oldStart            = {};
    bool       oldNonDirectoryBase = false;

    if (effectiveOldPath[0] != '/')
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, olddirfd, &oldStart, &oldNonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (oldNonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    vfs_path_t newStart            = {};
    bool       newNonDirectoryBase = false;

    if (effectiveNewPath[0] != '/')
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, newdirfd, &newStart, &newNonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (newNonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    return normalize_vfs_result(LogicCaps->virtualFileSystem->Rename(oldStart, effectiveOldPath, newStart, effectiveNewPath));
}

uint64_t VfsRequestManager::HandleRenameRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t renameatFrame = {};
    renameatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    renameatFrame.arg1                 = frame->arg0;
    renameatFrame.arg2                 = (uint64_t) LINUX_AT_FDCWD;
    renameatFrame.arg3                 = frame->arg1;
    return HandleRenameatRequest(&renameatFrame);
}

uint64_t VfsRequestManager::HandleReadlinkatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t   dirfd                                     = (int64_t) frame->arg0;
    char            path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const uintptr_t bufferAddress                             = (uintptr_t) frame->arg2;
    const uint64_t  bufferSize                                = frame->arg3;

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)) || is_obviously_invalid_user_pointer(reinterpret_cast<const void*>(bufferAddress)))
    {
        return LINUX_EFAULT;
    }

    if (bufferSize == 0)
    {
        return LINUX_EINVAL;
    }

    char        effectivePathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath                                            = path;

    if (path[0] != '/')
    {
        if (dirfd == LINUX_AT_FDCWD)
        {
            if (!LogicCaps->virtualFileSystem->JoinPath(currentProcess->cwdPath, path, effectivePathBuffer, sizeof(effectivePathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectivePath = effectivePathBuffer;
        }
        else
        {
            if (dirfd < 0)
            {
                return LINUX_EBADF;
            }

            const uint64_t     fd         = (uint64_t) dirfd;
            file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

            if (descriptor == nullptr)
            {
                return LINUX_EBADF;
            }

            file_t* baseFile = static_cast<file_t*>(descriptor->file);

            if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
            {
                return LINUX_ENOTDIR;
            }

            char basePath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

            if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(baseFile->path.dentry, basePath, sizeof(basePath)))
            {
                return LINUX_ENAMETOOLONG;
            }

            if (!LogicCaps->virtualFileSystem->JoinPath(basePath, path, effectivePathBuffer, sizeof(effectivePathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectivePath = effectivePathBuffer;
        }
    }

    char     kernelBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    uint64_t readlinkBufferSize                                = bufferSize;

    if (readlinkBufferSize > sizeof(kernelBuffer))
    {
        readlinkBufferSize = sizeof(kernelBuffer);
    }

    const int64_t result = LogicCaps->virtualFileSystem->Readlink({}, effectivePath, kernelBuffer, readlinkBufferSize);

    if (result < 0)
    {
        return normalize_vfs_result(result);
    }

    if (!safe_write_user(bufferAddress, kernelBuffer, (size_t) result))
    {
        return LINUX_EFAULT;
    }

    return (uint64_t) result;
}

uint64_t VfsRequestManager::HandleReadlinkRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t readlinkatFrame = {};
    readlinkatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    readlinkatFrame.arg1                 = frame->arg0;
    readlinkatFrame.arg2                 = frame->arg1;
    readlinkatFrame.arg3                 = frame->arg2;
    return HandleReadlinkatRequest(&readlinkatFrame);
}

uint64_t VfsRequestManager::HandleIoctlRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t*       file   = static_cast<file_t*>(descriptor->file);
    const int64_t result = LogicCaps->virtualFileSystem->Ioctl(file, frame->arg1, frame->arg2);

    if (result == -1)
    {
        return LINUX_ENOTTY;
    }

    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandleLinkatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t olddirfd                                     = (int64_t) frame->arg0;
    char          oldpath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const int64_t newdirfd                                     = (int64_t) frame->arg2;
    char          newpath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const int64_t flags                                        = (int64_t) frame->arg4;

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, oldpath, sizeof(oldpath)) || !copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg3, newpath, sizeof(newpath)))
    {
        return LINUX_EFAULT;
    }

    if ((flags & ~LINUX_AT_SYMLINK_FOLLOW) != 0)
    {
        return LINUX_EINVAL;
    }

    char        oldCwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectiveOldPath                                        = oldpath;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, olddirfd, oldpath, oldCwdResolvedPath, sizeof(oldCwdResolvedPath), &effectiveOldPath))
    {
        return LINUX_ENAMETOOLONG;
    }

    char        newCwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectiveNewPath                                        = newpath;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, newdirfd, newpath, newCwdResolvedPath, sizeof(newCwdResolvedPath), &effectiveNewPath))
    {
        return LINUX_ENAMETOOLONG;
    }

    vfs_path_t oldStart            = {};
    bool       oldNonDirectoryBase = false;

    if (effectiveOldPath[0] != '/')
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, olddirfd, &oldStart, &oldNonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (oldNonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    vfs_path_t newStart            = {};
    bool       newNonDirectoryBase = false;

    if (effectiveNewPath[0] != '/')
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, newdirfd, &newStart, &newNonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (newNonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    const bool followSymlink = (flags & LINUX_AT_SYMLINK_FOLLOW) != 0;
    return normalize_vfs_result(LogicCaps->virtualFileSystem->Link(oldStart, effectiveOldPath, newStart, effectiveNewPath, followSymlink));
}

uint64_t VfsRequestManager::HandleLinkRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t linkatFrame = {};
    linkatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    linkatFrame.arg1                 = frame->arg0;
    linkatFrame.arg2                 = (uint64_t) LINUX_AT_FDCWD;
    linkatFrame.arg3                 = frame->arg1;
    linkatFrame.arg4                 = 0;
    return HandleLinkatRequest(&linkatFrame);
}

uint64_t VfsRequestManager::HandleSymlinkatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    char          target[ProcessManager::MAX_CWD_PATH_LENGTH]   = {};
    const int64_t dirfd                                         = (int64_t) frame->arg1;
    char          linkPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg0, target, sizeof(target)) || !copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg2, linkPath, sizeof(linkPath)))
    {
        return LINUX_EFAULT;
    }

    char        effectiveLinkPathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectiveLinkPath                                            = linkPath;

    if (linkPath[0] != '/')
    {
        if (dirfd == LINUX_AT_FDCWD)
        {
            if (!LogicCaps->virtualFileSystem->JoinPath(currentProcess->cwdPath, linkPath, effectiveLinkPathBuffer, sizeof(effectiveLinkPathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectiveLinkPath = effectiveLinkPathBuffer;
        }
        else
        {
            if (dirfd < 0)
            {
                return LINUX_EBADF;
            }

            const uint64_t     fd         = (uint64_t) dirfd;
            file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

            if (descriptor == nullptr)
            {
                return LINUX_EBADF;
            }

            file_t* baseFile = static_cast<file_t*>(descriptor->file);

            if (baseFile->inode == nullptr || baseFile->inode->type != INODE_DIRECTORY)
            {
                return LINUX_ENOTDIR;
            }

            char basePath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

            if (!LogicCaps->virtualFileSystem->BuildAbsolutePathFromDentry(baseFile->path.dentry, basePath, sizeof(basePath)))
            {
                return LINUX_ENAMETOOLONG;
            }

            if (!LogicCaps->virtualFileSystem->JoinPath(basePath, linkPath, effectiveLinkPathBuffer, sizeof(effectiveLinkPathBuffer)))
            {
                return LINUX_ENAMETOOLONG;
            }

            effectiveLinkPath = effectiveLinkPathBuffer;
        }
    }

    const int64_t result = LogicCaps->virtualFileSystem->Symlink({}, target, effectiveLinkPath);
    return normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandleSymlinkRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t symlinkatFrame = {};
    symlinkatFrame.arg0                 = frame->arg0;
    symlinkatFrame.arg1                 = (uint64_t) LINUX_AT_FDCWD;
    symlinkatFrame.arg2                 = frame->arg1;
    return HandleSymlinkatRequest(&symlinkatFrame);
}

uint64_t VfsRequestManager::HandleFaccessatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t faccessat2Frame = {};
    faccessat2Frame.arg0                 = frame->arg0;
    faccessat2Frame.arg1                 = frame->arg1;
    faccessat2Frame.arg2                 = frame->arg2;
    faccessat2Frame.arg3                 = 0;
    return HandleFaccessat2Request(&faccessat2Frame);
}

uint64_t VfsRequestManager::HandleFaccessat2Request(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t dirfd                                     = (int64_t) frame->arg0;
    char          path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const int64_t mode                                      = (int64_t) frame->arg2;
    const int64_t flags                                     = (int64_t) frame->arg3;

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    if ((mode & ~(LINUX_R_OK | LINUX_W_OK | LINUX_X_OK)) != 0)
    {
        return LINUX_EINVAL;
    }

    if ((flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH | LINUX_AT_EACCESS)) != 0)
    {
        return LINUX_EINVAL;
    }

    if (path[0] == '\0')
    {
        if ((flags & LINUX_AT_EMPTY_PATH) == 0)
        {
            return LINUX_ENOENT;
        }

        if (dirfd < 0)
        {
            return LINUX_EBADF;
        }

        const uint64_t     fd         = (uint64_t) dirfd;
        file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

        if (descriptor == nullptr)
        {
            return LINUX_EBADF;
        }

        file_t* file = static_cast<file_t*>(descriptor->file);

        if (file->inode == nullptr)
        {
            return LINUX_EIO;
        }

        return inode_allows_access_mode(file->inode, mode) ? 0 : LINUX_EACCES;
    }

    vfs_path_t  start                                                = {};
    bool        nonDirectoryBase                                     = false;
    char        cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath                                        = path;

    if (!expand_cwd_path(LogicCaps->virtualFileSystem, currentProcess, dirfd, path, cwdResolvedPath, sizeof(cwdResolvedPath), &effectivePath))
    {
        return LINUX_ENAMETOOLONG;
    }

    if ((flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0)
    {
        inode_t*      inode      = nullptr;
        const int64_t statResult = LogicCaps->virtualFileSystem->StatNoFollow(start, effectivePath, &inode);

        if (statResult < 0 || inode == nullptr)
        {
            return normalize_vfs_result(statResult);
        }

        return inode_allows_access_mode(inode, mode) ? 0 : LINUX_EACCES;
    }

    if (effectivePath[0] == '/')
    {
        start = {};
    }
    else
    {
        if (!resolve_relative_base_for_dirfd(currentProcess, dirfd, &start, &nonDirectoryBase))
        {
            return LINUX_EBADF;
        }

        if (nonDirectoryBase)
        {
            return LINUX_ENOTDIR;
        }
    }

    vfs_path_t resolved = {};

    if (!LogicCaps->virtualFileSystem->ResolvePath(start, effectivePath, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }

    return inode_allows_access_mode(resolved.dentry->inode, mode) ? 0 : LINUX_EACCES;
}

uint64_t VfsRequestManager::HandleAccessRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t faccessat2Frame = {};
    faccessat2Frame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    faccessat2Frame.arg1                 = frame->arg0;
    faccessat2Frame.arg2                 = frame->arg1;
    faccessat2Frame.arg3                 = 0;
    return HandleFaccessat2Request(&faccessat2Frame);
}

uint64_t VfsRequestManager::MetadataRequest(const arch_syscall_frame_t* frame, bool ownership, bool descriptorOnly)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }
    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }
    process_t* process = ResourceCaps->processManager->GetCurrentProcess();
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }
    FileDescriptorGuard descriptors(process);
    const int32_t       dirfd = (int32_t) frame->arg0;
    const uint32_t      flags = descriptorOnly ? 0x1000U : (uint32_t) (ownership ? frame->arg4 : frame->arg3);
    if ((flags & ~(0x1000U | 0x100U)) != 0)
    {
        return LINUX_EINVAL;
    }
    char path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    if (!descriptorOnly && !(frame->arg1 == 0 && (flags & 0x1000)) && !safe_read_user_string(frame->arg1, path, sizeof(path), nullptr))
    {
        return LINUX_EFAULT;
    }
    inode_t* inode = nullptr;
    if (descriptorOnly || (path[0] == 0 && (flags & 0x1000)))
    {
        if (dirfd == LINUX_AT_FDCWD && !descriptorOnly)
        {
            vfs_path_t resolved = {};
            if (!LogicCaps->virtualFileSystem->ResolvePath({}, process->cwdPath, &resolved))
            {
                return LINUX_ENOENT;
            }
            inode = resolved.dentry->inode;
        }
        else
        {
            file_descriptor_t* fd = dirfd >= 0 ? ProcessManager::GetFileDescriptor(process, (uint32_t) dirfd) : nullptr;
            if (fd == nullptr)
            {
                return LINUX_EBADF;
            }
            inode = static_cast<file_t*>(fd->file)->inode;
        }
    }
    else
    {
        if (path[0] == 0)
        {
            return LINUX_ENOENT;
        }
        vfs_path_t  start                                        = {};
        char        scratch[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
        const char* effectivePath                                = nullptr;
        uint64_t    result                                       = prepare_request_path(LogicCaps->virtualFileSystem, process, dirfd, path, scratch, sizeof(scratch), &effectivePath, &start);
        if (result != 0)
        {
            return result;
        }
        if (flags & 0x100)
        {
            const int64_t status = LogicCaps->virtualFileSystem->StatNoFollow(start, effectivePath, &inode);
            if (status < 0)
            {
                return normalize_vfs_result(status);
            }
        }
        else
        {
            vfs_path_t resolved = {};
            if (!LogicCaps->virtualFileSystem->ResolvePath(start, effectivePath, &resolved) || resolved.dentry == nullptr)
            {
                return LINUX_ENOENT;
            }
            inode = resolved.dentry->inode;
        }
    }
    const uint32_t first  = (uint32_t) (descriptorOnly ? frame->arg1 : frame->arg2);
    const uint32_t second = (uint32_t) (descriptorOnly ? frame->arg2 : frame->arg3);
    return LogicCaps->virtualFileSystem->ChangeMetadata(inode, ownership, first, second);
}

uint64_t VfsRequestManager::HandleFchownatRequest(const arch_syscall_frame_t* frame)
{
    return MetadataRequest(frame, true);
}

uint64_t VfsRequestManager::HandleChownRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t fchownatFrame = {};
    fchownatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    fchownatFrame.arg1                 = frame->arg0;
    fchownatFrame.arg2                 = frame->arg1;
    fchownatFrame.arg3                 = frame->arg2;
    fchownatFrame.arg4                 = 0;
    return HandleFchownatRequest(&fchownatFrame);
}

uint64_t VfsRequestManager::HandleFchownRequest(const arch_syscall_frame_t* frame)
{
    return MetadataRequest(frame, true, true);
}

uint64_t VfsRequestManager::HandleLchownRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t fchownatFrame = {};
    fchownatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    fchownatFrame.arg1                 = frame->arg0;
    fchownatFrame.arg2                 = frame->arg1;
    fchownatFrame.arg3                 = frame->arg2;
    fchownatFrame.arg4                 = (uint64_t) LINUX_AT_SYMLINK_NOFOLLOW;
    return HandleFchownatRequest(&fchownatFrame);
}

uint64_t VfsRequestManager::HandleFchmodat2Request(const arch_syscall_frame_t* frame)
{
    return MetadataRequest(frame, false);
}

uint64_t VfsRequestManager::HandleFchmodatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t fchmodat2Frame = {};
    fchmodat2Frame.arg0                 = frame->arg0;
    fchmodat2Frame.arg1                 = frame->arg1;
    fchmodat2Frame.arg2                 = frame->arg2;
    fchmodat2Frame.arg3                 = 0;
    return HandleFchmodat2Request(&fchmodat2Frame);
}

uint64_t VfsRequestManager::HandleChmodRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t fchmodat2Frame = {};
    fchmodat2Frame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    fchmodat2Frame.arg1                 = frame->arg0;
    fchmodat2Frame.arg2                 = frame->arg1;
    fchmodat2Frame.arg3                 = 0;
    return HandleFchmodat2Request(&fchmodat2Frame);
}

uint64_t VfsRequestManager::HandleFchmodRequest(const arch_syscall_frame_t* frame)
{
    return MetadataRequest(frame, false, true);
}

uint64_t VfsRequestManager::HandleMountRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }
    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }
    process_t* process = ResourceCaps->processManager->AcquireCurrentCredentials();
    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }
    const bool privileged = process->credentials.user.effective == 0;
    ResourceCaps->processManager->ReleaseCredentials(process);
    if (!privileged)
    {
        return (uint64_t) -1;
    }
    char source[512] = {}, target[512] = {}, type[64] = {};
    if (!safe_read_user_string(frame->arg1, target, sizeof(target), nullptr) || (frame->arg0 != 0 && !safe_read_user_string(frame->arg0, source, sizeof(source), nullptr)) || (frame->arg2 != 0 && !safe_read_user_string(frame->arg2, type, sizeof(type), nullptr)))
    {
        return LINUX_EFAULT;
    }
    char absolute[512] = {};
    if (!LogicCaps->virtualFileSystem->JoinPath(process->cwdPath, target, absolute, sizeof(absolute)))
    {
        return LINUX_ENAMETOOLONG;
    }
    if (target[0] == 0)
    {
        return LINUX_ENOENT;
    }
    return LogicCaps->virtualFileSystem->Mount(source, absolute, frame->arg2 != 0 ? type : nullptr, frame->arg3, frame->arg4);
}

uint64_t VfsRequestManager::HandleStatfsRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    char path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg0, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    char        effectivePathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath                                            = path;

    if (path[0] != '/')
    {
        if (!LogicCaps->virtualFileSystem->JoinPath(currentProcess->cwdPath, path, effectivePathBuffer, sizeof(effectivePathBuffer)))
        {
            return LINUX_ENAMETOOLONG;
        }

        effectivePath = effectivePathBuffer;
    }

    vfs_path_t resolved = {};

    if (!LogicCaps->virtualFileSystem->ResolvePath({}, effectivePath, &resolved) || resolved.dentry == nullptr || resolved.dentry->inode == nullptr)
    {
        return LINUX_ENOENT;
    }

    linux_statfs_t statfsData = {};
    fill_linux_statfs_from_inode(resolved.dentry->inode, &statfsData);

    if (!safe_write_user((uintptr_t) frame->arg1, &statfsData, sizeof(statfsData)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleFstatfsRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);

    if (file->inode == nullptr)
    {
        return LINUX_EIO;
    }

    linux_statfs_t statfsData = {};
    fill_linux_statfs_from_inode(file->inode, &statfsData);

    if (!safe_write_user((uintptr_t) frame->arg1, &statfsData, sizeof(statfsData)))
    {
        return LINUX_EFAULT;
    }

    return 0;
}

uint64_t VfsRequestManager::HandleMknodatRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const int64_t  dirfd                                     = (int64_t) frame->arg0;
    char           path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const uint32_t mode                                      = (uint32_t) frame->arg2;
    const uint64_t dev                                       = frame->arg3;

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg1, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    char           cwdResolvedPath[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char*    effectivePath                                        = nullptr;
    vfs_path_t     start                                                = {};
    const uint64_t pathResult                                           = prepare_request_path(LogicCaps->virtualFileSystem, currentProcess, dirfd, path, cwdResolvedPath, sizeof(cwdResolvedPath), &effectivePath, &start);

    if (pathResult != 0)
    {
        return pathResult;
    }

    return normalize_vfs_result(LogicCaps->virtualFileSystem->Mknod(start, effectivePath, mode, dev));
}

uint64_t VfsRequestManager::HandleMknodRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t mknodatFrame = {};
    mknodatFrame.arg0                 = (uint64_t) LINUX_AT_FDCWD;
    mknodatFrame.arg1                 = frame->arg0;
    mknodatFrame.arg2                 = frame->arg1;
    mknodatFrame.arg3                 = frame->arg2;
    return HandleMknodatRequest(&mknodatFrame);
}

uint64_t VfsRequestManager::HandleTruncateRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || ResourceCaps->virtualMemoryManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    char path[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    if (!copy_user_path(ResourceCaps->virtualMemoryManager, currentProcess, (uintptr_t) frame->arg0, path, sizeof(path)))
    {
        return LINUX_EFAULT;
    }

    const int64_t length = (int64_t) frame->arg1;

    if (length < 0)
    {
        return LINUX_EINVAL;
    }

    char        effectivePathBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};
    const char* effectivePath                                            = path;

    if (path[0] != '/')
    {
        if (!LogicCaps->virtualFileSystem->JoinPath(currentProcess->cwdPath, path, effectivePathBuffer, sizeof(effectivePathBuffer)))
        {
            return LINUX_ENAMETOOLONG;
        }

        effectivePath = effectivePathBuffer;
    }

    return normalize_vfs_result(LogicCaps->virtualFileSystem->TruncatePath({}, effectivePath, (uint64_t) length));
}

uint64_t VfsRequestManager::HandleFtruncateRequest(const arch_syscall_frame_t* frame)
{
    FileDescriptorGuard descriptors(ResourceCaps != nullptr && ResourceCaps->processManager != nullptr ? ResourceCaps->processManager->GetCurrentProcess() : nullptr);

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (ResourceCaps == nullptr || ResourceCaps->processManager == nullptr || LogicCaps == nullptr || LogicCaps->virtualFileSystem == nullptr)
    {
        return LINUX_ENOSYS;
    }

    process_t* currentProcess = ResourceCaps->processManager->GetCurrentProcess();

    if (currentProcess == nullptr)
    {
        return LINUX_ESRCH;
    }

    const uint64_t     fd         = frame->arg0;
    file_descriptor_t* descriptor = ProcessManager::GetFileDescriptor(currentProcess, fd);

    if (descriptor == nullptr)
    {
        return LINUX_EBADF;
    }

    const int64_t length = (int64_t) frame->arg1;

    if (length < 0)
    {
        return LINUX_EINVAL;
    }

    file_t* file = static_cast<file_t*>(descriptor->file);
    const int64_t result = LogicCaps->virtualFileSystem->TruncateFile(file, (uint64_t) length);
    return file->operations != nullptr && file->operations->linuxErrors ? (uint64_t) result : normalize_vfs_result(result);
}

uint64_t VfsRequestManager::HandlePipeRequest(const arch_syscall_frame_t* frame)
{

    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    arch_syscall_frame_t pipe2Frame = {};
    pipe2Frame.arg0                 = frame->arg0;
    pipe2Frame.arg1                 = 0;
    return HandlePipe2Request(&pipe2Frame);
}

uint64_t VfsRequestManager::HandlePipe2Request(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }
    if (LogicCaps == nullptr || LogicCaps->interProcessCommunicationManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    return LogicCaps->interProcessCommunicationManager->CreatePipe(frame->arg0, (uint32_t) frame->arg1);
}

uint64_t VfsRequestManager::HandleMemfd_createRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }
    if (LogicCaps == nullptr || LogicCaps->interProcessCommunicationManager == nullptr)
    {
        return LINUX_ENOSYS;
    }
    return LogicCaps->interProcessCommunicationManager->CreateMemoryFile(frame->arg0, (uint32_t) frame->arg1);
}
