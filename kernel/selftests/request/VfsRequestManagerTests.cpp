#include "RequestTestSupport.hpp"

namespace request_selftest
{
// Keep the shared initramfs file unchanged even if a write assertion fails.
// The independent VFS handle also supplies expected bytes for syscall reads.
class FileContentGuard
{
public:
    explicit FileContentGuard(bool restore) : restore(restore)
    {
        vfs  = static_cast<Dispatcher*>(platform.dispacher)->GetLogicLayerCaps()->virtualFileSystem;
        file = vfs->Open({}, "/test.txt", 0);

        if (file != nullptr)
        {
            count = vfs->Read(file, bytes, sizeof(bytes));
        }

        valid = expect(file != nullptr && count > 0 && static_cast<uint64_t>(count) == file->inode->size, "snapshot complete initramfs file before syscall contract");
    }

    ~FileContentGuard()
    {
        if (file == nullptr)
        {
            return;
        }

        if (valid && restore)
        {
            require_cleanup(vfs->Seek(file, 0, 0) == 0 && vfs->Write(file, bytes, count) == count, "restore initramfs bytes after write contract");

            char actual[128] = {};

            require_cleanup(vfs->Seek(file, 0, 0) == 0 && vfs->Read(file, actual, count) == count && memcmp(actual, bytes, count) == 0, "verify restored initramfs contents");
        }

        require_cleanup(vfs->Close(file) == 0, "close snapshot VFS handle");
    }

    FileContentGuard(const FileContentGuard&) = delete;

    FileContentGuard& operator=(const FileContentGuard&) = delete;

    bool Matches(const char* expected, size_t length)
    {
        char actual[128] = {};

        return expect(length <= sizeof(actual) && vfs->Seek(file, 0, 0) == 0 && vfs->Read(file, actual, length) == static_cast<int64_t>(length) &&
                          memcmp(actual, expected, length) == 0,
            "written bytes are visible through an independent VFS handle");
    }

    bool    valid      = false;
    int64_t count      = 0;
    char    bytes[128] = {};

private:
    VirtualFileSystem* vfs  = nullptr;
    file_t*            file = nullptr;
    bool               restore;
};

static bool validate_vfs_rw(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(ctx->syscallNumber == SYSCALL_write);

    if (!snapshot.valid)
    {
        return false;
    }

    const uint64_t fd = syscall_call(SYSCALL_open, request_user_address("/test.txt", sizeof("/test.txt")));

    if (!expect(static_cast<int64_t>(fd) >= 0, "open read/write fixture"))
    {
        return false;
    }

    char           buffer[32] = {};
    const uint64_t user       = request_user_address(buffer, sizeof(buffer));
    const size_t   count      = snapshot.count < sizeof(buffer) ? snapshot.count : sizeof(buffer);
    bool           ok         = true;

    if (ctx->syscallNumber == SYSCALL_read)
    {
        ok &= equal(syscall_call(SYSCALL_read, fd, user, count), count, "read returns requested fixture bytes");

        ok &= expect(memcmp(buffer, snapshot.bytes, count) == 0, "read copies exact file contents");
    }
    else
    {
        memset(buffer, 'W', count);

        ok &= equal(syscall_call(SYSCALL_write, fd, user, count), count, "write returns requested fixture bytes");

        char actual[32] = {};

        ok &= equal(syscall_call(SYSCALL_pread64, fd, request_user_address(actual, sizeof(actual)), count), count, "read back written bytes");

        ok &= expect(memcmp(actual, buffer, count) == 0, "write changes file contents");
    }

    ok &= equal(syscall_call(SYSCALL_lseek, fd, 0, 1), count, "read/write advances file position");

    ok &= equal(syscall_call(ctx->syscallNumber, UINT64_MAX, user, count), LINUX_EBADF, "read/write invalid descriptor");

    ok &= equal(syscall_call(ctx->syscallNumber, fd, CANONICAL_KERNEL_BASE, count), LINUX_EFAULT, "read/write rejects kernel buffer");

    require_cleanup(syscall_call(SYSCALL_close, fd) == 0, "close read/write fixture descriptor");

    return ok;
}

struct test_linux_iovec_t
{
    void*    iov_base;
    uint64_t iov_len;
};

static bool validate_pread64(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(false);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    char                 readBuffer[32] = {};
    arch_syscall_frame_t preadFrame     = {};

    preadFrame.syscall_number = SYSCALL_pread64;
    preadFrame.arg0           = fd;
    preadFrame.arg1           = request_user_address(readBuffer, sizeof(readBuffer));
    preadFrame.arg2           = sizeof(readBuffer);
    preadFrame.arg3           = 0;

    const uint64_t preadResult = dispatcher_dispatch_syscall(&preadFrame);

    arch_syscall_frame_t badFdFrame = preadFrame;

    badFdFrame.arg0 = fd + 1;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t badOffsetFrame = preadFrame;

    badOffsetFrame.arg3 = (uint64_t) -1LL;

    const uint64_t badOffsetResult = dispatcher_dispatch_syscall(&badOffsetFrame);

    const bool positionValid = equal(syscall_call(SYSCALL_lseek, fd, 0, 1), 0, "positional/vector I/O file position");

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(positionValid && preadResult == sizeof(readBuffer) && memcmp(readBuffer, snapshot.bytes, sizeof(readBuffer)) == 0 && badFdResult == LINUX_EBADF &&
                          badOffsetResult == LINUX_EINVAL && closeResult == 0);
}

static bool validate_pwrite64(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(true);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    static const char    payload[]   = "Arx pread/pwrite selftest";
    arch_syscall_frame_t pwriteFrame = {};

    pwriteFrame.syscall_number = SYSCALL_pwrite64;
    pwriteFrame.arg0           = fd;
    pwriteFrame.arg1           = request_user_address(payload, sizeof(payload));
    pwriteFrame.arg2           = (uint64_t) (sizeof(payload) - 1);
    pwriteFrame.arg3           = 0;

    const uint64_t pwriteResult = dispatcher_dispatch_syscall(&pwriteFrame);

    arch_syscall_frame_t badFdFrame = pwriteFrame;

    badFdFrame.arg0 = fd + 1;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t badOffsetFrame = pwriteFrame;

    badOffsetFrame.arg3 = (uint64_t) -1LL;

    const uint64_t badOffsetResult = dispatcher_dispatch_syscall(&badOffsetFrame);

    const bool positionValid = equal(syscall_call(SYSCALL_lseek, fd, 0, 1), 0, "positional/vector I/O file position");

    const bool contentsValid = snapshot.Matches(payload, sizeof(payload) - 1);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(positionValid && contentsValid && pwriteResult == (uint64_t) (sizeof(payload) - 1) && badFdResult == LINUX_EBADF && badOffsetResult == LINUX_EINVAL &&
                          closeResult == 0);
}

static bool validate_readv(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(false);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    char               readA[8] = {};
    char               readB[8] = {};
    test_linux_iovec_t iov[2]   = {
        {(void*) (uintptr_t) request_user_address(readA, sizeof(readA)), sizeof(readA)},
        {(void*) (uintptr_t) request_user_address(readB, sizeof(readB)), sizeof(readB)},
    };

    arch_syscall_frame_t readvFrame = {};

    readvFrame.syscall_number = SYSCALL_readv;
    readvFrame.arg0           = fd;
    readvFrame.arg1           = request_user_address(iov, sizeof(iov));
    readvFrame.arg2           = 2;

    const uint64_t readvResult = dispatcher_dispatch_syscall(&readvFrame);

    arch_syscall_frame_t badIovcntFrame = readvFrame;

    badIovcntFrame.arg2 = 1025;

    const uint64_t badIovcntResult = dispatcher_dispatch_syscall(&badIovcntFrame);

    const bool positionValid = equal(syscall_call(SYSCALL_lseek, fd, 0, 1), 16, "positional/vector I/O file position");

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(positionValid && readvResult == sizeof(readA) + sizeof(readB) && memcmp(readA, snapshot.bytes, sizeof(readA)) == 0 &&
                          memcmp(readB, snapshot.bytes + sizeof(readA), sizeof(readB)) == 0 && badIovcntResult == LINUX_EINVAL && closeResult == 0);
}

static bool validate_writev(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(true);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    static char        partA[] = "Arx-";
    static char        partB[] = "writev";
    test_linux_iovec_t iov[2]  = {
        {(void*) (uintptr_t) request_user_address(partA, sizeof(partA)), (uint64_t) (sizeof(partA) - 1)},
        {(void*) (uintptr_t) request_user_address(partB, sizeof(partB)), (uint64_t) (sizeof(partB) - 1)},
    };

    arch_syscall_frame_t writevFrame = {};

    writevFrame.syscall_number = SYSCALL_writev;
    writevFrame.arg0           = fd;
    writevFrame.arg1           = request_user_address(iov, sizeof(iov));
    writevFrame.arg2           = 2;

    const uint64_t writevResult = dispatcher_dispatch_syscall(&writevFrame);

    arch_syscall_frame_t badFdFrame = writevFrame;

    badFdFrame.arg0 = fd + 1;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    const bool positionValid = equal(syscall_call(SYSCALL_lseek, fd, 0, 1), 10, "positional/vector I/O file position");

    const bool contentsValid = snapshot.Matches("Arx-writev", 10);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    const uint64_t expected = (uint64_t) ((sizeof(partA) - 1) + (sizeof(partB) - 1));

    return REQUEST_EXPECT(positionValid && contentsValid && writevResult == expected && badFdResult == LINUX_EBADF && closeResult == 0);
}

static bool validate_preadv(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(false);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    char               readA[8] = {};
    char               readB[8] = {};
    test_linux_iovec_t iov[2]   = {
        {(void*) (uintptr_t) request_user_address(readA, sizeof(readA)), sizeof(readA)},
        {(void*) (uintptr_t) request_user_address(readB, sizeof(readB)), sizeof(readB)},
    };

    arch_syscall_frame_t preadvFrame = {};

    preadvFrame.syscall_number = SYSCALL_preadv;
    preadvFrame.arg0           = fd;
    preadvFrame.arg1           = request_user_address(iov, sizeof(iov));
    preadvFrame.arg2           = 2;
    preadvFrame.arg3           = 0;

    const uint64_t preadvResult = dispatcher_dispatch_syscall(&preadvFrame);

    arch_syscall_frame_t badOffsetFrame = preadvFrame;

    badOffsetFrame.arg3 = (uint64_t) -1LL;

    const uint64_t badOffsetResult = dispatcher_dispatch_syscall(&badOffsetFrame);

    const bool positionValid = equal(syscall_call(SYSCALL_lseek, fd, 0, 1), 0, "positional/vector I/O file position");

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(positionValid && preadvResult == sizeof(readA) + sizeof(readB) && memcmp(readA, snapshot.bytes, sizeof(readA)) == 0 &&
                          memcmp(readB, snapshot.bytes + sizeof(readA), sizeof(readB)) == 0 && badOffsetResult == LINUX_EINVAL && closeResult == 0);
}

static bool validate_pwritev(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(true);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    static char        partA[] = "Arx-";
    static char        partB[] = "pwritev";
    test_linux_iovec_t iov[2]  = {
        {(void*) (uintptr_t) request_user_address(partA, sizeof(partA)), (uint64_t) (sizeof(partA) - 1)},
        {(void*) (uintptr_t) request_user_address(partB, sizeof(partB)), (uint64_t) (sizeof(partB) - 1)},
    };

    arch_syscall_frame_t pwritevFrame = {};

    pwritevFrame.syscall_number = SYSCALL_pwritev;
    pwritevFrame.arg0           = fd;
    pwritevFrame.arg1           = request_user_address(iov, sizeof(iov));
    pwritevFrame.arg2           = 2;
    pwritevFrame.arg3           = 0;

    const uint64_t pwritevResult = dispatcher_dispatch_syscall(&pwritevFrame);

    arch_syscall_frame_t badOffsetFrame = pwritevFrame;

    badOffsetFrame.arg3 = (uint64_t) -1LL;

    const uint64_t badOffsetResult = dispatcher_dispatch_syscall(&badOffsetFrame);

    const bool positionValid = equal(syscall_call(SYSCALL_lseek, fd, 0, 1), 0, "positional/vector I/O file position");

    const bool contentsValid = snapshot.Matches("Arx-pwritev", 11);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    const uint64_t expected = (uint64_t) ((sizeof(partA) - 1) + (sizeof(partB) - 1));

    return REQUEST_EXPECT(positionValid && contentsValid && pwritevResult == expected && badOffsetResult == LINUX_EINVAL && closeResult == 0);
}

static bool validate_open(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(closeResult == 0);
}

static bool validate_creat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t creatFrame = {};

    creatFrame.syscall_number = SYSCALL_creat;
    creatFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    creatFrame.arg1           = 0644;

    uint64_t fdResult = dispatcher_dispatch_syscall(&creatFrame);

    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(closeResult == 0);
}

static bool validate_openat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openatFrame = {};

    openatFrame.syscall_number = SYSCALL_openat;
    openatFrame.arg0           = LINUX_AT_FDCWD;
    openatFrame.arg1           = request_user_address("/test.txt", sizeof("/test.txt"));
    openatFrame.arg2           = 0;
    openatFrame.arg3           = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openatFrame);

    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    if (closeResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t rootDirFrame = {};

    rootDirFrame.syscall_number = SYSCALL_open;
    rootDirFrame.arg0           = request_user_address("/", sizeof("/"));
    rootDirFrame.arg1           = 0;
    rootDirFrame.arg2           = 0;

    const uint64_t rootDirFd = dispatcher_dispatch_syscall(&rootDirFrame);

    if ((int64_t) rootDirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t relativeOpenat = {};

    relativeOpenat.syscall_number = SYSCALL_openat;
    relativeOpenat.arg0           = rootDirFd;
    relativeOpenat.arg1           = request_user_address("test.txt", sizeof("test.txt"));
    relativeOpenat.arg2           = 0;
    relativeOpenat.arg3           = 0;

    const uint64_t relativeFd = dispatcher_dispatch_syscall(&relativeOpenat);

    arch_syscall_frame_t closeRootDir = {};

    closeRootDir.syscall_number = SYSCALL_close;
    closeRootDir.arg0           = rootDirFd;

    const uint64_t closeRootDirResult = dispatcher_dispatch_syscall(&closeRootDir);

    if ((int64_t) relativeFd < 0 || closeRootDirResult != 0)
    {
        return false;
    }

    closeFrame.arg0 = relativeFd;

    const uint64_t closeRelativeResult = dispatcher_dispatch_syscall(&closeFrame);

    if (closeRelativeResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t regularFileFrame = {};

    regularFileFrame.syscall_number = SYSCALL_open;
    regularFileFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    regularFileFrame.arg1           = 0;
    regularFileFrame.arg2           = 0;

    const uint64_t regularFd = dispatcher_dispatch_syscall(&regularFileFrame);

    if ((int64_t) regularFd < 0)
    {
        return false;
    }

    relativeOpenat.arg0 = regularFd;

    const uint64_t nonDirOpenResult = dispatcher_dispatch_syscall(&relativeOpenat);

    closeFrame.arg0 = regularFd;

    const uint64_t closeRegularResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(nonDirOpenResult == LINUX_ENOTDIR && closeRegularResult == 0);
}

static bool validate_close(const syscall_case_t* ctx)
{
    if (ctx == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdResult;

    const uint64_t firstClose = dispatcher_dispatch_syscall(&closeFrame);

    if (firstClose != 0)
    {
        return false;
    }

    const uint64_t secondClose = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(secondClose == LINUX_EBADF);
}

static bool validate_close_range(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openA = {};

    openA.syscall_number = SYSCALL_open;
    openA.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openA.arg1           = 0;
    openA.arg2           = 0;

    const uint64_t fdA = dispatcher_dispatch_syscall(&openA);

    if ((int64_t) fdA < 0)
    {
        return false;
    }

    arch_syscall_frame_t openB = openA;

    const uint64_t fdB = dispatcher_dispatch_syscall(&openB);

    if ((int64_t) fdB < 0)
    {
        return false;
    }

    const uint64_t first = (fdA < fdB) ? fdA : fdB;
    const uint64_t last  = (fdA < fdB) ? fdB : fdA;

    arch_syscall_frame_t closeRange = {};

    closeRange.syscall_number = SYSCALL_close_range;
    closeRange.arg0           = first;
    closeRange.arg1           = last;
    closeRange.arg2           = 0;

    const uint64_t closeRangeResult = dispatcher_dispatch_syscall(&closeRange);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdA;

    const uint64_t closeA = dispatcher_dispatch_syscall(&closeFrame);

    closeFrame.arg0 = fdB;

    const uint64_t closeB = dispatcher_dispatch_syscall(&closeFrame);

    arch_syscall_frame_t badRange = closeRange;

    badRange.arg0 = 10;
    badRange.arg1 = 5;

    const uint64_t badRangeResult = dispatcher_dispatch_syscall(&badRange);

    // Validate CLOSE_RANGE_UNSHARE is accepted (no-op in current kernel model).
    const uint64_t fdC = dispatcher_dispatch_syscall(&openA);

    const uint64_t fdD = dispatcher_dispatch_syscall(&openB);

    if ((int64_t) fdC < 0 || (int64_t) fdD < 0)
    {
        return false;
    }

    const uint64_t       first2            = (fdC < fdD) ? fdC : fdD;
    const uint64_t       last2             = (fdC < fdD) ? fdD : fdC;
    arch_syscall_frame_t closeRangeUnshare = {};

    closeRangeUnshare.syscall_number = SYSCALL_close_range;
    closeRangeUnshare.arg0           = first2;
    closeRangeUnshare.arg1           = last2;
    closeRangeUnshare.arg2           = (1ULL << 1);

    const uint64_t closeRangeUnshareResult = dispatcher_dispatch_syscall(&closeRangeUnshare);

    closeFrame.arg0 = fdC;

    const uint64_t closeC = dispatcher_dispatch_syscall(&closeFrame);

    closeFrame.arg0 = fdD;

    const uint64_t closeD = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(closeRangeResult == 0 && closeA == LINUX_EBADF && closeB == LINUX_EBADF && badRangeResult == LINUX_EINVAL && closeRangeUnshareResult == 0 &&
                          closeC == LINUX_EBADF && closeD == LINUX_EBADF);
}

static bool validate_fcntl(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    constexpr uint64_t TEST_F_GETFD    = 1;
    constexpr uint64_t TEST_F_SETFD    = 2;
    constexpr uint64_t TEST_F_GETFL    = 3;
    constexpr uint64_t TEST_F_SETFL    = 4;
    constexpr uint64_t TEST_F_DUPFD    = 0;
    constexpr uint64_t TEST_FD_CLOEXEC = 1;

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    arch_syscall_frame_t fcntlGet = {};

    fcntlGet.syscall_number = SYSCALL_fcntl;
    fcntlGet.arg0           = fd;
    fcntlGet.arg1           = TEST_F_GETFD;
    fcntlGet.arg2           = 0;

    const uint64_t getBefore = dispatcher_dispatch_syscall(&fcntlGet);

    arch_syscall_frame_t fcntlSet = fcntlGet;

    fcntlSet.arg1 = TEST_F_SETFD;
    fcntlSet.arg2 = TEST_FD_CLOEXEC;

    const uint64_t setResult = dispatcher_dispatch_syscall(&fcntlSet);

    const uint64_t getAfter = dispatcher_dispatch_syscall(&fcntlGet);

    arch_syscall_frame_t fcntlGetfl = fcntlGet;

    fcntlGetfl.arg1 = TEST_F_GETFL;

    const uint64_t getflBefore = dispatcher_dispatch_syscall(&fcntlGetfl);

    arch_syscall_frame_t fcntlSetfl = fcntlGet;

    fcntlSetfl.arg1 = TEST_F_SETFL;
    fcntlSetfl.arg2 = getflBefore | 04000ULL;

    const uint64_t setflResult = dispatcher_dispatch_syscall(&fcntlSetfl);

    const uint64_t getflAfter = dispatcher_dispatch_syscall(&fcntlGetfl);

    arch_syscall_frame_t fcntlDup = fcntlGet;

    fcntlDup.arg1 = TEST_F_DUPFD;
    fcntlDup.arg2 = fd + 1;

    const uint64_t dupFd = dispatcher_dispatch_syscall(&fcntlDup);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeOriginal = dispatcher_dispatch_syscall(&closeFrame);

    uint64_t closeDup = LINUX_EBADF;

    if ((int64_t) dupFd >= 0)
    {
        closeFrame.arg0 = dupFd;

        closeDup = dispatcher_dispatch_syscall(&closeFrame);
    }

    return REQUEST_EXPECT(getBefore == 0 && setResult == 0 && getAfter == TEST_FD_CLOEXEC && setflResult == 0 && getflAfter == (getflBefore | 04000ULL) && (int64_t) dupFd >= 0 &&
                          closeOriginal == 0 && closeDup == 0);
}

static bool validate_dup(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t oldFd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) oldFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dupFrame = {};

    dupFrame.syscall_number = SYSCALL_dup;
    dupFrame.arg0           = oldFd;

    const uint64_t newFd = dispatcher_dispatch_syscall(&dupFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = oldFd;

    const uint64_t closeOld = dispatcher_dispatch_syscall(&closeFrame);

    uint64_t closeNew = LINUX_EBADF;

    if ((int64_t) newFd >= 0)
    {
        closeFrame.arg0 = newFd;

        closeNew = dispatcher_dispatch_syscall(&closeFrame);
    }

    return REQUEST_EXPECT((int64_t) newFd >= 0 && closeOld == 0 && closeNew == 0);
}

static bool validate_dup2(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t oldFd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) oldFd < 0)
    {
        return false;
    }

    const uint64_t tmpFd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) tmpFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dup2Frame = {};

    dup2Frame.syscall_number = SYSCALL_dup2;
    dup2Frame.arg0           = oldFd;
    dup2Frame.arg1           = tmpFd;

    const uint64_t dup2Result = dispatcher_dispatch_syscall(&dup2Frame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = oldFd;

    const uint64_t closeOld = dispatcher_dispatch_syscall(&closeFrame);

    closeFrame.arg0 = tmpFd;

    const uint64_t closeNew = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(dup2Result == tmpFd && closeOld == 0 && closeNew == 0);
}

static bool validate_dup3(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    constexpr uint64_t TEST_O_CLOEXEC = 02000000ULL;

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t oldFd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) oldFd < 0)
    {
        return false;
    }

    const uint64_t tmpFd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) tmpFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t dup3Frame = {};

    dup3Frame.syscall_number = SYSCALL_dup3;
    dup3Frame.arg0           = oldFd;
    dup3Frame.arg1           = tmpFd;
    dup3Frame.arg2           = TEST_O_CLOEXEC;

    const uint64_t dup3Result = dispatcher_dispatch_syscall(&dup3Frame);

    arch_syscall_frame_t sameFrame = dup3Frame;

    sameFrame.arg1 = oldFd;

    const uint64_t sameResult = dispatcher_dispatch_syscall(&sameFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = oldFd;

    const uint64_t closeOld = dispatcher_dispatch_syscall(&closeFrame);

    closeFrame.arg0 = tmpFd;

    const uint64_t closeNew = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(dup3Result == tmpFd && sameResult == LINUX_EINVAL && closeOld == 0 && closeNew == 0);
}

static bool validate_ioctl(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_ENOTTY = (uint64_t) -25;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    arch_syscall_frame_t ioctlFrame = {};

    ioctlFrame.syscall_number = SYSCALL_ioctl;
    ioctlFrame.arg0           = fd;
    ioctlFrame.arg1           = 0;
    ioctlFrame.arg2           = 0;

    const uint64_t ioctlResult = dispatcher_dispatch_syscall(&ioctlFrame);

    arch_syscall_frame_t badFdFrame = ioctlFrame;

    badFdFrame.arg0 = fd + 1;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(ioctlResult == TEST_LINUX_ENOTTY && badFdResult == LINUX_EBADF && closeResult == 0);
}

static bool validate_lseek(const syscall_case_t* ctx)
{
    FileContentGuard snapshot(false);

    if (!snapshot.valid)
    {
        return false;
    }

    if (ctx == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    arch_syscall_frame_t lseekEnd = {};

    lseekEnd.syscall_number = SYSCALL_lseek;
    lseekEnd.arg0           = fdResult;
    lseekEnd.arg1           = 0;
    lseekEnd.arg2           = 2;

    const uint64_t endOffset = dispatcher_dispatch_syscall(&lseekEnd);

    arch_syscall_frame_t lseekBad = lseekEnd;

    lseekBad.arg2 = 99;

    const uint64_t badWhence = dispatcher_dispatch_syscall(&lseekBad);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(endOffset == static_cast<uint64_t>(snapshot.count) && badWhence == LINUX_EINVAL && closeResult == 0);
}

static bool validate_getcwd(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    char cwdBuffer[ProcessManager::MAX_CWD_PATH_LENGTH] = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_getcwd;
    frame.arg0           = request_user_address(cwdBuffer, sizeof(cwdBuffer));
    frame.arg1           = sizeof(cwdBuffer);

    const uint64_t getcwdResult = dispatcher_dispatch_syscall(&frame);

    if (getcwdResult == 0 || getcwdResult >= sizeof(cwdBuffer))
    {
        return false;
    }

    if (cwdBuffer[getcwdResult - 1] != '\0')
    {
        return false;
    }

    arch_syscall_frame_t badSizeFrame = frame;

    badSizeFrame.arg1 = 1;

    const uint64_t badSizeResult = dispatcher_dispatch_syscall(&badSizeFrame);

    return REQUEST_EXPECT(badSizeResult == LINUX_ERANGE);
}

static bool validate_chdir(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t chdirRoot = {};

    chdirRoot.syscall_number = SYSCALL_chdir;
    chdirRoot.arg0           = request_user_address("/", sizeof("/"));

    const uint64_t chdirRootResult = dispatcher_dispatch_syscall(&chdirRoot);

    if (chdirRootResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t chdirBad = {};

    chdirBad.syscall_number = SYSCALL_chdir;
    chdirBad.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));

    const uint64_t chdirBadResult = dispatcher_dispatch_syscall(&chdirBad);

    return REQUEST_EXPECT(chdirBadResult == LINUX_ENOTDIR);
}

static bool validate_fchdir(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};

    openDir.syscall_number = SYSCALL_open;
    openDir.arg0           = request_user_address("/", sizeof("/"));
    openDir.arg1           = 0;
    openDir.arg2           = 0;

    const uint64_t dirFd = dispatcher_dispatch_syscall(&openDir);

    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t fchdirFrame = {};

    fchdirFrame.syscall_number = SYSCALL_fchdir;
    fchdirFrame.arg0           = dirFd;

    const uint64_t fchdirResult = dispatcher_dispatch_syscall(&fchdirFrame);

    arch_syscall_frame_t closeDir = {};

    closeDir.syscall_number = SYSCALL_close;
    closeDir.arg0           = dirFd;

    const uint64_t closeDirResult = dispatcher_dispatch_syscall(&closeDir);

    arch_syscall_frame_t openFile = {};

    openFile.syscall_number = SYSCALL_open;
    openFile.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFile.arg1           = 0;
    openFile.arg2           = 0;

    const uint64_t fileFd = dispatcher_dispatch_syscall(&openFile);

    if ((int64_t) fileFd < 0)
    {
        return false;
    }

    fchdirFrame.arg0 = fileFd;

    const uint64_t fileFchdirResult = dispatcher_dispatch_syscall(&fchdirFrame);

    closeDir.arg0 = fileFd;

    const uint64_t closeFileResult = dispatcher_dispatch_syscall(&closeDir);

    return REQUEST_EXPECT(fchdirResult == 0 && closeDirResult == 0 && fileFchdirResult == LINUX_ENOTDIR && closeFileResult == 0);
}

static bool validate_fstat(const syscall_case_t* ctx)
{
    if (ctx == nullptr)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    openFrame.arg1           = 0;
    openFrame.arg2           = 0;

    uint64_t fdResult = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fdResult < 0)
    {
        return false;
    }

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t fstatFrame = {};

    fstatFrame.syscall_number = SYSCALL_fstat;
    fstatFrame.arg0           = fdResult;
    fstatFrame.arg1           = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t fstatResult = dispatcher_dispatch_syscall(&fstatFrame);

    arch_syscall_frame_t badFdFrame = fstatFrame;

    badFdFrame.arg0 = fdResult + 1;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fdResult;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    constexpr uint32_t linux_s_ifmt  = 0170000U;
    constexpr uint32_t linux_s_ifreg = 0100000U;

    const bool modeLooksRegular = (statBuffer.st_mode & linux_s_ifmt) == linux_s_ifreg;
    const bool sizeReasonable   = statBuffer.st_size >= 0;
    const bool blockSizeSet     = statBuffer.st_blksize > 0;

    return REQUEST_EXPECT(fstatResult == 0 && statBuffer.st_nlink >= 1 && modeLooksRegular && sizeReasonable && blockSizeSet && badFdResult == LINUX_EBADF && closeResult == 0);
}

static bool validate_newfstatat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_newfstatat;
    frame.arg0           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2           = request_user_address(&statBuffer, sizeof(statBuffer));
    frame.arg3           = 0;

    const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFlagFrame = frame;

    badFlagFrame.arg3 = 0x80000000ULL;

    const uint64_t badFlagResult = dispatcher_dispatch_syscall(&badFlagFrame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg1 = request_user_address("/does-not-exist", sizeof("/does-not-exist"));

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(okResult == 0 && statBuffer.st_nlink >= 1 && badFlagResult == LINUX_EINVAL && badPathResult == LINUX_ENOENT);
}

static bool validate_stat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_stat;
    frame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1           = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t nullBufferFrame = frame;

    nullBufferFrame.arg1 = 0;

    const uint64_t nullBufferResult = dispatcher_dispatch_syscall(&nullBufferFrame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = request_user_address("/does-not-exist", sizeof("/does-not-exist"));

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(okResult == 0 && statBuffer.st_nlink >= 1 && nullBufferResult == LINUX_EFAULT && badPathResult == LINUX_ENOENT);
}

static bool validate_lstat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t statBuffer = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_lstat;
    frame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1           = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t okResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t nullPathFrame = frame;

    nullPathFrame.arg0 = 0;

    const uint64_t nullPathResult = dispatcher_dispatch_syscall(&nullPathFrame);

    return REQUEST_EXPECT(okResult == 0 && statBuffer.st_nlink >= 1 && nullPathResult == LINUX_EFAULT);
}

static bool validate_symlink(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t linkFrame = {};

    linkFrame.syscall_number = SYSCALL_symlink;
    linkFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    linkFrame.arg1           = request_user_address("/tmp-syscall-link", sizeof("/tmp-syscall-link"));

    const uint64_t linkResult = dispatcher_dispatch_syscall(&linkFrame);

    if (linkResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t readFrame  = {};
    char                 target[64] = {};

    readFrame.syscall_number = SYSCALL_readlink;
    readFrame.arg0           = request_user_address("/tmp-syscall-link", sizeof("/tmp-syscall-link"));
    readFrame.arg1           = request_user_address(target, sizeof(target));
    readFrame.arg2           = sizeof(target);

    const uint64_t readResult = dispatcher_dispatch_syscall(&readFrame);

    return REQUEST_EXPECT((int64_t) readResult > 0 && memcmp(target, "/test.txt", (size_t) readResult) == 0);
}

static bool validate_symlinkat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};

    openDir.syscall_number = SYSCALL_open;
    openDir.arg0           = request_user_address("/", sizeof("/"));

    const uint64_t dirFd = dispatcher_dispatch_syscall(&openDir);

    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t linkFrame = {};

    linkFrame.syscall_number = SYSCALL_symlinkat;
    linkFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    linkFrame.arg1           = dirFd;
    linkFrame.arg2           = request_user_address("tmp-syscall-link-at", sizeof("tmp-syscall-link-at"));

    const uint64_t linkResult = dispatcher_dispatch_syscall(&linkFrame);

    arch_syscall_frame_t closeDir = {};

    closeDir.syscall_number = SYSCALL_close;
    closeDir.arg0           = dirFd;

    (void) dispatcher_dispatch_syscall(&closeDir);

    if (linkResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t readFrame  = {};
    char                 target[64] = {};

    readFrame.syscall_number = SYSCALL_readlink;
    readFrame.arg0           = request_user_address("/tmp-syscall-link-at", sizeof("/tmp-syscall-link-at"));
    readFrame.arg1           = request_user_address(target, sizeof(target));
    readFrame.arg2           = sizeof(target);

    const uint64_t readResult = dispatcher_dispatch_syscall(&readFrame);

    return REQUEST_EXPECT((int64_t) readResult > 0 && memcmp(target, "/test.txt", (size_t) readResult) == 0);
}

static bool validate_readlink(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t createFrame = {};

    createFrame.syscall_number = SYSCALL_symlink;
    createFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    createFrame.arg1           = request_user_address("/tmp-syscall-link-read", sizeof("/tmp-syscall-link-read"));

    const uint64_t createResult = dispatcher_dispatch_syscall(&createFrame);

    if (createResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t frame      = {};
    char                 target[64] = {};

    frame.syscall_number = SYSCALL_readlink;
    frame.arg0           = request_user_address("/tmp-syscall-link-read", sizeof("/tmp-syscall-link-read"));
    frame.arg1           = request_user_address(target, sizeof(target));
    frame.arg2           = sizeof(target);

    const uint64_t readResult = dispatcher_dispatch_syscall(&frame);

    return REQUEST_EXPECT((int64_t) readResult > 0);
}

static bool validate_readlinkat(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};

    openDir.syscall_number = SYSCALL_open;
    openDir.arg0           = request_user_address("/", sizeof("/"));

    const uint64_t dirFd = dispatcher_dispatch_syscall(&openDir);

    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    arch_syscall_frame_t createFrame = {};

    createFrame.syscall_number = SYSCALL_symlinkat;
    createFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    createFrame.arg1           = dirFd;
    createFrame.arg2           = request_user_address("tmp-syscall-link-at-read", sizeof("tmp-syscall-link-at-read"));

    const uint64_t createResult = dispatcher_dispatch_syscall(&createFrame);

    if (createResult != 0)
    {
        arch_syscall_frame_t closeDir = {};

        closeDir.syscall_number = SYSCALL_close;
        closeDir.arg0           = dirFd;

        (void) dispatcher_dispatch_syscall(&closeDir);

        return false;
    }

    arch_syscall_frame_t frame      = {};
    char                 target[64] = {};

    frame.syscall_number = SYSCALL_readlinkat;
    frame.arg0           = dirFd;
    frame.arg1           = request_user_address("tmp-syscall-link-at-read", sizeof("tmp-syscall-link-at-read"));
    frame.arg2           = request_user_address(target, sizeof(target));
    frame.arg3           = sizeof(target);

    const uint64_t readResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t closeDir = {};

    closeDir.syscall_number = SYSCALL_close;
    closeDir.arg0           = dirFd;

    (void) dispatcher_dispatch_syscall(&closeDir);

    return REQUEST_EXPECT((int64_t) readResult > 0);
}

static bool validate_getdents64(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openDir = {};

    openDir.syscall_number = SYSCALL_open;
    openDir.arg0           = request_user_address("/", sizeof("/"));

    const uint64_t dirFd = dispatcher_dispatch_syscall(&openDir);

    if ((int64_t) dirFd < 0)
    {
        return false;
    }

    char                 buffer[512] = {};
    arch_syscall_frame_t frame       = {};

    frame.syscall_number = SYSCALL_getdents64;
    frame.arg0           = dirFd;
    frame.arg1           = request_user_address(buffer, sizeof(buffer));
    frame.arg2           = sizeof(buffer);

    const uint64_t getdentsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFdFrame = frame;

    badFdFrame.arg0 = dirFd + 1024;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = dirFd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT((int64_t) getdentsResult > 0 && getdentsResult <= sizeof(buffer) && badFdResult == LINUX_EBADF && closeResult == 0);
}

static bool validate_access_family(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EACCES = (uint64_t) -13;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = ctx->syscallNumber;

    if (ctx->syscallNumber == SYSCALL_access)
    {
        frame.arg0 = request_user_address("/test.txt", sizeof("/test.txt"));
        frame.arg1 = 0;
    }
    else
    {
        frame.arg0 = (uint64_t) LINUX_AT_FDCWD;
        frame.arg1 = request_user_address("/test.txt", sizeof("/test.txt"));
        frame.arg2 = 0;
        frame.arg3 = 0;
    }

    const uint64_t existsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t execCheckFrame = frame;

    if (ctx->syscallNumber == SYSCALL_access)
    {
        execCheckFrame.arg1 = 1;
    }
    else
    {
        execCheckFrame.arg2 = 1;
    }

    const uint64_t execCheckResult = dispatcher_dispatch_syscall(&execCheckFrame);

    arch_syscall_frame_t missingFrame = frame;

    if (ctx->syscallNumber == SYSCALL_access)
    {
        missingFrame.arg0 = request_user_address("/definitely-missing-selftest-file", sizeof("/definitely-missing-selftest-file"));
    }
    else
    {
        missingFrame.arg1 = request_user_address("/definitely-missing-selftest-file", sizeof("/definitely-missing-selftest-file"));
    }

    const uint64_t missingResult = dispatcher_dispatch_syscall(&missingFrame);

    arch_syscall_frame_t badModeFrame = frame;

    if (ctx->syscallNumber == SYSCALL_access)
    {
        badModeFrame.arg1 = 8;
    }
    else
    {
        badModeFrame.arg2 = 8;
    }

    const uint64_t badModeResult = dispatcher_dispatch_syscall(&badModeFrame);

    bool flagsResultOk = true;

    if (ctx->syscallNumber == SYSCALL_faccessat2)
    {
        arch_syscall_frame_t badFlagsFrame = frame;

        badFlagsFrame.arg3 = 1ULL << 20;

        flagsResultOk = dispatcher_dispatch_syscall(&badFlagsFrame) == LINUX_EINVAL;
    }

    return REQUEST_EXPECT(existsResult == 0 && execCheckResult == TEST_LINUX_EACCES && missingResult == LINUX_ENOENT && badModeResult == LINUX_EINVAL && flagsResultOk);
}

static bool validate_statfs(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    struct test_linux_statfs_t
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

    test_linux_statfs_t  statfsData = {};
    arch_syscall_frame_t frame      = {};

    frame.syscall_number = SYSCALL_statfs;
    frame.arg0           = request_user_address("/", sizeof("/"));
    frame.arg1           = request_user_address(&statfsData, sizeof(statfsData));

    const uint64_t statfsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t missingFrame = frame;

    missingFrame.arg0 = request_user_address("/definitely-missing-selftest-file", sizeof("/definitely-missing-selftest-file"));

    const uint64_t missingResult = dispatcher_dispatch_syscall(&missingFrame);

    return REQUEST_EXPECT(statfsResult == 0 && statfsData.f_bsize > 0 && missingResult == LINUX_ENOENT);
}

static bool validate_fstatfs(const syscall_case_t* ctx)
{
    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    struct test_linux_statfs_t
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

    test_linux_statfs_t statfsData = {};

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_fstatfs;
    frame.arg0           = fd;
    frame.arg1           = request_user_address(&statfsData, sizeof(statfsData));

    const uint64_t fstatfsResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFdFrame = frame;

    badFdFrame.arg0 = fd + 1024;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(fstatfsResult == 0 && statfsData.f_bsize > 0 && badFdResult == LINUX_EBADF && closeResult == 0);
}

static bool validate_mkdirat(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_mkdirat;
    frame.arg0           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1           = request_user_address("/tmp-syscall-mkdirat", sizeof("/tmp-syscall-mkdirat"));
    frame.arg2           = 0755;

    const uint64_t mkdirResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg1 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(mkdirResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_mkdir(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_mkdir;
    frame.arg0           = request_user_address("/tmp-syscall-mkdir", sizeof("/tmp-syscall-mkdir"));
    frame.arg1           = 0755;

    const uint64_t mkdirResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(mkdirResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_unlinkat(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_unlinkat;
    frame.arg0           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2           = 0;

    const uint64_t unlinkResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFlagsFrame = frame;

    badFlagsFrame.arg2 = (1ULL << 20);

    const uint64_t badFlagsResult = dispatcher_dispatch_syscall(&badFlagsFrame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg1 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(unlinkResult == TEST_LINUX_EROFS && badFlagsResult == LINUX_EINVAL && badPathResult == LINUX_EFAULT);
}

static bool validate_unlink(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_unlink;
    frame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));

    const uint64_t unlinkResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(unlinkResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_rmdir(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EBUSY = (uint64_t) -16;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_rmdir;
    frame.arg0           = request_user_address("/", sizeof("/"));

    const uint64_t rmdirResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(rmdirResult == TEST_LINUX_EBUSY && badPathResult == LINUX_EFAULT);
}

static bool validate_renameat(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_renameat;
    frame.arg0           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg3           = request_user_address("/test.txt.renamed", sizeof("/test.txt.renamed"));

    const uint64_t renameResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg1 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(renameResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_rename(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_rename;
    frame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1           = request_user_address("/test.txt.renamed2", sizeof("/test.txt.renamed2"));

    const uint64_t renameResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(renameResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_linkat(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_linkat;
    frame.arg0           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg2           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg3           = request_user_address("/tmp-syscall-linkat", sizeof("/tmp-syscall-linkat"));
    frame.arg4           = 0;

    const uint64_t linkResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badFlagsFrame = frame;

    badFlagsFrame.arg4 = (1ULL << 20);

    const uint64_t badFlagsResult = dispatcher_dispatch_syscall(&badFlagsFrame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg1 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(linkResult == TEST_LINUX_EROFS && badFlagsResult == LINUX_EINVAL && badPathResult == LINUX_EFAULT);
}

static bool validate_link(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_link;
    frame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1           = request_user_address("/tmp-syscall-link", sizeof("/tmp-syscall-link"));

    const uint64_t linkResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(linkResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_mknodat(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_mknodat;
    frame.arg0           = (uint64_t) LINUX_AT_FDCWD;
    frame.arg1           = request_user_address("/tmp-syscall-mknodat", sizeof("/tmp-syscall-mknodat"));
    frame.arg2           = 0644;
    frame.arg3           = 0;

    const uint64_t mknodatResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg1 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(mknodatResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_mknod(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_mknod;
    frame.arg0           = request_user_address("/tmp-syscall-mknod", sizeof("/tmp-syscall-mknod"));
    frame.arg1           = 0644;
    frame.arg2           = 0;

    const uint64_t mknodResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badPathFrame = frame;

    badPathFrame.arg0 = 0;

    const uint64_t badPathResult = dispatcher_dispatch_syscall(&badPathFrame);

    return REQUEST_EXPECT(mknodResult == TEST_LINUX_EROFS && badPathResult == LINUX_EFAULT);
}

static bool validate_truncate(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    test_linux_stat_t    statBuffer = {};
    arch_syscall_frame_t statFrame  = {};

    statFrame.syscall_number = SYSCALL_stat;
    statFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    statFrame.arg1           = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t statResult = dispatcher_dispatch_syscall(&statFrame);

    if (statResult != 0)
    {
        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_truncate;
    frame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));
    frame.arg1           = (uint64_t) statBuffer.st_size;

    const uint64_t truncateResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badLengthFrame = frame;

    badLengthFrame.arg1 = (uint64_t) -1LL;

    const uint64_t badLengthResult = dispatcher_dispatch_syscall(&badLengthFrame);

    arch_syscall_frame_t growFrame = frame;

    growFrame.arg1 = (uint64_t) statBuffer.st_size + 1ULL;

    const uint64_t growResult = dispatcher_dispatch_syscall(&growFrame);

    return REQUEST_EXPECT(truncateResult == 0 && badLengthResult == LINUX_EINVAL && growResult == TEST_LINUX_EROFS);
}

static bool validate_ftruncate(const syscall_case_t* ctx)
{
    constexpr uint64_t TEST_LINUX_EROFS = (uint64_t) -30;

    if (ctx == nullptr || ctx->frame == nullptr)
    {
        return false;
    }

    struct test_linux_timespec_t
    {
        int64_t tv_sec;
        int64_t tv_nsec;
    };

    struct test_linux_stat_t
    {
        uint64_t              st_dev;
        uint64_t              st_ino;
        uint64_t              st_nlink;
        uint32_t              st_mode;
        uint32_t              st_uid;
        uint32_t              st_gid;
        int32_t               pad0;
        uint64_t              st_rdev;
        int64_t               st_size;
        int64_t               st_blksize;
        int64_t               st_blocks;
        test_linux_timespec_t st_atim;
        test_linux_timespec_t st_mtim;
        test_linux_timespec_t st_ctim;
        int64_t               reserved[3];
    };

    arch_syscall_frame_t openFrame = {};

    openFrame.syscall_number = SYSCALL_open;
    openFrame.arg0           = request_user_address("/test.txt", sizeof("/test.txt"));

    const uint64_t fd = dispatcher_dispatch_syscall(&openFrame);

    if ((int64_t) fd < 0)
    {
        return false;
    }

    test_linux_stat_t    statBuffer = {};
    arch_syscall_frame_t fstatFrame = {};

    fstatFrame.syscall_number = SYSCALL_fstat;
    fstatFrame.arg0           = fd;
    fstatFrame.arg1           = request_user_address(&statBuffer, sizeof(statBuffer));

    const uint64_t fstatResult = dispatcher_dispatch_syscall(&fstatFrame);

    if (fstatResult != 0)
    {
        arch_syscall_frame_t closeFrame = {};

        closeFrame.syscall_number = SYSCALL_close;
        closeFrame.arg0           = fd;

        (void) dispatcher_dispatch_syscall(&closeFrame);

        return false;
    }

    arch_syscall_frame_t frame = {};

    frame.syscall_number = SYSCALL_ftruncate;
    frame.arg0           = fd;
    frame.arg1           = (uint64_t) statBuffer.st_size;

    const uint64_t ftruncateResult = dispatcher_dispatch_syscall(&frame);

    arch_syscall_frame_t badLengthFrame = frame;

    badLengthFrame.arg1 = (uint64_t) -1LL;

    const uint64_t badLengthResult = dispatcher_dispatch_syscall(&badLengthFrame);

    arch_syscall_frame_t growFrame = frame;

    growFrame.arg1 = (uint64_t) statBuffer.st_size + 1ULL;

    const uint64_t growResult = dispatcher_dispatch_syscall(&growFrame);

    arch_syscall_frame_t badFdFrame = frame;

    badFdFrame.arg0 = fd + 1024;

    const uint64_t badFdResult = dispatcher_dispatch_syscall(&badFdFrame);

    arch_syscall_frame_t closeFrame = {};

    closeFrame.syscall_number = SYSCALL_close;
    closeFrame.arg0           = fd;

    const uint64_t closeResult = dispatcher_dispatch_syscall(&closeFrame);

    return REQUEST_EXPECT(ftruncateResult == 0 && badLengthResult == LINUX_EINVAL && growResult == TEST_LINUX_EROFS && badFdResult == LINUX_EBADF && closeResult == 0);
}

static bool test_ipc_pipe(process_t* process, uint64_t base, uint64_t number)
{
    int32_t        descriptors[2] = {-1, -1};
    const uint32_t flags          = number == SYSCALL_pipe2 ? 04000 | 02000000 : 0;

    bool ok = expect(syscall_call(number, base, flags) == 0, "pipe syscall creates both endpoints");

    fixture_load(process, base, descriptors, sizeof(descriptors));

    if (descriptors[0] < 0 || descriptors[1] < 0)
    {
        return false;
    }

    const uint64_t reader = descriptors[0], writer = descriptors[1];

    ok &= expect(syscall_call(SYSCALL_fcntl, reader, 3) == (flags & 04000) && syscall_call(SYSCALL_fcntl, writer, 3) == (1 | (flags & 04000)) &&
                     syscall_call(SYSCALL_fcntl, reader, 1) == (uint64_t) ((flags & 02000000) != 0),
        "pipe access modes and descriptor flags");

    syscall_call(SYSCALL_fcntl, reader, 4, 04000);

    syscall_call(SYSCALL_fcntl, writer, 4, 04001);

    ok &= expect(syscall_call(SYSCALL_read, reader, base + 512, 1) == (uint64_t) -11 && syscall_call(SYSCALL_read, writer, base + 512, 1) == LINUX_EBADF &&
                     syscall_call(SYSCALL_write, reader, base + 512, 1) == LINUX_EBADF,
        "empty nonblocking pipe and endpoint direction validation");

    const char text[] = "pipe-data";

    fixture_store(process, base + 128, text, sizeof(text));

    ok &= expect(syscall_call(SYSCALL_write, writer, base + 128, sizeof(text)) == sizeof(text), "pipe write payload");

    ok &= expect(syscall_call(SYSCALL_read, reader, CANONICAL_KERNEL_BASE, sizeof(text)) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_read, reader, base + 512, sizeof(text)) == sizeof(text),
        "pipe read fault preserves data");

    char output[sizeof(text)] = {};

    fixture_load(process, base + 512, output, sizeof(output));

    ok &= expect(memcmp(text, output, sizeof(text)) == 0, "pipe payload round trip");

    ok &= expect(syscall_call(SYSCALL_lseek, reader, 0, 0) == (uint64_t) -29, "pipe ESPIPE");

    ok &= expect(syscall_call(SYSCALL_fcntl, writer, 1031, 4096) == 4096 && syscall_call(SYSCALL_fcntl, reader, 1032) == 4096, "pipe capacity control shared by endpoints");

    ok &= expect(syscall_call(SYSCALL_write, writer, base + 128, 4095) == 4095 && syscall_call(SYSCALL_write, writer, base + 128, 2) == (uint64_t) -11,
        "PIPE_BUF nonblocking writes are atomic");

    ok &= expect(syscall_call(SYSCALL_ioctl, reader, 0x541b, base + 64) == 0, "pipe FIONREAD");

    int32_t queued = 0;

    fixture_load(process, base + 64, &queued, sizeof(queued));

    ok &= expect(queued == 4095 && syscall_call(SYSCALL_read, reader, base + 8192, 4096) == 4095, "failed atomic pipe write leaves queue unchanged");

    file_iovec_t vectors[2] = {{base + 128, 2}, {base + 130, 3}};

    fixture_store(process, base + 256, vectors, sizeof(vectors));

    ok &= expect(syscall_call(SYSCALL_writev, writer, base + 256, 2) == 5 && syscall_call(SYSCALL_read, reader, base + 512, 5) == 5, "pipe writev gathers into one atomic write");

    syscall_call(SYSCALL_fcntl, reader, 4, 0);

    syscall_peer_context_t peer = {nullptr, SYSCALL_write, {writer, base + 128, sizeof(text)}, UINT64_MAX};

    ok &= with_syscall_peer(process, &peer, [&]()
    {
        return expect(syscall_call(SYSCALL_read, reader, base + 512, sizeof(text)) == sizeof(text) && peer.result == sizeof(text), "blocking pipe read wakes on peer write");
    });
    ok &= expect(syscall_call(SYSCALL_write, writer, base + 128, 4096) == 4096, "fill pipe before blocking writer test");

    syscall_call(SYSCALL_fcntl, writer, 4, 1);

    peer = {nullptr, SYSCALL_read, {reader, base + 8192, 4096}, UINT64_MAX};

    ok &= with_syscall_peer(process, &peer, [&]()
    {
        return expect(syscall_call(SYSCALL_write, writer, base + 128, 1) == 1 && peer.result == 4096, "blocking pipe writer wakes after peer drains buffer");
    });
    ok &= expect(syscall_call(SYSCALL_read, reader, base + 512, 1) == 1, "drain blocking writer test byte");

    const uint64_t alias = syscall_call(SYSCALL_dup, writer);

    syscall_call(SYSCALL_close, writer);

    ok &= expect((int64_t) alias >= 0 && syscall_call(SYSCALL_write, alias, base + 128, 1) == 1 && syscall_call(SYSCALL_read, reader, base + 512, 1) == 1,
        "dup keeps pipe write endpoint alive");

    if ((int64_t) alias >= 0)
    {
        syscall_call(SYSCALL_close, alias);
    }

    ok &= expect(syscall_call(SYSCALL_read, reader, base + 512, 1) == 0, "last writer close produces pipe EOF");

    event_pollfd_t poll = {(int32_t) reader, 1, 0};

    fixture_store(process, base + 256, &poll, sizeof(poll));

    ok &= expect(syscall_call(SYSCALL_poll, base + 256, 1, 0) == 1, "pipe close wakes poll");

    fixture_load(process, base + 256, &poll, sizeof(poll));

    ok &= expect((poll.revents & 0x10) != 0, "pipe POLLHUP");

    syscall_call(SYSCALL_close, reader);

    if (syscall_call(SYSCALL_pipe2, base, 0) == 0)
    {
        fixture_load(process, base, descriptors, sizeof(descriptors));

        syscall_call(SYSCALL_close, descriptors[0]);

        auto           task    = static_cast<Dispatcher*>(platform.dispacher)->GetResourceLayerCaps()->taskManager->GetCurrentTask();
        const uint64_t pending = task->signalPending;

        ok &= expect(syscall_call(SYSCALL_write, descriptors[1], base + 128, 1) == (uint64_t) -32 && (task->signalPending & (1ULL << 12)) != 0,
            "pipe without readers returns EPIPE and queues SIGPIPE");

        task->signalPending = pending;

        syscall_call(SYSCALL_close, descriptors[1]);
    }
    else
    {
        ok &= expect(false, "broken-pipe fixture");
    }

    const size_t before = descriptor_count(process);

    ok &= expect(syscall_call(SYSCALL_pipe2, CANONICAL_KERNEL_BASE, 0) == LINUX_EFAULT && descriptor_count(process) == before, "pipe output fault rolls back both descriptors");

    const uint64_t limit = process->fileLimit.current;

    process->fileLimit.current = 1;

    ok &= expect(syscall_call(SYSCALL_pipe2, base, 0) == LINUX_EMFILE && descriptor_count(process) == before, "pipe second-fd allocation rollback");

    process->fileLimit.current = limit;

    ok &= expect(syscall_call(SYSCALL_pipe2, base, 1) == LINUX_EINVAL && syscall_call(SYSCALL_pipe2, base, 0200) == LINUX_EOPNOTSUPP,
        "pipe invalid flags and notification-pipe prerequisite");

    if (syscall_call(SYSCALL_pipe2, base, 040000 | 04000) == 0)
    {
        fixture_load(process, base, descriptors, sizeof(descriptors));

        const uint64_t packetReader = descriptors[0], packetWriter = descriptors[1];

        ok &= expect(syscall_call(SYSCALL_write, packetWriter, base + 128, 7) == 7 && syscall_call(SYSCALL_write, packetWriter, base + 128, 9) == 9,
            "packet pipe queues separate writes");

        ok &= expect(syscall_call(SYSCALL_read, packetReader, base + 512, 3) == 3 && syscall_call(SYSCALL_read, packetReader, base + 512, 32) == 9 &&
                         syscall_call(SYSCALL_read, packetReader, base + 512, 1) == (uint64_t) -11,
            "short packet read discards only remainder of first packet");

        ok &= expect(syscall_call(SYSCALL_write, packetWriter, base + 128, 4097) == 4097 && syscall_call(SYSCALL_read, packetReader, base + 8192, 8192) == 4096 &&
                         syscall_call(SYSCALL_read, packetReader, base + 512, 32) == 1,
            "packet writes split at PIPE_BUF");

        syscall_call(SYSCALL_write, packetWriter, base + 128, 7);

        ok &= expect(syscall_call(SYSCALL_read, packetReader, CANONICAL_KERNEL_BASE, 7) == LINUX_EFAULT && syscall_call(SYSCALL_read, packetReader, base + 512, 32) == 7,
            "packet read fault preserves packet");

        ok &= expect(syscall_call(SYSCALL_fcntl, packetWriter, 4, 04000) == 0 && syscall_call(SYSCALL_write, packetWriter, base + 128, 7) == 7 &&
                         syscall_call(SYSCALL_read, packetReader, base + 512, 3) == 3 && syscall_call(SYSCALL_read, packetReader, base + 512, 32) == 4,
            "fcntl O_DIRECT change affects subsequent writes");

        syscall_call(SYSCALL_fcntl, packetWriter, 4, 04000 | 040000);

        syscall_call(SYSCALL_write, packetWriter, base + 128, 7);

        ok &= expect(syscall_call(SYSCALL_fcntl, packetReader, 1031, 4096) == 4096 && syscall_call(SYSCALL_read, packetReader, base + 512, 3) == 3 &&
                         syscall_call(SYSCALL_read, packetReader, base + 512, 32) == (uint64_t) -11,
            "pipe resize preserves packet markers");

        syscall_call(SYSCALL_close, packetReader);

        syscall_call(SYSCALL_close, packetWriter);
    }
    else
    {
        ok &= expect(false, "packet pipe creation");
    }

    return ok;
}

static bool test_ipc_memory_file(process_t* process, uint64_t base)
{
    const char name[] = "arx-memfd";

    fixture_store(process, base + 128, name, sizeof(name));

    const uint64_t fd = syscall_call(SYSCALL_memfd_create, base + 128, 3);

    if (!expect((int64_t) fd >= 0, "memfd creates anonymous regular file"))
    {
        return false;
    }

    bool ok = expect(syscall_call(SYSCALL_fcntl, fd, 1) == 1 && syscall_call(SYSCALL_fcntl, fd, 1034) == 0, "memfd CLOEXEC and initially unsealed state");

    const char text[] = "memory-file";

    fixture_store(process, base + 512, text, sizeof(text));

    ok &= expect(syscall_call(SYSCALL_write, fd, base + 512, sizeof(text)) == sizeof(text) && syscall_call(SYSCALL_pread64, fd, base + 1024, sizeof(text), 0) == sizeof(text),
        "memfd write and positioned read");

    char output[sizeof(text)] = {};

    fixture_load(process, base + 1024, output, sizeof(output));

    ok &= expect(memcmp(text, output, sizeof(text)) == 0 && syscall_call(SYSCALL_lseek, fd, 0, 1) == sizeof(text), "memfd positioned read preserves shared file offset");

    ok &= expect(syscall_call(SYSCALL_ftruncate, fd, PAGE_SIZE * 2 + 17) == 0 && syscall_call(SYSCALL_pread64, fd, base + 1024, 32, PAGE_SIZE) == 32, "memfd sparse growth");

    uint8_t zeros[32] = {};

    fixture_load(process, base + 1024, zeros, sizeof(zeros));

    bool zero = true;

    for (uint8_t byte : zeros)
    {
        zero &= byte == 0;
    }

    ok &= expect(zero, "memfd holes read as zero");

    ok &= expect(syscall_call(SYSCALL_ftruncate, fd, 3) == 0 && syscall_call(SYSCALL_ftruncate, fd, 32) == 0 && syscall_call(SYSCALL_pread64, fd, base + 1024, 29, 3) == 29,
        "memfd shrink discards partial-page tail");

    fixture_load(process, base + 1024, zeros, 29);

    zero = true;

    for (size_t i = 0; i < 29; ++i)
    {
        zero &= zeros[i] == 0;
    }

    ok &= expect(zero, "memfd truncated bytes do not reappear on growth");

    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 1033, 2 | 4 | 8) == 0 && syscall_call(SYSCALL_write, fd, base + 512, 1) == (uint64_t) -1 &&
                     syscall_call(SYSCALL_ftruncate, fd, 31) == (uint64_t) -1 && syscall_call(SYSCALL_ftruncate, fd, 33) == (uint64_t) -1,
        "memfd write/grow/shrink seals return EPERM");

    ok &= expect(syscall_call(SYSCALL_fcntl, fd, 1033, 1) == 0 && syscall_call(SYSCALL_fcntl, fd, 1033, 16) == (uint64_t) -1, "memfd seal-seal is irreversible");

    syscall_call(SYSCALL_close, fd);

    const uint64_t fixed = syscall_call(SYSCALL_memfd_create, base + 128, 0);

    ok &= expect((int64_t) fixed >= 0 && syscall_call(SYSCALL_fcntl, fixed, 1034) == 1, "memfd defaults to F_SEAL_SEAL");

    if ((int64_t) fixed >= 0)
    {
        syscall_call(SYSCALL_close, fixed);
    }

    ok &= expect(syscall_call(SYSCALL_memfd_create, CANONICAL_KERNEL_BASE, 0) == LINUX_EFAULT && syscall_call(SYSCALL_memfd_create, base + 128, 0x8000) == LINUX_EINVAL &&
                     syscall_call(SYSCALL_memfd_create, base + 128, 4) == LINUX_EOPNOTSUPP,
        "memfd bad names/flags and hugepage prerequisite");

    const uint64_t mappedFd = syscall_call(SYSCALL_memfd_create, base + 128, 2);

    if (!expect(static_cast<int64_t>(mappedFd) >= 0, "mapped memfd fixture"))
    {
        return false;
    }

    ok &= expect(syscall_call(SYSCALL_ftruncate, mappedFd, 3 * PAGE_SIZE) == 0, "size mapped memfd");

    const uint64_t first = syscall_call(SYSCALL_mmap, 0, 3 * PAGE_SIZE, 3, 1, mappedFd, 0);

    const uint64_t alias = syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 1, mappedFd, PAGE_SIZE);

    const uint64_t privateMap = syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 2, mappedFd, PAGE_SIZE);

    if (static_cast<int64_t>(first) < 0 || static_cast<int64_t>(alias) < 0 || static_cast<int64_t>(privateMap) < 0)
    {
        syscall_call(SYSCALL_close, mappedFd);

        return expect(false, "map memfd shared, offset alias and private view");
    }

    uint32_t word = 71, observed = 0;

    ok &= expect(safe_write_user(first + PAGE_SIZE, &word, sizeof(word)) && safe_read_user(alias, &observed, sizeof(observed)) && observed == word,
        "memfd aliases share canonical offset pages");

    ok &= expect(syscall_call(SYSCALL_pread64, mappedFd, base + 1024, sizeof(word), PAGE_SIZE) == sizeof(word), "memfd read sees mapped store");

    fixture_load(process, base + 1024, &observed, sizeof(observed));

    ok &= expect(observed == word, "mapped store reaches file I/O");

    word = 83;

    fixture_store(process, base + 512, &word, sizeof(word));

    ok &= expect(syscall_call(SYSCALL_pwrite64, mappedFd, base + 512, sizeof(word), PAGE_SIZE) == sizeof(word) && safe_read_user(alias, &observed, sizeof(observed)) &&
                     observed == word,
        "file write reaches mapped alias");

    word = 97;

    ok &= expect(safe_write_user(privateMap, &word, sizeof(word)) && safe_read_user(alias, &observed, sizeof(observed)) && observed == 83,
        "private file store detaches from shared page");

    ok &= expect(syscall_call(SYSCALL_madvise, privateMap, PAGE_SIZE, 4) == 0 && safe_read_user(privateMap, &observed, sizeof(observed)) && observed == 83,
        "private file DONTNEED reloads canonical data");

    ok &= expect(syscall_call(SYSCALL_ftruncate, mappedFd, PAGE_SIZE) == LINUX_EOPNOTSUPP && syscall_call(SYSCALL_fcntl, mappedFd, 1033, 8) == static_cast<uint64_t>(-16),
        "mapped truncate and conflicting write seal fail explicitly");

    ok &= expect(syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 1, mappedFd, 3 * PAGE_SIZE) == LINUX_EOPNOTSUPP, "memfd mapping beyond EOF rejected");

    ok &= expect(syscall_call(SYSCALL_madvise, first, PAGE_SIZE, 8) == LINUX_EINVAL, "mapped MADV_FREE remains unsupported");

    ok &= expect(syscall_call(SYSCALL_fcntl, mappedFd, 1033, 16) == 0, "future write seal permits existing shared writers");

    ok &= expect(syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 1, mappedFd, 0) == static_cast<uint64_t>(-1), "future write seal rejects new shared writable mapping");

    const uint64_t sealedRead = syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 1, 1, mappedFd, 0);

    ok &= expect(static_cast<int64_t>(sealedRead) >= 0 && syscall_call(SYSCALL_mprotect, sealedRead, PAGE_SIZE, 3) == static_cast<uint64_t>(-13),
        "sealed read mapping cannot gain WRITE");

    if (static_cast<int64_t>(sealedRead) >= 0)
    {
        syscall_call(SYSCALL_munmap, sealedRead, PAGE_SIZE);
    }

    auto* object = fixture_region(process, first)->object;

    vmm_retain_object(object);

    auto* clone = fixture_vmm()->CloneUserAddressSpace(process->addressSpace);

    ok &= expect(clone != nullptr, "fork mapped memfd");

    ok &= expect(syscall_call(SYSCALL_close, mappedFd) == 0 && syscall_call(SYSCALL_munmap, first + PAGE_SIZE, PAGE_SIZE) == 0, "close fd and partially unmap parent");

    ok &= expect(safe_read_user(alias, &observed, sizeof(observed)) && observed == 83, "offset alias survives descriptor close and partial unmap");

    if (clone != nullptr)
    {
        auto* childRegion = fixture_vmm()->FindRegion(clone, first + PAGE_SIZE);

        ok &= expect(childRegion != nullptr && childRegion->object == object && *static_cast<uint32_t*>(childRegion->backing) == 83,
            "fork retains shared memfd page after parent unmap");

        fixture_vmm()->DestroyUserAddressSpace(clone);
    }

    ok &= expect(syscall_call(SYSCALL_munmap, first, 3 * PAGE_SIZE) == 0 && syscall_call(SYSCALL_munmap, alias, PAGE_SIZE) == 0 &&
                     syscall_call(SYSCALL_munmap, privateMap, PAGE_SIZE) == 0,
        "release all surviving memfd views");

    ok &= expect(object->mappings == 0 && object->references == 1, "backing owner references drain after close, fork teardown and unmap");

    vmm_put_object(object);

    const uint64_t tailFd = syscall_call(SYSCALL_memfd_create, base + 128, 0);

    if (static_cast<int64_t>(tailFd) < 0)
    {
        return expect(false, "partial EOF memfd fixture");
    }

    ok &= expect(syscall_call(SYSCALL_ftruncate, tailFd, 17) == 0, "size partial EOF file");

    const uint64_t tailMap = syscall_call(SYSCALL_mmap, 0, PAGE_SIZE, 3, 1, tailFd, 0);

    if (static_cast<int64_t>(tailMap) >= 0)
    {
        word = 123;

        ok &= expect(safe_write_user(tailMap + 128, &word, sizeof(word)) && syscall_call(SYSCALL_ftruncate, tailFd, 256) == 0 &&
                         safe_read_user(tailMap + 128, &observed, sizeof(observed)) && observed == 0,
            "file extension does not expose mapped stores past old EOF");

        ok &= expect(safe_write_user(tailMap + 300, &word, sizeof(word)) && syscall_call(SYSCALL_pwrite64, tailFd, base + 512, sizeof(word), 500) == sizeof(word) &&
                         safe_read_user(tailMap + 300, &observed, sizeof(observed)) && observed == 0,
            "sparse write extension clears the mapped EOF gap");

        ok &= expect(syscall_call(SYSCALL_munmap, tailMap, PAGE_SIZE) == 0 && syscall_call(SYSCALL_ftruncate, tailFd, 16) == 0, "memfd shrink works after final unmap");
    }
    else
    {
        ok &= expect(false, "map partial EOF page");
    }

    syscall_call(SYSCALL_close, tailFd);

    return ok;
}

static bool test_vfs_metadata(process_t* process, uint64_t base, uint64_t number)
{
    const char path[] = "/test.txt", missing[] = "/missing-metadata", empty[] = "";

    fixture_store(process, base + 128, path, sizeof(path));

    fixture_store(process, base + 256, missing, sizeof(missing));

    fixture_store(process, base + 384, empty, sizeof(empty));

    const uint64_t disk = syscall_call(SYSCALL_open, base + 128, 0, 0);

    bool ownership = number == SYSCALL_fchownat || number == SYSCALL_chown || number == SYSCALL_fchown || number == SYSCALL_lchown;
    bool direct    = number == SYSCALL_fchown || number == SYSCALL_fchmod;
    auto call      = [&](uint64_t pointer, uint64_t fd, uint64_t flags)
    {
        if (direct)
        {
            return ownership ? syscall_call(number, fd, 23, 24) : syscall_call(number, fd, 0600);
        }

        if (number == SYSCALL_fchownat)
        {
            return syscall_call(number, fd, pointer, 23, 24, flags);
        }

        if (number == SYSCALL_fchmodat || number == SYSCALL_fchmodat2)
        {
            return syscall_call(number, fd, pointer, 0600, flags);
        }

        return ownership ? syscall_call(number, pointer, 23, 24) : syscall_call(number, pointer, 0600);
    };
    bool ok = expect((int64_t) disk >= 0 && call(base + 128, direct ? disk : (uint64_t) -100, 0) == (uint64_t) -30, "metadata changes honor readonly cpio backend");

    ok &= expect(call(direct ? 0 : base + 256, direct ? UINT32_MAX : (uint64_t) -100, 0) == (direct ? LINUX_EBADF : LINUX_ENOENT), "metadata target resolution precedes mutation");

    if (!direct)
    {
        ok &= expect(call(CANONICAL_KERNEL_BASE, (uint64_t) -100, 0) == LINUX_EFAULT, "metadata syscall rejects kernel path pointer");
    }

    if ((int64_t) disk >= 0)
    {
        syscall_call(SYSCALL_close, disk);
    }

    const uint64_t anonymous = syscall_call(SYSCALL_memfd_create, base + 384, 0);

    ok &= expect((int64_t) anonymous >= 0 && syscall_call(SYSCALL_fchmod, anonymous, 0640) == 0 && syscall_call(SYSCALL_fchown, anonymous, 31, 32) == 0,
        "anonymous inode metadata is mutable");

    if ((int64_t) anonymous >= 0)
    {
        ok &= expect(syscall_call(SYSCALL_fchmodat2, anonymous, base + 384, 0600, 0x1000) == 0 && syscall_call(SYSCALL_fchownat, anonymous, 0, UINT32_MAX, UINT32_MAX, 0x1000) == 0,
            "metadata AT_EMPTY_PATH including null path");

        ok &= expect(syscall_call(SYSCALL_fstat, anonymous, base + 512) == 0, "metadata fstat snapshot");

        // Linux x86-64 stat mode/uid/gid fields begin at byte 24.
        uint32_t metadata[3] = {};

        fixture_load(process, base + 512 + 24, metadata, sizeof(metadata));

        ok &= expect((metadata[0] & 07777) == 0600 && metadata[1] == 31 && metadata[2] == 32, "fstat reports committed mode and ownership");

        const auto credentials = process->credentials;

        process->credentials.user.filesystem = 99;

        ok &= expect(syscall_call(SYSCALL_fchmod, anonymous, 0777) == (uint64_t) -1 && syscall_call(SYSCALL_fchown, anonymous, 99, 99) == (uint64_t) -1,
            "metadata rejects unprivileged nonowner");

        process->credentials = credentials;

        syscall_call(SYSCALL_close, anonymous);
    }

    return ok;
}

struct mount_test_backend_t;

struct mount_test_node_t
{
    inode_t               inode;
    mount_test_backend_t* owner;
    inode_t*              parent;
    inode_t*              alias;
    char                  name[32];
    char                  bytes[64];
    bool                  present;
};

struct mount_test_backend_t
{
    mount_test_node_t root;
    mount_test_node_t nodes[8];
};

static unsigned mount_test_releases = 0;

static int64_t mount_test_metadata(inode_t*, uint32_t mode, uint32_t, uint32_t)
{
    return mode == 0777 ? -5 : 0;
}

static mount_test_node_t* mount_test_find(inode_t* directory, const char* name)
{
    auto state = static_cast<mount_test_node_t*>(directory->privateData)->owner;

    for (auto& node : state->nodes)
    {
        if (node.present && node.parent == directory && strcmp(node.name, name) == 0)
        {
            return &node;
        }
    }

    return nullptr;
}

static inode_t* mount_test_lookup(inode_t* directory, const char* name)
{
    auto node = mount_test_find(directory, name);

    return node != nullptr ? (node->alias != nullptr ? node->alias : &node->inode) : nullptr;
}

extern const inode_operations_t mount_test_inode_operations;
extern const file_operations_t  mount_test_file_operations;

static int64_t mount_test_create(inode_t* directory, const char* name, uint32_t mode, inode_type_t type)
{
    auto state = static_cast<mount_test_node_t*>(directory->privateData)->owner;

    if (strlen(name) >= 32)
    {
        return -36;
    }

    if (mount_test_find(directory, name) != nullptr)
    {
        return -17;
    }

    for (auto& node : state->nodes)
    {
        if (node.inode.inodeNumber != 0)
        {
            continue;
        }

        node         = {};
        node.present = true;
        node.owner   = state;
        node.parent  = directory;

        memcpy(node.name, name, strlen(name) + 1);

        node.inode.inodeNumber   = &node - state->nodes + 2;
        node.inode.type          = type;
        node.inode.mode          = mode & 07777;
        node.inode.metadataValid = true;
        node.inode.linkCount     = 1;
        node.inode.inodeOps      = &mount_test_inode_operations;
        node.inode.fileOps       = &mount_test_file_operations;
        node.inode.privateData   = &node;
        node.inode.filesystem    = directory->filesystem;

        return 0;
    }

    return -28;
}

static int64_t mount_test_mkdir(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes)
{
    const int64_t result = mount_test_create(directory, name, attributes->mode, INODE_DIRECTORY);

    if (result == 0)
    {
        auto inode = mount_test_lookup(directory, name);

        inode->uid = attributes->uid;
        inode->gid = attributes->gid;
    }

    return result;
}

static int64_t mount_test_mknod(inode_t* directory, const char* name, const inode_creation_attributes_t* attributes, uint64_t)
{
    const int64_t result = mount_test_create(directory, name, attributes->mode, INODE_REGULAR);

    if (result == 0)
    {
        auto inode = mount_test_lookup(directory, name);

        inode->uid = attributes->uid;
        inode->gid = attributes->gid;
    }

    return result;
}

static int64_t mount_test_unlink(inode_t* directory, const char* name, bool directoryEntry)
{
    auto node = mount_test_find(directory, name);

    if (node == nullptr)
    {
        return -2;
    }

    if ((node->inode.type == INODE_DIRECTORY) != directoryEntry)
    {
        return directoryEntry ? -20 : -21;
    }

    node->present = false;

    inode_t* inode = node->alias != nullptr ? node->alias : &node->inode;

    --inode->linkCount;

    return 0;
}

static int64_t mount_test_rename(inode_t* directory, const char* name, inode_t* destination, const char* newName)
{
    auto node = mount_test_find(directory, name);

    if (node == nullptr)
    {
        return -2;
    }

    if (strlen(newName) >= 32 || mount_test_find(destination, newName) != nullptr)
    {
        return -17;
    }

    node->parent = destination;

    memcpy(node->name, newName, strlen(newName) + 1);

    return 0;
}

static int64_t mount_test_link(inode_t* directory, const char* name, inode_t* inode)
{
    const int64_t result = mount_test_create(directory, name, inode->mode, inode->type);

    if (result == 0)
    {
        mount_test_find(directory, name)->alias = inode;

        ++inode->linkCount;
    }

    return result;
}

static int64_t mount_test_symlink(inode_t* directory, const char* name, const char* target, const inode_creation_attributes_t* attributes)
{
    if (strlen(target) >= 64)
    {
        return -36;
    }

    const int64_t result = mount_test_create(directory, name, 0777, INODE_SYMLINK);

    if (result == 0)
    {
        auto node = mount_test_find(directory, name);

        memcpy(node->bytes, target, strlen(target) + 1);

        node->inode.size = strlen(target);
        node->inode.uid  = attributes->uid;
        node->inode.gid  = attributes->gid;
    }

    return result;
}

static int64_t mount_test_truncate(inode_t* inode, uint64_t size)
{
    if (size > 64)
    {
        return -27;
    }

    auto node = static_cast<mount_test_node_t*>(inode->privateData);

    if (size < inode->size)
    {
        memset(node->bytes + size, 0, 64 - size);
    }

    return 0;
}

static int64_t mount_test_read(file_t* file, void* output, uint64_t count)
{
    auto         node  = static_cast<mount_test_node_t*>(file->inode->privateData);
    const size_t bytes = file->offset < file->inode->size ? (count < file->inode->size - file->offset ? count : file->inode->size - file->offset) : 0;

    memcpy(output, node->bytes + file->offset, bytes);

    file->offset += bytes;

    return (int64_t) bytes;
}

static int64_t mount_test_write(file_t* file, const void* input, uint64_t count)
{
    auto         node  = static_cast<mount_test_node_t*>(file->inode->privateData);
    const size_t bytes = file->offset < 64 ? (count < 64 - file->offset ? count : 64 - file->offset) : 0;

    memcpy(node->bytes + file->offset, input, bytes);

    file->offset += bytes;

    if (file->offset > file->inode->size)
    {
        file->inode->size = file->offset;
    }

    return (int64_t) bytes;
}

const inode_operations_t mount_test_inode_operations = {mount_test_lookup, mount_test_metadata, mount_test_mkdir, mount_test_unlink, mount_test_rename,
    mount_test_link, mount_test_mknod, mount_test_symlink, mount_test_truncate};
const file_operations_t  mount_test_file_operations  = {nullptr, mount_test_read, mount_test_write};

static int64_t mount_test_mount(filesystem_t* fs, const filesystem_mount_request_t* request)
{
    auto state = static_cast<mount_test_backend_t*>(kzalloc(sizeof(mount_test_backend_t)));

    if (state == nullptr)
    {
        return -12;
    }

    fs->privateData                 = state;
    state->root.owner               = state;
    state->root.inode.type          = INODE_DIRECTORY;
    state->root.inode.mode          = 0755;
    state->root.inode.metadataValid = true;
    state->root.inode.linkCount     = 2;
    state->root.inode.inodeOps      = &mount_test_inode_operations;
    state->root.inode.privateData   = &state->root;
    state->root.inode.filesystem    = fs;
    fs->rootInode                   = &state->root.inode;

    mount_test_create(fs->rootInode, "data", 0644, INODE_REGULAR);

    return request->source != nullptr && strcmp(request->source, "fail") == 0 ? -28 : request->options != nullptr && strcmp(request->options, "test=1") != 0 ? -22
                                                                                                                                                             : 0;
}

static void mount_test_unmount(filesystem_t* fs)
{
    ++mount_test_releases;

    kfree(fs->privateData);
}

static int64_t mount_test_remount(filesystem_t*, uint64_t, const char* options)
{
    return options != nullptr && strcmp(options, "reject") == 0 ? -22 : 0;
}

static const filesystem_type_t mount_test_type = {"arx-test-backend", mount_test_mount, mount_test_unmount, mount_test_remount};

static bool test_vfs_backend_syscalls(process_t* process, uint64_t base)
{
    auto    dispatcher = static_cast<Dispatcher*>(platform.dispacher);
    auto    resources  = dispatcher->GetResourceLayerCaps();
    inode_t directory  = {};

    directory.type = INODE_DIRECTORY;

    dentry_t root = {}, points[3] = {};

    root.name  = "/";
    root.inode = &directory;

    mount_t rootMount = {};

    rootMount.root = &root;

    const unsigned releases = mount_test_releases;
    bool           ok       = true;
    {
        VirtualFileSystem local(resources);
        LogicLayerCaps    logic = *dispatcher->GetLogicLayerCaps();

        logic.virtualFileSystem = &local;

        VfsRequestManager manager(resources, &logic);

        ok &= expect(local.SetRootMount(&rootMount) && local.RegisterFileSystemType(&mount_test_type) && !local.RegisterFileSystemType(&mount_test_type),
            "filesystem registration rejects duplicate names");

        const char* names[] = {"one", "two", "three"};

        for (size_t i = 0; i < 3; ++i)
        {
            points[i].name   = names[i];
            points[i].parent = &root;
            points[i].inode  = &directory;

            ok &= local.CacheDentry(&points[i]);
        }

        auto store = [&](uint64_t offset, const char* text)
        {
            fixture_store(process, base + offset, text, strlen(text) + 1);

            return base + offset;
        };
        auto mount = [&](const char* source, const char* target, uint64_t flags, const char* options = nullptr)
        {
            arch_syscall_frame_t frame = {};

            frame.arg0 = source != nullptr ? store(128, source) : 0;
            frame.arg1 = store(256, target);
            frame.arg2 = store(384, mount_test_type.name);
            frame.arg3 = flags;
            frame.arg4 = options != nullptr ? store(512, options) : 0;

            return manager.HandleMountRequest(&frame);
        };
        ok &= expect(mount("fail", "/one", 0) == (uint64_t) -28 && mount_test_releases == releases + 1, "failed backend mount releases private state");

        ok &= expect(mount("device", "/one", 0, "test=1") == 0, "mount registered backend with copied source and options");

        arch_syscall_frame_t metadata = {};

        metadata.arg0 = store(256, "/one/data");
        metadata.arg1 = 0600;

        ok &= expect(manager.HandleChmodRequest(&metadata) == 0, "chmod dispatches to mounted inode callback");

        vfs_path_t resolved = {};

        ok &= expect(local.ResolvePath({}, "/one/data", &resolved) && resolved.dentry->inode->mode == 0600, "mounted metadata change is committed");

        metadata.arg1 = 0777;

        ok &= expect(manager.HandleChmodRequest(&metadata) == (uint64_t) -5 && resolved.dentry->inode->mode == 0600, "backend metadata failure preserves cached attributes");

        EventManager notifications(resources, logic.scheduler, &local);

        local.SetEventManager(&notifications);

        const uint64_t notify = notifications.CreateInotify(04000);
        const uint64_t watch  = notifications.AddWatch((int32_t) notify, store(256, "/one"), 0xc0);

        ok &= expect((int64_t) notify >= 0 && (int64_t) watch > 0, "watch registered filesystem directory");

        auto call = [&](auto handler, uint64_t first, uint64_t second = 0, uint64_t third = 0)
        {
            arch_syscall_frame_t frame = {};

            frame.arg0 = first;
            frame.arg1 = second;
            frame.arg2 = third;

            return (manager.*handler)(&frame);
        };
        ok &= expect(call(&VfsRequestManager::HandleMkdirRequest, store(256, "/one/newdir"), 0750) == 0 && local.ResolvePath({}, "/one/newdir", &resolved) &&
                         resolved.dentry->inode->type == INODE_DIRECTORY,
            "mkdir dispatches to mounted directory callback");

        ok &= expect(call(&VfsRequestManager::HandleMknodRequest, store(256, "/one/newfile"), 0100600) == 0 && local.ResolvePath({}, "/one/newfile", &resolved),
            "mknod publishes backend node through lookup");

        const uint64_t nativeFile = call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/newfile"), 2);

        ok &= expect((int64_t) nativeFile >= 0 && call(&VfsRequestManager::HandleWriteRequest, nativeFile, store(512, "contents"), 8) == 8 &&
                         call(&VfsRequestManager::HandleFtruncateRequest, nativeFile, 3) == 0,
            "mounted file I/O and inode truncate callback");

        if ((int64_t) nativeFile >= 0)
        {
            auto file = static_cast<file_t*>(ProcessManager::GetFileDescriptor(process, nativeFile)->file);

            ok &= expect(file->offset == 8 && file->inode->size == 3, "ftruncate preserves file offset while shrinking backend inode");

            ok &= expect(call(&VfsRequestManager::HandleFcntlRequest, nativeFile, 4, 02000) == 0 &&
                             call(&VfsRequestManager::HandleWriteRequest, nativeFile, store(512, "z"), 1) == 1 && file->offset == 4 && file->inode->size == 4 &&
                             (file->statusFlags & 3) == 2,
                "native append uses inode size and F_SETFL preserves access mode");
        }

        const uint64_t inodeWatch = notifications.AddWatch((int32_t) notify, store(256, "/one/newfile"), 4);

        ok &= expect(call(&VfsRequestManager::HandleLinkRequest, store(128, "/one/newfile"), store(256, "/one/alias")) == 0, "hard link dispatches by source inode");

        ok &=
            expect((int64_t) inodeWatch > 0 && call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16, "hard link change emits watched inode attribute event");

        ok &= expect(call(&VfsRequestManager::HandleRenameRequest, store(128, "/one/newfile"), store(256, "/one/renamed")) == 0 &&
                         !local.ResolvePath({}, "/one/newfile", &resolved) && local.ResolvePath({}, "/one/renamed", &resolved),
            "rename invalidates cached old name and publishes destination");

        ok &= expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 64, "backend rename emits paired directory records");

        uint32_t moves[16] = {};

        fixture_load(process, base + 1024, moves, sizeof(moves));

        ok &= expect(moves[1] == 0x40 && moves[9] == 0x80 && moves[2] != 0 && moves[2] == moves[10], "inotify rename pair carries matching nonzero cookie");

        ok &= expect(call(&VfsRequestManager::HandleSymlinkRequest, store(128, "data"), store(256, "/one/symlink")) == 0 &&
                         call(&VfsRequestManager::HandleReadlinkRequest, store(256, "/one/symlink"), base + 1024, 64) == 4,
            "mounted backend symlink and readlink callbacks");

        ok &= expect(call(&VfsRequestManager::HandleUnlinkRequest, store(256, "/one/renamed")) == 0 && !local.ResolvePath({}, "/one/renamed", &resolved) &&
                         local.ResolvePath({}, "/one/alias", &resolved),
            "unlink invalidates name while preserving backend hard link");

        ok &= expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16, "unlink with surviving hard link preserves inode watch");

        ok &= expect(call(&VfsRequestManager::HandleUnlinkRequest, store(256, "/one/alias")) == 0 && call(&VfsRequestManager::HandleRmdirRequest, store(256, "/one/newdir")) == 0,
            "mounted unlink and rmdir dispatch");

        ok &= expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16, "unlinked open inode keeps watch until last close");

        if ((int64_t) nativeFile >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, nativeFile);
        }

        ok &= expect(call(&VfsRequestManager::HandleReadRequest, notify, base + 1024, 512) == 16 && notifications.RemoveWatch((int32_t) notify, (int32_t) inodeWatch) == LINUX_EINVAL,
            "last close of unlinked inode removes watch");

        uint32_t removed[4] = {};

        fixture_load(process, base + 1024, removed, sizeof(removed));

        ok &= expect(removed[0] == inodeWatch && removed[1] == 0x8000, "inode removal queues IN_IGNORED even when DELETE_SELF was not requested");

        char savedCwd[ProcessManager::MAX_CWD_PATH_LENGTH];

        memcpy(savedCwd, process->Fs()->cwdPath, sizeof(savedCwd));

        ok &= expect(call(&VfsRequestManager::HandleChdirRequest, store(256, "/one")) == 0 && strcmp(process->Fs()->cwdPath, "/one") == 0,
            "chdir retains mountpoint in working directory path");

        const uint64_t created = call(&VfsRequestManager::HandleOpenRequest, store(256, "via-open"), 2 | 0100 | 0200 | 02000000, 0640);

        ok &= expect((int64_t) created >= 0 && call(&VfsRequestManager::HandleOpenRequest, store(256, "via-open"), 2 | 0100 | 0200, 0640) == (uint64_t) -17,
            "native open creates relative file and honors O_EXCL");

        if ((int64_t) created >= 0)
        {
            ok &= expect(call(&VfsRequestManager::HandleFcntlRequest, created, 1) == 1, "open O_CLOEXEC sets descriptor flag");

            call(&VfsRequestManager::HandleCloseRequest, created);
        }

        const uint64_t truncated = call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/data"), 2 | 01000);

        ok &= expect((int64_t) truncated >= 0 && local.ResolvePath({}, "/one/data", &resolved) && resolved.dentry->inode->size == 0, "native O_TRUNC dispatches inode truncation");

        if ((int64_t) truncated >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, truncated);
        }

        vfs_path_t mountedRoot = {};

        local.ResolvePath({}, "/one", &mountedRoot);

        ok &= expect(local.ChangeMetadata(mountedRoot.dentry->inode, false, 02777, 0, mountedRoot.mount, mountedRoot.dentry) == 0 &&
                         local.ChangeMetadata(mountedRoot.dentry->inode, true, UINT32_MAX, 42, mountedRoot.mount, mountedRoot.dentry) == 0,
            "prepare setgid parent for backend creation attributes");

        const auto     savedCredentials = process->credentials;
        const uint32_t savedUmask       = process->Fs()->umask;

        process->credentials.user.filesystem  = 99;
        process->credentials.group.filesystem = 77;

        process->Fs()->umask = 0077;

        const uint64_t ownedFile = call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/owned"), 2 | 0100 | 0200, 0);

        ok &= expect((int64_t) ownedFile >= 0 && local.ResolvePath({}, "/one/owned", &resolved) && resolved.dentry->inode->uid == 99 && resolved.dentry->inode->gid == 42 &&
                         resolved.dentry->inode->mode == 0,
            "backend receives creator uid and inherited gid; creator may open mode-zero file");

        if ((int64_t) ownedFile >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, ownedFile);
        }

        ok &= expect(call(&VfsRequestManager::HandleMkdirRequest, store(256, "/one/owned-dir"), 0777) == 0 && local.ResolvePath({}, "/one/owned-dir", &resolved) &&
                         resolved.dentry->inode->uid == 99 && resolved.dentry->inode->gid == 42 && resolved.dentry->inode->mode == 02700,
            "backend directory attributes include umask and inherited setgid");

        process->credentials = savedCredentials;

        process->Fs()->umask = savedUmask;

        local.ChangeMetadata(mountedRoot.dentry->inode, false, 0755, 0, mountedRoot.mount, mountedRoot.dentry);

        local.ChangeMetadata(mountedRoot.dentry->inode, true, UINT32_MAX, 0, mountedRoot.mount, mountedRoot.dentry);

        process->credentials.user.filesystem = 99;

        ok &= expect(call(&VfsRequestManager::HandleOpenRequest, store(256, "/one/via-open"), 2) == LINUX_EACCES &&
                         call(&VfsRequestManager::HandleMkdirRequest, store(256, "/one/denied"), 0755) == LINUX_EACCES,
            "native open and directory mutations enforce filesystem credentials");

        process->credentials = savedCredentials;

        memcpy(process->Fs()->cwdPath, savedCwd, sizeof(savedCwd));

        if ((int64_t) notify >= 0)
        {
            call(&VfsRequestManager::HandleCloseRequest, notify);
        }

        ok &= expect(mount("/one", "/two", 4096) == 0, "bind mounts retain the original backend");

        // A bind remount does not require a source or filesystem name.
        arch_syscall_frame_t remount = {};

        remount.arg1 = store(256, "/two");
        remount.arg3 = 4096 | 32 | 1;

        ok &= expect(manager.HandleMountRequest(&remount) == 0, "bind remount installs per-mount readonly flag");

        metadata.arg0 = store(256, "/two/data");
        metadata.arg1 = 0600;

        ok &= expect(manager.HandleChmodRequest(&metadata) == (uint64_t) -30, "readonly bind rejects metadata mutation");

        ok &= expect(call(&VfsRequestManager::HandleOpenRequest, store(256, "/two/data"), 1) == (uint64_t) -30 &&
                         call(&VfsRequestManager::HandleLinkRequest, store(128, "/one/data"), store(256, "/two/cross")) == (uint64_t) -18,
            "readonly open and cross-mount link return native errno");

        metadata.arg0 = store(256, "/one/data");

        ok &= expect(manager.HandleChmodRequest(&metadata) == 0, "readonly bind leaves original filesystem writable");

        ok &= expect(mount("/one", "/three", 8192) == 0 && local.ResolvePath({}, "/three/data", &resolved) && !local.ResolvePath({}, "/one/data", &resolved),
            "move mount changes namespace attachment");

        ok &=
            expect(mount(nullptr, "/three", 32 | 1, "reject") == (uint64_t) -22 && mount(nullptr, "/three", 32 | 1) == 0, "filesystem remount calls backend and propagates failure");

        metadata.arg0 = store(256, "/three/data");

        ok &= expect(manager.HandleChmodRequest(&metadata) == (uint64_t) -30, "readonly filesystem remount rejects mutation");

        local.SetEventManager(nullptr);
    }

    ok &= expect(mount_test_releases == releases + 2, "VFS teardown releases mounted backend once despite bind alias");

    return ok;
}

static bool test_vfs_mount_framework(process_t* process, uint64_t base)
{
    const char target[] = "/", unknown[] = "arx-unknown", type[] = "cpio";

    fixture_store(process, base + 128, target, sizeof(target));

    fixture_store(process, base + 256, unknown, sizeof(unknown));

    fixture_store(process, base + 384, type, sizeof(type));

    bool ok = expect(syscall_call(SYSCALL_mount, 0, base + 128, base + 256, 0) == (uint64_t) -19, "mount unknown filesystem ENODEV");

    ok &= expect(syscall_call(SYSCALL_mount, 0, base + 128, base + 384, 0) == LINUX_EOPNOTSUPP && syscall_call(SYSCALL_mount, 0, base + 128, 0, 4096) == LINUX_EINVAL,
        "mount boot cpio requires an archive and bind requires a source");

    ok &= expect(syscall_call(SYSCALL_mount, 0, CANONICAL_KERNEL_BASE, base + 384, 0) == LINUX_EFAULT &&
                     syscall_call(SYSCALL_mount, 0, base + 128, base + 384, 1ULL << 40) == LINUX_EINVAL,
        "mount pointer and flag validation");

    const auto credentials = process->credentials;

    process->credentials.user.effective = 99;

    ok &= expect(syscall_call(SYSCALL_mount, 0, 0, 0, 0) == (uint64_t) -1, "mount requires privilege before inspecting user arguments");

    process->credentials = credentials;

    ok &= test_vfs_backend_syscalls(process, base);

    return ok;
}

template <uint64_t Number>
static bool validate_vfs_extensions(const syscall_case_t*)
{
    return with_process([](process_t* process)
    {
        process->credentials = {};

        const uint64_t base = fixture_map(8);

        if ((int64_t) base < 0)
        {
            return false;
        }

        bool ok = Number == SYSCALL_pipe || Number == SYSCALL_pipe2 ? test_ipc_pipe(process, base, Number)
                  : Number == SYSCALL_memfd_create                  ? test_ipc_memory_file(process, base)
                  : Number == SYSCALL_mount                         ? test_vfs_mount_framework(process, base)
                                                                    : test_vfs_metadata(process, base, Number);

        ok &= expect(descriptor_count(process) == 0, "VFS extension fixture releases every descriptor");

        return ok;
    });
}

void test_vfs_manager(RequestLayerCaps* caps)
{
    static const syscall_test_t<VfsRequestManager> tests[] = {
        {"HandleOpenatRequest", &VfsRequestManager::HandleOpenatRequest, SYSCALL_openat, isolated<validate_openat>},
        {"HandleOpenRequest", &VfsRequestManager::HandleOpenRequest, SYSCALL_open, isolated<validate_open>},
        {"HandleCreatRequest", &VfsRequestManager::HandleCreatRequest, SYSCALL_creat, isolated<validate_creat>},
        {"HandleCloseRequest", &VfsRequestManager::HandleCloseRequest, SYSCALL_close, isolated<validate_close>},
        {"HandleClose_rangeRequest", &VfsRequestManager::HandleClose_rangeRequest, SYSCALL_close_range, isolated<validate_close_range>},
        {"HandleMkdiratRequest", &VfsRequestManager::HandleMkdiratRequest, SYSCALL_mkdirat, isolated<validate_mkdirat>},
        {"HandleMkdirRequest", &VfsRequestManager::HandleMkdirRequest, SYSCALL_mkdir, isolated<validate_mkdir>},
        {"HandleReadRequest", &VfsRequestManager::HandleReadRequest, SYSCALL_read, isolated<validate_vfs_rw>},
        {"HandleWriteRequest", &VfsRequestManager::HandleWriteRequest, SYSCALL_write, isolated<validate_vfs_rw>},
        {"HandlePread64Request", &VfsRequestManager::HandlePread64Request, SYSCALL_pread64, isolated<validate_pread64>},
        {"HandlePwrite64Request", &VfsRequestManager::HandlePwrite64Request, SYSCALL_pwrite64, isolated<validate_pwrite64>},
        {"HandleReadvRequest", &VfsRequestManager::HandleReadvRequest, SYSCALL_readv, isolated<validate_readv>},
        {"HandleWritevRequest", &VfsRequestManager::HandleWritevRequest, SYSCALL_writev, isolated<validate_writev>},
        {"HandlePreadvRequest", &VfsRequestManager::HandlePreadvRequest, SYSCALL_preadv, isolated<validate_preadv>},
        {"HandlePwritevRequest", &VfsRequestManager::HandlePwritevRequest, SYSCALL_pwritev, isolated<validate_pwritev>},
        {"HandleLseekRequest", &VfsRequestManager::HandleLseekRequest, SYSCALL_lseek, isolated<validate_lseek>},
        {"HandleGetcwdRequest", &VfsRequestManager::HandleGetcwdRequest, SYSCALL_getcwd, isolated<validate_getcwd>},
        {"HandleChdirRequest", &VfsRequestManager::HandleChdirRequest, SYSCALL_chdir, isolated<validate_chdir>},
        {"HandleFchdirRequest", &VfsRequestManager::HandleFchdirRequest, SYSCALL_fchdir, isolated<validate_fchdir>},
        {"HandleGetdents64Request", &VfsRequestManager::HandleGetdents64Request, SYSCALL_getdents64, isolated<validate_getdents64>},
        {"HandleUnlinkatRequest", &VfsRequestManager::HandleUnlinkatRequest, SYSCALL_unlinkat, isolated<validate_unlinkat>},
        {"HandleUnlinkRequest", &VfsRequestManager::HandleUnlinkRequest, SYSCALL_unlink, isolated<validate_unlink>},
        {"HandleRmdirRequest", &VfsRequestManager::HandleRmdirRequest, SYSCALL_rmdir, isolated<validate_rmdir>},
        {"HandleFcntlRequest", &VfsRequestManager::HandleFcntlRequest, SYSCALL_fcntl, isolated<validate_fcntl>},
        {"HandleDupRequest", &VfsRequestManager::HandleDupRequest, SYSCALL_dup, isolated<validate_dup>},
        {"HandleDup2Request", &VfsRequestManager::HandleDup2Request, SYSCALL_dup2, isolated<validate_dup2>},
        {"HandleDup3Request", &VfsRequestManager::HandleDup3Request, SYSCALL_dup3, isolated<validate_dup3>},
        {"HandleNewfstatatRequest", &VfsRequestManager::HandleNewfstatatRequest, SYSCALL_newfstatat, isolated<validate_newfstatat>},
        {"HandleStatRequest", &VfsRequestManager::HandleStatRequest, SYSCALL_stat, isolated<validate_stat>},
        {"HandleFstatRequest", &VfsRequestManager::HandleFstatRequest, SYSCALL_fstat, isolated<validate_fstat>},
        {"HandleLstatRequest", &VfsRequestManager::HandleLstatRequest, SYSCALL_lstat, isolated<validate_lstat>},
        {"HandleRenameatRequest", &VfsRequestManager::HandleRenameatRequest, SYSCALL_renameat, isolated<validate_renameat>},
        {"HandleRenameRequest", &VfsRequestManager::HandleRenameRequest, SYSCALL_rename, isolated<validate_rename>},
        {"HandleReadlinkatRequest", &VfsRequestManager::HandleReadlinkatRequest, SYSCALL_readlinkat, isolated<validate_readlinkat>},
        {"HandleReadlinkRequest", &VfsRequestManager::HandleReadlinkRequest, SYSCALL_readlink, isolated<validate_readlink>},
        {"HandleIoctlRequest", &VfsRequestManager::HandleIoctlRequest, SYSCALL_ioctl, isolated<validate_ioctl>},
        {"HandleLinkatRequest", &VfsRequestManager::HandleLinkatRequest, SYSCALL_linkat, isolated<validate_linkat>},
        {"HandleLinkRequest", &VfsRequestManager::HandleLinkRequest, SYSCALL_link, isolated<validate_link>},
        {"HandleSymlinkatRequest", &VfsRequestManager::HandleSymlinkatRequest, SYSCALL_symlinkat, isolated<validate_symlinkat>},
        {"HandleSymlinkRequest", &VfsRequestManager::HandleSymlinkRequest, SYSCALL_symlink, isolated<validate_symlink>},
        {"HandleFaccessatRequest", &VfsRequestManager::HandleFaccessatRequest, SYSCALL_faccessat, isolated<validate_access_family>},
        {"HandleFaccessat2Request", &VfsRequestManager::HandleFaccessat2Request, SYSCALL_faccessat2, isolated<validate_access_family>},
        {"HandleAccessRequest", &VfsRequestManager::HandleAccessRequest, SYSCALL_access, isolated<validate_access_family>},
        {"HandleFchownatRequest", &VfsRequestManager::HandleFchownatRequest, SYSCALL_fchownat, validate_vfs_extensions<SYSCALL_fchownat>},
        {"HandleChownRequest", &VfsRequestManager::HandleChownRequest, SYSCALL_chown, validate_vfs_extensions<SYSCALL_chown>},
        {"HandleFchownRequest", &VfsRequestManager::HandleFchownRequest, SYSCALL_fchown, validate_vfs_extensions<SYSCALL_fchown>},
        {"HandleLchownRequest", &VfsRequestManager::HandleLchownRequest, SYSCALL_lchown, validate_vfs_extensions<SYSCALL_lchown>},
        {"HandleFchmodat2Request", &VfsRequestManager::HandleFchmodat2Request, SYSCALL_fchmodat2, validate_vfs_extensions<SYSCALL_fchmodat2>},
        {"HandleFchmodatRequest", &VfsRequestManager::HandleFchmodatRequest, SYSCALL_fchmodat, validate_vfs_extensions<SYSCALL_fchmodat>},
        {"HandleChmodRequest", &VfsRequestManager::HandleChmodRequest, SYSCALL_chmod, validate_vfs_extensions<SYSCALL_chmod>},
        {"HandleFchmodRequest", &VfsRequestManager::HandleFchmodRequest, SYSCALL_fchmod, validate_vfs_extensions<SYSCALL_fchmod>},
        {"HandleMountRequest", &VfsRequestManager::HandleMountRequest, SYSCALL_mount, validate_vfs_extensions<SYSCALL_mount>},
        {"HandleStatfsRequest", &VfsRequestManager::HandleStatfsRequest, SYSCALL_statfs, isolated<validate_statfs>},
        {"HandleFstatfsRequest", &VfsRequestManager::HandleFstatfsRequest, SYSCALL_fstatfs, isolated<validate_fstatfs>},
        {"HandleMknodatRequest", &VfsRequestManager::HandleMknodatRequest, SYSCALL_mknodat, isolated<validate_mknodat>},
        {"HandleMknodRequest", &VfsRequestManager::HandleMknodRequest, SYSCALL_mknod, isolated<validate_mknod>},
        {"HandleTruncateRequest", &VfsRequestManager::HandleTruncateRequest, SYSCALL_truncate, isolated<validate_truncate>},
        {"HandleFtruncateRequest", &VfsRequestManager::HandleFtruncateRequest, SYSCALL_ftruncate, isolated<validate_ftruncate>},
        {"HandlePipeRequest", &VfsRequestManager::HandlePipeRequest, SYSCALL_pipe, validate_vfs_extensions<SYSCALL_pipe>},
        {"HandlePipe2Request", &VfsRequestManager::HandlePipe2Request, SYSCALL_pipe2, validate_vfs_extensions<SYSCALL_pipe2>},
        {"HandleMemfd_createRequest", &VfsRequestManager::HandleMemfd_createRequest, SYSCALL_memfd_create, validate_vfs_extensions<SYSCALL_memfd_create>},
    };

    VfsRequestManager unavailable(nullptr, nullptr);

    for (const auto& test : tests)
    {
        arch_syscall_frame_t frame = {};

        if ((unavailable.*test.method)(&frame) != LINUX_ENOSYS || (caps->vfsRequestManager != nullptr && (caps->vfsRequestManager->*test.method)(nullptr) != LINUX_EINVAL))
        {
            expect(false, "VFS missing dependency/null-frame validation");
        }
    }

    run_syscalls(caps->vfsRequestManager, tests);
}

} // namespace request_selftest
