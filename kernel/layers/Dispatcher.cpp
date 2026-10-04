#include "layers/Dispatcher.hpp"

#include "layers/Resource/ResourceLayerFactory.hpp"

#include <platform.h>

extern "C"
{
#include <selftests/selftests.h>
}

Dispatcher::Dispatcher()
{
    resourceLayerFactory = new ResourceLayerFactory();
    logicLayerFactory    = new LogicLayerFactory();
    requestLayerFactory  = new RequestLayerFactory();
}

Dispatcher::~Dispatcher()
{
    delete resourceLayerFactory;
    delete logicLayerFactory;
    delete requestLayerFactory;
}

void Dispatcher::StartKernel()
{
    ResourceLayerCaps* ResourceLayerImportCaps = resourceLayerFactory->Create();

    resourcelayer_selftests((void*) ResourceLayerImportCaps);

    LogicLayerCaps* LogicLayerImportCaps = logicLayerFactory->Create(ResourceLayerImportCaps);

    requestLayerFactory->Create();

    logiclayer_selftests((void*) ResourceLayerImportCaps, (void*) LogicLayerImportCaps);
}

ResourceLayerCaps* Dispatcher::GetResourceLayerCaps() const
{
    return resourceLayerFactory != nullptr ? resourceLayerFactory->GetCaps() : nullptr;
}

LogicLayerCaps* Dispatcher::GetLogicLayerCaps() const
{
    return logicLayerFactory != nullptr ? logicLayerFactory->GetCaps() : nullptr;
}

RequestLayerCaps* Dispatcher::GetRequestLayerCaps() const
{
    return requestLayerFactory != nullptr ? requestLayerFactory->GetCaps() : nullptr;
}

uint64_t Dispatcher::DispatchSyscall(const arch_syscall_frame_t* frame) const
{
    if (frame == nullptr)
    {
        return (uint64_t) -22;
    }

    RequestLayerCaps* caps = GetRequestLayerCaps();
    if (caps == nullptr)
    {
        return (uint64_t) -38;
    }

    switch (frame->syscall_number)
    {
        case SYSCALL_exit: return caps->processRequestManager->HandleexitRequest(frame);
        case SYSCALL_exit_group: return caps->processRequestManager->Handleexit_groupRequest(frame);
        case SYSCALL_fork: return caps->processRequestManager->HandleforkRequest(frame);
        case SYSCALL_vfork: return caps->processRequestManager->HandlevforkRequest(frame);
        case SYSCALL_clone: return caps->processRequestManager->HandlecloneRequest(frame);
        case SYSCALL_clone3: return caps->processRequestManager->Handleclone3Request(frame);
        case SYSCALL_execve: return caps->processRequestManager->HandleexecveRequest(frame);
        case SYSCALL_wait4: return caps->processRequestManager->Handlewait4Request(frame);
        case SYSCALL_waitid: return caps->processRequestManager->HandlewaitidRequest(frame);
        case SYSCALL_set_tid_address: return caps->processRequestManager->Handleset_tid_addressRequest(frame);
        case SYSCALL_gettid: return caps->processRequestManager->HandlegettidRequest(frame);
        case SYSCALL_getpid: return caps->processRequestManager->HandlegetpidRequest(frame);
        case SYSCALL_getppid: return caps->processRequestManager->HandlegetppidRequest(frame);
        case SYSCALL_getpgid: return caps->processRequestManager->HandlegetpgidRequest(frame);
        case SYSCALL_getpgrp: return caps->processRequestManager->HandlegetpgrpRequest(frame);
        case SYSCALL_setpgid: return caps->processRequestManager->HandlesetpgidRequest(frame);
        case SYSCALL_getsid: return caps->processRequestManager->HandlegetsidRequest(frame);
        case SYSCALL_setsid: return caps->processRequestManager->HandlesetsidRequest(frame);

        case SYSCALL_sched_yield: return caps->schedulerRequestManager->Handlesched_yieldRequest(frame);
        case SYSCALL_sched_getaffinity: return caps->schedulerRequestManager->Handlesched_getaffinityRequest(frame);
        case SYSCALL_getcpu: return caps->schedulerRequestManager->HandlegetcpuRequest(frame);

        case SYSCALL_time: return caps->timeRequestManager->HandletimeRequest(frame);
        case SYSCALL_gettimeofday: return caps->timeRequestManager->HandlegettimeofdayRequest(frame);
        case SYSCALL_clock_gettime: return caps->timeRequestManager->Handleclock_gettimeRequest(frame);
        case SYSCALL_clock_getres: return caps->timeRequestManager->Handleclock_getresRequest(frame);
        case SYSCALL_clock_nanosleep: return caps->timeRequestManager->Handleclock_nanosleepRequest(frame);
        case SYSCALL_nanosleep: return caps->timeRequestManager->HandlenanosleepRequest(frame);
        case SYSCALL_getitimer: return caps->timeRequestManager->HandlegetitimerRequest(frame);
        case SYSCALL_setitimer: return caps->timeRequestManager->HandlesetitimerRequest(frame);
        case SYSCALL_alarm: return caps->timeRequestManager->HandlealarmRequest(frame);

        case SYSCALL_mmap: return caps->memoryRequestManager->HandlemmapRequest(frame);
        case SYSCALL_munmap: return caps->memoryRequestManager->HandlemunmapRequest(frame);
        case SYSCALL_mprotect: return caps->memoryRequestManager->HandlemprotectRequest(frame);
        case SYSCALL_mincore: return caps->memoryRequestManager->HandlemincoreRequest(frame);
        case SYSCALL_madvise: return caps->memoryRequestManager->HandlemadviseRequest(frame);
        case SYSCALL_brk: return caps->memoryRequestManager->HandlebrkRequest(frame);

        case SYSCALL_openat: return caps->vfsRequestManager->HandleopenatRequest(frame);
        case SYSCALL_open: return caps->vfsRequestManager->HandleopenRequest(frame);
        case SYSCALL_creat: return caps->vfsRequestManager->HandlecreatRequest(frame);
        case SYSCALL_close: return caps->vfsRequestManager->HandlecloseRequest(frame);
        case SYSCALL_close_range: return caps->vfsRequestManager->Handleclose_rangeRequest(frame);
        case SYSCALL_mkdirat: return caps->vfsRequestManager->HandlemkdiratRequest(frame);
        case SYSCALL_mkdir: return caps->vfsRequestManager->HandlemkdirRequest(frame);
        case SYSCALL_read: return caps->vfsRequestManager->HandlereadRequest(frame);
        case SYSCALL_write: return caps->vfsRequestManager->HandlewriteRequest(frame);
        case SYSCALL_pread64: return caps->vfsRequestManager->Handlepread64Request(frame);
        case SYSCALL_pwrite64: return caps->vfsRequestManager->Handlepwrite64Request(frame);
        case SYSCALL_readv: return caps->vfsRequestManager->HandlereadvRequest(frame);
        case SYSCALL_writev: return caps->vfsRequestManager->HandlewritevRequest(frame);
        case SYSCALL_preadv: return caps->vfsRequestManager->HandlepreadvRequest(frame);
        case SYSCALL_pwritev: return caps->vfsRequestManager->HandlepwritevRequest(frame);
        case SYSCALL_lseek: return caps->vfsRequestManager->HandlelseekRequest(frame);
        case SYSCALL_getcwd: return caps->vfsRequestManager->HandlegetcwdRequest(frame);
        case SYSCALL_chdir: return caps->vfsRequestManager->HandlechdirRequest(frame);
        case SYSCALL_fchdir: return caps->vfsRequestManager->HandlefchdirRequest(frame);
        case SYSCALL_getdents64: return caps->vfsRequestManager->Handlegetdents64Request(frame);
        case SYSCALL_unlinkat: return caps->vfsRequestManager->HandleunlinkatRequest(frame);
        case SYSCALL_unlink: return caps->vfsRequestManager->HandleunlinkRequest(frame);
        case SYSCALL_rmdir: return caps->vfsRequestManager->HandlermdirRequest(frame);
        case SYSCALL_fcntl: return caps->vfsRequestManager->HandlefcntlRequest(frame);
        case SYSCALL_dup: return caps->vfsRequestManager->HandledupRequest(frame);
        case SYSCALL_dup2: return caps->vfsRequestManager->Handledup2Request(frame);
        case SYSCALL_dup3: return caps->vfsRequestManager->Handledup3Request(frame);
        case SYSCALL_newfstatat: return caps->vfsRequestManager->HandlenewfstatatRequest(frame);
        case SYSCALL_stat: return caps->vfsRequestManager->HandlestatRequest(frame);
        case SYSCALL_fstat: return caps->vfsRequestManager->HandlefstatRequest(frame);
        case SYSCALL_lstat: return caps->vfsRequestManager->HandlelstatRequest(frame);
        case SYSCALL_renameat: return caps->vfsRequestManager->HandlerenameatRequest(frame);
        case SYSCALL_rename: return caps->vfsRequestManager->HandlerenameRequest(frame);
        case SYSCALL_readlinkat: return caps->vfsRequestManager->HandlereadlinkatRequest(frame);
        case SYSCALL_readlink: return caps->vfsRequestManager->HandlereadlinkRequest(frame);
        case SYSCALL_ioctl: return caps->vfsRequestManager->HandleioctlRequest(frame);
        case SYSCALL_linkat: return caps->vfsRequestManager->HandlelinkatRequest(frame);
        case SYSCALL_link: return caps->vfsRequestManager->HandlelinkRequest(frame);
        case SYSCALL_symlinkat: return caps->vfsRequestManager->HandlesymlinkatRequest(frame);
        case SYSCALL_symlink: return caps->vfsRequestManager->HandlesymlinkRequest(frame);
        case SYSCALL_faccessat: return caps->vfsRequestManager->HandlefaccessatRequest(frame);
        case SYSCALL_faccessat2: return caps->vfsRequestManager->Handlefaccessat2Request(frame);
        case SYSCALL_access: return caps->vfsRequestManager->HandleaccessRequest(frame);
        case SYSCALL_fchownat: return caps->vfsRequestManager->HandlefchownatRequest(frame);
        case SYSCALL_chown: return caps->vfsRequestManager->HandlechownRequest(frame);
        case SYSCALL_fchown: return caps->vfsRequestManager->HandlefchownRequest(frame);
        case SYSCALL_lchown: return caps->vfsRequestManager->HandlelchownRequest(frame);
        case SYSCALL_fchmodat2: return caps->vfsRequestManager->Handlefchmodat2Request(frame);
        case SYSCALL_fchmodat: return caps->vfsRequestManager->HandlefchmodatRequest(frame);
        case SYSCALL_chmod: return caps->vfsRequestManager->HandlechmodRequest(frame);
        case SYSCALL_fchmod: return caps->vfsRequestManager->HandlefchmodRequest(frame);
        case SYSCALL_mount: return caps->vfsRequestManager->HandlemountRequest(frame);
        case SYSCALL_statfs: return caps->vfsRequestManager->HandlestatfsRequest(frame);
        case SYSCALL_fstatfs: return caps->vfsRequestManager->HandlefstatfsRequest(frame);
        case SYSCALL_mknodat: return caps->vfsRequestManager->HandlemknodatRequest(frame);
        case SYSCALL_mknod: return caps->vfsRequestManager->HandlemknodRequest(frame);
        case SYSCALL_truncate: return caps->vfsRequestManager->HandletruncateRequest(frame);
        case SYSCALL_ftruncate: return caps->vfsRequestManager->HandleftruncateRequest(frame);
        case SYSCALL_pipe: return caps->vfsRequestManager->HandlepipeRequest(frame);
        case SYSCALL_pipe2: return caps->vfsRequestManager->Handlepipe2Request(frame);
        case SYSCALL_memfd_create: return caps->vfsRequestManager->Handlememfd_createRequest(frame);

        case SYSCALL_poll: return caps->eventRequestManager->HandlepollRequest(frame);
        case SYSCALL_ppoll: return caps->eventRequestManager->HandleppollRequest(frame);
        case SYSCALL_select: return caps->eventRequestManager->HandleselectRequest(frame);
        case SYSCALL_pselect6: return caps->eventRequestManager->Handlepselect6Request(frame);
        case SYSCALL_epoll_create: return caps->eventRequestManager->Handleepoll_createRequest(frame);
        case SYSCALL_epoll_create1: return caps->eventRequestManager->Handleepoll_create1Request(frame);
        case SYSCALL_epoll_ctl: return caps->eventRequestManager->Handleepoll_ctlRequest(frame);
        case SYSCALL_epoll_wait: return caps->eventRequestManager->Handleepoll_waitRequest(frame);
        case SYSCALL_epoll_pwait: return caps->eventRequestManager->Handleepoll_pwaitRequest(frame);
        case SYSCALL_epoll_pwait2: return caps->eventRequestManager->Handleepoll_pwait2Request(frame);
        case SYSCALL_inotify_init: return caps->eventRequestManager->Handleinotify_initRequest(frame);
        case SYSCALL_inotify_init1: return caps->eventRequestManager->Handleinotify_init1Request(frame);
        case SYSCALL_inotify_add_watch: return caps->eventRequestManager->Handleinotify_add_watchRequest(frame);
        case SYSCALL_inotify_rm_watch: return caps->eventRequestManager->Handleinotify_rm_watchRequest(frame);
        case SYSCALL_eventfd: return caps->eventRequestManager->HandleeventfdRequest(frame);
        case SYSCALL_eventfd2: return caps->eventRequestManager->Handleeventfd2Request(frame);
        case SYSCALL_signalfd: return caps->eventRequestManager->HandlesignalfdRequest(frame);
        case SYSCALL_signalfd4: return caps->eventRequestManager->Handlesignalfd4Request(frame);

        case SYSCALL_futex: return caps->syncRequestManager->HandlefutexRequest(frame);
        case SYSCALL_set_robust_list: return caps->syncRequestManager->Handleset_robust_listRequest(frame);
        case SYSCALL_get_robust_list: return caps->syncRequestManager->Handleget_robust_listRequest(frame);

        case SYSCALL_rt_sigreturn: return caps->signalRequestManager->Handlert_sigreturnRequest(frame);
        case SYSCALL_rt_sigprocmask: return caps->signalRequestManager->Handlert_sigprocmaskRequest(frame);
        case SYSCALL_rt_sigaction: return caps->signalRequestManager->Handlert_sigactionRequest(frame);
        case SYSCALL_sigaltstack: return caps->signalRequestManager->HandlesigaltstackRequest(frame);
        case SYSCALL_kill: return caps->signalRequestManager->HandlekillRequest(frame);
        case SYSCALL_tgkill: return caps->signalRequestManager->HandletgkillRequest(frame);
        case SYSCALL_pause: return caps->signalRequestManager->HandlepauseRequest(frame);

        case SYSCALL_getuid: return caps->credentialRequestManager->HandlegetuidRequest(frame);
        case SYSCALL_getgid: return caps->credentialRequestManager->HandlegetgidRequest(frame);
        case SYSCALL_geteuid: return caps->credentialRequestManager->HandlegeteuidRequest(frame);
        case SYSCALL_getegid: return caps->credentialRequestManager->HandlegetegidRequest(frame);
        case SYSCALL_getresuid: return caps->credentialRequestManager->HandlegetresuidRequest(frame);
        case SYSCALL_getresgid: return caps->credentialRequestManager->HandlegetresgidRequest(frame);
        case SYSCALL_setuid: return caps->credentialRequestManager->HandlesetuidRequest(frame);
        case SYSCALL_setgid: return caps->credentialRequestManager->HandlesetgidRequest(frame);
        case SYSCALL_setreuid: return caps->credentialRequestManager->HandlesetreuidRequest(frame);
        case SYSCALL_setregid: return caps->credentialRequestManager->HandlesetregidRequest(frame);
        case SYSCALL_setresuid: return caps->credentialRequestManager->HandlesetresuidRequest(frame);
        case SYSCALL_setresgid: return caps->credentialRequestManager->HandlesetresgidRequest(frame);
        case SYSCALL_setfsuid: return caps->credentialRequestManager->HandlesetfsuidRequest(frame);
        case SYSCALL_setfsgid: return caps->credentialRequestManager->HandlesetfsgidRequest(frame);
        case SYSCALL_getgroups: return caps->credentialRequestManager->HandlegetgroupsRequest(frame);
        case SYSCALL_setgroups: return caps->credentialRequestManager->HandlesetgroupsRequest(frame);

        case SYSCALL_arch_prctl: return caps->systemRequestManager->Handlearch_prctlRequest(frame);
        case SYSCALL_umask: return caps->systemRequestManager->HandleumaskRequest(frame);
        case SYSCALL_prlimit64: return caps->systemRequestManager->Handleprlimit64Request(frame);
        case SYSCALL_getrlimit: return caps->systemRequestManager->HandlegetrlimitRequest(frame);
        case SYSCALL_setrlimit: return caps->systemRequestManager->HandlesetrlimitRequest(frame);
        case SYSCALL_prctl: return caps->systemRequestManager->HandleprctlRequest(frame);
        case SYSCALL_uname: return caps->systemRequestManager->HandleunameRequest(frame);
        case SYSCALL_sysinfo: return caps->systemRequestManager->HandlesysinfoRequest(frame);
        case SYSCALL_getrandom: return caps->systemRequestManager->HandlegetrandomRequest(frame);

        case SYSCALL_socket: return caps->socketRequestManager->HandlesocketRequest(frame);
        case SYSCALL_socketpair: return caps->socketRequestManager->HandlesocketpairRequest(frame);
        case SYSCALL_bind: return caps->socketRequestManager->HandlebindRequest(frame);
        case SYSCALL_connect: return caps->socketRequestManager->HandleconnectRequest(frame);
        case SYSCALL_listen: return caps->socketRequestManager->HandlelistenRequest(frame);
        case SYSCALL_accept4: return caps->socketRequestManager->Handleaccept4Request(frame);
        case SYSCALL_accept: return caps->socketRequestManager->HandleacceptRequest(frame);
        case SYSCALL_recvfrom: return caps->socketRequestManager->HandlerecvfromRequest(frame);
        case SYSCALL_recvmsg: return caps->socketRequestManager->HandlerecvmsgRequest(frame);
        case SYSCALL_sendto: return caps->socketRequestManager->HandlesendtoRequest(frame);
        case SYSCALL_sendmsg: return caps->socketRequestManager->HandlesendmsgRequest(frame);
        case SYSCALL_shutdown: return caps->socketRequestManager->HandleshutdownRequest(frame);
        case SYSCALL_getsockopt: return caps->socketRequestManager->HandlegetsockoptRequest(frame);
        case SYSCALL_setsockopt: return caps->socketRequestManager->HandlesetsockoptRequest(frame);
        case SYSCALL_getsockname: return caps->socketRequestManager->HandlegetsocknameRequest(frame);
        case SYSCALL_getpeername: return caps->socketRequestManager->HandlegetpeernameRequest(frame);

        default:
            return (uint64_t) -38;
    }
}

extern "C" uint64_t dispatcher_dispatch_syscall(const arch_syscall_frame_t* frame)
{
    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        return (uint64_t) -38;
    }

    return dispatcher->DispatchSyscall(frame);
}
