# Logic layer

The Logic layer implements kernel behavior using Resource services. It decides
how processes run, how their memory and files behave, and how credentials, waits,
signals, sockets, and IPC interact. Request translates the syscall ABI into these
operations; Dispatcher and kernel code also call Logic directly.

`LogicLayerFactory.hpp` declares the exported capabilities, and
`LogicLayerFactory.cpp` constructs and connects the components. KernelArch.md
describes the stack, Resource.md explains the lower-layer mechanisms, and
Request.md covers syscall validation and dispatch.

## 1. Initialization and wiring

Dispatcher creates Logic after Resource and its self-tests. On first creation,
`LogicLayerFactory::Create` requires Resource capabilities and returns null if
they are absent. It constructs components in this order:

1. Scheduler, VirtualFileSystem, ElfMapper, and CredentialManager.
2. EventManager, using Scheduler and VFS; register it with both consumers.
3. SynchronizationManager, using Scheduler; register it as Scheduler's priority
   manager. Its constructor also installs an opaque Dispatcher priority-cleanup
   route in TaskManager.
4. SignalManager, then ProcessController with both Resource and Logic capabilities.
5. SocketManager, then InterProcessCommunicationManager, each using VFS, Event,
   and Signal services.

Scheduler construction establishes bootstrap process/task bookkeeping and prepares
idle contexts. Scheduling starts inactive. VFS construction initializes namespace
and cache state; creating it does not mount the root filesystem.

After construction, Dispatcher runs Logic self-tests and creates Request. Normal
boot mounts the root and activates scheduling later in `kernel.cpp`. `GetCaps`
returns null before creation; repeated `Create` calls return the existing
capabilities without rewiring their dependencies.

### LogicLayerFactory

| Function | What it does |
| --- | --- |
| `LogicLayerFactory()` | Initialize empty factory-owned capability/component state. |
| `~LogicLayerFactory()` | Delete owned components and capabilities; not a live-kernel shutdown protocol. |
| `Create(resourceLayerCaps)` | Construct and wire Logic using Resource imports, or return existing capabilities; first creation rejects null imports. |
| `GetCaps()` | Return borrowed capabilities, or null before creation. |

## 2. Components and public APIs

The sections follow factory construction order. Parameter names identify the
inputs; the public headers give their types and defaults. Cross-component calls
also follow the lifetime and locking rules later in this guide.

### Scheduler

Owns runnable-task selection, CPU placement, scheduling policy, sleeping/waking,
and execution accounting. `Scheduler.hpp` declares the API; `Scheduler.cpp` and
`SchedulerPolicies.cpp` implement it. `scheduling_attributes_t` describes requested
policy; `task_scheduling_t` stores effective task policy in Resource-owned state.

| Function | What it does |
| --- | --- |
| `Scheduler(resourceLayerCaps)` | Initialize queues, bootstrap bookkeeping, and idle task contexts; scheduling starts inactive. |
| `~Scheduler()` | Default destructor; it is not a coordinated scheduler shutdown. |
| `UserPriority(selector, who, modify, nice)` | Read/change priorities for the selected process, group, or user scope. |
| `ConfigurePolicy(task, attributes)` | Validate and apply normal, realtime, or deadline scheduling attributes. |
| `ReadPolicy(task, attributes)` | Copy a task's current configured policy attributes. |
| `ValidateClonePolicy(parent)` | Reject cloning a deadline task without reset-on-fork. |
| `InheritPolicy(parent, child)` | Initialize child policy, honoring reset-on-fork behavior. |
| `ApplyPriorityDonation(task, nice, policy, priority, donor)` | Apply effective priority/policy inherited from a waiting donor. |
| `RetirePolicy(task)` | Release scheduling reservations and clear policy references during retirement. |
| `AccountSchedulingCurrent(beginExecution)` | Mark scheduling-overhead and task-execution accounting boundaries. |
| `AcquireTask(tid, modify, target)` | Resolve an authorized target and retain its dispatch claim. |
| `SetUserNice(task, nice)` | Check caller privilege before changing a task's nice value. |
| `GetCpu(cpu, node)` | Report the calling CPU and optional NUMA node. |
| `SetPriorityManager(manager)` | Connect synchronization/priority-inheritance coordination. |
| `AvailableCpuMask()` | Build a CPU mask from the platform's configured CPU count. |
| `SetAffinity(task, mask)` | Apply a usable CPU mask, respecting scheduling/priority constraints. |
| `SetNice(task, nice)` | Update base nice and refresh effective priorities. |
| `ActivateScheduling()` | Enable scheduling on the tracked CPUs. |
| `SetEventManager(events)` | Connect event generation and wait coordination. |
| `IsSchedulingActive()` | Report whether scheduling is enabled on the calling CPU. |
| `RunSchedulingAlgorithm(cpuId, reason, publish, placement)` | Decide policy/placement and return a selected task claim without switching contexts. |
| `RequestReschedule(cpus)` | Mark target CPUs for rescheduling and issue required architecture notifications. |
| `ServicePendingReschedule(syscallBoundary)` | Service deferred scheduling at an allowed boundary. |
| `ScheduleProcess(processId)` | Validate a process and arrange execution through the scheduler. |
| `EnqueueProcess(cpuId, processId)` | Publish process work for policy/placement evaluation on a CPU. |
| `PublishTask(process, task)` | Attach a new task and publish it, undoing attachment if publication fails. |
| `RunNextReadyProcess(cpuId, reason)` | Select ready work and perform the corresponding scheduling transition. |
| `KillProcess(processId, exitStatus, onlyIfExited, retired)` | Request termination/retirement and optionally report whether resource retirement finished. |
| `SleepUntil(monotonicDeadline)` | Block the current task until a deadline or interruption. |
| `WaitForEvent(sequence, expected, monotonicDeadline, uninterruptible)` | Sleep while a stable sequence address still has the expected value. |
| `WakeEventWaiters(sequence)` | Wake tasks waiting on the supplied sequence address. |
| `WakeSignalWaiters(process)` | Wake affected waits when process/task signals become pending. |
| `WakeSleepingTasks(cpuId)` | Process expired sleeps and relevant timer work for a CPU. |
| `AccountCurrent(userMode)` | Charge current execution to user or kernel time. |
| `ExitCurrentTask()` | Retire the current task and switch away; never return. |
| `ReapExitedProcesses()` | Retry reclamation of exited processes after tasks become quiescent. |
| `NotifyProcessExit(process)` | Publish completed exit and wake/notify the parent once. |
| `ChangeStoppedState(process, signal, stopped)` | Update stop/continue state and notify scheduling/waiting participants. |

Debug builds publish scheduler CPU accounting, task policy/eligibility and indexed
sleep views, plus synchronization waiter/PI views. Manager-owned counters separate
API outcomes from committed wait/wake/requeue/donation transitions. Inspection
uses existing task/process slot identities and reports interrupted updates without
acquiring queue locks or predicting the next task. GdbCommands.md documents the
commands, bounds and native-reader contracts.

### VirtualFileSystem

Owns namespace traversal, mount registration, dentries/inodes, file operations,
and backend access rules. `VirtualFileSystem.hpp` defines the API and callback
records; `VirtualFileSystem.cpp` implements them. Registered filesystem descriptors
and backend identities must remain valid until teardown.

Debug views publish namespace, mount, lazy-cache, overlay and retained-entry
roots with operation outcomes. Visible cpio contents combine the actual backend
archive with runtime state and mount crossings, including uncached names.
Callback-only backends report known paths and incomplete enumeration. Private
Resource nodes are decoded only after native capability/operation verification;
VirtualFileSystemTypes.hpp owns their shared retained layouts. GdbCommands.md
describes path identities, precedence, bounds and native-reader semantics.

| Function | What it does |
| --- | --- |
| `ObserveFileLocked(file, observer)` | Add a weak final-close/readiness observer while holding `ReadinessLock`. |
| `UnobserveFileLocked(file, observer)` | Detach a weak observer under the same lock. |
| `NotifyReadiness(file, events)` | Publish source-specific readiness after producer state changes, outside its state lock. |
| `VirtualFileSystem(resourceLayerCaps)` | Initialize namespace, caches, locks, and Resource imports. |
| `SetEventManager(events)` | Connect filesystem/readiness notifications to EventManager. |
| `InitializeAnonymousInode(inode, type, mode)` | Initialize identity and metadata for an anonymous file's inode. |
| `NotifyFile(file, mask)` | Publish a file's filesystem-notification event with current namespace context. |
| `~VirtualFileSystem()` | Release owned cache/namespace state and registered mount state. |
| `HashDentryKey(parent, name)` | Compute the parent/name cache key. |
| `CacheDentry(dentry)` | Insert a dentry into the name cache. |
| `FindDentry(parent, name)` | Find an already-cached child without backend lookup. |
| `Lookup(parent, name)` | Find a child through the cache or backing filesystem. |
| `SetRootMount(rootMount)` | Install a valid root mount in the namespace. |
| `RegisterMount(mount)` | Add a mount to the tracked mount set. |
| `RegisterFileSystemType(type)` | Register persistent backend mount/unmount/remount callbacks. |
| `MountRootFileSystem(filesystemType, source)` | Build the root namespace from Resource filesystem capabilities. |
| `ResolvePath(start, path, result, accessError)` | Resolve a path across namespace components/mounts and report access failure when requested. |
| `BuildAbsolutePath(path, output, capacity)` | Reconstruct an absolute path from a mount/dentry pair. |
| `BuildAbsolutePathFromDentry(dentry, outPath, outPathSize)` | Reconstruct a path from cached dentry ancestry. |
| `JoinPath(base, path, outPath, outPathSize)` | Combine a base path and relative name within the output capacity. |
| `StatNoFollow(start, path, outInode, resolvedPath)` | Resolve metadata without following the final symlink. |
| `Symlink(start, target, linkPath)` | Create a symbolic link through the namespace/backend contract. |
| `Readlink(start, path, buffer, bufferSize)` | Copy a symlink target into a kernel buffer. |
| `Mkdir(start, path, mode)` | Create a directory with checked creation attributes. |
| `Unlink(start, path, directory)` | Remove a file or directory entry according to the requested kind. |
| `Rename(oldStart, oldPath, newStart, newPath)` | Rename/move an entry while updating namespace/cache state. |
| `Link(oldStart, oldPath, newStart, newPath, followSymlink)` | Create a hard link with the requested source-symlink treatment. |
| `Mknod(start, path, mode, device)` | Create a supported special node through its backend. |
| `SocketNode(path, create, inode)` | Create or resolve a socket pathname; creation returns an owned open-count claim. |
| `ReleaseSocketNode(inode)` | Release the pathname identity claim held by a bound endpoint. |
| `TruncatePath(start, path, size)` | Resolve and resize a file by path. |
| `TruncateFile(file, size)` | Resize an open file through its inode/backend. |
| `ChangeMetadata(inode, ownership, first, second, mount, dentry)` | Check and apply ownership or mode changes, updating cached metadata after success. |
| `Mount(source, target, type, flags, options)` | Perform a mount/remount/bind operation supported by the requested flags/backend. |
| `Open(start, path, flags, error, mode)` | Resolve/create an accessible file and return an owned open-file reference. |
| `Read(file, buffer, count)` | Read into kernel memory at the current file position. |
| `Write(file, buffer, count)` | Write kernel-buffer data at the current file position. |
| `ReadAt(file, buffer, count, offset)` | Perform positioned reading while preserving the shared file position. |
| `WriteAt(file, buffer, count, offset)` | Perform positioned writing while preserving the shared file position. |
| `Seek(file, offset, whence)` | Change the file position using backend seek rules. |
| `ReadDirectory(file, entry)` | Advance an open directory's iteration state. |
| `Ioctl(file, request, argument)` | Invoke a file's supported control operation. |
| `Mmap(file, address, length, prot, flags, offset)` | Invoke the file's mapping callback; this is separate from process mapping policy. |
| `AcquireMappingBacking(file, offset, length, protection, shared, object, maximumProtection)` | Acquire retained backing and mapping/protection reservations for a file range. |
| `ReleaseMappingBacking(object, shared, maximumProtection)` | Release those backing/reservation claims. |
| `Retain(file)` | Add a live open-file reference. |
| `CheckExecutable(file)` | Check whether the current credentials/mount permit execution. |
| `Poll(file)` | Return a nonblocking readiness snapshot. |
| `Close(file)` | Drop a file reference; final close detaches observers and releases owned state. |
| `ReleaseFileHandle(file)` | Adapt `Close` to the opaque descriptor release callback. |

The public callback records extend this interface. Filesystem callbacks receive
copied kernel strings; inode callbacks operate on stable backend identities.

| Callback | What it does |
| --- | --- |
| `filesystem_type_t::Mount` | Initialize a backend filesystem from its mount request. |
| `filesystem_type_t::Unmount` | Release backend state, including partially failed mount initialization. |
| `filesystem_type_t::Remount` | Apply supported mount-option/flag changes. |
| `inode_operations_t::Lookup` | Find one backend child inode. |
| `inode_operations_t::SetMetadata` | Persist the requested mode/owner fields atomically under inode I/O protection. |
| `inode_operations_t::Mkdir` | Create a backend directory with supplied creation attributes. |
| `inode_operations_t::Unlink` | Remove the requested file/directory entry. |
| `inode_operations_t::Rename` | Move/rename a backend entry between directories. |
| `inode_operations_t::Link` | Add a hard link to a backend inode. |
| `inode_operations_t::Mknod` | Create a backend special node. |
| `inode_operations_t::Symlink` | Create a backend symbolic link. |
| `inode_operations_t::Truncate` | Resize backend file storage. |
| `file_operations_t::Open` | Initialize backend state for an opened file. |
| `file_operations_t::Read` | Read bytes into a kernel buffer. |
| `file_operations_t::Write` | Write bytes from a kernel buffer. |
| `file_operations_t::Seek` | Change the backend file position. |
| `file_operations_t::ReadDirectory` | Return the next backend directory entry. |
| `file_operations_t::Ioctl` | Perform a supported device/file control request. |
| `file_operations_t::Mmap` | Perform the backend's optional file-mapping operation. |
| `file_operations_t::Release` | Release backend state on final file close. |
| `file_operations_t::Poll` | Return a nonblocking readiness snapshot without publishing events. |
| `file_operations_t::ReadUser` | Read directly into validated user memory using the file's transaction rules. |
| `file_operations_t::WriteUser` | Write from validated user memory using the file's transaction rules. |
| `file_operations_t::ReadVectorUser` | Read into user-address ranges described by kernel iovec records. |
| `file_operations_t::WriteVectorUser` | Write from user-address ranges described by kernel iovec records. |
| `file_operations_t::Truncate` | Resize an open backend file. |
| `file_operations_t::Control` | Apply supported descriptor-control operations. |
| `file_operations_t::GetBacking` | Return borrowed canonical backing and allowed mapping protections under the inode lock. |
| `file_operations_t::DropReference` | Atomically drop a file reference within the backend's ownership graph. |
| `file_close_observer_t::Closing` | Detach a weak observer during final close under readiness protection. |

Kernel-buffer I/O and user-buffer anonymous-file callbacks have different locking
contracts, as described below; these callbacks are not interchangeable. The
`linuxErrors` flag tells Request that file-operation errors already use Linux errno
values rather than the older backend's generic error convention.

### ElfMapper

Validates ELF headers/segments and maps images using Resource memory services.
`ElfMapper.hpp` declares parsed metadata and the API; `ElfMapper.cpp` implements it.
Mapped backing survives independently of metadata and the original file handle.

| Function | What it does |
| --- | --- |
| `ElfMapper(resourceLayerCaps)` | Bind Resource memory services. |
| `~ElfMapper()` | Default destructor; mapping/metadata ownership is handled by the callers. |
| `ReadElf(file, metadata)` | Validate an ELF image and populate parsed program/segment metadata. |
| `LoadExecutable(process, file, metadata)` | Map an executable image and initialize its process image/heap metadata. |
| `LoadLibrary(process, file, metadata, requestedLoadBias)` | Map an ET_DYN image at a chosen/free bias without linking dependencies or changing the heap. |

### CredentialManager

Applies identity/group-change rules to Resource process credentials.
`CredentialManager.hpp` defines change operations; `CredentialManager.cpp`
implements credential acquisition, permission checks, and replacement.

| Function | What it does |
| --- | --- |
| `CredentialManager(resourceLayerCaps)` | Bind ProcessManager for credential operations. |
| `GetCredentials(credentials)` | Copy the caller's user/group identity state into kernel memory. |
| `ChangeIdentity(group, change, real, effective, saved)` | Validate and apply the selected UID/GID identity change. |
| `GetGroups(groups, capacity, count)` | Report group count and copy supplementary groups when capacity permits. |
| `MaySetGroups()` | Check whether the caller may replace its supplementary groups. |
| `SetGroups(groups, count)` | Validate and publish a new immutable supplementary-group list. |

### EventManager

Connects file readiness and notifications to Scheduler waits. `EventManager.hpp`
defines poll/epoll records and the API; `EventManager.cpp` and
`EventNotifications.cpp` implement events and notification descriptors.

| Function | What it does |
| --- | --- |
| `EventManager(resources, scheduler, vfs)` | Bind collaborators and initialize event/observer state. |
| `CreateCounter(initial, flags)` | Install an eventfd-style counter descriptor. |
| `CreateInotify(flags)` | Install a filesystem-notification descriptor. |
| `AddWatch(fd, path, mask)` | Add/update an inotify watch for a user-supplied path. |
| `RemoveWatch(fd, watch)` | Remove a watch from its notification descriptor. |
| `SignalDescriptor(fd, mask, flags)` | Create or update a signalfd-style descriptor's selected signals. |
| `NotifyVfs(inode, mask, parent, name, cookie, unlinked)` | Publish a filesystem event to matching watches. |
| `NotifySignals(process)` | Refresh signal-descriptor readiness for a process. |
| `CreateEpoll(flags)` | Install an epoll descriptor. |
| `ControlEpoll(epfd, operation, fd, event)` | Add, modify, or remove a watched descriptor. |
| `WaitEpoll(epfd, address, count, deadline)` | Wait for ready events and copy them to user memory. |
| `Poll(descriptors, count, deadline)` | Fill kernel poll records with readiness, blocking when required. |
| `Notify()` | Advance the general event sequence and wake its waiters. |
| `NotifySource(file, events)` | Publish source-specific readiness to observers, including edge-triggered watches. |
| `Sequence()` | Read the current general event sequence. |
| `Wait(expected, deadline)` | Wait while the general sequence remains unchanged. |

### SynchronizationManager

Implements futex state, priority inheritance, robust-list retirement, and child-TID
wakeups. `SynchronizationManager.hpp` declares its bounded waiter/priority state;
`SynchronizationManager.cpp` implements it using Scheduler and Resource lifetimes.

| Function | What it does |
| --- | --- |
| `SynchronizationManager(resources, scheduler)` | Initialize waiter/priority tables and register task priority cleanup. |
| `Wait(address, expected, bits, deadline, shared, priorityDestination)` | Atomically validate/enroll a futex wait, optionally preparing priority requeue. |
| `Wake(address, count, bits, shared)` | Wake matching waiters on a private/shared futex key. |
| `Requeue(address, destination, wakeCount, moveCount, compare, expected, shared)` | Wake some waiters and move others, optionally checking the source value. |
| `WakeOperation(address, destination, wakeCount, secondCount, operation, shared)` | Apply an encoded atomic user-word operation and conditional wakeups. |
| `PriorityFutex(address, command, deadline, shared)` | Perform a supported priority-inheritance futex operation. |
| `RequeuePriority(address, destination, wakeCount, moveCount, expected, shared)` | Transfer eligible waiters toward a priority-inheritance futex. |
| `LockSchedulingChange(task, affinity)` | Hold priority coordination while checking a task scheduling/affinity change. |
| `UnlockSchedulingChange()` | Release successful scheduling-change coordination. |
| `RefreshPriorities()` | Recompute inherited priorities and request necessary rescheduling. |
| `SetRobustList(head)` | Register the current task's robust-list head and cleanup hook. |
| `ConfigureChildTid(task, owner, address)` | Retain memory ownership and arrange clear-child-TID cleanup/wakeup. |
| `GetRobustList(tid, head, length)` | Read an authorized task's robust-list registration. |

### SignalManager

Manages signal actions, masks, queues, and user delivery. `SignalManager.hpp`
defines signal records and the API; `SignalManager.cpp` coordinates Resource state,
Scheduler wakeups, and architecture signal frames. Queue helpers require the
target's `SignalDispositionGuard`.

| Function | What it does |
| --- | --- |
| `SignalManager(resources, scheduler)` | Bind process/task state and scheduling services. |
| `Queue(process, task, signal, sender)` | Publish a process- or task-directed signal and sender record. |
| `Peek(process, task, mask, sender)` | Select a pending signal permitted by the supplied selection mask without consuming it. |
| `Consume(process, task, signal)` | Consume the selected pending signal/queued record. |
| `Discard(pending, queue, signal)` | Remove a signal's pending bit and queued records. |
| `ReserveExit(child, recipient)` | Reserve notification capacity and retain the recipient for a child's eventual exit. |
| `ReleaseExit(resources, child, deliver)` | Release the reservation, optionally publishing its exit signal. |
| `Action(signal, action, previous)` | Query or replace a validated signal disposition. |
| `Mask(operation, mask, previous)` | Query/change the current task's mask, keeping SIGKILL and SIGSTOP unblocked. |
| `BeginTemporaryMask(mask)` | Save and replace the task mask for an atomic masked wait. |
| `FinishTemporaryMask(deferRestore)` | Restore the saved mask now, or leave restoration for signal delivery. |
| `Stack(stack, previous, rsp)` | Query/change alternate-stack state using the current user stack pointer. |
| `Send(pid, tid, signal, queued)` | Resolve targets, check permissions, enqueue signals, and notify affected waits. |
| `Pause()` | Wait for signal interruption. |
| `Deliver(frame, result, restartable)` | Apply pending signal behavior and prepare a user handler frame when needed. |
| `Restore(frame)` | Validate/restore a user signal context, mask, and saved result. |

### ProcessController

Coordinates lifecycle and process-memory policy across Logic and Resource.
`ProcessController.hpp` declares it; `ProcessLifecycle.cpp` and
`ProcessMemoryManager.cpp` implement the same class. ProcessMemoryManager is not
a separate component or factory export.

| Function | What it does |
| --- | --- |
| `ProcessController(resources, logic)` | Bind lower-layer records and peer Logic services, initializing lifecycle serialization. |
| `Clone(frame, arguments)` | Validate clone semantics and create/share process/task state before publishing child execution. |
| `Execute(frame)` | Replace the calling process's image and saved user entry state. |
| `Wait(selector, status, options, usage, waitid, type)` | Select/await child state and return wait status/accounting through the requested ABI. |
| `Group(operation, pid, group)` | Perform supported process-group/session queries or changes. |
| `SetTidAddress(address)` | Register clear-child-TID state and return current task identity. |
| `Exit(status, group)` | Coordinate current-thread or whole-process exit and resource retirement. |
| `MapProcessMemory(address, length, protection, flags, fd, offset)` | Apply anonymous/file-backed mapping policy and return the chosen user address. |
| `UnmapProcessMemory(address, length)` | Release a user range and its mapping/backing claims. |
| `ProtectProcessMemory(address, length, protection)` | Validate and change user-region access permissions. |
| `QueryProcessMemoryResidency(address, length, vector)` | Report mapped-page residency to a user vector. |
| `AdviseProcessMemory(address, length, advice)` | Apply supported memory advice to a user range. |
| `ResizeProcessBreak(address)` | Adjust heap backing and return the resulting program break. |
| `ResourceLimit(resources, pid, resource, proposed, previous)` | Read/change supported process CPU or descriptor limits with caller authorization. |

### SocketManager

Implements Unix-domain stream, datagram, and sequenced-packet sockets using VFS
files. `SocketManager.hpp` defines address/message/control records and the API;
`SocketManager.cpp` coordinates endpoints, readiness, credentials, and descriptor
passing. User-address arguments are validated/copied by the manager.

Debug builds publish the existing endpoint catalogue, FIFO accepts/messages,
ancillary ownership and maintained operation/queue counters. UNIX address lengths
preserve abstract embedded-NUL names. Inspection reads stored readiness without
calling Poll; observed graphs exclude transient references and do not prove leaks.
GdbCommands.md documents bounded endpoint/message views and native contracts.

| Function | What it does |
| --- | --- |
| `SocketManager(resources, vfs, events, signals)` | Bind descriptor, namespace, readiness, and signal services. |
| `Socket(domain, type, protocol)` | Create a supported local socket and return its descriptor. |
| `Socketpair(domain, type, protocol, output)` | Create connected local endpoints and copy their descriptors to user memory. |
| `Bind(fd, address, length)` | Bind an endpoint to a supported local address. |
| `Connect(fd, address, length)` | Connect to a local peer, waiting or returning nonblocking status as required. |
| `Listen(fd, backlog)` | Put a connection-oriented socket into listening state. |
| `Accept(fd, address, length, flags)` | Accept a queued peer and optionally copy its address to user memory. |
| `Sendto(fd, data, size, flags, address, length)` | Send user data to a connected or explicitly addressed peer. |
| `Recvfrom(fd, data, size, flags, address, length)` | Receive user data and optionally report its source address. |
| `Message(fd, message, flags, send)` | Transfer vectored messages and supported ancillary credentials/file descriptors. |
| `Shutdown(fd, how)` | Disable the requested receive/send directions and notify peers/waiters. |
| `GetOption(fd, level, option, value, length)` | Copy a supported socket option to user memory. |
| `SetOption(fd, level, option, value, length)` | Read and apply a supported user-provided option. |
| `Name(fd, address, length, peer)` | Copy the local or peer socket address to user memory. |

### InterProcessCommunicationManager

Creates pipes and anonymous memory files through VFS file operations.
Debug builds expose descriptor/queued-right reachable pipe and memory-file state,
including logical packet metadata, seals, backing pages and mapping relationships.
There is no IPC object catalogue. Default inspection excludes payload, and transfer
counters distinguish returned/copied bytes from queue consumption and discards.
IpcState.hpp supplies shared runtime/native-reader layouts; GdbCommands.md defines
the commands, bounds and reachability limits.
`InterProcessCommunicationManager.hpp` declares the API;
`InterProcessCommunicationManager.cpp` implements buffering, readiness, and
memory-file backing. Blocking pipe operations use Event and Signal services.

| Function | What it does |
| --- | --- |
| `InterProcessCommunicationManager(resources, vfs, events, signals)` | Bind process descriptors, files, notifications, and signal services. |
| `CreatePipe(output, flags)` | Create read/write pipe endpoints and copy both descriptors to user memory. |
| `CreateMemoryFile(name, flags)` | Create a named anonymous memory-backed file and return its descriptor. |


## 3. Ownership and shared objects

The factory owns its ten components and capability structure. Resource imports
and cross-component pointers are borrowed. Callers must keep the layer stack
alive while services, registered callbacks, or file-operation tables are in use.
Normal boot retains it for the kernel lifetime; factory destruction does not
provide a live-kernel shutdown protocol.

| Structure | Role and lifetime |
| --- | --- |
| `LogicLayerCaps` | Borrowed access to the ten factory-owned components. |
| `process_t`, `task_t` | Resource-owned records retained or claimed while Logic coordinates their use. |
| `scheduling_decision_t` | Scheduler selection whose dispatch claim is consumed when the task is scheduled. |
| `file_t` | Reference-counted open file; retained users release it through VFS close. |
| `inode_t`, `dentry_t`, `mount_t` | VFS namespace/backing objects with lifetimes distinct from an open descriptor. |
| `elf_metadata_t` | Parsed image metadata, separate from the backing retained by mapped segments. |
| `signal_action_t`, `signal_stack_t` | Shared records defined in signal_types.h; Resource's SignalState.hpp owns queue storage, and saved machine context remains architecture-specific. |

Descriptor entries hold file references. Readiness registrations are weak
observers detached during final close, rather than owners of the file. Memory
mappings retain backing independently of the original open file or ELF metadata.
Process/task retirement can remain pending after an exit request succeeds.
Debug lifecycle counters distinguish clone publication, committed exec, exit
requests and wait outcomes. Resource-owned incarnation records follow Scheduler
publication, exit, stop/continue and reparenting without changing ownership rules.
Credential counters distinguish actual changes, no-ops and silent filesystem-ID
refusals; credentials still live only in process records. Shared-resource views
resolve task overrides explicitly and identify files through verified operation
metadata. Memory operations extend the same ProcessController lifecycle record
with map/unmap/protect/residency/advice/break outcomes, including early rejection
and silent break refusal. ElfMapper publishes parse, validation and image-load
outcomes without retaining temporary segment metadata. GdbCommands.md documents
limits and partial-state interpretation.

## 4. General interactions

ProcessController coordinates lifecycle work across Resource process/task records,
VFS, ElfMapper, Scheduler, synchronization, and signals. Its memory implementation
chooses mapping policy and uses VirtualMemoryManager to apply it. Executable and
library mappings use ElfMapper; it does not perform userspace dynamic linking.

VFS wraps Resource filesystem capabilities for the boot archive and also supports
registered filesystem callbacks. SocketManager and IPC create VFS-compatible
files, so descriptor access, readiness, and final release share the same file
model. Credential state is stored in Resource process records and changed through
CredentialManager; VFS and lifecycle code also enforce operation-specific rules.

EventManager ties readiness changes to Scheduler waits and wakeups. SignalManager
uses Scheduler to wake or stop affected tasks and architecture hooks to prepare
or restore user signal frames. SynchronizationManager coordinates futex state and
priority donation with Scheduler, and supplies immutable Dispatcher cleanup
routes for priority state, robust lists, and child-TID state. TaskManager holds
opaque handles to these routes; only Dispatcher invokes the Logic providers.
Descriptor tables likewise use Dispatcher file-operation routes for VFS retain
and final close. VFS backing providers are entered from platform through
Dispatcher and MemoryRequestManager, preserving failed-writeback retry.

Components may also call Resource directly. Request.md describes syscall decoding;
the component sections above describe the callable Logic interfaces.

## 5. Synchronization and execution constraints

Logic has no layer-wide lock. Scheduler protects its queues and policy state;
ProcessController serializes lifecycle transitions. Mapping transactions pin the
current process and hold its address-space operation lock with preemption disabled,
then release both before returning.

VFS separates namespace mutation, mount/cache state, inode/file I/O, reference
counts, and readiness locking. Its `Poll` callback is a nonblocking state snapshot:
it must not publish events or mutate observer registrations. Blocking anonymous
file callbacks instead run with a retained file outside descriptor and VFS I/O
locks, owning their user-copy/state transaction.

Event producers publish changed state before notifying outside their state locks.
Sequence counters let waits detect a change around enrollment. Synchronization
waiters retain shared backing identity where necessary, so unmapping a user
address does not silently replace the identity of a queued shared futex.

Signal queue helpers require `SignalDispositionGuard` for their target. Credentials
have their own acquisition/release guards. A borrowed process, task, or file lookup
must be retained where the operation outlives the protecting lock. Sleeping and
context switching must follow Scheduler's boundary rules; services that block are
not general interrupt-context APIs.

## 6. Results and current limits

Most syscall-facing services use Linux-style negative errors encoded in
`uint64_t`; VFS also exposes signed results and booleans, and ElfMapper uses boolean
success. There is no uniform layer-wide status type. Check the called operation's
return contract and release acquired references on failure.

| Area | Current boundary |
| --- | --- |
| Lifecycle and user execution | x86_64 provides the working user context path; AArch64 switching/user entry remains stubbed. |
| ELF loading | Target machine validation is architecture-specific; mapping a supported image is separate from executing it or linking dependencies. |
| Sockets | Local Unix-domain transport only; network domains and nonzero protocols are rejected. |
| Waiting and synchronization | Sleep/futex queues use fixed capacities tied to the task pool; waits can fail or be interrupted. |
| Signals | Delivery/restoration depends on architecture hooks; AArch64 signal contexts remain unsupported. |
| Filesystems | Operations depend on the mounted backend; CPIO growth and namespace mutations are limited as described in Resource.md. |

Scheduler chooses policy and placement; TaskManager and architecture code perform
the actual context transition. This separation also means a populated Logic
capability table does not prove every platform-dependent operation is available.

## 7. Self-tests and inspection

`logictests.cpp` assembles ten component suites. `SelfTestManager.cpp` runs them
from Dispatcher before Request construction. Later post-start/post-init suites
exercise cross-layer scheduling, memory, process lifecycle, waits, signals, events,
and task-context behavior.

| Area | Test files |
| --- | --- |
| Execution and memory | `SchedulerTests.cpp`, `ProcessControllerTests.cpp`, `ElfMapperTests.cpp`. |
| Files and identity | `VirtualFileSystemTests.cpp`, `CredentialManagerTests.cpp`. |
| Waiting and signals | `EventManagerTests.cpp`, `SynchronizationManagerTests.cpp`, `SignalManagerTests.cpp`. |
| Communication | `SocketManagerTests.cpp`, `InterProcessCommunicationManagerTests.cpp`. |

Use the factory to follow imports and callback registration, then the relevant
interface, implementation, and tests. `debug-kernel.sh` and `gdb_extention.py`
provide the existing debugger workflow. Dispatcher.md explains the entry bridges
and Request.md explains how syscall arguments reach these services.
