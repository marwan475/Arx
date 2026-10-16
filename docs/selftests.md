# Kernel self-tests

`kernel/selftests/SelfTestManager.hpp` is the C++ framework interface.
`kernel/selftests/selftests.h` is the C interface; its recording functions forward
to the same manager. C++ tests call `SelfTestManager::Get()` directly. The manager
owns recorded cases, failure details, group snapshots, case totals, and output.
The C-linkage forwarding functions are implemented in `SelfTestManager.cpp`.

Each group directory owns its runner and test declarations. `SelfTestManager`
calls group runners rather than listing individual tests. The platform runner
and header are C; the other group runners and headers are C++.

| Directory | Runner | Existing tests, in order |
|---|---|---|
| `platform/` | `platformtests.c` | Data structures, memory, klib, initialized CPU/device/terminal invariants |
| `resource/` | `resourcetests.cpp` | One unit suite per resource manager and filesystem backend |
| `logic/` | `logictests.cpp` | One unit suite per logic manager |
| `poststartkernel/` | `poststartkerneltests.cpp` | Full-kernel scenarios using real capability wiring |
| `request/` | `requesttests.cpp` | Syscalls |

Framework and reusable fixtures remain at the `selftests/` root. Request sources
remain unchanged by the manager-test redesign. The build includes
`kernel/selftests` in its header search path for the syscall file's manager include.

## Manager unit suites

Each initialized manager has a `<ManagerName>Tests.cpp` file. Unit tests call the
exported manager's public API with real dependencies, rather than constructing
replacement managers or invoking request handlers. Cases describe functionality,
not individual assertions; related operations can share one case.

| Resource suite | Contract cases |
|---|---|
| PhysicalMemoryManager | Accounting, aligned allocation/free |
| VirtualMemoryManager | Isolated roots, anonymous pages/protection/discard, cloning, reservations |
| ProcessManager | Identity/lifecycle, descriptors, task ownership, limits/umask, group references, initial user stack |
| TaskManager | Allocation/lookup, dispatch ownership, current identity/name |
| TimeManager | Clock validation/resolution, deadline arithmetic, alarm expiration |
| InitRamFileSystemManager | Archive enumeration/lookup, metadata and index bounds |
| ResourceFileSystem | Backend validation, mutation rejection, truncate growth boundary |

| Logic suite | Contract cases |
|---|---|
| Scheduler | Initialized BSP context, invalid queue/dispatch/wait input |
| ProcessController | Group/session identity, map/protect/unmap, child-TID ownership |
| VirtualFileSystem | Path joining, anonymous inode metadata, references, positioned/sequential I/O dispatch |
| ElfMapper | Input/truncation validation, valid ELF64 segment parsing, malformed segment rejection |
| CredentialManager | Identity transitions, supplementary groups, privilege rejection |
| EventManager | Generation publication, counters, epoll membership, flag validation |
| SynchronizationManager | Futex mismatch/empty queues, input validation, robust metadata |
| SignalManager | Action/mask round trips, temporary masks, alternate stacks |
| SocketManager | Protocol validation, nonblocking local transfer/shutdown |
| InterProcessCommunicationManager | Nonblocking pipes, memory files, flag validation |

`ManagerSuite` reports manager availability and its case totals. Missing manager
capabilities fail the prerequisite and block that suite's cases. Unsupported
environmental capabilities, such as an absent monotonic clock, are explicit skips.

`TestFixture` owns a disposable process, task, and isolated user address space. It
temporarily establishes a synchronous current identity under a preemption guard,
then restores its caller and releases owned state. It must never sleep or dispatch
its synthetic task. `TestFile` retains a descriptor's file under the descriptor
guard and releases that reference before the process fixture is reclaimed.

## Initialization and phases

The manager has trivial static storage and requires no global constructor, heap
allocation, exceptions, or RTTI. `platform_selftests()` calls `RunPlatformTests()`,
which explicitly initializes the manager before running the first tests.
`selftest_reset_context()` resets recorded results only; it must not run while
tests are recording and does not reset SMP fixture state.

The group order is platform, resource, logic, poststartkernel, request, summary:

| Hook | Manager phase | Prerequisite |
|---|---|---|
| `kernel_bootstrap` | Platform | Memory and architecture initialized |
| `Dispatcher::StartKernel` | Resource | Resource capabilities created |
| `Dispatcher::StartKernel` | Logic | Logic capabilities created |
| `kmain` | Publish mount readiness only | Root filesystem mount attempted |
| Each CPU's post-init task | Poststartkernel | Successful mount and running process/task context |
| BSP's post-init task | Request | Poststartkernel runner returned |

`kmain` publishes the mount result through `SetRootFileSystemMounted()` before
queuing any per-CPU post-init tasks. Post-start tests no longer run in `kmain`.
Every CPU enters the poststartkernel runner from its running post-init task.
Only the BSP opens/closes the group and records ordinary cases.

When mounted, each CPU runs SMP tests and publishes its local totals through the
existing SMP collector. The BSP waits for all workers, records the SMP aggregate,
then coordinates a cross-CPU TLB remap probe before releasing the APs. It runs
mounted VFS/ELF and process/task scenarios, followed by memory, event/timer,
scheduled synchronization, signal-frame, and cross-process communication scenarios.
The mounted/SMP fixtures must not overlap: they modify
the same mounted file and create processes/mappings in the shared address space.
After poststartkernel finishes, the BSP runs the request group, prints the summary,
and prints the error log. Request tests are deliberately deferred until this point,
rather than running immediately after request-layer initialization.

When mounting fails, every CPU skips the poststartkernel test bodies. The BSP
records one failed `poststartkernel_vfs_mount` prerequisite, closes the group, and
continues to request tests and the summary. No CPU waits for skipped SMP workers.
Ordinary case/group recording remains serial on the BSP; the manager does not
make active-case recording safe for concurrent callers.

`ScheduledFixture` uses real kernel task contexts in an owned process for the
bounded waiter/waker scenarios. Publication-to-parking and peer completion are
protected from same-CPU timer preemption, so the scenarios do not rely on a lucky
interleaving. Peer shutdown and ready-queue removal precede fixture reclamation.

The memory scenarios exercise real safe-copy protection/COW and remote TLB
invalidation. Communication scenarios transfer retained endpoints between distinct
process identities and address spaces. The signal scenario verifies preparation
and restoration of the real x86-64 user signal frame; it does not execute a signal
handler. Real user entry/return is covered separately by the existing ELF fixture.

## Recording contract

- Pair `CaseBegin`/`CaseEnd` (C: `selftest_case_begin`/`selftest_case_end`).
- Record failure messages while the case is active. Messages are copied into
  manager storage and also sent immediately to the error logger.
- Case and group names are borrowed and must remain valid through the summary;
  existing callers use string literals. The context pointer remains stable.
- Qualified case names identify group, manager/scenario, and functionality.
  `TestCase` streams failed expectations with expected/actual values where useful.
  Successful checks are counted without printing an individual line.
- New cases have PASS, FAIL, SKIP, or BLOCKED status. Executed totals preserve
  `tests_ran = tests_passed + tests_failed`; skipped and blocked cases have separate
  counters. A new case with no checks becomes BLOCKED. Legacy zero-check behavior
  remains available through the original recording API for request compatibility.
- Case duration uses the architecture monotonic clock when available; unavailable
  timing is printed explicitly instead of reporting a fabricated zero duration.
- Syscall results explicitly use `SELFTEST_COUNT_IMPLEMENTED` so the summary
  retains its `implemented=` label without inspecting the test name. Other
  cases default to `SELFTEST_COUNT_PASSES`.
- Ordinary failures allow independent cases to continue. Failed prerequisites
  block dependent work. Fixture restoration/reclamation failures are unsafe and
  stop further initialization, including invocation of request tests.

Storage limits remain 128 case records, 256 detail bytes per case including the
terminator, 64 failed-case names, and 16 nested groups. Existing overflow behavior
keeps aggregate totals independent of retained records. Every completed case and
failure streams immediately. The summary reports omitted case/name counts, and
truncated retained detail previews are marked explicitly.

## Completion and controlled failure

`Finalize()` prints the test summary followed by `error_logger_print()` exactly
once on the BSP. The global error logger is never reset by self-test setup, so its
final snapshot includes earlier initialization errors as well as test failures.
Bootstrap post-init task creation failures and unsafe fixture failures use the
same finalization path before halting the BSP.

SMP and TLB control polling has a five-second clock deadline and a 100-million-poll
fallback bound. Scheduled wait tests use one-second deadlines; the timer scenario
sleeps for one millisecond. A timed-out worker is not assumed cancelled: the BSP
reports unsafe state and stops before running another suite against its resources.
These bounds apply when control returns to the runner. They cannot recover a hard
lockup, a context switch that never returns, a failed hardware timer, or an existing
request panic. No external watchdog is introduced here.

Randomized/endurance stress tests are outside the default boot suites. Build and
boot execution are performed by the developer; source-only checks are not a
substitute for running both supported architecture targets.
