# Arx syscall support report

Source snapshot: October 15, 2026.

The current table contains **166 named syscalls**. Of these, **14 have no functional implementation**. The remaining calls have varying levels of support. This report primarily describes the x86-64 implementation; AArch64 has additional foundational gaps described below.

The selftest's **153 implemented** figure includes a user-copy fixture. It does not mean that 153 syscalls provide complete Linux-compatible behavior. Passing tests establish the supported scenarios, including deliberate rejection of unsupported operations.

## Syscalls with no functional implementation

The process calls below are stubs returning `ENOSYS`. `getrandom` validates flags but cannot produce random bytes.

| Syscalls | Kernel functionality still needed |
|---|---|
| `fork`, `vfork` | Process duplication, address-space inheritance or sharing, descriptor inheritance, and parent/child synchronization. |
| `clone`, `clone3` | Thread groups, clone-flag handling, shared resource ownership, child context and TLS setup, and TID publication. |
| `execve` | A complete executable replacement transaction: replace the address space, build arguments and environment, reset appropriate process state, and close `CLOEXEC` descriptors. The existing ELF loader is only part of this. |
| `wait4`, `waitid` | Zombie retention, child exit notifications, wait queues, status reporting, and reaping. |
| `set_tid_address` | Store the clear-child-TID address, clear it during thread exit, and wake futex waiters. |
| `getpgid`, `getpgrp`, `setpgid`, `getsid`, `setsid` | Process-group and session objects, membership tracking, and associated lifetime rules. |
| `getrandom` | Entropy collection, a seeded cryptographically secure random number generator, and readiness/blocking behavior. |

Sources: [ProcessRequestManager.cpp](../kernel/layers/Request/ProcessRequestManager.cpp), [SystemRequestManager.cpp](../kernel/layers/Request/SystemRequestManager.cpp).

## Syscalls with partial support

Some gaps require new kernel subsystems. Others are ABI and correctness work within existing managers.

| Syscalls or family | Working now | Missing support or required kernel work |
|---|---|---|
| `exit`, `exit_group` | Process termination and cleanup path. | Both use the same implementation. Linux thread-only exit versus whole-thread-group exit needs proper thread-group lifecycle support. |
| `mmap` | Eager private anonymous mappings, including supported fixed-address modes. | File-backed mappings, shared mappings, and huge pages. Needs shared backing objects, page ownership, fault handling, and mapping invalidation. |
| `munmap`, `mprotect`, `madvise`, `mincore` | Support around tracked anonymous mappings; limited handling of existing mappings. | General treatment of ELF, stack, and file-backed regions. `madvise` only accepts advice values 0 through 4; advanced advice needs paging, reclamation, and dirty-page tracking. |
| `memfd_create` | Anonymous file I/O, truncation, and basic seals. | No `mmap` integration; currently limited to 1 MiB per file. Huge pages and newer execution-policy flags are absent. |
| `futex` | Private wait/wake, bitset operations, requeue, compare-requeue, and wake-op. | Shared futex keys, priority inheritance, PI requeue, and other commands. Needs shared-memory identity and scheduler priority-inheritance support. |
| `set_robust_list`, `get_robust_list` | Registration and non-PI owner-death cleanup. | PI robust futexes; cross-process inspection needs an access-control or ptrace policy. |
| `rt_sigaction`, `kill`, `tgkill`, related signal calls | Ordinary signal handling on x86-64, masks, alternate stacks, and signal return. | Real-time signal queues, stop/continue job control, and group/broadcast delivery. |
| `signalfd`, `signalfd4` | Ordinary pending signals, readable records, blocking/nonblocking operation, and readiness notification. | Real-time queued signals and their full associated information; depends on the signal subsystem above. |
| `clock_gettime`, `clock_getres`, `clock_nanosleep` | Realtime and monotonic clocks. | CPU-time clocks and other clock types need additional clocks and accounting. |
| `getitimer`, `setitimer` | `ITIMER_REAL`; virtual/profiling timers can be queried or disarmed. | Arming `ITIMER_VIRTUAL` or `ITIMER_PROF` needs user/kernel CPU-time accounting. |
| `mount` | Registered-backend mounts, bind mounts, moves, and supported remounts. | Recursive bind mounts and propagation modes. The production filesystem remains boot cpio; ext4 is not installed. |
| VFS mutation calls: `mkdir*`, `unlink*`, `rename*`, `link*`, `mknod*`, `symlink*`, `chmod*`, `chown*`, `truncate*` | Backend callbacks and the supported anonymous-file operations. | Actual storage behavior depends on a backend. Boot cpio rejects most mutations. Named FIFO and device nodes also need filesystem-to-IPC/device integration. |
| `open*`, `creat`, `access`, `faccessat*` | Basic opens; native backends support creation, truncation, permissions, and creation attributes. | Complete pathname permission and flag handling. The access helper currently checks owner permission bits rather than selecting permissions from the caller's credentials. Legacy cpio open behavior is also less complete. |
| `stat*`, `statfs`, `fstatfs` | Basic inode metadata and ABI structures. | Several fields remain synthetic, including timestamps/device information and filesystem capacity/type reporting. These need backend metadata and statistics callbacks. |
| `fcntl`, `ioctl` | Descriptor flags, duplication, supported status flags, pipe sizing, seals, and selected object-specific ioctls. | Advisory/OFD file locking and most device/terminal controls need locking and driver subsystems. |
| `close_range` | Closing descriptors and marking them `CLOEXEC`. | `CLOSE_RANGE_UNSHARE` needs genuine descriptor-table detachment once shared descriptor tables exist. |
| `pipe`, `pipe2` | Byte streams, packet mode, blocking/nonblocking I/O, readiness, EOF, and `SIGPIPE`. | Notification/watch-queue pipes; dynamically accounted capacity beyond the current 64 KiB maximum. |
| `inotify_*` | Watches, common VFS events, rename cookies, overflow, and watch retirement. | `IN_EXCL_UNLINK` is explicitly unsupported. Full coverage still depends on complete backend event integration. |
| `epoll_*` | Core readiness, edge-triggered and one-shot behavior. | Unsupported advanced flags include `EPOLLEXCLUSIVE` and `EPOLLWAKEUP`, requiring additional waiter/power-management behavior. |
| Socket family, all 16 handlers | Local `AF_UNIX` stream/datagram/seqpacket operations, socket pairs, and abstract names. | Pathname sockets, ancillary descriptor/credential transfer, broader options, and timeout behavior. IPv4/IPv6 networking remains intentionally absent. |
| `getrlimit`, `setrlimit`, `prlimit64` | Current-process `RLIMIT_NOFILE`. | Other resource limits and cross-process operations need accounting, enforcement, and authorization. |
| `prctl` | `PR_SET_NAME`, `PR_GET_NAME`. | Other operations require their respective security, process-control, or architecture features. |
| `sched_getaffinity`, `getcpu` | Basic current-process CPU reporting. | Configurable affinity, other-task queries, and real NUMA-node reporting. |
| `arch_prctl` | x86-64 FS/GS base operations. | Other architecture-specific operations. |
| `sysinfo` | Uptime, RAM totals, and task count. | Load averages and shared/buffer/swap accounting are not populated. |

Relevant implementations:

- [ProcessMemoryManager.cpp](../kernel/layers/Resource/ProcessMemoryManager.cpp)
- [VirtualFileSystem.hpp](../kernel/layers/Logic/VirtualFileSystem.hpp) and [VirtualFileSystem.cpp](../kernel/layers/Logic/VirtualFileSystem.cpp)
- [VfsRequestManager.cpp](../kernel/layers/Request/VfsRequestManager.cpp)
- [InterProcessCommunicationManager.cpp](../kernel/layers/Logic/InterProcessCommunicationManager.cpp)
- [SynchronizationManager.cpp](../kernel/layers/Logic/SynchronizationManager.cpp) and [SyncRequestManager.cpp](../kernel/layers/Request/SyncRequestManager.cpp)
- [SignalManager.cpp](../kernel/layers/Logic/SignalManager.cpp)
- [EventManager.cpp](../kernel/layers/Logic/EventManager.cpp) and [EventNotifications.cpp](../kernel/layers/Logic/EventNotifications.cpp)
- [SocketManager.cpp](../kernel/layers/Logic/SocketManager.cpp)
- [TimeManager.cpp](../kernel/layers/Resource/TimeManager.cpp)
- [SystemRequestManager.cpp](../kernel/layers/Request/SystemRequestManager.cpp) and [SchedulerRequestManager.cpp](../kernel/layers/Request/SchedulerRequestManager.cpp)

## Linux syscalls absent from the Arx table

Many Linux syscalls are not declared or routed by Arx. They reach the dispatcher's default `ENOSYS` path. The following are significant examples, not an exhaustive Linux-wide list.

| Area | Missing entries |
|---|---|
| Storage | `umount2`, `fsync`, `fdatasync`, `sync`, `syncfs`, `fallocate`, `statx`, `openat2`, `renameat2`, `flock`, `utimensat`, and extended-attribute calls. |
| Memory and IPC | `mremap`, `msync`, memory-locking calls, and System V shared memory, semaphores, and message queues. |
| Timers and signals | `timerfd_*`, POSIX `timer_*`, `clock_settime`, `rt_sigpending`, `rt_sigtimedwait`, `rt_sigsuspend`, and queued-signal submission calls. |
| Processes and security | `execveat`, `sched_setaffinity`, scheduler policy/priority calls, `getrusage`, `times`, `ptrace`, `unshare`, `setns`, capabilities, and `seccomp`. |
| Advanced I/O | `sendfile`, `splice`, `tee`, `vmsplice`, `copy_file_range`, asynchronous I/O, `io_uring_*`, `sendmmsg`, and `recvmmsg`. |

Sources: [RequestLayerFactory.hpp](../kernel/layers/Request/RequestLayerFactory.hpp), [Dispatcher.cpp](../kernel/layers/Dispatcher.cpp).

## AArch64 limitations

AArch64 has additional foundational gaps. Its isolated user-address-space creation, strict user-page access checks, clock backend, and user signal contexts are still stubs. A successful AArch64 build therefore does not establish equivalent userspace syscall support.

Sources: [arch_paging.c](../arch/aarch64/arch_paging.c), [arch_signal.cpp](../arch/aarch64/arch_signal.cpp).

## Recommended implementation order

1. **Shared VM backing and page faults.** Establish page ownership, mapping lifetimes, shared mappings, and file-backed paging. This supports memfd mapping, shared futexes, and future process duplication.
2. **Process and thread lifecycle.** Implement creation, executable replacement, thread groups, exit distinctions, clear-child-TID handling, waits, and reaping. Add process groups and sessions for job control.
3. **Complete VFS metadata and permissions.** Finish pathname permission checks, flag handling, backend statistics, and metadata reporting.
4. **Storage infrastructure and ext4.** Add sleepable filesystem I/O, block storage, page cache/writeback, and the ext4 backend. Keep backend behavior behind the VFS interfaces.

Entropy collection and CSPRNG support for `getrandom` can proceed independently.
