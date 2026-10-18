# Request layer

The Request layer turns syscall arguments and common interrupt requests into
kernel operations. Its managers decode the ABI, check inputs, arrange user-memory
access, and call Logic or Resource services. They return values in the syscall
ABI's format; architecture code handles the machine entry and return.

`RequestLayerFactory.hpp` declares the exported managers and syscall numbers.
`RequestLayerFactory.cpp` constructs the layer. KernelArch.md describes the layer
boundaries, Dispatcher.md explains routing, and x86_64.md covers machine entry.

## 1. Initialization

Dispatcher creates Request last in `StartKernel`, after Resource and Logic and
their construction-time self-tests. `RequestLayerFactory::Create` receives both
lower-layer capability sets, allocates `RequestLayerCaps`, and constructs its
twelve managers in the order listed below.

Most constructors receive Resource and Logic capabilities. Memory receives only
Logic; System receives only Resource. Interrupt receives neither and looks up its
services through the published Dispatcher when handling a scheduling request.
Receiving a capability set does not mean a manager retains or uses every service.

`GetCaps` returns null before creation. Later `Create` calls return the existing
capabilities without rebuilding managers or replacing their dependencies. Request
self-tests run later in scheduled post-init work on the bootstrap CPU.

### RequestLayerFactory

| Function | What it does |
| --- | --- |
| `RequestLayerFactory()` | Initialize empty factory-owned capability state. |
| `~RequestLayerFactory()` | Delete the twelve managers and capability structure. |
| `Create(resourceLayerCaps, logicLayerCaps)` | Construct handlers with lower-layer imports once, or return existing capabilities. |
| `GetCaps()` | Return borrowed Request capabilities, or null before creation. |

## 2. Managers and public APIs

Each manager has matching `.hpp` and `.cpp` files named after it.
`SchedulerPolicyRequests.cpp` also implements scheduler policy requests.
All syscall handlers take `const arch_syscall_frame_t* frame`: its syscall number
selects the operation and `arg0` through `arg5` carry the Linux ABI arguments.
They return a `uint64_t` result or an encoded negative errno, as described below.
The tables identify each public function and explain each routed syscall;
implemented behavior and limits take precedence over general Linux semantics.

### ProcessRequestManager

Handles Linux process/thread lifecycle, execution, exit, waiting, process/task
identity, and process-group/session syscalls. Delegates lifecycle policy to
ProcessController and uses Resource records for identity queries.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `ProcessRequestManager(resourceLayerCaps, logicLayerCaps)` | — | Bind Resource records and Logic lifecycle services. |
| `HandleExitRequest(frame)` | `exit` | End the calling thread with a status; process exit follows when its last thread retires. |
| `HandleExit_groupRequest(frame)` | `exit_group` | Terminate all threads in the calling process with the supplied status. |
| `HandleForkRequest(frame)` | `fork` | Create a child process with copied process state and copy-on-write user memory. |
| `HandleVforkRequest(frame)` | `vfork` | Create a child sharing user memory and suspend the caller until the child executes a new image or exits. |
| `HandleCloneRequest(frame)` | `clone` | Create a child task/process with selected sharing, stack, TLS, and TID-pointer options. |
| `HandleClone3Request(frame)` | `clone3` | Create a child using a size-checked user clone-arguments record rather than separate register arguments. |
| `HandleExecveRequest(frame)` | `execve` | Replace the current process image using an executable path, argument list, and environment; prepare its new user entry state. |
| `HandleWait4Request(frame)` | `wait4` | Wait for a selected child's state change and return its identity, status, and optional resource usage. |
| `HandleWaitidRequest(frame)` | `waitid` | Wait using an explicit child-selection kind and return siginfo-style status plus optional usage. |
| `HandleSet_tid_addressRequest(frame)` | `set_tid_address` | Register the user word cleared/woken when this thread exits, and return its TID. |
| `HandleGettidRequest(frame)` | `gettid` | Return the calling thread's task identity. |
| `HandleGetpidRequest(frame)` | `getpid` | Return the calling process identity shared by its threads. |
| `HandleGetppidRequest(frame)` | `getppid` | Return the calling process's parent identity. |
| `HandleGetpgidRequest(frame)` | `getpgid` | Return the selected process's process-group identity. |
| `HandleGetpgrpRequest(frame)` | `getpgrp` | Return the caller's process-group identity. |
| `HandleSetpgidRequest(frame)` | `setpgid` | Move an eligible process into the requested process group. |
| `HandleGetsidRequest(frame)` | `getsid` | Return the selected process's session identity. |
| `HandleSetsidRequest(frame)` | `setsid` | Create a new session/process group for an eligible caller and return its identity. |

Successful child creation returns the child's identity to the parent; the child
resumes with a zero result. Successful exec instead replaces the saved user entry
state with the new image.

### SchedulerRequestManager

Handles Linux yielding, scheduler policy/attributes, priority, CPU affinity, and
CPU-location syscalls. Uses Scheduler for policy and placement, and Resource task
claims for stable target access.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `SchedulerRequestManager(resourceLayerCaps, logicLayerCaps)` | — | Bind task/process records and Logic scheduling services. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_setparam` | Change a target task's scheduling priority while retaining its policy. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_getparam` | Copy a target task's configured scheduling priority to user memory. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_setscheduler` | Change a target task's scheduling policy and priority, including supported reset-on-fork behavior. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_getscheduler` | Return a target task's policy and reset-on-fork flag. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_get_priority_max` | Return the maximum priority for a supported policy: 99 for FIFO/RR, zero for normal/deadline. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_get_priority_min` | Return the minimum priority for a supported policy: one for FIFO/RR, zero for normal/deadline. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_rr_get_interval` | Copy the task's round-robin quantum; report a zero interval for other policies. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_setattr` | Read and validate extended scheduling attributes, including supported deadline parameters, then apply them. |
| `HandleSchedulingPolicyRequest(frame)` | `sched_getattr` | Copy a target task's extended scheduling attributes within the supplied buffer size. |
| `HandleSched_yieldRequest(frame)` | `sched_yield` | Give Scheduler an opportunity to run other ready work on the current CPU. |
| `HandleSched_getaffinityRequest(frame)` | `sched_getaffinity` | Copy a target task's allowed CPU mask to user memory and return the mask size. |
| `HandleSched_setaffinityRequest(frame)` | `sched_setaffinity` | Read a user CPU mask and apply an authorized target task's CPU placement constraints. |
| `HandleGetpriorityRequest(frame)` | `getpriority` | Read priority for a process/group/user selection; the raw syscall result encodes nice as 20 minus nice. |
| `HandleSetpriorityRequest(frame)` | `setpriority` | Apply a clamped nice value to an authorized process/group/user selection. |
| `HandleGetcpuRequest(frame)` | `getcpu` | Copy the calling CPU number and optional NUMA node number to user memory. |

### TimeRequestManager

Handles Linux clock queries, sleeps, interval timers, and alarm syscalls. Uses
TimeManager for time conversion/timer state and Scheduler for sleeping.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `TimeRequestManager(resourceLayerCaps, logicLayerCaps)` | — | Bind Resource time services and Logic scheduling services. |
| `HandleTimeRequest(frame)` | `time` | Return realtime seconds and optionally copy the value to user memory. |
| `HandleGettimeofdayRequest(frame)` | `gettimeofday` | Copy realtime seconds/microseconds and, if requested, a zeroed legacy timezone record. |
| `HandleClock_gettimeRequest(frame)` | `clock_gettime` | Copy a supported realtime, monotonic, or monotonic-raw clock value; CPU clock queries are unsupported. |
| `HandleClock_getresRequest(frame)` | `clock_getres` | Report a supported clock's resolution, allowing a null output pointer. |
| `HandleClock_nanosleepRequest(frame)` | `clock_nanosleep` | Sleep using supported realtime/monotonic relative or absolute deadlines, reporting a remainder for interrupted relative sleeps. |
| `HandleNanosleepRequest(frame)` | `nanosleep` | Sleep for a relative monotonic duration and optionally report remaining time after interruption. |
| `HandleGetitimerRequest(frame)` | `getitimer` | Copy the current process's selected real, virtual, or profiling interval timer. |
| `HandleSetitimerRequest(frame)` | `setitimer` | Replace a selected process interval timer and optionally copy out its previous state. |
| `HandleAlarmRequest(frame)` | `alarm` | Replace the real-time alarm and return its previous remaining seconds; zero cancels it. |

### MemoryRequestManager

Handles Linux user-memory mapping, unmapping, protection, residency, advice, and
program-break syscalls. Validates ABI arguments before delegating process-memory
policy to ProcessController.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `MemoryRequestManager(logicLayerCaps)` | — | Bind Logic process-memory policy through ProcessController. |
| `HandleMmapRequest(frame)` | `mmap` | Validate and create an anonymous or file-backed user mapping, returning its address. |
| `HandleMunmapRequest(frame)` | `munmap` | Remove mappings in a validated user range and release their backing claims. |
| `HandleMprotectRequest(frame)` | `mprotect` | Change access permissions on a validated user-memory range. |
| `HandleMincoreRequest(frame)` | `mincore` | Copy per-page residency information for a user-memory range. |
| `HandleMadviseRequest(frame)` | `madvise` | Apply supported memory advice to a user range; unsupported advice, including MADV_FREE, is rejected. |
| `HandleBrkRequest(frame)` | `brk` | Query or resize the process heap and return the resulting break, retaining the old break on failure. |

### VfsRequestManager

Handles Linux path, file I/O, descriptor, directory, metadata, mount, pipe, and
memory-file syscalls. Combines VFS operations, Resource descriptor/process state,
and IPC services.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `VfsRequestManager(resourceLayerCaps, logicLayerCaps)` | — | Bind descriptor/process resources and Logic VFS/IPC services. |

#### Opening and descriptor control

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `HandleOpenatRequest(frame)` | `openat` | Open/create a file, resolving a relative path against a directory descriptor or the current directory. |
| `HandleOpenRequest(frame)` | `open` | Open/create a file by path using the current directory for relative names. |
| `HandleCreatRequest(frame)` | `creat` | Create or truncate a file and open it for writing. |
| `HandleCloseRequest(frame)` | `close` | Remove one descriptor and drop its owned file reference. |
| `HandleClose_rangeRequest(frame)` | `close_range` | Close a descriptor interval or mark it close-on-exec; the accepted UNSHARE flag currently does not copy the table. |
| `HandleFcntlRequest(frame)` | `fcntl` | Handle supported descriptor duplication/flags, file status flags, and backend controls such as pipe size or memory-file seals. |
| `HandleDupRequest(frame)` | `dup` | Create another descriptor for the same open file, sharing its file position. |
| `HandleDup2Request(frame)` | `dup2` | Duplicate into a specified descriptor, replacing an existing entry; identical source/target is allowed. |
| `HandleDup3Request(frame)` | `dup3` | Duplicate into a different specified descriptor with supported close-on-exec flags. |
| `HandleIoctlRequest(frame)` | `ioctl` | Dispatch a descriptor-specific control request through its file operations. |

#### Data and directory I/O

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `HandleReadRequest(frame)` | `read` | Read into a user buffer from an open descriptor and return bytes transferred; zero indicates end-of-file. |
| `HandleWriteRequest(frame)` | `write` | Write user-buffer bytes to an open descriptor and return the transferred count. |
| `HandlePread64Request(frame)` | `pread64` | Read at an explicit file offset without changing the shared file position. |
| `HandlePwrite64Request(frame)` | `pwrite64` | Write at an explicit file offset without changing the shared file position. |
| `HandleReadvRequest(frame)` | `readv` | Read sequentially into multiple user buffers described by an iovec array. |
| `HandleWritevRequest(frame)` | `writev` | Write sequentially from multiple user buffers described by an iovec array. |
| `HandlePreadvRequest(frame)` | `preadv` | Read into an iovec array at an explicit offset without advancing the shared file position. |
| `HandlePwritevRequest(frame)` | `pwritev` | Write from an iovec array at an explicit offset without advancing the shared file position. |
| `HandleLseekRequest(frame)` | `lseek` | Change an open file's position relative to the start, current position, or end, returning the new offset. |
| `HandleGetcwdRequest(frame)` | `getcwd` | Copy the absolute current-directory path and return its byte length including the terminating NUL. |
| `HandleChdirRequest(frame)` | `chdir` | Replace the shared current working directory using a directory path. |
| `HandleFchdirRequest(frame)` | `fchdir` | Replace the shared current working directory using an open directory descriptor. |
| `HandleGetdents64Request(frame)` | `getdents64` | Copy packed Linux directory-entry records from an open directory, advancing its iteration position. |

#### Paths and links

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `HandleMkdiratRequest(frame)` | `mkdirat` | Create a directory relative to a directory descriptor or the current directory. |
| `HandleMkdirRequest(frame)` | `mkdir` | Create a directory by path, applying creation mode and filesystem policy. |
| `HandleUnlinkatRequest(frame)` | `unlinkat` | Remove a directory-relative name; the supported remove-directory flag selects directory removal. |
| `HandleUnlinkRequest(frame)` | `unlink` | Remove a non-directory pathname; open-file references can outlive the removed name. |
| `HandleRmdirRequest(frame)` | `rmdir` | Remove a directory through the directory-removal path. |
| `HandleRenameatRequest(frame)` | `renameat` | Rename/move an entry using source and destination directory descriptors. |
| `HandleRenameRequest(frame)` | `rename` | Rename/move an entry by source and destination paths. |
| `HandleReadlinkatRequest(frame)` | `readlinkat` | Copy a directory-relative symlink's target bytes without adding a terminating NUL. |
| `HandleReadlinkRequest(frame)` | `readlink` | Copy a symlink target by path without adding a terminating NUL. |
| `HandleLinkatRequest(frame)` | `linkat` | Create a hard link with directory-relative source/destination and supported symlink handling. |
| `HandleLinkRequest(frame)` | `link` | Create a hard link using source and destination paths. |
| `HandleSymlinkatRequest(frame)` | `symlinkat` | Create a symbolic link with a directory-relative destination name. |
| `HandleSymlinkRequest(frame)` | `symlink` | Create a symbolic link containing the supplied target string. |
| `HandleMknodatRequest(frame)` | `mknodat` | Create a supported special node relative to a directory descriptor. |
| `HandleMknodRequest(frame)` | `mknod` | Create a supported special node by path. |

#### Metadata, access, and mounts

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `HandleNewfstatatRequest(frame)` | `newfstatat` | Copy file metadata for a directory-relative path with supported symlink/empty-path flags. |
| `HandleStatRequest(frame)` | `stat` | Copy metadata for a pathname, following its final symlink. |
| `HandleFstatRequest(frame)` | `fstat` | Copy metadata for an open descriptor. |
| `HandleLstatRequest(frame)` | `lstat` | Copy metadata for a pathname without following its final symlink. |
| `HandleFaccessatRequest(frame)` | `faccessat` | Check requested access for a directory-relative path using the legacy no-flags form. |
| `HandleFaccessat2Request(frame)` | `faccessat2` | Check requested path access with supported symlink/empty-path resolution; AT_EACCESS is accepted without changing identity selection. |
| `HandleAccessRequest(frame)` | `access` | Check path existence or requested mode using the current simplified owner-permission-bit check. |
| `HandleFchownatRequest(frame)` | `fchownat` | Change file ownership using directory-relative resolution and supported symlink/empty-path flags. |
| `HandleChownRequest(frame)` | `chown` | Change ownership of a pathname's target. |
| `HandleFchownRequest(frame)` | `fchown` | Change ownership through an open descriptor. |
| `HandleLchownRequest(frame)` | `lchown` | Change ownership without following the final symlink. |
| `HandleFchmodat2Request(frame)` | `fchmodat2` | Change permission bits for a directory-relative target with supported flags. |
| `HandleFchmodatRequest(frame)` | `fchmodat` | Change permission bits using the legacy directory-relative interface. |
| `HandleChmodRequest(frame)` | `chmod` | Change permission bits of a pathname's target. |
| `HandleFchmodRequest(frame)` | `fchmod` | Change permission bits through an open descriptor. |
| `HandleMountRequest(frame)` | `mount` | Apply a supported mount, remount, or bind operation after copying paths/options and checking caller privilege. |
| `HandleStatfsRequest(frame)` | `statfs` | Copy filesystem statistics for the filesystem containing a path; current values are synthesized by the VFS request code. |
| `HandleFstatfsRequest(frame)` | `fstatfs` | Copy the synthesized filesystem statistics through an open descriptor. |
| `HandleTruncateRequest(frame)` | `truncate` | Resize a file by path, subject to backend growth/shrink support. |
| `HandleFtruncateRequest(frame)` | `ftruncate` | Resize a writable open file, subject to backend support. |

#### IPC descriptors

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `HandlePipeRequest(frame)` | `pipe` | Create a pipe's read/write descriptors and copy them to user memory. |
| `HandlePipe2Request(frame)` | `pipe2` | Create pipe descriptors with supported nonblocking/close-on-exec flags. |
| `HandleMemfd_createRequest(frame)` | `memfd_create` | Create an anonymous memory-backed file with a user-supplied name and supported flags. |

The access family's final permission check currently tests inode owner bits;
it does not implement real/effective credential selection. `AT_EACCESS` is accepted
without changing that check. `close_range` accepts `CLOSE_RANGE_UNSHARE` without
implementing table copying. These are current compatibility limits.

### EventRequestManager

Handles Linux readiness polling/select, epoll, eventfd, inotify, and signalfd
syscalls. Uses EventManager and coordinates temporary signal masks for masked waits.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `EventRequestManager(resources, logic)` | — | Bind process/time resources and Logic event/signal services. |
| `HandlePollRequest(frame)` | `poll` | Wait for readiness in a user pollfd array using a millisecond timeout. |
| `HandlePpollRequest(frame)` | `ppoll` | Wait on a pollfd array using a timespec timeout and optional temporary signal mask. |
| `HandleSelectRequest(frame)` | `select` | Wait on read/write/exception descriptor sets using the legacy timeval timeout. |
| `HandlePselect6Request(frame)` | `pselect6` | Wait on descriptor sets using a timespec timeout and optional signal-mask argument bundle. |
| `HandleEpoll_createRequest(frame)` | `epoll_create` | Create an epoll descriptor after validating the legacy positive size hint. |
| `HandleEpoll_create1Request(frame)` | `epoll_create1` | Create an epoll descriptor with supported creation flags. |
| `HandleEpoll_ctlRequest(frame)` | `epoll_ctl` | Add, modify, or delete a descriptor watch in an epoll set. |
| `HandleEpoll_waitRequest(frame)` | `epoll_wait` | Wait for epoll events and copy ready records to user memory, using a millisecond timeout. |
| `HandleEpoll_pwaitRequest(frame)` | `epoll_pwait` | Wait for epoll events with an optional temporary signal mask. |
| `HandleEpoll_pwait2Request(frame)` | `epoll_pwait2` | Wait for epoll events using a timespec timeout and optional temporary signal mask. |
| `HandleInotify_initRequest(frame)` | `inotify_init` | Create a filesystem-notification descriptor with default flags. |
| `HandleInotify_init1Request(frame)` | `inotify_init1` | Create a filesystem-notification descriptor with supported nonblocking/close-on-exec flags. |
| `HandleInotify_add_watchRequest(frame)` | `inotify_add_watch` | Add or update a pathname watch and return its watch identity. |
| `HandleInotify_rm_watchRequest(frame)` | `inotify_rm_watch` | Remove a watch from a filesystem-notification descriptor. |
| `HandleEventfdRequest(frame)` | `eventfd` | Create a readable/writable event counter initialized to the supplied value. |
| `HandleEventfd2Request(frame)` | `eventfd2` | Create an event counter with supported semaphore, nonblocking, and close-on-exec flags. |
| `HandleSignalfdRequest(frame)` | `signalfd` | Create or update a descriptor that exposes selected pending signals as readable records. |
| `HandleSignalfd4Request(frame)` | `signalfd4` | Create/update a signal descriptor with supported nonblocking/close-on-exec flags. |

### SyncRequestManager

Handles Linux futex and robust-list syscalls. Decodes operations, alignment, and
timeouts before calling SynchronizationManager and TimeManager.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `SyncRequestManager(resources, logic)` | — | Bind Resource time services and Logic synchronization services. |
| `HandleFutexRequest(frame)` | `futex` | Decode supported atomic user-word wait, wake, requeue, and priority-inheritance operations and optional timeouts. |
| `HandleSet_robust_listRequest(frame)` | `set_robust_list` | Register the calling thread's robust futex-list head for exit cleanup; require the supported 24-byte header size. |
| `HandleGet_robust_listRequest(frame)` | `get_robust_list` | Copy an authorized thread's robust-list head pointer and header length to user memory. |

### SignalRequestManager

Handles Linux signal actions, masks, sending/queuing, alternate stacks, pause,
and signal-return syscalls. SignalManager owns queue/delivery policy and uses
architecture hooks for saved user contexts.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `SignalRequestManager(resources, logic)` | — | Bind Logic signal services; the Resource constructor argument is unused. |
| `HandleRt_sigreturnRequest(frame)` | `rt_sigreturn` | Restore registers, signal mask, alternate-stack state, and saved result from a validated user signal frame. |
| `HandleRt_sigprocmaskRequest(frame)` | `rt_sigprocmask` | Query/change the calling thread's blocked-signal mask; SIGKILL and SIGSTOP remain unblocked. |
| `HandleRt_sigactionRequest(frame)` | `rt_sigaction` | Query/change a signal handler and its flags/mask using the supported signal-set size. |
| `HandleSigaltstackRequest(frame)` | `sigaltstack` | Query/change the calling thread's alternate stack for signal handlers. |
| `HandleKillRequest(frame)` | `kill` | Send a signal to a permitted process or process-group selection; zero checks target/permission without delivery. |
| `HandleTgkillRequest(frame)` | `tgkill` | Send a signal to one thread while verifying its process identity. |
| `HandleQueuedSignalRequest(frame)` | `rt_sigqueueinfo` | Send a process-directed signal with supported user-supplied sender/value information. |
| `HandleQueuedSignalRequest(frame)` | `rt_tgsigqueueinfo` | Send queued sender/value information to a specific thread in a specified process. |
| `HandlePauseRequest(frame)` | `pause` | Wait until a deliverable signal interrupts the calling task. |

Queued signal information currently supports the kill/queue sender-value layout;
other siginfo union layouts are rejected.

### CredentialRequestManager

Handles Linux user/group identity and supplementary-group syscalls. Uses
CredentialManager for credential rules and copies ABI results to user memory.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `CredentialRequestManager(resourceLayerCaps, logicLayerCaps)` | — | Bind Logic credential services and Resource support for group-buffer copies. |
| `HandleGetuidRequest(frame)` | `getuid` | Return the caller's real user ID. |
| `HandleGetgidRequest(frame)` | `getgid` | Return the caller's real group ID. |
| `HandleGeteuidRequest(frame)` | `geteuid` | Return the caller's effective user ID used for privilege checks. |
| `HandleGetegidRequest(frame)` | `getegid` | Return the caller's effective group ID. |
| `HandleGetresuidRequest(frame)` | `getresuid` | Copy the real, effective, and saved user IDs to three user outputs. |
| `HandleGetresgidRequest(frame)` | `getresgid` | Copy the real, effective, and saved group IDs to three user outputs. |
| `HandleSetuidRequest(frame)` | `setuid` | Apply permitted user-ID changes, with different rules for privileged and unprivileged callers. |
| `HandleSetgidRequest(frame)` | `setgid` | Apply permitted group-ID changes under credential rules. |
| `HandleSetreuidRequest(frame)` | `setreuid` | Change selected real/effective user IDs, updating the saved identity as required. |
| `HandleSetregidRequest(frame)` | `setregid` | Change selected real/effective group IDs under the corresponding saved-ID rules. |
| `HandleSetresuidRequest(frame)` | `setresuid` | Change selected real/effective/saved user IDs; an all-ones argument leaves that field unchanged. |
| `HandleSetresgidRequest(frame)` | `setresgid` | Change selected real/effective/saved group IDs; an all-ones argument leaves that field unchanged. |
| `HandleSetfsuidRequest(frame)` | `setfsuid` | Attempt to replace the filesystem user ID and return its previous value. |
| `HandleSetfsgidRequest(frame)` | `setfsgid` | Attempt to replace the filesystem group ID and return its previous value. |
| `HandleGetgroupsRequest(frame)` | `getgroups` | Return supplementary-group count and optionally copy the group list to user memory. |
| `HandleSetgroupsRequest(frame)` | `setgroups` | Replace the supplementary-group list after privilege, count, and user-buffer validation. |

### SystemRequestManager

Handles Linux system information, process controls/names, resource limits, umask,
architecture controls, and random-data requests. Uses Resource services and the
shared resource-limit policy helper; random-data requests currently return
`-ENOSYS`.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `SystemRequestManager(resourceLayerCaps)` | — | Bind Resource system, process, time, memory, and task services. |
| `HandleArch_prctlRequest(frame)` | `arch_prctl` | Get/set the calling x86_64 thread's FS/GS user TLS base; other architectures return -ENOSYS. |
| `HandleUmaskRequest(frame)` | `umask` | Replace the shared file-creation permission mask and return its previous value. |
| `HandlePrlimit64Request(frame)` | `prlimit64` | Read/change a selected process's supported resource limits, optionally returning previous limits; only CPU and descriptor limits are supported. |
| `HandleGetrlimitRequest(frame)` | `getrlimit` | Copy the caller's selected supported resource limit to user memory. |
| `HandleSetrlimitRequest(frame)` | `setrlimit` | Replace the caller's selected supported resource limit from user memory. |
| `HandlePrctlRequest(frame)` | `prctl` | Get/set the calling task's name through PR_GET_NAME/PR_SET_NAME; other controls are unsupported. |
| `HandleUnameRequest(frame)` | `uname` | Copy system identification strings using the Linux utsname layout. |
| `HandleSysinfoRequest(frame)` | `sysinfo` | Copy uptime, load, memory, and live-task statistics using the Linux sysinfo layout. |
| `HandleGetrandomRequest(frame)` | `getrandom` | Validate request flags, then return -ENOSYS because no entropy source or CSPRNG is implemented. |

### SocketRequestManager

Handles Linux socket creation, binding, connection, listening/acceptance, message
transfer, shutdown, address queries, and option syscalls. SocketManager validates
user buffers and implements the supported local transport.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `SocketRequestManager(resources, logic)` | — | Capture SocketManager when required process/Logic services exist; otherwise handlers return -ENOSYS. |
| `HandleSocketRequest(frame)` | `socket` | Create a supported Unix-domain stream, datagram, or sequenced-packet socket and return its descriptor. |
| `HandleSocketpairRequest(frame)` | `socketpair` | Create two connected local socket endpoints and copy their descriptors to user memory. |
| `HandleBindRequest(frame)` | `bind` | Assign a supported local pathname/abstract address to a socket endpoint. |
| `HandleConnectRequest(frame)` | `connect` | Connect a local socket to a peer, applying blocking/nonblocking connection behavior. |
| `HandleListenRequest(frame)` | `listen` | Make a connection-oriented socket accept incoming peers, with a bounded backlog. |
| `HandleAccept4Request(frame)` | `accept4` | Accept a connection with supported nonblocking/close-on-exec flags on the new descriptor. |
| `HandleAcceptRequest(frame)` | `accept` | Accept a pending connection and optionally return its peer address, using default creation flags. |
| `HandleRecvfromRequest(frame)` | `recvfrom` | Receive bytes into user memory and optionally copy the source address. |
| `HandleRecvmsgRequest(frame)` | `recvmsg` | Receive into user iovecs and copy supported ancillary credentials or transferred descriptors. |
| `HandleSendtoRequest(frame)` | `sendto` | Send user-buffer bytes to a connected or explicitly addressed local peer. |
| `HandleSendmsgRequest(frame)` | `sendmsg` | Send user iovecs and supported ancillary credentials or file descriptors. |
| `HandleShutdownRequest(frame)` | `shutdown` | Disable the selected read/write directions and notify affected peers/waiters. |
| `HandleGetsockoptRequest(frame)` | `getsockopt` | Copy a supported socket option and its result length to user memory. |
| `HandleSetsockoptRequest(frame)` | `setsockopt` | Read and apply a supported socket option from user memory. |
| `HandleGetsocknameRequest(frame)` | `getsockname` | Copy the socket's local bound address to user memory. |
| `HandleGetpeernameRequest(frame)` | `getpeername` | Copy the connected peer's address to user memory. |

### InterruptRequestManager

Handles common kernel interrupt requests, not Linux syscalls. Scheduling request
`1` coordinates sleeper wakeups and ready-task scheduling through the published
Dispatcher; unknown requests are ignored. Hardware vectors and acknowledgment
remain in architecture code.

| Function | Linux syscall | What it does |
| --- | --- | --- |
| `InterruptRequestManager()` | — | Initialize per-CPU scheduling tick counters. |
| `HandleInterruptRequest(requestNumber)` | — | Dispatch common scheduling request 1; ignore unknown request numbers. |

## 3. Capabilities, structures, and ownership

| Structure | Meaning and lifetime |
| --- | --- |
| `RequestLayerCaps` | Factory-owned pointers to the twelve managers, borrowed by Dispatcher and tests. |
| `ResourceLayerCaps`, `LogicLayerCaps` | Borrowed lower-layer services; Request does not own or delete them. |
| `arch_syscall_frame_t` | Caller-owned syscall number, six arguments, and saved return state, valid during dispatch. |
| `process_clone_arguments_t` | ProcessController's kernel representation of decoded process-creation arguments. |
| `user_memory_access_t` | Temporary pinned process/address-space context used by Klib's copy helpers. |

ABI records are converted where the operation needs them. Filesystem and system
information handlers define Linux-compatible output records; time handlers use
`time_timespec_t` and `time_timeval_t`. The layouts belong to the ABI rather than
being arbitrary copies of internal kernel objects.

The factory deletes its managers and capability structure when destroyed. Normal
boot keeps them alive for the kernel lifetime. Capability pointers provide access
to services, without memory isolation or reference-counted lifetime guarantees.
There is no coordinated Request shutdown or dependency-replacement protocol.

## 4. Syscall handling

Architecture entry code supplies a saved frame to Dispatcher. Its syscall switch
selects one of eleven syscall managers and forwards the frame. Interrupt requests
use a separate entry point and the twelfth manager.

Handlers interpret argument widths, signed values, flags, sizes, alignment, and
optional pointers according to the operation. They reject unsupported inputs,
prepare kernel-side records, and delegate work. Validation is shared with the
lower-layer service: some handlers perform user copies themselves, while others
pass user addresses to a collaborator that owns validation and copying.

Handlers usually forward the collaborator's status or result. Where needed, they
convert internal records and copy outputs back to user memory. Return types are
`uint64_t`; negative Linux-style errors retain their signed bit pattern. Success
may be zero, a byte count, an identifier, an address, or another operation-specific
value. Program-break requests return the resulting break rather than following a
uniform zero-or-error convention.

Dispatcher returns the result to architecture code. On x86_64, the return path
services pending scheduling and signal delivery before accounting user execution
and restoring machine state. Execution and signal-return operations can modify
the saved frame through their collaborators; it is not an immutable snapshot.

## 5. User memory and synchronization

Klib supplies `safe_read_user`, `safe_write_user`, and `safe_read_user_string` in
`klib.c`. They acquire a current-process memory context through Dispatcher and
MemoryRequestManager's stateless kernel-service entry, check
the canonical user range and page permissions, and copy through physical-page
aliases. Writable copy-on-write pages are resolved as part of validation.
The string helper requires a terminator within the supplied capacity.

During a copy, the process is pinned, preemption is disabled, and the address-space
operation lock is held. The helpers release that context before returning.
Code using the explicit locked helpers must pair a successful `begin_user_memory`
with `end_user_memory`, and must not keep that context across a sleep or switch.
A pointer accepted during one copy is not guaranteed to remain valid for a later
copy. Multiple output writes are not an atomic transaction; a later fault can
leave earlier outputs changed.

Request has no single lock around all handlers. Descriptor operations use
`FileDescriptorGuard` where needed; it protects the process's descriptor table and
disables preemption for its scope. Task lookups can use retained dispatch claims,
which must be released after use. Other synchronization belongs to the relevant
Logic or Resource manager. Blocking operations must obey those services' calling
constraints and cannot be treated as interrupt-safe entry points.

Kernel-service ingress also uses typed Request entry methods without fabricating
a syscall frame. MemoryRequestManager handles user-memory begin/end, minor-fault
accounting, and routed backing write/release; SchedulerRequestManager handles
mode accounting and pending reschedules; SignalRequestManager handles signal
delivery on user return. These methods do not require factory-created Request
instances, so Resource tests retain their early-boot access. A service needing
Logic still checks that its published capabilities are available.

## 6. Interrupt requests

`InterruptRequestManager.hpp` defines scheduling request `1`. Architecture code
submits this common request through Dispatcher rather than passing a raw hardware
vector. The manager maintains per-CPU tick counters; the current scheduling
interval is one request.

For a valid CPU and available services, it wakes sleeping tasks, checks whether
scheduling is active, clears the pending-reschedule flag, handles an exited user
task, and queues/runs ready work. Unknown requests are ignored. Hardware interrupt
acknowledgment and safe preemption boundaries remain in architecture code.

## 7. Errors and current limits

| Condition | Typical result |
| --- | --- |
| Invalid arguments, flags, layout size, or alignment | `-EINVAL`. |
| Failed user-memory copy | `-EFAULT`. |
| Required current process or task is missing | `-ESRCH`. |
| Missing service or unrecognized syscall | `-ENOSYS`. |
| Recognized but unsupported operation | Operation-specific error, including `-EOPNOTSUPP` or `-EINVAL`. |
| Interrupted blocking operation | `-EINTR`, with restart/signal behavior handled by collaborators and the return path. |

These are common conventions, not a universal validation order. Some handlers
accept a null frame for operations needing no arguments or applying a default;
Dispatcher itself rejects a null syscall frame. The syscall switch assumes its
selected manager exists in the completed factory output.

The syscall constants and Linux-style records do not imply complete Linux support.
Supported flags and suboperations are checked by each handler and collaborator.
`getrandom` currently returns `-ENOSYS` because no entropy source or CSPRNG exists.

The implemented user syscall entry/return path is x86_64. `arch_prctl` has an
x86_64 implementation and returns `-ENOSYS` outside that architecture. AArch64
still has stubbed context switching and user entry; its signal preparation and
restoration return `-EOPNOTSUPP`. A common frame declaration alone does not provide
a working AArch64 syscall path.

## 8. Self-tests and inspection

`SelfTestManager.cpp` invokes the Request group during post-init on the bootstrap
CPU. `requesttests.cpp` checks capabilities and runs user-copy coverage plus suites
for all eleven syscall managers. There is no separate Interrupt manager suite in
that group.

Each syscall manager has a corresponding test file:

| Area | Test files |
| --- | --- |
| Processes and scheduling | `ProcessRequestManagerTests.cpp`, `SchedulerRequestManagerTests.cpp`, `TimeRequestManagerTests.cpp`. |
| Memory and files | `MemoryRequestManagerTests.cpp`, `VfsRequestManagerTests.cpp`. |
| Events and synchronization | `EventRequestManagerTests.cpp`, `SyncRequestManagerTests.cpp`, `SignalRequestManagerTests.cpp`. |
| Identity and system services | `CredentialRequestManagerTests.cpp`, `SystemRequestManagerTests.cpp`, `SocketRequestManagerTests.cpp`. |

`RequestTestSupport.hpp` and `RequestTestSupport.cpp` provide shared fixtures,
syscall dispatch helpers, and temporary real user mappings. The fixtures preserve
the production user-page checks instead of allowing kernel pointers through copy
helpers. `SignalAbiFixture.hpp` supplies signal ABI fixtures.

`syscalltests.cpp` also contains ABI and syscall-test code, but is not listed in
the current Makefile self-test sources. The active manager group is assembled by
`requesttests.cpp`. Start with `Dispatcher.cpp` to identify
the selected handler, then inspect that manager, its lower-layer collaborator,
and its test file. `debug-kernel.sh` and `gdb_extention.py` provide the existing
debugger workflow.
