#pragma once

#include "layers/Request/CredentialRequestManager.hpp"
#include "layers/Request/EventRequestManager.hpp"
#include "layers/Request/InterruptRequestManager.hpp"
#include "layers/Request/MemoryRequestManager.hpp"
#include "layers/Request/ProcessRequestManager.hpp"
#include "layers/Request/SchedulerRequestManager.hpp"
#include "layers/Request/SignalRequestManager.hpp"
#include "layers/Request/SocketRequestManager.hpp"
#include "layers/Request/SyncRequestManager.hpp"
#include "layers/Request/SystemRequestManager.hpp"
#include "layers/Request/TimeRequestManager.hpp"
#include "layers/Request/VfsRequestManager.hpp"

#include <stdint.h>

struct ResourceLayerCaps;
struct LogicLayerCaps;

// Shared Linux ABI-style syscall return codes used by request managers.
constexpr uint64_t LINUX_ESRCH  = (uint64_t) -3;
constexpr uint64_t LINUX_EIO    = (uint64_t) -5;
constexpr uint64_t LINUX_EBADF  = (uint64_t) -9;
constexpr uint64_t LINUX_EFAULT = (uint64_t) -14;
constexpr uint64_t LINUX_EINVAL = (uint64_t) -22;
constexpr uint64_t LINUX_ENOSYS = (uint64_t) -38;
constexpr int64_t  LINUX_AT_FDCWD = -100;

enum : uint64_t
{
    SYSCALL_read              = 0,
    SYSCALL_write             = 1,
    SYSCALL_open              = 2,
    SYSCALL_close             = 3,
    SYSCALL_stat              = 4,
    SYSCALL_fstat             = 5,
    SYSCALL_lstat             = 6,
    SYSCALL_poll              = 7,
    SYSCALL_lseek             = 8,
    SYSCALL_mmap              = 9,
    SYSCALL_mprotect          = 10,
    SYSCALL_munmap            = 11,
    SYSCALL_brk               = 12,
    SYSCALL_rt_sigaction      = 13,
    SYSCALL_rt_sigprocmask    = 14,
    SYSCALL_rt_sigreturn      = 15,
    SYSCALL_ioctl             = 16,
    SYSCALL_pread64           = 17,
    SYSCALL_pwrite64          = 18,
    SYSCALL_readv             = 19,
    SYSCALL_writev            = 20,
    SYSCALL_access            = 21,
    SYSCALL_pipe              = 22,
    SYSCALL_select            = 23,
    SYSCALL_sched_yield       = 24,
    SYSCALL_mincore           = 27,
    SYSCALL_madvise           = 28,
    SYSCALL_dup               = 32,
    SYSCALL_dup2              = 33,
    SYSCALL_pause             = 34,
    SYSCALL_nanosleep         = 35,
    SYSCALL_getitimer         = 36,
    SYSCALL_alarm             = 37,
    SYSCALL_setitimer         = 38,
    SYSCALL_getpid            = 39,
    SYSCALL_socket            = 41,
    SYSCALL_connect           = 42,
    SYSCALL_accept            = 43,
    SYSCALL_sendto            = 44,
    SYSCALL_recvfrom          = 45,
    SYSCALL_sendmsg           = 46,
    SYSCALL_recvmsg           = 47,
    SYSCALL_shutdown          = 48,
    SYSCALL_bind              = 49,
    SYSCALL_listen            = 50,
    SYSCALL_getsockname       = 51,
    SYSCALL_getpeername       = 52,
    SYSCALL_socketpair        = 53,
    SYSCALL_setsockopt        = 54,
    SYSCALL_getsockopt        = 55,
    SYSCALL_clone             = 56,
    SYSCALL_fork              = 57,
    SYSCALL_vfork             = 58,
    SYSCALL_execve            = 59,
    SYSCALL_exit              = 60,
    SYSCALL_wait4             = 61,
    SYSCALL_kill              = 62,
    SYSCALL_uname             = 63,
    SYSCALL_fcntl             = 72,
    SYSCALL_truncate          = 76,
    SYSCALL_ftruncate         = 77,
    SYSCALL_getcwd            = 79,
    SYSCALL_chdir             = 80,
    SYSCALL_fchdir            = 81,
    SYSCALL_rename            = 82,
    SYSCALL_mkdir             = 83,
    SYSCALL_rmdir             = 84,
    SYSCALL_creat             = 85,
    SYSCALL_link              = 86,
    SYSCALL_unlink            = 87,
    SYSCALL_symlink           = 88,
    SYSCALL_readlink          = 89,
    SYSCALL_chmod             = 90,
    SYSCALL_fchmod            = 91,
    SYSCALL_chown             = 92,
    SYSCALL_fchown            = 93,
    SYSCALL_lchown            = 94,
    SYSCALL_umask             = 95,
    SYSCALL_gettimeofday      = 96,
    SYSCALL_getrlimit         = 97,
    SYSCALL_sysinfo           = 99,
    SYSCALL_getuid            = 102,
    SYSCALL_getgid            = 104,
    SYSCALL_setuid            = 105,
    SYSCALL_setgid            = 106,
    SYSCALL_geteuid           = 107,
    SYSCALL_getegid           = 108,
    SYSCALL_setpgid           = 109,
    SYSCALL_getppid           = 110,
    SYSCALL_getpgrp           = 111,
    SYSCALL_setsid            = 112,
    SYSCALL_setreuid          = 113,
    SYSCALL_setregid          = 114,
    SYSCALL_getgroups         = 115,
    SYSCALL_setgroups         = 116,
    SYSCALL_setresuid         = 117,
    SYSCALL_getresuid         = 118,
    SYSCALL_setresgid         = 119,
    SYSCALL_getresgid         = 120,
    SYSCALL_getpgid           = 121,
    SYSCALL_setfsuid          = 122,
    SYSCALL_setfsgid          = 123,
    SYSCALL_getsid            = 124,
    SYSCALL_sigaltstack       = 131,
    SYSCALL_mknod             = 133,
    SYSCALL_statfs            = 137,
    SYSCALL_fstatfs           = 138,
    SYSCALL_prctl             = 157,
    SYSCALL_arch_prctl        = 158,
    SYSCALL_setrlimit         = 160,
    SYSCALL_mount             = 165,
    SYSCALL_gettid            = 186,
    SYSCALL_time              = 201,
    SYSCALL_futex             = 202,
    SYSCALL_sched_getaffinity = 204,
    SYSCALL_epoll_create      = 213,
    SYSCALL_getdents64        = 217,
    SYSCALL_set_tid_address   = 218,
    SYSCALL_clock_gettime     = 228,
    SYSCALL_clock_getres      = 229,
    SYSCALL_clock_nanosleep   = 230,
    SYSCALL_exit_group        = 231,
    SYSCALL_epoll_wait        = 232,
    SYSCALL_epoll_ctl         = 233,
    SYSCALL_tgkill            = 234,
    SYSCALL_waitid            = 247,
    SYSCALL_inotify_init      = 253,
    SYSCALL_inotify_add_watch = 254,
    SYSCALL_inotify_rm_watch  = 255,
    SYSCALL_openat            = 257,
    SYSCALL_mkdirat           = 258,
    SYSCALL_mknodat           = 259,
    SYSCALL_fchownat          = 260,
    SYSCALL_newfstatat        = 262,
    SYSCALL_unlinkat          = 263,
    SYSCALL_renameat          = 264,
    SYSCALL_linkat            = 265,
    SYSCALL_symlinkat         = 266,
    SYSCALL_readlinkat        = 267,
    SYSCALL_fchmodat          = 268,
    SYSCALL_faccessat         = 269,
    SYSCALL_pselect6          = 270,
    SYSCALL_ppoll             = 271,
    SYSCALL_set_robust_list   = 273,
    SYSCALL_get_robust_list   = 274,
    SYSCALL_epoll_pwait       = 281,
    SYSCALL_signalfd          = 282,
    SYSCALL_eventfd           = 284,
    SYSCALL_accept4           = 288,
    SYSCALL_signalfd4         = 289,
    SYSCALL_eventfd2          = 290,
    SYSCALL_epoll_create1     = 291,
    SYSCALL_dup3              = 292,
    SYSCALL_pipe2             = 293,
    SYSCALL_inotify_init1     = 294,
    SYSCALL_preadv            = 295,
    SYSCALL_pwritev           = 296,
    SYSCALL_prlimit64         = 302,
    SYSCALL_getcpu            = 309,
    SYSCALL_getrandom         = 318,
    SYSCALL_memfd_create      = 319,
    SYSCALL_clone3            = 435,
    SYSCALL_close_range       = 436,
    SYSCALL_faccessat2        = 439,
    SYSCALL_epoll_pwait2      = 441,
    SYSCALL_fchmodat2         = 452
};

struct RequestLayerCaps
{
    ProcessRequestManager*    processRequestManager;
    SchedulerRequestManager*  schedulerRequestManager;
    TimeRequestManager*       timeRequestManager;
    MemoryRequestManager*     memoryRequestManager;
    VfsRequestManager*        vfsRequestManager;
    EventRequestManager*      eventRequestManager;
    SyncRequestManager*       syncRequestManager;
    SignalRequestManager*     signalRequestManager;
    CredentialRequestManager* credentialRequestManager;
    SystemRequestManager*     systemRequestManager;
    SocketRequestManager*     socketRequestManager;
    InterruptRequestManager*  interruptRequestManager;
};

class RequestLayerFactory
{
public:
    RequestLayerFactory();
    ~RequestLayerFactory();

    RequestLayerCaps* Create(ResourceLayerCaps* resourceLayerCaps, LogicLayerCaps* logicLayerCaps);

    RequestLayerCaps* GetCaps() const;

private:
    RequestLayerCaps* RequestLayerExportCaps;
};
