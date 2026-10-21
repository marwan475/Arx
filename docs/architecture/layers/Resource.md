# Resource layer

The Resource layer provides the kernel's memory, process, task, time, DMA, PCI, and
filesystem resources. Logic builds behavior and policy on these services; Request
can also use them directly. Platform mechanisms and architecture hooks provide
the underlying machine operations.

`ResourceLayerFactory.hpp` declares the exported capabilities, and
`ResourceLayerFactory.cpp` constructs them. KernelArch.md describes the layer
stack; Platform.md covers the lower-level mechanisms, and Logic.md describes
their main consumers.

## 1. Initialization

Dispatcher creates Resource first in `StartKernel`, after platform bootstrap has
established memory, CPU state, and boot-image information. The factory constructs
the components in the table below, then publishes `ResourceLayerCaps`.

DMA receives the physical/virtual memory managers and `platform.dma_ops`.
TimeManager initializes its clock backend. PciManager copies the platform PCI
operation table without invoking it. After `StartKernel` returns, `kmain` calls
`EnumerateDevices` on the bootstrap CPU before post-init tasks are queued.
InitRamFileSystemManager indexes the boot archive using its platform address
and size; ResourceFileSystem then wraps
that manager and exports filesystem callbacks.

`GetCaps` returns null before creation. Repeated `Create` calls return the existing
capabilities without rebuilding components. Dispatcher runs Resource self-tests
before creating Logic. A capability pointer does not certify that every service
is available: clock and DMA support still depend on their backends.

### ResourceLayerFactory

| Function | What it does |
| --- | --- |
| `ResourceLayerFactory()` | Initialize empty factory-owned capability/component state. |
| `~ResourceLayerFactory()` | Delete owned components and capabilities; not a live-kernel shutdown protocol. |
| `Create()` | Construct Resource managers and filesystem adapter once, or return existing capabilities. |
| `GetCaps()` | Return borrowed capabilities, or null before creation. |

## 2. Components and public APIs

The sections follow factory construction order. Function descriptions summarize
the public headers; callers must also follow the ownership and locking rules below.

### PhysicalMemoryManager

Provides physical backing through the platform PMM. `PhysicalMemoryManager.hpp`
declares the interface; `PhysicalMemoryManager.cpp` forwards allocation and gathers
memory statistics.

| Function | What it does |
| --- | --- |
| `Alloc(size)` | Allocate physical backing and return its kernel-accessible pointer, or null. |
| `AllocDma(size, maximumAddress, physicalAddress)` | Allocate address-constrained backing and report its physical address. |
| `Free(addr)` | Return a physical allocation to the PMM. |
| `GetMemoryInfo(totalBytes, freeBytes)` | Sum total/free memory across normal and DMA NUMA zones. |
| `GetSharedMemoryBytes()` | Report resident shared-memory backing tracked by VMM. |

### ProcessManager

Owns process records and their shared resource references. `ProcessManager.hpp`
declares process state, descriptor/filesystem/credential structures, and guards;
`ProcessManager.cpp` implements their storage and lifetime operations. The pool
holds 64 process slots. Process exit policy belongs to ProcessController/Scheduler.
Debug builds add slot incarnations, captured parent identities and maintained
reservation/publication/reclamation counts. TaskManager similarly distinguishes
its physical 128-slot inventory from mutable TIDs. Dispatcher registers borrowed
process/task roots; bounded views show lifecycle, CPU claims and task-specific
shared contexts. Reclaimed diagnostic records are cleared, with no retained
process/task history. GdbCommands.md defines the command and native contracts.

| Function | What it does |
| --- | --- |
| `ProcessManager()` | Initialize the process table and per-CPU running-process pointers. |
| `SnapshotUsage(process, usage, descendants)` | Snapshot process accounting, optionally including reaped child usage. |
| `ReapUsage(parent, child)` | Add a reaped child's accounting to its parent. |
| `CreateDescriptorTable()` | Allocate a new, empty, reference-counted descriptor table. |
| `CopyDescriptorTable(source)` | Copy entries while Dispatcher retains each referenced file through its opaque operations route. |
| `ReleaseDescriptorTable(table)` | Drop a table reference; final release closes entries and frees storage. |
| `CopyFilesystemContext(source)` | Copy cwd/umask state, or create defaults when source is null. |
| `ReleaseFilesystemContext(context)` | Drop a shared filesystem-context reference and free it on final release. |
| `CopySignalDispositions(source)` | Copy signal actions, or initialize default dispositions. |
| `ReleaseSignalDispositions(dispositions)` | Drop a shared disposition reference and free it on final release. |
| `ReleaseProcessResources(process)` | Detach and release descriptor, filesystem, and signal resources. |
| `AllocateProcess()` | Reserve a process slot with default shared resources, without attaching an address space. |
| `CreateProcess(addressSpace, preferredId)` | Create a process around an address space, optionally requesting a particular slot ID. |
| `FreeProcess(process, onlyIfExited)` | Reclaim an eligible process and its owned resources; reject live claims or running use. |
| `AddTask(process, task)` | Attach an allocated task to a live process. |
| `RemoveTask(process, task)` | Unlink a task from the process's task list. |
| `AddFileDescriptor(process, file, flags, operations)` | Install a file handle and its borrowed Dispatcher operations route in the first available descriptor slot. |
| `AddFileDescriptorFrom(process, file, flags, minFd, operations)` | Install a handle and its route at or above a requested descriptor number. |
| `GetFileDescriptor(process, fd)` | Return a borrowed entry; use it within the descriptor-table guard. |
| `EnsureFileDescriptorCapacity(process, requiredIndex)` | Grow the descriptor table within the process limit. |
| `BuildUserInitialStack(process, layout, outUserRsp)` | Build the initial user stack from arguments, environment, and auxiliary values. |
| `ActivateProcessAddressSpace(process)` | Make the process/address space current on the calling CPU. |
| `GetTasks(process)` | Return the borrowed head of a process's task list. |
| `GetRunningProcess(cpuId)` | Return the process associated with a CPU. |
| `SetRunningProcess(cpuId, process)` | Update that association and switch the address space when updating the local CPU. |
| `GetCurrentProcess()` | Return the calling CPU's process. |
| `AcquireCurrentMemoryProcess(requireExclusive)` | Pin current process memory, optionally rejecting an address space shared by another process. |
| `AcquireProcess(id, requireRealTimer)` | Retain a process by ID, optionally requiring an armed real timer. |
| `ReleaseProcess(process)` | Release a process-lifetime claim. |
| `ReleaseMemoryProcess(process)` | Release a process-memory claim. |
| `RetainMemoryProcess(process)` | Add a memory claim to an already-valid process. |
| `GetCurrentProgramBreak()` | Return the current process's program break, or zero if unavailable. |
| `GetProcesses()` | Expose the mutable process table for coordinated kernel inspection. |
| `GetProcesses() const` | Expose the same table through a const view. |
| `IsProcessRunning(process)` | Check whether any CPU currently associates the process with running execution. |
| `TryReapExitedProcess(processId)` | Attempt reclamation of an exited, non-running process. |
| `GetCapacity()` | Return the fixed process-table capacity. |
| `ExchangeUmask(process, mask)` | Replace the shared filesystem creation mask and return its previous value. |
| `ChangeFileLimit(process, proposed, previous)` | Read/update descriptor limits, validating changes and optionally reporting the old limits. |
| `AcquireCurrentCredentials()` | Lock current credentials with preemption disabled; return the guarded process. |
| `ReleaseCredentials(process)` | Unlock credentials and restore preemption nesting. |
| `CreateGroupList(ids, count)` | Allocate an immutable, reference-counted supplementary-group list. |
| `ReleaseGroupList(groups)` | Drop a group-list reference and free it on final release. |

Public helpers in the same header support those contracts. Both guard types are
noncopyable. Holding the credential guard forbids user copies and manager-lock calls.

| Helper function | What it does |
| --- | --- |
| `process_t::Files()` | Select the descriptor table visible to the active task/process. |
| `process_t::DescriptorLimit()` | Return the process's current descriptor limit. |
| `process_t::Fs()` | Select the active filesystem context. |
| `process_t::Signals()` | Return the process's shared signal dispositions. |
| `WorkingDirectorySnapshot(process)` | Copy a coherent cwd path from the shared filesystem context. |
| `pending_signals(process, task)` | Combine pending signals, excluding blocked and ignored signals while honoring unblockable ones. |
| `SignalDispositionGuard(process)` | Acquire the target's shared signal-disposition protection. |
| `~SignalDispositionGuard()` | Release protection if still held. |
| `SignalDispositionGuard::Release()` | Release the guard early. |
| `SignalDispositionGuard::Current(process)` | Return the active guard's dispositions, or the current/fallback dispositions when unguarded. |
| `FileDescriptorGuard(process)` | Lock the selected descriptor table with preemption disabled. |
| `~FileDescriptorGuard()` | Restore the prior guard context, unlock if owned, and restore preemption nesting. |

### TaskManager

Owns 128 task slots, stacks, saved machine contexts, and execution claims.
`TaskManager.hpp` declares the API, scheduling attributes, per-task scheduling
state and scheduling constants; `TaskManager.cpp` implements task transitions.

| Function | What it does |
| --- | --- |
| `TaskManager()` | Initialize task slots, running/switching records, and accounting state. |
| `AllocateTask()` | Reserve and initialize a task slot without constructing its execution stack. |
| `CreateKernelTask(entry, arg)` | Allocate a stack/context that begins at a kernel entry function. |
| `CreateUserBootstrapTask(userRip, userRsp, arg0, arg1)` | Create a kernel bootstrap context that enters the supplied user state. |
| `CreateUserCloneTask(frame, id)` | Create an x86_64 child context from a saved syscall frame. |
| `FreeTask(task, retiredStack, retiredResources)` | Free an eligible slot; optionally transfer stack/resource cleanup to the caller. |
| `ReleaseTaskResources(resources)` | Drop detached task descriptor/filesystem references. |
| `SetPriorityCleanup(cleanup)` | Install a borrowed opaque Dispatcher route for policy cleanup. |
| `CleanupCurrentTask()` | Ask Dispatcher to run the current task's applicable priority/retirement cleanup. |
| `CleanupRetiredTask(task)` | Run cleanup for a dying, quiescent task while protecting it from reclamation. |
| `ReleaseTaskStack(stack)` | Free a detached kernel stack after it is safe to reclaim. |
| `ExecuteTask(task, releaseDispatchClaim)` | Validate and switch to a task, optionally consuming a dispatch claim. |
| `RetainForDispatch(task)` | Claim an eligible task so selection cannot race retirement. |
| `ReleaseDispatchClaim(task)` | Release a claim that was not consumed by execution. |
| `MarkDying(task)` | Mark a task for retirement. |
| `CompleteSwitch()` | Finalize the outgoing task's state after its context is saved on the incoming stack. |
| `MarkSleeping(task)` | Mark the current task as enrolled in a pending sleep. |
| `FinishSleeping(task)` | Clear pending/blocked/uninterruptible sleep state. |
| `GetRunningTask(cpuId)` | Return a CPU's current task record. |
| `SetRunningTask(cpuId, task)` | Update a CPU's running-task association after validation. |
| `GetCurrentTask()` | Return the calling CPU's running task. |
| `GetActiveTask()` | Resolve the active task without a manager instance. |
| `AccountCurrent(userMode)` | Charge elapsed execution time to the current task/process. |
| `AccountActive(userMode)` | Account the active task through the shared entry point. |
| `HasRunningTasks(process)` | Check whether a process still has executing or switching tasks. |
| `ReadSystemStatistics(now, loads, liveTasks)` | Update load averages and report live task count. |
| `FindTask(id)` | Find a task by identity; callers needing stability must claim it. |
| `SetCurrentIdentity(id)` | Set the current task's identity. |
| `GetTasks()` | Expose the mutable task-slot array. |
| `GetTasks() const` | Expose the array through a const view. |
| `GetCapacity()` | Return the fixed task-slot capacity. |
| `CountTasks()` | Count allocated task slots. |
| `GetCurrentName(name)` | Copy the current task's fixed-size name. |
| `SetCurrentName(name)` | Replace the current task's name. |
| `GetCurrentTlsBase(gs, value)` | Read the current x86_64 FS/GS user TLS base; unsupported targets return false. |
| `SetCurrentTlsBase(gs, value)` | Update the current x86_64 TLS base and saved task state. |

### VirtualMemoryManager

Provides address spaces, page mappings, and shared/file-backed storage.
`VirtualMemoryManager.hpp` declares the interface; `VirtualMemoryManager.cpp`
implements it over platform VMM. Mapping transaction primitives require the
address-space operation lock; backing-page I/O must follow caller serialization.

Debug builds publish platform-owned kernel/pending-writeback roots and
Resource-owned memory operation outcomes. Bounded views inspect inactive process
spaces, region lists, backing/shared-page references and selected x86_64 page
tables without target calls. Reclamation counters distinguish actual space
release from reference drops or deferred teardown. GdbCommands.md defines the
shared identities, units and snapshot limits. IPC memory-file views reuse these
backing/page identities to correlate known mappings; backing can survive a final
file close without preserving the original memory-file object.

| Function | What it does |
| --- | --- |
| `CreateUserAddressSpace()` | Allocate an isolated user root and free-region metadata. |
| `CloneUserAddressSpace(source)` | Clone user mappings using retained/shared or copy-on-write backing. |
| `DestroyUserAddressSpace(space)` | Release an isolated address-space root after all process/CPU users have detached. |
| `IsPageAvailable(space, address)` | Check that a user page is free in both region and mapping state. |
| `AllocateAnonymousPage(space, address, protection, heap)` | Allocate zeroed page backing and install a user mapping. |
| `CreateBacking(size, file)` | Create a reference-counted memory/file backing object. |
| `RetainBacking(object)` | Add a backing-object reference. |
| `ReleaseBacking(object)` | Drop a backing reference and reclaim it when eligible. |
| `FlushBacking(object)` | Write dirty file-backed pages through their writeback contract. |
| `RetryWriteback()` | Retry retained writeback work after earlier failures. |
| `GetBackingPage(object, offset, create)` | Find a cached page, optionally creating/populating it. |
| `ResizeVolatileBacking(object, size)` | Resize volatile storage; reject unsupported mapped shrink operations. |
| `MapBackingPage(space, address, object, offset, protection, shared, maximumProtection)` | Retain backing and map or reserve its page with the permitted protections. |
| `ReleaseAnonymousPage(space, address, force)` | Unmap/release owned backing; preserve a recoverable mapping on non-forced writeback failure. |
| `ProtectAnonymousPage(space, address, protection)` | Change an owned user page's recorded protection and mapping permissions. |
| `DiscardAnonymousPage(space, address)` | Discard private contents, restoring zero/file backing as appropriate. |
| `ReleaseAnonymousPages(space)` | Lock an address space and force-release all owned user pages. |
| `MapPage(va, pa, flags, space)` | Install one virtual-to-physical page mapping. |
| `UnmapPage(va, space)` | Remove one page mapping. |
| `MapRange(vaStart, paStart, size, flags, space)` | Install mappings across a range. |
| `UnmapRange(vaStart, size, space)` | Remove mappings across a range. |
| `ProtectPage(va, flags, space)` | Change one page's machine mapping flags. |
| `ProtectRange(vaStart, size, flags, space)` | Change mapping flags across a range. |
| `SwitchAddressSpace(space)` | Activate the selected address space. |
| `VirtToPhys(va, space)` | Translate a virtual address through the selected page tables. |
| `ReserveRegion(space, size, type)` | Reserve virtual address space without allocating physical backing. |
| `FreeRegion(space, addr)` | Release virtual-region metadata/reservation. |
| `FindRegion(space, addr)` | Return borrowed metadata for the region containing an address. |

### DmaManager

Owns tracked coherent DMA allocations using physical memory and platform DMA
operations. `DmaManager.hpp` defines devices, status, and allocation handles;
`DmaManager.cpp` implements validation, mapping, and rollback. It is noncopyable.
Debug builds retain operation outcomes, effective constraints and cleanup phases
beside the manager's records. Dispatcher registers the provider after Resource
creation; temporary test managers do not register themselves. Bounded inspection
distinguishes successful buffers from failed-allocation cleanup handles and shows
requested, mapped and reserved bytes separately. A releasing record may remain
linked after its backing is returned; interrupted operations report partial state.
GdbCommands.md documents the DMA view and its native-reader contract.

| Function | What it does |
| --- | --- |
| `DmaManager(physical, virtualMemory, operations)` | Bind memory services and copy the platform DMA operation table. |
| `~DmaManager()` | Enforce that no tracked allocations remain; live allocations cause a panic. |
| `AllocateCoherent(device, size, alignment, boundary, allocation)` | Validate device constraints, allocate zeroed backing, and produce CPU/device addresses. |
| `FreeCoherent(allocation)` | Unmap and release a tracked buffer; failures preserve a retryable handle. |
| `WriteBarrier()` | Order descriptor stores before device publication/doorbells. |
| `ReadBarrier()` | Order completion observation before reading device-written data. |
| `CountAllocations()` | Return the current number of tracked allocations. |

### TimeManager

Provides clock readings, monotonic deadlines, and process timers.
`TimeManager.hpp` defines time records and injectable clock callbacks;
`TimeManager.cpp` implements clock conversion and timer state.

Debug builds publish clock configuration, successful normal-path samples and
process timer/accounting roots. Real-timer deadlines use monotonic nanoseconds;
CPU timers use charged user or total user-plus-kernel nanoseconds. Per-process
timer markers identify interrupted updates. Inspection never calls a clock backend
or claims the samples are current. Timer commit counts remain separate from API
return outcomes, including output-copy failure after a commit. GdbCommands.md
specifies the time views and exact-build reader contracts.

| Function | What it does |
| --- | --- |
| `TimeManager()` | Initialize the live architecture clock backend and report its availability. |
| `TimeManager(backend)` | Initialize an injected backend, used for isolated deterministic tests. |
| `ReadClock(clock, value)` | Read realtime, monotonic, or monotonic-raw time; CPU clock queries are unsupported. |
| `ReadResolution(clock, value)` | Report the supported clock's resolution. |
| `CalculateSleepDeadline(clock, absolute, request, monotonicDeadline)` | Validate a duration/time and convert it to the scheduler's monotonic deadline. |
| `GetTimer(which, destination)` | Copy the current process's selected interval timer to user memory. |
| `SetTimer(which, source, previous)` | Replace an interval timer from user memory, optionally copying out its previous state. |
| `CancelAlarm()` | Cancel the real-time alarm and return its previous remaining seconds. |
| `SetAlarm(seconds)` | Replace the process's real-time alarm and return previous remaining seconds. |
| `ExpireRealTimer(process, now)` | Detect expiry and publish pending SIGALRM for a retained process. |
| `RestartRealTimer(process)` | Rearm an expired repeating real timer at its next future interval. |
| `ExpireCpuTimers(process)` | Detect CPU-time timer expirations and publish their pending signals. |

The injected `time_clock_backend_t` has two callbacks: `initialize` supplies the
realtime epoch, availability, and resolution; `read` supplies elapsed monotonic
nanoseconds. Live use must share Scheduler's monotonic epoch. CPU interval timers
use execution accounting even though CPU clock queries are not yet supported.

### PciManager

`PciManager.hpp` defines the discovery/query interface and `PciStatus`;
`PciManager.cpp` wraps an injected platform enumeration backend. The factory owns
the noncopyable manager and publishes `ResourceLayerCaps::pciManager`. Platform
owns the inventory; manager destruction does not release its records or mappings.

| Function | What it does |
| --- | --- |
| `PciManager(operations)` | Copy the operation table and start in `NotEnumerated`; context must outlive calls. |
| `EnumerateDevices()` | Invoke the backend once on the BSP during startup and cache the outcome, including failures. |
| `GetStatus()` | Report `NotEnumerated`, `Success`, `Unsupported`, `NoRegions`, or `Failed`. |
| `IsTruncated()` | Report whether discovery stopped because inventory capacity was exhausted. |
| `CountDevices()` | Return the captured device-function count, initially zero. |
| `GetDevice(index)` | Borrow the original const platform record, or return null for an invalid index. |
| `FindDevice(segment, bus, device, function)` | Find an exact segment-qualified address, or return null for invalid or absent addresses. |

Queries do not trigger enumeration. After discovery is published to the other
CPUs, concurrent read-only queries need no manager lock; concurrent enumeration
and runtime rescans are unsupported. Success with zero devices differs from no
ECAM regions, an unsupported backend, or failed enumeration. A truncated inventory
remains usable up to its 256 recorded device functions.

Returned records preserve the pointer identity required by platform DMA. They
capture discovery-time configuration values, not live register state; BAR metadata
has no probed resource lengths. Resource provides no configuration writes, BAR
mapping, driver-selection policy, device claiming, or hotplug in this interface.

### InitRamFileSystemManager

Indexes up to 256 CPIO entries without copying their file data out of the boot
image. `InitRamFileSystemManager.hpp` declares the archive records;
`InitRamFileSystemManager.cpp` parses and searches them. Debug builds expose the
bounded parsed array, load outcome and current count for metadata-only archive
hierarchies and exact-name inspection, including entries never cached by VFS.

| Function | What it does |
| --- | --- |
| `InitRamFileSystemManager(initramfsSize, initramfsAddress)` | Parse the supplied CPIO image into the bounded archive index. |
| `find(path)` | Return the mutable indexed entry matching a path, or null. |
| `GetArchiveAt(index)` | Return a const indexed entry, or null when out of range. |
| `GetArchiveCount()` | Return the number of indexed entries. |

### ResourceFileSystem

Adapts the archive index into the filesystem callback table consumed by VFS.
`ResourceFileSystem.hpp` declares the class, callback table and retained
filesystem/node layouts used by matching native readers. `ResourceFileSystem.cpp`
implements their operations. Every table operation forwards to the matching
method below, passing the table as its context argument.
Debug records track public backend outcomes and returned I/O
bytes; thunks do not double count. Archive mode and backend kind are distinct:
the current backend exposes directories or regular files.

| Function | What it does |
| --- | --- |
| `ResourceFileSystem(initRamFileSystemManager)` | Bind the archive manager and initialize callback thunks. |
| `GetCaps()` | Return the borrowed embedded context/function-pointer table. |
| `MountFilesystem(type, source)` | Create a CPIO filesystem handle using the supplied or default archive manager. |
| `GetRootNode(filesystem)` | Create the backend root-directory handle. |
| `GetNodeInfo(node, info)` | Report identity, size, type, permissions, and ownership. |
| `Lookup(directory, name)` | Resolve one child component into a backend node. |
| `ReadDirectory(directory, cursor, entry)` | Advance directory iteration: return one entry, end-of-directory, or an error. |
| `Read(node, offset, buffer, size)` | Copy existing archive bytes into kernel memory. |
| `Write(node, offset, buffer, size)` | Overwrite existing archive bytes without extending the file. |
| `Mkdir(path, mode)` | Reject directory creation with `-EROFS`. |
| `Unlink(path, directory)` | Reject removal with `-EROFS`. |
| `Rename(oldPath, newPath)` | Reject renaming with `-EROFS`. |
| `Link(oldPath, newPath, followSymlink)` | Reject hard-link creation with `-EROFS`. |
| `Mknod(path, mode, device)` | Reject node creation with `-EROFS`. |
| `Truncate(path, size)` | Shrink an existing file; reject growth or invalid paths. |


## 3. Ownership and lifetime

The factory owns its managers, ResourceFileSystem adapter, and capability
structure. Callers borrow these objects. Normal boot keeps them for the kernel
lifetime; factory destruction is not a coordinated shutdown of live resources.
DMA explicitly requires devices to be quiesced and allocations freed before its
manager is destroyed.

| Resource | Ownership contract |
| --- | --- |
| Physical allocation | The returned address is released through PhysicalMemoryManager; mapping it does not transfer ownership automatically. |
| `virt_addr_space_t` | Created/cloned and destroyed through VirtualMemoryManager, with process ownership tracked separately. |
| `virt_backing_object_t` | Reference-counted backing retained independently by mappings and files. |
| `process_t` | A ProcessManager slot; lifetime claims and memory claims protect different aspects of it. |
| `task_t` | A TaskManager slot with a stack/context; dispatch claims prevent retirement while selected for execution. |
| `DmaAllocation` | A tracked handle containing CPU address, device address, size, and allocation ID. |
| `initramfs_archive_t` | Indexed archive metadata whose data pointer refers into the supplied boot image. |

`AcquireProcess`/`ReleaseProcess` retain process lifetime.
`AcquireCurrentMemoryProcess`/`ReleaseMemoryProcess` pin its memory context; they
do not by themselves lock mappings. Task selection uses
`RetainForDispatch`/`ReleaseDispatchClaim`, or transfers that claim into execution.
Raw lookup results do not provide these guarantees.

Descriptor tables, filesystem contexts, signal dispositions, and supplementary
group lists can be shared between processes/tasks. Their references and release
operations are separate from freeing a process slot. Task retirement similarly
separates stopping execution from releasing its stack and attached resources.

## 4. Service boundaries

Resource manages storage and execution mechanisms. Logic decides scheduling,
process lifecycle, mapping policy, and namespace behavior. TaskManager performs
context transitions chosen by Scheduler; VirtualMemoryManager applies the mapping
operations chosen by ProcessController and ElfMapper.

PhysicalMemoryManager wraps platform PMM allocation; VirtualMemoryManager wraps
VMM operations and manages user backing. DmaManager uses the platform DMA operation
table rather than treating CPU pointers as device addresses. TimeManager's default
backend calls architecture clock hooks; its alternate backend supports isolated,
deterministic tests. PciManager explicitly triggers platform PCI discovery and
borrows its stable records; architecture-specific scanning stays beneath Resource.

Necessary upward work passes through Dispatcher. TaskManager stores opaque
priority and retirement routes; Dispatcher invokes their policy providers while
Resource keeps the task pinned. Descriptor entries similarly carry an opaque
file-operations route, used by Dispatcher for retain and final release. Resource
cannot access the providers' callable fields. The provider owns each immutable
route and must keep it and its context alive until the last user releases them.
Dispatcher also supplies the scheduling-accounting bridge used at context-switch
boundaries. Resource does not construct the Logic managers that own that policy.

signal_types.h holds shared signal ABI records, masks and C++ queue storage below
both Arch and the layers. TaskManager and ProcessManager use these definitions
without including SignalManager.hpp. The queue definition is guarded for C++ so
C consumers retain the shared ABI declarations.

## 5. Filesystem capabilities

`ResourceFileSystem.hpp` defines `ResourceLayerFileSystemCaps`: a borrowed context
pointer plus callbacks for mounting, lookup, node information, directory iteration,
I/O, and mutations. Its context identifies the factory-owned adapter.
`resource_fs_t` and `resource_node_t` are opaque backend handles; VFS wraps them
in its own filesystem, inode, and dentry objects.

The backend currently accepts the `cpio` type. It reads existing archive contents,
writes only within an existing file's size, and allows shrinking through truncate.
It cannot grow files; mkdir, unlink, rename, link, and mknod return `-EROFS`.
Directory information can be derived from archive paths. This backend is distinct
from the VFS namespace and its registered filesystem-type interface.

The capability table has no unmount or node-release callbacks. It should not be
treated as a complete backend reclamation protocol. Archive data also remains
dependent on the supplied boot image's lifetime.

## 6. Synchronization and failures

There is no single Resource lock. Manager locks protect process/task slots;
separate locks protect descriptors, credentials, timers, and address-space state.
`FileDescriptorGuard` keeps a borrowed descriptor valid while holding its table
lock and disabling preemption.

Multi-step mapping operations use the address-space operation lock. Methods
documented as transaction primitives expect the caller to hold it; page-table and
backing locks protect narrower internal state. A process pin is not a substitute
for this lock, and a lock is not a replacement for retaining the owning object.

DMA allocation/free are task or initialization-context operations. Stop device
access before freeing a buffer. Failed unmapping preserves its handle/backing;
failed rollback may leave an ID-only cleanup handle that must be retried.
Read/write barriers order device publication and completion observation.

Error conventions vary: allocations may return null, mapping operations may
return false, DMA returns `DmaStatus`, and time/backing operations use Linux-style
status values. Filesystem callbacks include negative errors and some generic
`-1` failures. Callers must follow the selected method's contract.

Process and task pools are bounded to 64 processes and 128 tasks; the archive
index holds at most 256 entries. DMA support depends on platform operations and
device constraints. Missing monotonic or realtime clocks limit the corresponding
time services. AArch64 context switching and user entry remain stubbed; common
manager interfaces do not establish working execution support there.

## 7. Self-tests and inspection

`resourcetests.cpp` assembles eight component suites. `SelfTestManager.cpp` runs
them from Dispatcher before Logic construction. Later integration tests cover
cross-layer memory, task, timing, and process behavior.

| Area | Test files |
| --- | --- |
| Memory and DMA | `PhysicalMemoryManagerTests.cpp`, `VirtualMemoryManagerTests.cpp`, `DmaManagerTests.cpp`; the production DMA contract runs after PCI discovery. |
| PCI discovery | `PciManagerTests.cpp`; isolated backend tests run with Resource, production inventory checks run after discovery in `kmain`. |
| Execution and time | `ProcessManagerTests.cpp`, `TaskManagerTests.cpp`, `TimeManagerTests.cpp`. |
| Filesystem backend | `InitRamFileSystemManagerTests.cpp`, `ResourceFileSystemTests.cpp`. |

Start with the factory for dependencies, then the component's public interface,
implementation, and tests. `debug-kernel.sh` and `debug/gdb/arx.py` provide the
existing debugger workflow. Platform.md explains the PMM/VMM and machine services
beneath these managers.

See [GdbCommands.md](../../debugging/GdbCommands.md) for the supported x86_64
commands, selectors and stopped-target limitations.
