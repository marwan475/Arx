# Kernel self-tests

Arx runs its self-tests as part of kernel startup. Platform tests check the boot
services, Resource and Logic unit tests run immediately after their layers are
initialized, post-start integration tests exercise live scheduling and shared
services, and Request tests exercise syscall contracts from the kernel side.
This guide covers x86_64.

`SelfTestManager.cpp` owns reporting and phase entry points. The group runners
choose the tests explicitly; there is no automatic discovery. KernelArch.md
describes the overall boot flow, and Resource.md, Logic.md, and Request.md describe
the services under test.

## 1. Execution order

| Phase | Invocation point | What must already exist |
| --- | --- | --- |
| Platform | `bootstrap.c` calls `platform_selftests` after waiting for initialized CPUs. | Memory, heap, terminal, CPU/NUMA state, and platform DMA initialization; PCI has not been enumerated. |
| Resource unit tests | `Dispatcher::StartKernel` calls `RunResourceLayerTests` immediately after Resource creation. | Platform services and the Resource capabilities; Logic has not been created. |
| Logic unit tests | `Dispatcher::StartKernel` calls `RunLogicLayerTests` immediately after Logic creation. | Resource and Logic capabilities; Request has not been created and timer scheduling is inactive. |
| PCI discovery | `kmain` calls `RunPciDiscoveryTests` after explicit enumeration following `StartKernel`. | All layers exist; live PCI discovery has been attempted; scheduling is inactive. |
| Post-start integration | Each CPU's scheduled post-init task calls `RunPostInitTests`, which enters `RunPostStartTests`. | Request creation, publication of the root-mount result, queued per-CPU post-init tasks, active scheduling, and scheduler timer calibration. |
| Request syscall tests | The BSP continues from post-start tests into `RunRequestTests`. | Live scheduler and layer capabilities; the integration phase has returned. |
| Final report | The BSP calls `Finalize` after Request tests. | Completed preceding phases, or an explicit unsafe-abort path. |

`kmain` first enumerates PCI through PciManager and runs the live inventory and
production DMA checks. It then mounts the cpio root filesystem, publishes the result with
`SetRootFileSystemMounted`, and creates a post-init process/task for every CPU.
It publishes `bsp_kmain_exited` and activates scheduling. The tests therefore run
in scheduled kernel tasks rather than continuing on the original bootstrap stack.

Case and group recording belongs to the BSP. APs participate in SMP and TLB
integration work without opening independent manager cases. The BSP waits for
SMP completion and checks cleanup before running other fixtures against shared
files and address spaces. After testing, post-init tasks halt in their kernel
loops; a successful suite does not terminate QEMU.

Reporting cases are not the same as independent behaviors or assertions. Manager
capability checks have their own cases, while Request groups syscall subtests and
additional validator checks inside manager-sized reporting cases. The inventory
below explains what is tested; runtime logs provide the current totals.

## 2. Platform tests before the layers

`platformtests.c` runs collections, memory, klib, and platform invariants in that
order. The C tests report through `selftests.h`, which forwards to the same
manager used by later C++ tests.

| Test | What it tests |
| --- | --- |
| `platform/collections/bitmap` | Bit initialization, set/clear ranges, searches, and full/empty boundaries. |
| `platform/collections/intrusive_list` | Link structure, traversal, insertion, removal, and safe removal during iteration. |
| `platform/collections/hash_table` | Table creation, insertion, value lookup, deletion, and absent keys. |
| `platform/memory/mapping_regions` | Page/range translation, protection and unmapping, invalid addresses, and virtual-region reservation bookkeeping. |
| `platform/memory/physical_allocation` | Buddy allocation alignment, nonoverlap, varied sizes, stress/exhaustion, and restored zone accounting. |
| `platform/memory/heap_cache` | Heap size classes, odd sizes, invalid sizes, distinct live objects, reuse, and cache growth. |
| `platform/klib/vmalloc` | Page-aligned virtual allocation, region metadata, translation, and complete release. |
| `platform/klib/kmalloc` | Small/odd allocations, active-pointer uniqueness, reuse, and zero/oversize rejection. |
| `platform/klib/kzalloc` | Zero-filled allocation and invalid-size rejection. |
| `platform/klib/sort` | Empty, singleton, sorted, duplicate, and full-width unsigned sorting inputs. |
| `platform/klib/wrapping_counter` | Counter rollover, unchanged samples, normal increments, and multiple elapsed epochs. |
| `platform/klib/error_logger` | Diagnostic formatting, bounded reads/storage, truncation, earliest-record preservation, and unpublished slots. |
| `platform/memory/numa_topology` | Synthetic SRAT domain normalization, memoryless nodes, overlapping ownership, malformed entries, and checksum rejection. |
| `platform/cpu/initialized_state` | CPU inventory, initialized flag, NUMA association, and agreement between active and recorded page tables. |
| `platform/cpu/preemption_balance` | Balanced preemption-disable depth changes. |
| `platform/terminal/boot_framebuffer` | Availability of the boot terminal framebuffer, with explicit skip when unavailable. |
| `platform/pci/not_enumerated` | Bootstrap leaves PCI status unstarted, storage absent, count zero and truncation clear. |

The memory tests allocate and free real boot resources; they are not host-side
allocator simulations. The NUMA parser tests supplement the actual boot inventory
with controlled input tables.

## 3. Resource unit tests after Resource initialization

`resourcetests.cpp` invokes each manager suite after `ResourceLayerFactory::Create`
returns. Tests call Resource services directly and use controlled fixtures where
needed. These are unit tests by boot phase and public contract; they still execute
inside the kernel and use real platform memory services.

Each manager has an additional case named `resource/<Manager>` checking that its
initialized capability is present. The following tables describe its behavior
cases; names are the suffix after `resource/<Manager>/`.

### PhysicalMemoryManager

`PhysicalMemoryManagerTests.cpp` checks physical ownership and accounting.

| Test | What it tests |
| --- | --- |
| `accounting` | Free/used totals remain within available physical memory and use page granularity. |
| `allocate_free` | Two-page contiguous allocation, alignment, inclusion of the last byte, and accounting restoration. |

### DmaManager

`DmaManagerTests.cpp` uses a mock backend whose device addresses differ from
physical addresses during Resource tests. Its `platform_contract` case runs
later, after PCI discovery in `kmain`, using a record returned by PciManager.

| Test | What it tests |
| --- | --- |
| `platform_contract` | Production coherent allocation/release, explicit unsupported/no-memory results, barriers, and restored accounting. |
| `coherent_mapping_lifetime` | Concurrent zeroed contiguous allocations, translated addresses, backend barriers, and complete mapping/page release. |
| `constraints` | CPU/device alignment, masks, boundaries, address overflow, and valid device address zero. |
| `invalid_arguments_handles` | Invalid sizes/constraints, foreign or tampered handles, live-output protection, double free, and stale identity rejection. |
| `failure_rollback` | Mapping/unmapping failures, retained cleanup-only handles, retryable release, and resource restoration. |
| `zone_exhaustion_cycles` | Dedicated DMA reserve policy, separation from Normal memory, exhaustion without fallback, and repeated allocation/release. |

If no reserved DMA pages exist, `resource/DmaManager/reserved_memory` reports
BLOCKED and the allocation-dependent cases are not reached.

### PciManager

`PciManagerTests.cpp` uses injected inventories during Resource tests. It checks
that construction and queries leave discovery unstarted, original pointer identity
and segment/BDF lookup, invalid indices/addresses, empty and failure outcomes,
truncation, malformed backend results, and caching of both success and failure.
`resource/PciManager/platform_inventory` runs after live discovery in `kmain`,
checking platform/manager agreement, stable DMA identities, and repeated calls
without replacing the inventory.

### VirtualMemoryManager

`VirtualMemoryManagerTests.cpp` checks page ownership and backing lifetimes.

| Test | What it tests |
| --- | --- |
| `dispatcher_backing_retry` | Failed file writeback retains its context, retries through Dispatcher/Request, and releases exactly once after success. |
| `shared_resident_accounting` | Shared page aliases reuse backing, avoid double charging, and retain the charge until the final owner releases it. |
| `address_space_lifecycle` | A new isolated user page-table root starts with unmapped user addresses. |
| `anonymous_page` | Allocation, translation, duplicate rejection, protection metadata, discard, and reservation release. |
| `clone_isolation` | Cloned roots initially share backing and destroying a clone preserves its source. |
| `region_reservation` | Page-rounded reservation metadata, lookup/type, and complete release. |

### ProcessManager

`ProcessManagerTests.cpp` checks process records and their owned contexts.

| Test | What it tests |
| --- | --- |
| `usage_aggregation` | Own versus reaped-child CPU/fault accounting, subsecond normalization, descendant propagation, and no invented disk I/O. |
| `private_task_retirement` | Thread-private descriptor contexts survive other owners retiring and drain references after the last owner exits. |
| `resource_reference_ownership` | Shared/private descriptor tables retain exactly their owned file references and isolate private flags. |
| `resource_copy_rollback` | Partial descriptor-copy failure rolls back acquired references without changing source entries. |
| `context_copy_isolation` | Copied filesystem/signal contexts isolate cwd, umask, and dispositions while shared owners retain lifetime. |
| `lifecycle_identity` | Current identity, process-slot lookup, running-process protection, capacity, and null-root rejection. |
| `descriptors` | Install/lookup/remove, descriptor flags, shared-table nested transactions, and capacity bounds. |
| `task_ownership` | Attach, idempotent duplicate attach, detach, and reattach. |
| `limits_umask` | Umask exchange and rejection of soft limits above hard limits. |
| `group_reference` | Supplementary-group list allocation, count, and copied ID contents. |
| `initial_user_stack` | Owned user-stack construction, alignment, argc, argument pointers, and copied text. |

### TaskManager

`TaskManagerTests.cpp` checks task slots, running identity, and retirement.

| Test | What it tests |
| --- | --- |
| `system_task_statistics` | Live-thread statistics exclude idle/reserved/exiting slots and include attached peer threads. |
| `thread_cpu_accounting` | User/kernel CPU charges follow task ownership, aggregate across threads, exclude sleep, and survive retirement. |
| `allocation_lookup` | Allocated-task discovery, count changes, removal, and count restoration. |
| `dispatch_claim` | Dispatch claims prevent reclamation and double free is rejected. |
| `retirement_cleanup_once` | Priority cleanup runs outside the scheduler lock exactly once across repeated retirement/free. |
| `name_and_current_identity` | Current-task lookup, running-task protection, name round trip, and capacity. |

### TimeManager

`TimeManagerTests.cpp` combines controlled counter inputs with actual clock queries.

| Test | What it tests |
| --- | --- |
| `source_selection_and_conversion` | HPET/PM-timer fallback, fractional conversion precision, overflow, and unusable-source rejection. |
| `source_wrap_and_interleaving` | Hardware wrap extension, competing-reader retry, unchanged samples, and extended-epoch overflow. |
| `missing_rtc_and_deadlines` | Explicit missing-UTC behavior, usable monotonic/raw clocks, sleep-clock rules, and relative/absolute deadline arithmetic. |
| `fallback_alarm` | Alarm arming/cancellation from monotonic time without an RTC and failure without a usable source. |
| `cpu_expiration_and_limits` | User/profiling timers, periodic catch-up, ignored signals, SIGXCPU/SIGKILL thresholds, and exited-process exclusion. |
| `invalid_clock_deadline` | Invalid clock IDs, negative seconds, and nanosecond bounds. |
| `monotonic_resolution` | Nondecreasing clock samples and positive reported resolution, or explicit unsupported skip. |
| `timer_input_validation` | Invalid interval-timer selectors for query and update. |
| `absolute_deadline_arithmetic` | Exact seconds/nanoseconds conversion and overflow rejection. |
| `alarm_expiration` | Deadline-based expiration publishes SIGALRM once and leaves no remaining canceled time. |

### InitRamFileSystemManager and ResourceFileSystem

`InitRamFileSystemManagerTests.cpp` checks archive indexing; `ResourceFileSystemTests.cpp`
checks backend contracts.

| Manager / test | What it tests |
| --- | --- |
| `InitRamFileSystemManager/enumeration_lookup` | Bounded entry inventory, names, backing bytes, and agreement between enumeration and lookup. |
| `InitRamFileSystemManager/missing_bounds` | Past-end/overflow indexes and missing paths. |
| `ResourceFileSystem/backend_validation` | Unsupported backend and missing-node/parent rejection. |
| `ResourceFileSystem/mutation_rejection` | Read-only cpio directory mutation and missing-entry truncation errors. |
| `ResourceFileSystem/truncate_growth_boundary` | Archive files cannot grow beyond their backing; unavailable archive entries are reported explicitly. |

## 4. Logic unit tests after Logic initialization

`logictests.cpp` runs after `LogicLayerFactory::Create` has constructed and wired
the managers. Resource remains available, but the root filesystem has not yet
been mounted by `kmain`. Scheduler initialization seeds bootstrap and idle-task
state without activating timer scheduling.

Each manager has a separate `logic/<Manager>` capability case. Behavior names
below omit the `logic/<Manager>/` prefix.

### Scheduler

`SchedulerTests.cpp` checks decisions and ownership without starting normal scheduling.

| Test | What it tests |
| --- | --- |
| `initialization` | Scheduling remains inactive and the BSP has seeded process/task identity. |
| `idle_placement` | Every CPU retains a distinct pinned idle task after priority refresh. |
| `priority_permissions` | Privileged priority changes, unprivileged improvement rejection, allowed reductions, and preserved state after errors. |
| `queue_validation` | Invalid process queue/dispatch requests and missing wait-sequence rejection. |
| `deferred_retirement` | Pending dispatch claims delay retirement until release, after which retry unlinks and reclaims the task. |

### VirtualFileSystem

`VirtualFileSystemTests.cpp` uses controlled file/backend objects.

| Test | What it tests |
| --- | --- |
| `mapping_prepare_ownership` | Shared-write callback requirements, permission ceilings, borrowed ownership on failure, and balanced reservations. |
| `path_join` | Relative joining, absolute replacement, short buffers, and missing inputs. |
| `anonymous_inode` | Inode type, mode, and initial metadata. |
| `file_reference` | Retain/release counts and null-handle rejection. |
| `positioned_io` | Backend dispatch, byte preservation, shared offset rules, seek/EOF, and error propagation. |

### ElfMapper

`ElfMapperTests.cpp` builds controlled ELF metadata and read failures.

| Test | What it tests |
| --- | --- |
| `input_validation` | Missing file, metadata, and inode inputs. |
| `truncated_header` | Short ELF headers are rejected without allocating metadata. |
| `parse_load_segment` | ELF64 entry/load metadata, BSS size, and preservation of file position. |
| `malformed_segment` | File bytes exceeding segment memory size are rejected. |
| `library_contract` | Executable/library distinction, load-bias alignment/collision, nonoverlapping placement, address limits, and unchanged heap. |
| `library_rollback` | A read failure removes allocated mappings, metadata, and reservations. |

### CredentialManager

`CredentialManagerTests.cpp` checks direct credential operations.

| Test | What it tests |
| --- | --- |
| `identity_query_change` | Credential queries, no-op updates, state preservation, and invalid UID rejection. |
| `groups` | Privileged supplementary-group updates, values/counts, and short output buffers. |
| `unprivileged_rejection` | Dropped privilege cannot regain root and denied changes preserve identity. |

### EventManager

`EventManagerTests.cpp` checks notification and descriptor basics.

| Test | What it tests |
| --- | --- |
| `notification_sequence` | Notification advances the published generation. |
| `counter_nonblocking` | Counter creation, initial-value drain, exact output, and empty nonblocking reads. |
| `epoll_membership` | Add/delete membership and duplicate/missing-watch errors. |
| `flags_validation` | Unknown event-counter and epoll creation flags are rejected. |

### SynchronizationManager

`SynchronizationManagerTests.cpp` covers nonblocking futex paths and robust metadata.

| Test | What it tests |
| --- | --- |
| `wait_mismatch_wake_empty` | Compare mismatch never sleeps; empty wake/requeue operations return correctly. |
| `validation` | Word alignment, nonempty bitsets, and nonnegative requeue counts. |
| `robust_metadata` | Robust-list clearing/query, ABI size, and invalid-task rejection. |

### SignalManager

`SignalManagerTests.cpp` checks dispositions, pending records, and stack state.

| Test | What it tests |
| --- | --- |
| `action_roundtrip` | Action set/query, mask sanitization, invalid signals, and uncatchable-signal protection. |
| `mask_temporary_restore` | Mask replacement and restoration strip SIGKILL/SIGSTOP. |
| `realtime_queue` | RT number ordering, per-number FIFO payloads, bounded queues, separate process/thread budgets, and discard. |
| `reserved_exit_delivery` | Reserved child-exit capacity survives ordinary queue exhaustion and child reaping. |
| `alternate_stack` | Disabled-stack state and undersized-stack rejection. |

### ProcessController

`ProcessControllerTests.cpp` checks process identity and owned mappings.

| Test | What it tests |
| --- | --- |
| `group_identity` | Current group/session queries and invalid process selectors. |
| `anonymous_map_protect_unmap` | Anonymous mapping, translation, aligned protection, unmapping, and unmapped-range errors. |
| `child_tid` | Clear-child-TID registration returns the task identity and records ownership. |

### SocketManager and InterProcessCommunicationManager

`SocketManagerTests.cpp` and `InterProcessCommunicationManagerTests.cpp` use mapped
user buffers and disposable identities.

| Manager / test | What it tests |
| --- | --- |
| `SocketManager/protocol_validation` | Unsupported family, type, and protocol requests fail explicitly. |
| `SocketManager/local_pair_transfer` | Nonblocking local-pair byte transfer, empty receive, shutdown, and EOF. |
| `InterProcessCommunicationManager/pipe_nonblocking` | Empty pipe reads, payload transfer, and EOF after the final writer closes. |
| `InterProcessCommunicationManager/memory_file` | Anonymous memory-file creation, write/seek/read, and preserved contents. |
| `InterProcessCommunicationManager/flags_validation` | Unknown pipe and memory-file flags are rejected. |

## 5. Post-start integration tests

`poststartkerneltests.cpp` combines live scheduling, mounted filesystem operations,
user execution, memory, signals, and communication. The BSP opens the group while
all CPUs enter SMP work. APs subsequently service TLB-test commands. The BSP waits
for all SMP workers and safe cleanup, records their aggregate, and then runs TLB,
mounted VFS, ELF, task/process, memory, event, wait, signal, and communication tests.

If root mounting failed, the BSP reports `poststartkernel_vfs_mount` as FAIL and
all CPUs return from this group without waiting for workers they never started.
Request testing can still be entered afterward; the mount failure remains in the
overall result.

Names below omit `poststartkernel/`. The `ElfMapper` capability case is separate
from `ElfMapper/library_backing`.

| Test | What it tests |
| --- | --- |
| `smp/scheduler_and_filesystem` | Per-CPU process identity, disjoint shared-file writes, scheduler dispatch, worker completion, and safe reclamation. |
| `memory/cross_cpu_tlb_remap` | A remote CPU observes replacement physical backing after a shared virtual address is remapped. |
| `filesystem/mounted_io_and_lifecycle` | Mounted lookup, directory/stat/read/write/seek, content restoration, sync, close, and process-owned descriptors. |
| `ElfMapper/library_backing` | Independent processes share executable library pages, private writes detach writable pages, and mapping preserves heap state. |
| `executable/user_entry_return` | A real user ELF verifies syscall argument/return registers, stack, direction flag, FP/SIMD state, signal restorer, and exit. |
| `executable/dynamic_interpreter` | A launcher execs a musl-linked PIE; the interpreter loads its library, runs initialization, checks BSS/data, and completes its probe/exit. |
| `process/task_context_tls` | Task A→B→A switching preserves execution progress and isolated TLS bases, and refuses reclamation of the running task. |
| `process/scheduled_identity` | Scheduled tasks observe their own process/address-space identities and both complete their markers. |
| `memory/file_shared_io_writeback` | Shared file mappings and independent file I/O see each other's writes; unmap writes back restored data. |
| `memory/user_copy_and_protection` | Real user-copy accepts mapped writable memory and rejects read-only destinations/unmapped sources. |
| `memory/copy_on_write` | Writing through user-copy detaches cloned backing while the other root retains original bytes. |
| `events/counter_epoll_notification` | Counter writes publish epoll readiness/data and draining removes level readiness. |
| `timers/scheduled_sleep` | A scheduled sleep resumes no earlier than its monotonic deadline. |
| `scheduler/concurrent_siblings` | Two threads in one process run on different CPUs and exercise per-node allocation and cross-node freeing. |
| `synchronization/shared_cross_process` | Distinct processes/virtual aliases rendezvous through a shared futex key while unrelated backing and private wakes stay isolated. |
| `synchronization/shared_requeue` | Shared compare/requeue transfers retained backing keys and preserves waiter lifetime across mapping removal. |
| `synchronization/shared_wake_op` | A shared wake operation updates canonical backing and wakes exactly the intended waiter. |
| `synchronization/scheduled_wait_wake` | A real queued futex waiter parks, a peer wakes it, and exactly one waiter is removed. |
| `events/scheduled_wait_notify` | A parked event waiter resumes after a peer publishes a new generation. |
| `signals/user_frame_roundtrip` | Signal handler frames restore context, RT queues preserve repeated payloads, restart rules hold, and handlers/signalfd share consumption. |
| `ipc/cross_process_pipe` | A transferred pipe endpoint carries bytes between distinct process address spaces. |
| `sockets/cross_process_local_pair` | Transferred local-socket endpoints preserve payload between distinct process identities. |

The SMP scheduler and PID/VFS checks in `schedulertests.cpp` and `smpselftests.cpp`
publish local pass/failure counts and masks with atomics. They contribute checks
to the single BSP-owned SMP case, not a case per CPU. Four CPUs exercise the
normal configuration; cross-CPU remap and concurrent-sibling cases require at
least two. Bounded waits use `PollBudget` or deadlines, but cannot interrupt a CPU
that never returns from a context switch.

Some reporting cases contain several integration contracts:

| Inner test | What it tests |
| --- | --- |
| SMP PID/VFS task | Each CPU's disposable process uses its own file slot while preserving current identity and restoring shared contents. |
| SMP scheduler tasks | Multiple enqueued tasks enter on the intended CPU with the expected process identity and complete before cleanup. |
| Mounted namespace | Directory traversal, metadata, and read-only mutation errors use the mounted cpio backend. |
| Mounted I/O | Sequential/positioned access, seek, bounded truncation, and restored file contents agree across handles. |
| Mounted process lifecycle | Process-owned file handles and two-process synchronization retain correct offsets, ownership, and data. |
| User syscall transition | The static ELF checks argument/callee-saved registers, user stack, direction flag, SIMD state, MXCSR, and x87 control. |
| User signal transition | Handler entry on the alternate stack, signal context, restorer, and actual `rt_sigreturn` restore user execution. |
| Dynamic library initialization | The musl loader runs the constructor and preserves initialized data, zero-filled BSS, and writable library state. |
| Signal restart/consumption | SA_RESTART rewinds eligible interrupted calls; repeated RT records are consumed once across handlers and signalfd. |
| CPU expiration delivery | SIGVTALRM, SIGPROF, and SIGXCPU reach installed user handlers after accounting publishes them. |

## 6. Request syscall tests from the kernel side

`requesttests.cpp` runs after integration on the BSP. It first checks user-copy
fixtures, then Process, Scheduler, Time, Memory, VFS, Event, Sync, Signal,
Credential, System, and Socket managers. Each manager is reported as
`request/<manager>/syscalls`; `request/user_copy` is separate.

`RequestTestSupport.cpp` builds `arch_syscall_frame_t` arguments and calls
`dispatcher_dispatch_syscall`. Most behavioral calls therefore exercise Dispatcher,
Request decoding/user-copy, and the lower layers while executing in kernel mode.
They do not execute the CPU's userspace syscall-entry instruction. Direct handler
calls supplement this with null-frame and missing-dependency validation in the
suites that define those checks. Real userspace entry/return is covered by the
post-start executable fixtures instead.

The tables list each registered syscall subtest separately. Additional rows
identify substantial contracts nested inside those validators. A registered name
does not imply complete Linux syscall coverage: tests exercise the implemented
Arx contract, including explicit errors for unavailable behavior.

### User-copy fixture

`RequestTestSupport.cpp` checks alias read/write-through, rejects kernel and
out-of-range pointers, and verifies read-only output rejection. The other Request
validators exercise cross-page buffers and mapping/length faults within their
own syscall contracts. `request_user_fixture_t` maps kernel-owned buffers into
user-visible aliases, including nested iovec buffers, rather than bypassing
Request's user-access validation.

### ProcessRequestManager

`ProcessRequestManagerTests.cpp` checks process syscall contracts.

| Test | What it tests |
| --- | --- |
| `exit` | A scheduled child exits, publishes status, and releases resources before reaping. |
| `exit_group` | Group exit follows the child-execution and cleanup/reaping contract. |
| `fork` | Child execution, private address-space/COW behavior, inherited state, and reaping. |
| `vfork` | Shared-address-space child execution and parent resumption after child completion. |
| `clone` | Supported flag combinations, shared/private resources, thread identity, TID publication, inheritance, and rollback. |
| `clone3` | Structured clone arguments, size/flag validation, sharing combinations, and child execution. |
| `execve` | Image/stack replacement, private contexts after shared-resource cloning, CLOEXEC closure, and ignored-disposition preservation. |
| `wait4` | Nonblocking wait, stopped/continued/exit status, output faults, and deferred retirement before reaping. |
| `waitid` | Structured child status, observation/reaping options, and deferred-retirement visibility. |
| `set_tid_address` | Clear-child-TID registration, task identity, and the zero written by cleanup. |
| `gettid` | Current task identity through syscall dispatch. |
| `getpid` | Current process identity through syscall dispatch. |
| `getppid` | Parent identity through direct handler and Dispatcher paths. |
| `getpgid` | Process-group lookup and missing-process errors. |
| `getpgrp` | Current process-group identity. |
| `setpgid` | Group changes and invalid group/process requests. |
| `getsid` | Session lookup and missing-process errors. |
| `setsid` | Session creation and existing-group-leader rejection. |
| Clone resource matrix | CLONE_FILES/FS/SIGHAND/THREAD independently select shared contexts, while child exit drains only owned references. |
| Clone publication failure | Invalid namespace/pidfd combinations fail explicitly and failed publication does not perform clear-child-TID writes. |
| Child-exit reservation | Reserved RT notification capacity retains child status despite ordinary queue exhaustion. |

### SchedulerRequestManager

`SchedulerRequestManagerTests.cpp` checks yield, CPU identity, and affinity, with
additional policy/priority checks nested within its affinity validator.

| Test | What it tests |
| --- | --- |
| `sched_yield` | Dispatch accepts an explicit yield. |
| `sched_getaffinity` | Boot CPU mask, stored restrictions, undersized buffers, and invalid output pointers. |
| `getcpu` | Current CPU/NUMA node outputs and rejection of kernel pointers. |
| `sched_setaffinity` | Restriction, empty-mask rejection, actual self migration, return migration, and mask restoration. |
| `getpriority` / `setpriority` | Nice changes, raw return encoding, group/user selection, and missing-group errors. |
| `sched_setscheduler` / `sched_getscheduler` | Public round-robin activation and policy readback. |
| `sched_get_priority_min` / `sched_get_priority_max` | Public real-time priority bounds. |
| `sched_rr_get_interval` | The round-robin quantum's timespec ABI. |
| FIFO/RR decisions | Equal-priority FIFO yield, timer behavior, actual elapsed RR charging, and rotation at quantum expiry. |
| Deadline decisions | Partition admission, rollback on overcommit, class precedence, exhaustion/replenishment, and reset-on-fork. |
| Policy permissions | Unprivileged real-time activation and unenforced policies fail explicitly. |

### TimeRequestManager

`TimeRequestManagerTests.cpp` checks clock, sleep, and timer syscall contracts.

| Test | What it tests |
| --- | --- |
| `time` | Seconds return/output agreement, optional output, and explicit missing-RTC behavior. |
| `gettimeofday` | Normalized time, UTC timezone outputs, optional pointers, and exact ABI writes. |
| `clock_gettime` | Implemented clock IDs, normalized output, monotonic ordering, and invalid-clock/buffer handling. |
| `clock_getres` | Supported clock resolution and positive normalized results. |
| `clock_nanosleep` | Relative/absolute deadlines, clock rules, peer progress, overflow, and interruption remainder rules. |
| `nanosleep` | Relative sleep duration, real scheduling of a peer, read-only input, and signal interruption. |
| `getitimer` | Initial/disarmed state and remaining real/CPU timer intervals. |
| `setitimer` | Arming/cancellation, aliasing, committed changes before copyout faults, and real/CPU timer behavior. |
| `alarm` | Replacement/cancellation, rounded remaining seconds, 32-bit decoding, and SIGALRM expiration. |
| Real timer expiration | One-shot and periodic SIGALRM, rearming after consumption, and interruption of parked waits. |

### MemoryRequestManager

`MemoryRequestManagerTests.cpp` checks process lifetime pins before running its
memory syscall contracts.

| Test | What it tests |
| --- | --- |
| `mmap` | Placement/hints, fixed replacement and collision, zeroing, anonymous/shared/file backing, EOF reservations, and rollback. |
| `munmap` | Partial/spanning unmap, holes, released pages, intact neighbors, and reusable reservations. |
| `mprotect` | Subrange metadata/PTE permissions, PROT_NONE restoration, invalid ranges, and rejection of untracked backing. |
| `mincore` | Residency output across pages, PROT_NONE, exact vector length, holes, and zero-length queries. |
| `madvise` | Accepted hints preserve data; DONTNEED discards it; invalid/unsupported advice and holes return explicit errors. |
| `brk` | Query/growth/shrink, zeroing, partial-page retention, collisions, and unchanged break after invalid requests. |
| Memory lifetime | Process/root pins prevent teardown and unsafe shared-root mutation while preserving metadata lifetime. |

### VfsRequestManager

`VfsRequestManagerTests.cpp` checks filesystem syscall contracts. The boot cpio backend
rejects namespace mutation; a controlled writable backend separately exercises
successful mutation, metadata, and mount callbacks.

| Test | What it tests |
| --- | --- |
| `openat` | Directory-relative lookup and rejection of a nondirectory base descriptor. |
| `open` | Opening existing files, returned descriptor lifetime, and invalid inputs. |
| `creat` | Create/open flag behavior and backend-specific outcomes. |
| `close` | Descriptor release and repeated-close EBADF. |
| `close_range` | Inclusive closure, invalid ranges, CLOEXEC, and descriptor-table unsharing. |
| `mkdirat` | Relative directory creation's read-only-backend and pointer errors. |
| `mkdir` | Directory creation's read-only-backend and pointer errors. |
| `read` | Exact file bytes, requested progress, and shared-position advancement. |
| `write` | Visible file modification, requested progress, position advancement, and restored fixture contents. |
| `pread64` | Exact positioned bytes, preserved shared offset, and invalid descriptor/offset handling. |
| `pwrite64` | Positioned content changes, preserved shared offset, and invalid descriptor/offset handling. |
| `readv` | Scatter output across vectors with correct data and sequential position. |
| `writev` | Gathered writes produce the expected file bytes and sequential position. |
| `preadv` | Positioned scatter reads preserve the shared file offset. |
| `pwritev` | Positioned gathered writes preserve offset and reject invalid positions. |
| `lseek` | End-relative positioning, file length, and invalid origin rejection. |
| `getcwd` | Working-directory output and insufficient-buffer errors. |
| `chdir` | Directory changes and nondirectory rejection. |
| `fchdir` | Descriptor-based directory changes and regular-file rejection. |
| `getdents64` | Bounded directory record output and invalid descriptor handling. |
| `unlinkat` | Read-only namespace rejection plus flag/pointer validation. |
| `unlink` | Read-only unlink rejection and invalid paths. |
| `rmdir` | Busy root-directory and invalid-path errors. |
| `fcntl` | Descriptor/status flags, duplication, and preservation of access mode. |
| `dup` | A new descriptor keeps the open file alive independently. |
| `dup2` | Replacement of a selected descriptor and source/target lifetime. |
| `dup3` | Selected-target duplication and identical-source/target rejection. |
| `newfstatat` | Relative metadata, flag validation, and missing paths. |
| `stat` | Path metadata, missing files, and invalid output pointers. |
| `fstat` | Descriptor metadata type/link/size/block fields and invalid descriptors. |
| `lstat` | Metadata without final symlink traversal and invalid path pointers. |
| `renameat` | Relative rename errors on read-only cpio and invalid paths. |
| `rename` | Read-only rename rejection and invalid paths. |
| `readlinkat` | Directory-relative symlink target extraction. |
| `readlink` | Symlink target bytes without treating them as an ordinary file read. |
| `ioctl` | Unsupported operation ENOTTY and invalid descriptor EBADF. |
| `linkat` | Read-only hard-link rejection and flag/path validation. |
| `link` | Read-only hard-link rejection and invalid paths. |
| `symlinkat` | Relative symlink creation and target readback. |
| `symlink` | Symlink creation and exact target readback. |
| `faccessat` | Directory-relative existence/permission checks and mode/flag validation. |
| `faccessat2` | Access checks with explicit supported flag handling. |
| `access` | Existence, denied execution, missing paths, and invalid modes. |
| `fchownat` | Relative ownership changes, empty-path handling, and credentials/backend restrictions. |
| `chown` | Path ownership changes and metadata permission rules. |
| `fchown` | Descriptor ownership changes and committed metadata readback. |
| `lchown` | Ownership changes without following the final link. |
| `fchmodat2` | Mode updates with explicit flags and empty-path behavior. |
| `fchmodat` | Directory-relative mode changes and validation. |
| `chmod` | Path mode changes and owner/read-only restrictions. |
| `fchmod` | Descriptor mode changes and preserved unrelated metadata. |
| `mount` | Backend registration/dispatch, source/options copying, privilege, bind/move/remount, and cleanup. |
| `statfs` | Path filesystem statistics with valid block size and missing-path errors. |
| `fstatfs` | Descriptor filesystem statistics and invalid descriptors. |
| `mknodat` | Relative special-node creation's backend and path validation. |
| `mknod` | Special-node creation's backend and path validation. |
| `truncate` | Shrink, negative-length rejection, and backing-limited growth. |
| `ftruncate` | Descriptor shrink preserves offset and validates length, growth, and descriptor. |
| `pipe` | Endpoint transfer, EOF/EPIPE, atomic writes, blocking wakeups, and cleanup. |
| `pipe2` | Creation flags plus nonblocking and packet-mode pipe behavior. |
| `memfd_create` | Anonymous sparse file data, seals, shared/private mappings, EOF handling, and backing lifetime. |
| Writable backend mutations | mkdir/mknod/link/rename/symlink/unlink and truncate dispatch, cache updates, and inotify records. |
| Mount ownership/permissions | Creation IDs/umask/setgid, credential checks, read-only bind isolation, and one final backend release. |
| Pipe concurrency | Full-buffer blocking, PIPE_BUF atomicity, vector writes, poll closure, and packet truncation/fault preservation. |
| Memfd mapping coherence | File I/O and shared aliases agree; private writes detach; seals, fork, close, and partial unmap preserve ownership. |

### EventRequestManager

`EventRequestManagerTests.cpp` checks readiness and notification syscall contracts.

| Test | What it tests |
| --- | --- |
| `poll` | Ready counts/revents, ignored negative descriptors, POLLNVAL, timeouts, and real parked wakeups. |
| `ppoll` | Poll behavior with timespec deadlines and temporary signal masks. |
| `select` | Ready descriptor sets/counts, zero/finite waits, and descriptor-limit behavior. |
| `pselect6` | Select behavior with temporary masks and restoration on timeout/interruption. |
| `epoll_create` | Legacy creation argument validation and instance behavior. |
| `epoll_create1` | Creation flags and CLOEXEC. |
| `epoll_ctl` | ADD/MOD/DEL, duplicate keys, source lifetime, edge/one-shot/exclusive behavior, and nested graphs. |
| `epoll_wait` | Ready event/data ABI, bounded batches, fairness, timeouts, and parked wakeups. |
| `epoll_pwait` | Wait behavior with temporary mask restoration. |
| `epoll_pwait2` | Timespec wait behavior and mask/count validation. |
| `inotify_init` | Notification descriptor behavior and event record delivery. |
| `inotify_init1` | Creation flags, CLOEXEC, and nonblocking notification reads. |
| `inotify_add_watch` | Watch masks/collisions, paths, directory/oneshot behavior, and unlink filtering. |
| `inotify_rm_watch` | Removal queues IN_IGNORED once and rejects invalid watch state. |
| `eventfd` | Counter drain/increment, exact eight-byte ABI, overflow, duplicate lifetime, and blocking reads/writes. |
| `eventfd2` | Counter creation flags, semaphore decrement, and vector/scatter behavior. |
| `signalfd` | Masked pending signals become exact records with sender metadata and single consumption. |
| `signalfd4` | Descriptor flags/mask update plus RT FIFO/payloads, bounded queues, and blocking delivery. |
| Concurrent readiness | Parked waiters retain the original source across descriptor reuse; pipe/socket edges and one-shot rearming retain wakeups. |
| Interrupted readiness | A real peer signal interrupts poll/select/epoll and restores temporary masks. |
| Epoll partial output | Copyout faults preserve untransferred edges and disarm only delivered one-shot records. |
| Nested/exclusive epoll | Depth/cycle limits, propagated edges, fair exclusive selection, and weak-watch final-close cleanup. |
| Inotify unlink identity | Hard links, renames, name reuse, and IN_EXCL_UNLINK distinguish directory-entry identity from live inode ownership. |
| Notification bounds | Short/faulting reads retain records; full queues emit the overflow marker. |

### SyncRequestManager

`SyncRequestManagerTests.cpp` checks futex and robust-list syscall contracts.

| Test | What it tests |
| --- | --- |
| `futex` | Compare/wake, bitsets, deadlines, private/shared keys, requeue, wake-op, PI, and argument validation. |
| `set_robust_list` | Registration, ABI length, clearing, lifetime pins, and owner-death updates. |
| `get_robust_list` | Current/sibling lookup, aliased output order, retained task snapshots, and invalid outputs/IDs. |
| Wake-op arithmetic | Atomic operations, sign extension, masked shifts, compare ordering, and unchanged data after access errors. |
| Requeue FIFO | Requeued waiters follow existing destination waiters and remain safely owned until completion. |
| Priority inheritance | Contention donates priority transitively, beats a medium competitor, hands off ownership, and restores priorities. |
| PI cancellation/death | Timeout/signal cancellation removes donation; owner death publishes OWNER_DIED without requiring robust registration. |
| Shared PI | Distinct processes and virtual aliases identify one shared lock and retain donation across sleeper migration. |
| Requeue PI | Atomic transfer grants ownership before return, donates real-time priority, and restores the owner. |

### SignalRequestManager

`SignalRequestManagerTests.cpp` checks signal syscall contracts.

| Test | What it tests |
| --- | --- |
| `rt_sigreturn` | Restores GP/FP context and signal masks, restarts eligible syscalls, and rejects privileged addresses/reserved FP bits. |
| `rt_sigprocmask` | Replace/query masks, strip unmaskable signals, and validate operation/size. |
| `rt_sigaction` | Raw action layout, query/update, supported child/restart flags, and pointer/flag validation. |
| `sigaltstack` | Stack install/query, active-stack protection, and minimum size. |
| `kill` | Process routing, existence probes, permission checks, and repeated RT signal queuing. |
| `tgkill` | Thread-targeted routing and identity/permission validation. |
| `pause` | A real peer signal wakes the parked task with EINTR. |
| Interrupted waits | Signals interrupt nanosleep/futex, remove queued waiters, and preserve owner-death wake semantics. |
| Ignored RT signals | Installing ignore discards all queued instances. |

`arch_selftest_signal_return` supplies independent wire-layout expectations for
the frame checks in `arch_selftest.cpp`; the executable fixture separately tests
the actual userspace restorer/syscall assembly path.

### CredentialRequestManager

`CredentialRequestManagerTests.cpp` checks identity and group syscall contracts.

| Test | What it tests |
| --- | --- |
| `getuid` | Real UID reflects current credentials and ignores unused arguments. |
| `getgid` | Real GID reflects current credentials and ignores unused arguments. |
| `geteuid` | Effective UID reflects credential changes. |
| `getegid` | Effective GID reflects credential changes. |
| `getresuid` | Three 32-bit IDs, cross-page/aliased outputs, neighboring-byte preservation, and partial-copy order. |
| `getresgid` | Real/effective/saved GID outputs and equivalent fault/alias semantics. |
| `setuid` | Privileged updates, low-32-bit argument decoding, and denied unprivileged changes. |
| `setgid` | Group-ID transitions and privilege enforcement. |
| `setreuid` | Real/effective swaps, saved-ID rules, and invalid unprivileged transitions. |
| `setregid` | Equivalent group-ID transition and privilege rules. |
| `setresuid` | Sentinel no-ops, privileged arbitrary IDs, unprivileged permutations, and atomic rejection. |
| `setresgid` | Equivalent real/effective/saved GID rules. |
| `setfsuid` | Returns the previous filesystem UID and silently refuses forbidden/invalid replacements. |
| `setfsgid` | Equivalent filesystem GID behavior. |
| `getgroups` | Count queries, sorted 32-bit IDs with duplicates, cross-page output, and capacity errors. |
| `setgroups` | Privilege-first validation, bounded full-size lists, rollback, clearing, and immutable inherited backing. |
| Credential slot reuse | Child cleanup releases group references and reused process slots do not retain old identity. |

### SystemRequestManager

`SystemRequestManagerTests.cpp` checks process configuration and system-information contracts.

| Test | What it tests |
| --- | --- |
| `arch_prctl` | x86_64 TLS-base state and invalid requests through the architecture fixture. |
| `umask` | Mask exchange and preservation/restoration of process filesystem state. |
| `prlimit64` | Current/cross-process limit exchange, credential permission checks, aliasing, and commit-before-copyout-fault behavior. |
| `getrlimit` | Cross-page limit snapshots and initial defaults. |
| `setrlimit` | Soft/hard bounds, irreversible hard-limit reduction, descriptor allocation effects, and invalid proposals. |
| `prctl` | Task name set/get, 15-byte truncation, mapped-boundary strings, and preservation after failed input. |
| `uname` | Identity strings, target machine, exact struct size, and NUL termination. |
| `sysinfo` | Uptime, memory units, live-thread count, exact ABI writes, and zeroed padding. |
| `getrandom` | Invalid flags and explicit missing-entropy behavior; a successful gap validator reports SKIP rather than entropy coverage. |
| CPU limits | Aliased exchanges preserve old values while committing timer enforcement; unsupported privilege overrides are denied. |

### SocketRequestManager

`SocketRequestManagerTests.cpp` checks local-socket syscall contracts.

| Test | What it tests |
| --- | --- |
| `socket` | Supported local types/flags, 32-bit decoding, and explicit unsupported family/protocol errors. |
| `socketpair` | Connected local endpoints, payload transfer, and descriptor ownership. |
| `bind` | Abstract names including embedded NULs, pathname backend creation, duplicate names, and permissions. |
| `connect` | Abstract/pathname resolution, listener queues, replacement inode identity, and access checks. |
| `listen` | Bound-listener prerequisite and connection queue behavior. |
| `accept4` | Creation flags, blocking wakeup, and output faults preserving pending connections. |
| `accept` | Queued connection acceptance and peer name output. |
| `recvfrom` | Blocking/nonblocking receive, peek/truncation, sender address, and fault-safe queue retention. |
| `recvmsg` | LP64 scatter headers, ancillary rights/credentials, truncation, and ownership cleanup. |
| `sendto` | Stream/datagram delivery, destination handling, bounded queues, and oversized-message atomic failure. |
| `sendmsg` | Gathered payload, ancillary validation, retained rights, and vector-overflow bounds. |
| `shutdown` | Direction validation, queued-data preservation, EOF/readiness, EPIPE, and SIGPIPE suppression. |
| `getsockopt` | SO_TYPE/SO_PEERCRED wire values, output bounds, and zero-length queries. |
| `setsockopt` | Buffer sizing and SO_PASSCRED credential delivery. |
| `getsockname` | Exact abstract-name bytes, supplied-capacity truncation, and full-length reporting. |
| `getpeername` | Connected peer identity and unconnected ENOTCONN. |
| SCM_RIGHTS lifetime | Peek/prefix delivery, descriptor limits, ancillary boundaries, sender exit, and unreachable reference-cycle reclamation. |
| SCM_CREDENTIALS | Credentials are validated and captured at send time; forged IDs fail and missing control space reports truncation. |
| Pathname lifecycle | Rename/unlink/rebind distinguishes socket inode identities, respects cwd/permissions, and releases unlinked nodes. |
| Blocking queues | Full sends wake after peer drain, real signals interrupt waits, and failed copyout preserves payload. |
| Socket readiness | Poll/epoll sees edges, shutdown and final close; dup preserves the endpoint after original close. |

## 7. Framework accounting and fixture lifetimes

### SelfTestManager

The singleton uses trivial static storage so early C tests do not depend on C++
global constructors. `RunPlatformTests` explicitly calls `Initialize` before any
recording. Names are borrowed for the boot lifetime; callers use stable strings.

| Operation | What it does |
| --- | --- |
| `Initialize()` | Reset counters, active case, retained details, group stack, and finalization state. |
| `GroupBegin()` / `GroupEnd()` | Save counter snapshots and print nested-group deltas; groups do not add test cases. |
| `CaseBegin()` | Set the single active case, reserve a retained result slot, and attempt a monotonic start timestamp. |
| `CaseEnd()` | Count one executed PASS/FAIL case from its assertion totals and print duration. |
| `CaseFinish()` | Finish PASS/FAIL normally, or separately count SKIP/BLOCKED without adding an executed case. |
| `RecordFailureDetail()` | Send details to the error logger and append a bounded preview to the active retained case. |
| `PrintSummary()` | Print current totals, failed names, retained details, and overall status. |
| `Finalize()` | Atomically permit one final summary/error-log dump. |
| `AbortUnsafe()` | Fail an active case when present, report blocked remaining work, finalize, disable interrupts, and halt. |

`tests_ran`, `tests_passed`, and `tests_failed` count executed cases. The per-case
`checks(pass=… fail=…)` fields count assertions. SKIP and BLOCKED have separate
counters. `SELFTEST_COUNT_IMPLEMENTED` changes the summary label to `implemented`;
it does not create syscall subcases or change case accounting.

The manager retains 256 cases, 256 failed names, 256 bytes per case's detail
buffer including its terminator, and 16 nested group snapshots. Beyond retained
capacity, result lines still print and executed totals still advance; the summary
reports omitted records. Overflow is not an automatic test failure. The single
active case and group stack are not a concurrent reporting API, despite atomic
counter updates. Tests must not overlap cases or record from AP workers.

`ManagerSuite` opens/closes a group through its constructor/destructor. Its default
capability check is a separate case. Request uses `CapabilityReport::InContract`
so availability is checked inside each manager's contract case instead.
`TestCase` closes through its destructor, counts `Check`/`Equal` calls, and marks a
case with no checks BLOCKED. Assertion failure wins over a requested skip/block.
The lower-level C `CaseEnd` path can still report PASS with zero checks, so case
totals alone do not demonstrate meaningful coverage.

### Fixtures

| Helper | Responsibility and constraint |
| --- | --- |
| `TestFixture.hpp` | Temporarily install disposable process/task/address-space identity with preemption disabled; never schedule or sleep while holding it. |
| `ScheduledFixture.hpp` | Run real main/peer tasks for parked-wait integration and own their retirement. |
| `MemoryFileFixture.hpp` | Supply a controlled file/backend for ELF and VFS contracts. |
| `TestFile.hpp` | Retain the file behind a descriptor under its table guard, then release that file reference during cleanup. |
| `SocketFileSystemFixture.hpp` | Supply a writable backend for socket namespace and filesystem callback contracts. |
| `SignalAbiFixture.hpp` | Supply independently declared signal wire-layout expectations. |
| `RequestTestSupport.hpp` | Own request reporting contexts, real user aliases, isolated process fixtures, runnable peers, and cleanup checks. |
| `PollBudget.hpp` | Bound cooperative control waits by five seconds when a clock exists and by 100 million polls regardless of clock availability. |
| `smpfixtures.hpp` | Retry process creation during bounded contention on a shared root's lifetime pin. |

Declare owned fixtures after their `TestCase`, so reverse destruction cleans up
before reporting the case. Request's `ProcessFixture` replaces process identity
while retaining the real calling task; it is distinct from the synchronous
`TestFixture`. Request peers explicitly manage preemption and scheduling for
blocking calls. User aliases must be released before destroying their owning
address space. Cleanup failures that make later tests unsafe call `AbortUnsafe`
rather than continuing with contaminated identity or live peers.

`SuiteContext` prints `RUN` and `PASS`/`FAIL`/`SKIP` lines for each registered
syscall and a per-manager `syscalls(pass=… fail=… skip=…)` total. Its expectations
also increment the enclosing `TestCase` assertion counts. A syscall can contain
many checks; those counters are different levels of aggregation. Unsupported
entries skip with a reason. Unimplemented entries run their explicit error
validator first and skip only if it succeeds. A manager with some successful
contracts and a coverage gap can therefore PASS while printing a syscall SKIP.

## 8. Build, boot, and capture results

Run commands from the repository root. Build prerequisites are GNU make, gcc/g++,
nasm, binutils/readelf, cpio, parted, dosfstools (`mkfs.fat`), and mtools
(`mcopy`/`mmd`), plus musl-gcc and an x86_64 musl runtime. QEMU and the repository's
UEFI firmware are needed to boot; GDB with Python support is needed for the
debugger extension. `Makefile` checks bootloader/config/linker inputs when making
the disk image.

```sh
make -j4 x86_64
```

The output is `bin/arx-x86_64.img`, with symbols in `bin/kernel-x86_64.elf` and
the archive in `bin/initramfs.cpio`. The build creates the static
`test_syscall_exit.elf` and runs `build-dynamic-fixtures.sh` to package
`dynamic_launcher.elf`, `dynamic_fixture.elf`, `libarx_dynamic_test.so`, the musl
loader/libc alias, and the musl license. The compiler/runtime can be selected with
`MUSL_CC` and `MUSL_RUNTIME`; defaults are `musl-gcc` and
`/lib/ld-musl-x86_64.so.1`. The check rejects a non-musl compiler/runtime and runs
on incremental builds as well.

Keep the checked-in initramfs data, including `test.txt` and the clone-matrix
directory: several tests open or temporarily modify those fixtures and restore
their contents. The dynamic probe requires its bundled loader and library, not
just the executable file.

For the configured graphical run, `make qemu-x86_64` uses q35, `-cpu max`, four
CPUs, 1 GiB RAM, and serial stdio. The QEMU target does not build its image first.
For a bounded headless run with a serial log:

```sh
timeout 45s qemu-system-x86_64 \
    -m 1024 -smp 4 -cpu max -machine q35 \
    -display none -monitor none \
    -serial file:/tmp/arx-selftests.log \
    -drive if=pflash,format=raw,readonly=on,file=firmware/x86_64/X86_64_UEFI.fd \
    -drive file=bin/arx-x86_64.img,format=raw \
    -snapshot -no-reboot
```

`-snapshot` isolates image writes for the run. QEMU needs a writable temporary
directory for its snapshot. Forty-five seconds is an observation window, not a
test deadline or a guarantee on slower machines. Inspect the captured serial log
before classifying the result; timeout exit code 124 is expected even after a
completed suite because the kernel remains running.

Normal `[SELFTEST]` results print without `DEBUG=1`. For source debugging, build
with `make -B DEBUG=1 x86_64`, or use `make x86_64-debug`, which forces that rebuild
and starts the debugger. Switching optimization/debug flags without a forced or
separate-output rebuild can leave old objects, because make does not track flags
as source dependencies. `BUILD_DIR` and `BIN_DIR` can be overridden on the make
command line for separate outputs.

## 9. Reading results and recognizing incomplete runs

| Observation | Meaning |
| --- | --- |
| Case `PASS` | Its reported failure count is zero; inspect its checks and any nested syscall gaps. |
| Case `FAIL` | At least one assertion failed, or FAIL was explicitly requested. |
| `SKIP` | An explicit environmental/coverage exclusion; it does not increase executed totals. |
| `BLOCKED` | Required execution/checks could not happen; it does not increase executed totals and makes a failure-free summary INCOMPLETE. |
| Overall `FAIL` | At least one executed case failed, taking precedence over blocked work. |
| Overall `INCOMPLETE` | No executed failures, but at least one case was blocked. |
| Overall `PASS` | No executed failures and no blocked cases; skipped syscall/contracts can still exist. |
| Missing duration | No usable monotonic start/end pair was available; this is not by itself a case failure. |

Follow `GROUP START`/`GROUP END`, case `START`/`RESULT`, nested Request results,
and the final `Arx kernel selftest summary`. An unmatched START identifies the
last entered case, not necessarily the code currently stuck: a worker, interrupt,
or scheduled peer may hold progress. A missing summary is an unfinished observed
run; it may be slow, blocked in firmware/boot, hung, faulted, or externally stopped.
A timeout alone cannot distinguish those causes.

An unsafe abort can print a complete-looking summary early. Look for
`init/remaining_groups`, a failed active case, and cleanup/initialization errors.
The other diagnostic cases are `poststartkernel_vfs_mount`,
`request/capabilities`, and `resource/DmaManager/reserved_memory`. Check phase
completion and retained/omitted counts rather than expecting every source-defined
case name to appear in every run. Earlier missing prerequisites can prevent later cases
from being entered at all.

For Request, read the syscall summary as well as its manager result. In particular,
`getrandom` currently validates invalid flags and the missing entropy subsystem,
then reports SKIP; this does not establish random-byte generation. Optional clock
paths may also print unavailable diagnostics inside an otherwise executed case.
Save the whole log: retained detail previews can truncate, and the error logger
also has bounded storage. Debug builds repeat some information through `KDEBUG`;
those repeated lines are not additional cases.

## 10. Registration, selection, and debugging

The authoritative compilation list is `SELFTEST_SRCS` in `Makefile`. Platform,
Resource, Logic, post-start, and Request runners explicitly call their suites.
A new source file needs both build registration and a reachable runner call.
Within a manager suite, `ManagerSuite::Run` supplies a stable contract name;
Request's `syscall_test_t` tables supply the handler name, syscall number,
behavioral validator, and any explicit coverage-gap reason.

There is no existing boot-command-line case filter, standalone unit-test binary,
`make test` target, automatic assertion-count coverage calculation, JUnit output,
or test-driven QEMU exit status in this harness. `QEMU_SMP` changes the CPU count;
`DEBUG` changes compilation/logging. Neither selects a test. Individual behavior
selection currently requires source changes to registration/call paths, while
preserving prerequisites and worker rendezvous.

`x86_64-debug` runs `debug-kernel.sh`, starts QEMU paused, loads
`debug/gdb/arx.py`, attaches on port 1234 by default, and breaks at `_start`.
`SERIAL_LOG` selects the debug serial file; its default is
`build/qemu-debug-serial.log`. The script stops its QEMU process on debugger exit.
Keep the loaded ELF and disk image from the same build.

| Debugging target | What to inspect |
| --- | --- |
| `SelfTestManager::CaseBegin` / `RecordFailureDetail` | Active contract and first failing expectation before bounded summary retention hides detail. |
| `SelfTestManager::AbortUnsafe` | Cleanup/initialization reason and the still-active case. |
| `request_selftest::SuiteContext::Begin` | The syscall within a manager-sized reporting case. |
| `thread apply all bt` | BSP/AP stacks when a scheduler, TLB, or fixture wait stops progressing. |
| `arx-debug status`, `arx-debug cpus topology` | Platform initialization and per-CPU state. |
| `arx-debug pmm nodes`, `arx-debug memory regions`, `arx-debug heap caches` | Physical, mapping, and heap ownership/accounting. |
| `arx-debug process details`, `arx-debug task details`, `arx-debug list` | Process/task identity, resources, and retained ownership. |
| `arx-debug vfs cache`, `arx-debug initramfs contents`, `arx-debug pci devices` | Filesystem fixtures, archive contents, and enumerated devices. |

See [GdbCommands.md](../../debugging/GdbCommands.md) for selectors, bounds and
command interpretation. There is no
dedicated self-test extension command; use ordinary GDB inspection of the manager
and fixture state. Record the first failing expectation and its prerequisites,
then trace the owning manager or peer. Keep source inventory, successful build,
and observed runtime completion distinct when reporting verification.
