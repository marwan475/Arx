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

    logiclayer_selftests((void*) ResourceLayerImportCaps, (void*) LogicLayerImportCaps);

    RequestLayerCaps* RequestLayerImportCaps = requestLayerFactory->Create();

    (void) RequestLayerImportCaps;
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
        case SYSCALL_exit:
            return caps->processRequestManager->HandleExitRequest(frame);
        case SYSCALL_exit_group:
            return caps->processRequestManager->HandleExit_groupRequest(frame);
        case SYSCALL_fork:
            return caps->processRequestManager->HandleForkRequest(frame);
        case SYSCALL_vfork:
            return caps->processRequestManager->HandleVforkRequest(frame);
        case SYSCALL_clone:
            return caps->processRequestManager->HandleCloneRequest(frame);
        case SYSCALL_clone3:
            return caps->processRequestManager->HandleClone3Request(frame);
        case SYSCALL_execve:
            return caps->processRequestManager->HandleExecveRequest(frame);
        case SYSCALL_wait4:
            return caps->processRequestManager->HandleWait4Request(frame);
        case SYSCALL_waitid:
            return caps->processRequestManager->HandleWaitidRequest(frame);
        case SYSCALL_set_tid_address:
            return caps->processRequestManager->HandleSet_tid_addressRequest(frame);
        case SYSCALL_gettid:
            return caps->processRequestManager->HandleGettidRequest(frame);
        case SYSCALL_getpid:
            return caps->processRequestManager->HandleGetpidRequest(frame);
        case SYSCALL_getppid:
            return caps->processRequestManager->HandleGetppidRequest(frame);
        case SYSCALL_getpgid:
            return caps->processRequestManager->HandleGetpgidRequest(frame);
        case SYSCALL_getpgrp:
            return caps->processRequestManager->HandleGetpgrpRequest(frame);
        case SYSCALL_setpgid:
            return caps->processRequestManager->HandleSetpgidRequest(frame);
        case SYSCALL_getsid:
            return caps->processRequestManager->HandleGetsidRequest(frame);
        case SYSCALL_setsid:
            return caps->processRequestManager->HandleSetsidRequest(frame);

        case SYSCALL_sched_yield:
            return caps->schedulerRequestManager->HandleSched_yieldRequest(frame);
        case SYSCALL_sched_getaffinity:
            return caps->schedulerRequestManager->HandleSched_getaffinityRequest(frame);
        case SYSCALL_getcpu:
            return caps->schedulerRequestManager->HandleGetcpuRequest(frame);

        case SYSCALL_time:
            return caps->timeRequestManager->HandleTimeRequest(frame);
        case SYSCALL_gettimeofday:
            return caps->timeRequestManager->HandleGettimeofdayRequest(frame);
        case SYSCALL_clock_gettime:
            return caps->timeRequestManager->HandleClock_gettimeRequest(frame);
        case SYSCALL_clock_getres:
            return caps->timeRequestManager->HandleClock_getresRequest(frame);
        case SYSCALL_clock_nanosleep:
            return caps->timeRequestManager->HandleClock_nanosleepRequest(frame);
        case SYSCALL_nanosleep:
            return caps->timeRequestManager->HandleNanosleepRequest(frame);
        case SYSCALL_getitimer:
            return caps->timeRequestManager->HandleGetitimerRequest(frame);
        case SYSCALL_setitimer:
            return caps->timeRequestManager->HandleSetitimerRequest(frame);
        case SYSCALL_alarm:
            return caps->timeRequestManager->HandleAlarmRequest(frame);

        case SYSCALL_mmap:
            return caps->memoryRequestManager->HandleMmapRequest(frame);
        case SYSCALL_munmap:
            return caps->memoryRequestManager->HandleMunmapRequest(frame);
        case SYSCALL_mprotect:
            return caps->memoryRequestManager->HandleMprotectRequest(frame);
        case SYSCALL_mincore:
            return caps->memoryRequestManager->HandleMincoreRequest(frame);
        case SYSCALL_madvise:
            return caps->memoryRequestManager->HandleMadviseRequest(frame);
        case SYSCALL_brk:
            return caps->memoryRequestManager->HandleBrkRequest(frame);

        case SYSCALL_openat:
            return caps->vfsRequestManager->HandleOpenatRequest(frame);
        case SYSCALL_open:
            return caps->vfsRequestManager->HandleOpenRequest(frame);
        case SYSCALL_creat:
            return caps->vfsRequestManager->HandleCreatRequest(frame);
        case SYSCALL_close:
            return caps->vfsRequestManager->HandleCloseRequest(frame);
        case SYSCALL_close_range:
            return caps->vfsRequestManager->HandleClose_rangeRequest(frame);
        case SYSCALL_mkdirat:
            return caps->vfsRequestManager->HandleMkdiratRequest(frame);
        case SYSCALL_mkdir:
            return caps->vfsRequestManager->HandleMkdirRequest(frame);
        case SYSCALL_read:
            return caps->vfsRequestManager->HandleReadRequest(frame);
        case SYSCALL_write:
            return caps->vfsRequestManager->HandleWriteRequest(frame);
        case SYSCALL_pread64:
            return caps->vfsRequestManager->HandlePread64Request(frame);
        case SYSCALL_pwrite64:
            return caps->vfsRequestManager->HandlePwrite64Request(frame);
        case SYSCALL_readv:
            return caps->vfsRequestManager->HandleReadvRequest(frame);
        case SYSCALL_writev:
            return caps->vfsRequestManager->HandleWritevRequest(frame);
        case SYSCALL_preadv:
            return caps->vfsRequestManager->HandlePreadvRequest(frame);
        case SYSCALL_pwritev:
            return caps->vfsRequestManager->HandlePwritevRequest(frame);
        case SYSCALL_lseek:
            return caps->vfsRequestManager->HandleLseekRequest(frame);
        case SYSCALL_getcwd:
            return caps->vfsRequestManager->HandleGetcwdRequest(frame);
        case SYSCALL_chdir:
            return caps->vfsRequestManager->HandleChdirRequest(frame);
        case SYSCALL_fchdir:
            return caps->vfsRequestManager->HandleFchdirRequest(frame);
        case SYSCALL_getdents64:
            return caps->vfsRequestManager->HandleGetdents64Request(frame);
        case SYSCALL_unlinkat:
            return caps->vfsRequestManager->HandleUnlinkatRequest(frame);
        case SYSCALL_unlink:
            return caps->vfsRequestManager->HandleUnlinkRequest(frame);
        case SYSCALL_rmdir:
            return caps->vfsRequestManager->HandleRmdirRequest(frame);
        case SYSCALL_fcntl:
            return caps->vfsRequestManager->HandleFcntlRequest(frame);
        case SYSCALL_dup:
            return caps->vfsRequestManager->HandleDupRequest(frame);
        case SYSCALL_dup2:
            return caps->vfsRequestManager->HandleDup2Request(frame);
        case SYSCALL_dup3:
            return caps->vfsRequestManager->HandleDup3Request(frame);
        case SYSCALL_newfstatat:
            return caps->vfsRequestManager->HandleNewfstatatRequest(frame);
        case SYSCALL_stat:
            return caps->vfsRequestManager->HandleStatRequest(frame);
        case SYSCALL_fstat:
            return caps->vfsRequestManager->HandleFstatRequest(frame);
        case SYSCALL_lstat:
            return caps->vfsRequestManager->HandleLstatRequest(frame);
        case SYSCALL_renameat:
            return caps->vfsRequestManager->HandleRenameatRequest(frame);
        case SYSCALL_rename:
            return caps->vfsRequestManager->HandleRenameRequest(frame);
        case SYSCALL_readlinkat:
            return caps->vfsRequestManager->HandleReadlinkatRequest(frame);
        case SYSCALL_readlink:
            return caps->vfsRequestManager->HandleReadlinkRequest(frame);
        case SYSCALL_ioctl:
            return caps->vfsRequestManager->HandleIoctlRequest(frame);
        case SYSCALL_linkat:
            return caps->vfsRequestManager->HandleLinkatRequest(frame);
        case SYSCALL_link:
            return caps->vfsRequestManager->HandleLinkRequest(frame);
        case SYSCALL_symlinkat:
            return caps->vfsRequestManager->HandleSymlinkatRequest(frame);
        case SYSCALL_symlink:
            return caps->vfsRequestManager->HandleSymlinkRequest(frame);
        case SYSCALL_faccessat:
            return caps->vfsRequestManager->HandleFaccessatRequest(frame);
        case SYSCALL_faccessat2:
            return caps->vfsRequestManager->HandleFaccessat2Request(frame);
        case SYSCALL_access:
            return caps->vfsRequestManager->HandleAccessRequest(frame);
        case SYSCALL_fchownat:
            return caps->vfsRequestManager->HandleFchownatRequest(frame);
        case SYSCALL_chown:
            return caps->vfsRequestManager->HandleChownRequest(frame);
        case SYSCALL_fchown:
            return caps->vfsRequestManager->HandleFchownRequest(frame);
        case SYSCALL_lchown:
            return caps->vfsRequestManager->HandleLchownRequest(frame);
        case SYSCALL_fchmodat2:
            return caps->vfsRequestManager->HandleFchmodat2Request(frame);
        case SYSCALL_fchmodat:
            return caps->vfsRequestManager->HandleFchmodatRequest(frame);
        case SYSCALL_chmod:
            return caps->vfsRequestManager->HandleChmodRequest(frame);
        case SYSCALL_fchmod:
            return caps->vfsRequestManager->HandleFchmodRequest(frame);
        case SYSCALL_mount:
            return caps->vfsRequestManager->HandleMountRequest(frame);
        case SYSCALL_statfs:
            return caps->vfsRequestManager->HandleStatfsRequest(frame);
        case SYSCALL_fstatfs:
            return caps->vfsRequestManager->HandleFstatfsRequest(frame);
        case SYSCALL_mknodat:
            return caps->vfsRequestManager->HandleMknodatRequest(frame);
        case SYSCALL_mknod:
            return caps->vfsRequestManager->HandleMknodRequest(frame);
        case SYSCALL_truncate:
            return caps->vfsRequestManager->HandleTruncateRequest(frame);
        case SYSCALL_ftruncate:
            return caps->vfsRequestManager->HandleFtruncateRequest(frame);
        case SYSCALL_pipe:
            return caps->vfsRequestManager->HandlePipeRequest(frame);
        case SYSCALL_pipe2:
            return caps->vfsRequestManager->HandlePipe2Request(frame);
        case SYSCALL_memfd_create:
            return caps->vfsRequestManager->HandleMemfd_createRequest(frame);

        case SYSCALL_poll:
            return caps->eventRequestManager->HandlePollRequest(frame);
        case SYSCALL_ppoll:
            return caps->eventRequestManager->HandlePpollRequest(frame);
        case SYSCALL_select:
            return caps->eventRequestManager->HandleSelectRequest(frame);
        case SYSCALL_pselect6:
            return caps->eventRequestManager->HandlePselect6Request(frame);
        case SYSCALL_epoll_create:
            return caps->eventRequestManager->HandleEpoll_createRequest(frame);
        case SYSCALL_epoll_create1:
            return caps->eventRequestManager->HandleEpoll_create1Request(frame);
        case SYSCALL_epoll_ctl:
            return caps->eventRequestManager->HandleEpoll_ctlRequest(frame);
        case SYSCALL_epoll_wait:
            return caps->eventRequestManager->HandleEpoll_waitRequest(frame);
        case SYSCALL_epoll_pwait:
            return caps->eventRequestManager->HandleEpoll_pwaitRequest(frame);
        case SYSCALL_epoll_pwait2:
            return caps->eventRequestManager->HandleEpoll_pwait2Request(frame);
        case SYSCALL_inotify_init:
            return caps->eventRequestManager->HandleInotify_initRequest(frame);
        case SYSCALL_inotify_init1:
            return caps->eventRequestManager->HandleInotify_init1Request(frame);
        case SYSCALL_inotify_add_watch:
            return caps->eventRequestManager->HandleInotify_add_watchRequest(frame);
        case SYSCALL_inotify_rm_watch:
            return caps->eventRequestManager->HandleInotify_rm_watchRequest(frame);
        case SYSCALL_eventfd:
            return caps->eventRequestManager->HandleEventfdRequest(frame);
        case SYSCALL_eventfd2:
            return caps->eventRequestManager->HandleEventfd2Request(frame);
        case SYSCALL_signalfd:
            return caps->eventRequestManager->HandleSignalfdRequest(frame);
        case SYSCALL_signalfd4:
            return caps->eventRequestManager->HandleSignalfd4Request(frame);

        case SYSCALL_futex:
            return caps->syncRequestManager->HandleFutexRequest(frame);
        case SYSCALL_set_robust_list:
            return caps->syncRequestManager->HandleSet_robust_listRequest(frame);
        case SYSCALL_get_robust_list:
            return caps->syncRequestManager->HandleGet_robust_listRequest(frame);

        case SYSCALL_rt_sigreturn:
            return caps->signalRequestManager->HandleRt_sigreturnRequest(frame);
        case SYSCALL_rt_sigprocmask:
            return caps->signalRequestManager->HandleRt_sigprocmaskRequest(frame);
        case SYSCALL_rt_sigaction:
            return caps->signalRequestManager->HandleRt_sigactionRequest(frame);
        case SYSCALL_sigaltstack:
            return caps->signalRequestManager->HandleSigaltstackRequest(frame);
        case SYSCALL_kill:
            return caps->signalRequestManager->HandleKillRequest(frame);
        case SYSCALL_tgkill:
            return caps->signalRequestManager->HandleTgkillRequest(frame);
        case SYSCALL_pause:
            return caps->signalRequestManager->HandlePauseRequest(frame);

        case SYSCALL_getuid:
            return caps->credentialRequestManager->HandleGetuidRequest(frame);
        case SYSCALL_getgid:
            return caps->credentialRequestManager->HandleGetgidRequest(frame);
        case SYSCALL_geteuid:
            return caps->credentialRequestManager->HandleGeteuidRequest(frame);
        case SYSCALL_getegid:
            return caps->credentialRequestManager->HandleGetegidRequest(frame);
        case SYSCALL_getresuid:
            return caps->credentialRequestManager->HandleGetresuidRequest(frame);
        case SYSCALL_getresgid:
            return caps->credentialRequestManager->HandleGetresgidRequest(frame);
        case SYSCALL_setuid:
            return caps->credentialRequestManager->HandleSetuidRequest(frame);
        case SYSCALL_setgid:
            return caps->credentialRequestManager->HandleSetgidRequest(frame);
        case SYSCALL_setreuid:
            return caps->credentialRequestManager->HandleSetreuidRequest(frame);
        case SYSCALL_setregid:
            return caps->credentialRequestManager->HandleSetregidRequest(frame);
        case SYSCALL_setresuid:
            return caps->credentialRequestManager->HandleSetresuidRequest(frame);
        case SYSCALL_setresgid:
            return caps->credentialRequestManager->HandleSetresgidRequest(frame);
        case SYSCALL_setfsuid:
            return caps->credentialRequestManager->HandleSetfsuidRequest(frame);
        case SYSCALL_setfsgid:
            return caps->credentialRequestManager->HandleSetfsgidRequest(frame);
        case SYSCALL_getgroups:
            return caps->credentialRequestManager->HandleGetgroupsRequest(frame);
        case SYSCALL_setgroups:
            return caps->credentialRequestManager->HandleSetgroupsRequest(frame);

        case SYSCALL_arch_prctl:
            return caps->systemRequestManager->HandleArch_prctlRequest(frame);
        case SYSCALL_umask:
            return caps->systemRequestManager->HandleUmaskRequest(frame);
        case SYSCALL_prlimit64:
            return caps->systemRequestManager->HandlePrlimit64Request(frame);
        case SYSCALL_getrlimit:
            return caps->systemRequestManager->HandleGetrlimitRequest(frame);
        case SYSCALL_setrlimit:
            return caps->systemRequestManager->HandleSetrlimitRequest(frame);
        case SYSCALL_prctl:
            return caps->systemRequestManager->HandlePrctlRequest(frame);
        case SYSCALL_uname:
            return caps->systemRequestManager->HandleUnameRequest(frame);
        case SYSCALL_sysinfo:
            return caps->systemRequestManager->HandleSysinfoRequest(frame);
        case SYSCALL_getrandom:
            return caps->systemRequestManager->HandleGetrandomRequest(frame);

        case SYSCALL_socket:
            return caps->socketRequestManager->HandleSocketRequest(frame);
        case SYSCALL_socketpair:
            return caps->socketRequestManager->HandleSocketpairRequest(frame);
        case SYSCALL_bind:
            return caps->socketRequestManager->HandleBindRequest(frame);
        case SYSCALL_connect:
            return caps->socketRequestManager->HandleConnectRequest(frame);
        case SYSCALL_listen:
            return caps->socketRequestManager->HandleListenRequest(frame);
        case SYSCALL_accept4:
            return caps->socketRequestManager->HandleAccept4Request(frame);
        case SYSCALL_accept:
            return caps->socketRequestManager->HandleAcceptRequest(frame);
        case SYSCALL_recvfrom:
            return caps->socketRequestManager->HandleRecvfromRequest(frame);
        case SYSCALL_recvmsg:
            return caps->socketRequestManager->HandleRecvmsgRequest(frame);
        case SYSCALL_sendto:
            return caps->socketRequestManager->HandleSendtoRequest(frame);
        case SYSCALL_sendmsg:
            return caps->socketRequestManager->HandleSendmsgRequest(frame);
        case SYSCALL_shutdown:
            return caps->socketRequestManager->HandleShutdownRequest(frame);
        case SYSCALL_getsockopt:
            return caps->socketRequestManager->HandleGetsockoptRequest(frame);
        case SYSCALL_setsockopt:
            return caps->socketRequestManager->HandleSetsockoptRequest(frame);
        case SYSCALL_getsockname:
            return caps->socketRequestManager->HandleGetsocknameRequest(frame);
        case SYSCALL_getpeername:
            return caps->socketRequestManager->HandleGetpeernameRequest(frame);

        default:
            return (uint64_t) -38;
    }
}

void Dispatcher::DispatchInterruptRequest(uint64_t interruptNumber) const
{
    RequestLayerCaps* caps = GetRequestLayerCaps();
    if (caps == nullptr || caps->interruptRequestManager == nullptr)
    {
        return;
    }

    caps->interruptRequestManager->HandleInterruptRequest(interruptNumber);
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

extern "C" void dispatcher_handle_interrupt_request(uint64_t interrupt_number)
{
    Dispatcher* dispatcher = (Dispatcher*) platform.dispacher;
    if (dispatcher == nullptr)
    {
        return;
    }

    dispatcher->DispatchInterruptRequest(interrupt_number);
}
