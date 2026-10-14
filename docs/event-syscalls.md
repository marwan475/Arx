# Event syscall implementation

October 13, 2026. Source implementation only: compilation, boot and runtime tests remain unverified.

## Implemented foundation

The approved logic-layer `EventManager` provides readiness scans, anonymous event counters and a shared notification generation. The logic factory constructs it after the scheduler and VFS and destroys it before those dependencies. The request factory supplies the manager through the logic capability set. Its source is registered in the common Makefile list.

The scheduler reuses its existing task sleep slots for event waiting, finite deadlines and process-exit wakeups. Waiters snapshot a generation before checking readiness. Parking rechecks that generation under the scheduler queue lock; producers increment it and wake matching parked tasks. A notification arriving between the scan and parking therefore causes a recheck rather than a missed wakeup. Infinite event waits do not require the monotonic clock; finite waits do.

VFS gains a readiness callback and retained-file user-I/O callbacks for blocking anonymous files. Eventfd reads/writes run outside descriptor-table and ordinary VFS I/O locks. A retained file reference protects the counter throughout a blocked operation. The counter lock protects state; it is released before waking or sleeping. User reads validate/copy their eight-byte result before consuming the counter.

Descriptors carry an optional generic release callback, registered by VFS-backed opens and eventfd creation and propagated by duplication. `ProcessManager` invokes these opaque handle destructors outside its manager lock during teardown, without importing a VFS or logic capability. This also covers scheduler retirement paths that bypass the existing normal-exit descriptor-close helper. Descriptors already closed by that helper have null handles and are not released again.

## Syscall status

Statuses describe the supported source behavior, not test execution.

| Syscall | Status | Supported behavior / limitation |
| --- | --- | --- |
| `poll` | Partial | Readiness, negative-fd skipping, unconditional error/hangup/invalid reporting, count limits, zero/finite/infinite waits. No signal interruption yet. |
| `ppoll` | Partial | Poll behavior with timespec timeout and raw remaining-time update. Non-null signal masks are rejected. |
| `select` | Partial | Native 64-bit fd sets, read/write/exception sets, ready-bit count, invalid-fd errors, timeval normalization/update and waits. No signal interruption. |
| `pselect6` | Partial | Select behavior with timespec timeout and raw signal argument-bundle decoding. Non-null signal masks are rejected. |
| `eventfd` | Partial | Shared 64-bit counter, blocking/nonblocking scalar read/write, readiness, duplication and cleanup. Vector I/O and signal interruption are missing. |
| `eventfd2` | Partial | Eventfd plus semaphore, NONBLOCK and CLOEXEC flags. Same limitations. |
| `epoll_create` | Blocked | Persistent interest/ready-queue implementation missing; returns `ENOSYS`. |
| `epoll_create1` | Blocked | Same dependency. |
| `epoll_ctl` | Blocked | Same dependency. |
| `epoll_wait` | Blocked | Same dependency. |
| `epoll_pwait` | Blocked | Epoll and atomic signal-mask support missing. |
| `epoll_pwait2` | Blocked | Same dependencies. |
| `inotify_init` | Blocked | Filesystem event queues and watch ownership missing. |
| `inotify_init1` | Blocked | Same dependency. |
| `inotify_add_watch` | Blocked | Filesystem mutation notifications and watch lifetime support missing. |
| `inotify_rm_watch` | Blocked | Same dependency. |
| `signalfd` | Blocked | Pending signals, signal masks and signal consumption missing. |
| `signalfd4` | Blocked | Same dependencies. |

All 18 handlers reject null internal frames with `EINVAL`. Unsupported handlers retain `ENOSYS` for ordinary calls. Missing dependencies on supported paths return `ENOSYS`.

## ABI and limits

The existing Linux x86-64 dispatch/frame convention is unchanged. `pollfd` size and result offset are asserted as eight and six bytes. Select sets use 64-bit words and are clamped to the current allocated descriptor-table capacity, independently of a lowered soft limit. Poll count is checked against `RLIMIT_NOFILE`. Poll copies only `revents` back; select returns the sum of ready bits across sets.

Raw `ppoll` and `pselect6` remaining-time updates are implemented. Read-only timeout storage does not turn a completed operation into `EFAULT`. Finite durations exceeding the existing time manager's nanosecond range return `EOVERFLOW`. Signal-mask size/buffer errors are validated; valid non-null masks return `EOPNOTSUPP` because atomic signal-mask replacement is absent.

Eventfd creation narrows its initial value to 32 bits. Reads require at least eight bytes and return eight; writes require exactly eight. Writes of `UINT64_MAX` are invalid; increments that exceed `UINT64_MAX - 1` wait or return `EAGAIN`. Normal reads drain the counter; semaphore reads remove one. Creation and descriptor-limit failures release the new object. CLOEXEC is a descriptor flag; actual exec closing remains dependent on the existing exec implementation. `F_SETFL` on these anonymous files currently supports only NONBLOCK and preserves the access mode. Other status-flag changes return `EOPNOTSUPP`.

Readiness/timeout rules were compared with [Linux select/poll](https://github.com/torvalds/linux/blob/master/fs/select.c); counter transfer rules were compared with [Linux eventfd](https://github.com/torvalds/linux/blob/master/fs/eventfd.c). One explicit difference: a failed eventfd read output copy preserves the counter in Arx, whereas Linux consumes it before its output copy. Eventfd vector I/O returns `EOPNOTSUPP`; positioned I/O returns `ESPIPE`; ordinary seek is a no-op.

## Dedicated tests

All six supported syscalls have separate `custom_case_validate_event<SYSCALL_...>` registrations in the existing `requestlayer_selftest`. **None has been compiled or executed here.**

| Tests | Coverage |
| --- | --- |
| `poll`, `ppoll` / `test_event_wait` | Empty-counter timeout, preserved input fields, ignored negative fd, invalid-fd readiness, counter read/write readiness, regular-file readiness at EOF, real parked wait with producer, count limits and bad/readonly buffers. |
| `select`, `pselect6` / `test_event_wait` | Cross-page fd sets, ready-bit counting, EBADF, descriptors above a lowered soft limit, regular-file EOF, producer wakeup, negative count and bad/readonly buffers. |
| Precise wait variants | Signal bundle/mask validation, unsupported mask replacement, invalid timespecs and finite timeout writeback. |
| All wait variants | Generation change before parking; finite empty wait if a monotonic clock exists. Clock unavailability is explicitly logged. |
| `eventfd`, `eventfd2` / `test_event_counter` | Initial counter, eight-byte wire transfers, NONBLOCK through fcntl, CLOEXEC, full counter and overflow, invalid lengths/values/buffers, output-fault preservation, dup/close sharing, blocked reads and writes, limits and cleanup. |
| `eventfd2` | Semaphore decrement and invalid flags. |
| `event_with_peer` | A real second process/task performs I/O through the dispatcher and actual user mappings; checks parked-task/process lifetime, peer teardown and release of the shared file reference. |
| All 18 handlers | Null-frame and missing-dependency checks. Unsupported handlers remain classified as unimplemented. |

Event custom validators own the real dispatcher invocation. The framework skips its preliminary zero-argument call for these cases: otherwise empty ppoll/select probes would block indefinitely and eventfd probes would allocate stray descriptors. This is not a mocked syscall implementation. Tests use the existing isolated process/address-space fixture and cleanup conventions. Blocking test regressions can require an external boot-test timeout; forced allocator failure and cross-CPU stress have not been executed.

## Remaining work

### Immediate blockers

| Feature / status | Impact / priority | Location and required change | Smallest next step |
| --- | --- | --- | --- |
| Compilation/runtime: pending | C++ integration and scheduling behavior unverified; P1 | Build registration and existing kernel selftests. | Build/boot using the commands below and inspect event test output. |
| Signals: absent | No EINTR/restart behavior or atomic masks; signalfd blocked; P1 | Signal request manager, task signal state and scheduler wait cancellation. | Implement pending signals and mask semantics before adding interruptible event waits. |

### Compatibility gaps

| Feature / status | Impact / priority | Location and required change | Smallest next step |
| --- | --- | --- | --- |
| Epoll: blocked | Applications requiring persistent event registrations remain unsupported; P1 | Event manager and anonymous-file operations. Needs interest ownership, ready queues, close/lifetime handling, level/edge/one-shot semantics and nesting rules. | Implement and test a clearly bounded level-triggered subset using the new readiness/wait primitives. |
| Inotify: blocked | No filesystem watches; P2 | VFS/resource mutation operations and event-file queues. Needs stable watch references, notification records, queue overflow and watch cleanup. | Add mutation notifications at successful VFS operations. |
| Eventfd vector I/O: unsupported | readv/writev callers receive an explicit error; P2 | VFS vector request path and anonymous user-I/O transaction. | Add an atomic eight-byte gather/scatter transaction without holding locks across sleep. |
| Output-fault consumption: differs from Linux | An eventfd counter survives a bad read output; P2 | Counter read transaction. | Decide whether strict Linux fault-side-effect compatibility is needed before changing the validated-before-consumption behavior. |
| Driver readiness: limited to current synchronous files and counters | Future sockets/pipes/devices need real readiness masks and notification calls; P1 when adding those sources | File operations and producer state transitions. | Supply a Poll callback and notify after publishing each source's readiness changes. |
| Cross-CPU races/process exit: unexecuted | Source locking/lifetime review is not SMP runtime evidence; P1 validation | Scheduler event queues, retained I/O and process teardown. | Run repeated wake/exit/close tests on multiple CPUs. |
| Other anonymous-file status flags: unsupported | Eventfd F_SETFL currently rejects flags besides access bits/NONBLOCK; P2 | VFS fcntl path. | Add flag-specific semantics when required. |

### Optimizations

| Feature / status | Impact / priority | Location and required change | Smallest next step |
| --- | --- | --- | --- |
| Global wake generation: implemented broadcast | Unrelated counters can wake/recheck each other's waiters; P3 | Event manager and scheduler queues. | Measure before introducing per-source subscriptions. |
| Remote scheduling kick: timer-driven | A remote waiter becomes runnable immediately but resumes on its CPU's next scheduling opportunity; no new reschedule IPI; P3 | Scheduler notification path/platform IPI support. | Measure latency before adding a dedicated kick. |
| Per-call virtual buffers: unoptimized | Poll/select allocate temporary mapped buffers; P3 | Event request marshalling. | Measure allocation cost and add bounded small-buffer fast paths if useful. |

## Files changed for this request

| Files | Responsibility |
| --- | --- |
| `kernel/layers/Logic/EventManager.hpp`, `.cpp` | New event/counter implementation and shared poll wire layout. |
| `kernel/layers/Logic/Scheduler.hpp`, `.cpp` | Sequence-aware parking and wakeup using existing sleep slots. |
| `kernel/layers/Logic/VirtualFileSystem.hpp`, `.cpp` | Readiness, anonymous user-I/O hooks and generic file release thunk. |
| `kernel/layers/Logic/LogicLayerFactory.hpp`, `.cpp` | Manager lifecycle/export. |
| `kernel/layers/Request/EventRequestManager.hpp`, `.cpp` | Six implementations, signal/timeout/fd-set ABI decoding, explicit unsupported handlers. |
| `kernel/layers/Request/RequestLayerFactory.cpp` | Event dependencies. |
| `kernel/layers/Request/VfsRequestManager.cpp` | Retained anonymous-file scalar I/O, status flags and release-callback propagation. |
| `kernel/layers/Resource/ProcessManager.hpp`, `.cpp` | Optional opaque descriptor destructor and teardown outside manager locks. |
| `kernel/selftests/syscalltests.cpp` | Six dedicated validators and real scheduling peers. |
| `Makefile` | Common build registration. |
| `docs/event-syscalls.md` | This report. |

Earlier system and credential changes remain in the workspace.

## Validation

`git diff --check` passed. Source consistency checks found 18 handler declarations with exactly one definition each, six supported syscall test registrations and one EventManager build entry. Review traced counter synchronization, lock-free parking boundaries, file references, descriptor duplication/teardown, ABI marshalling and unsupported paths.

No compiler, make, NASM or QEMU command was available in PATH. There is no successful compilation, kernel execution or user-reported runtime result for this change.

On the configured Linux build host:

```sh
make x86_64 DEBUG=1
make qemu-x86_64
```

Inspect the six `event::Handle...` custom test results and existing scheduler/time tests. Twelve unsupported event handlers should remain reported as unimplemented.
