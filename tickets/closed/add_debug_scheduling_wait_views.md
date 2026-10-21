# Add scheduling, blocked-task, futex and timer debug views

- Type: feature
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Explain why a task does not run or wake through correlated scheduler, sleep,
futex/priority-inheritance and timer information. Current task output exposes
raw context but omits eligibility, waits and effective policy; investigation
requires manually finding task slots, sleep indices, futex keys and clock domains.

Follow [the workflow](../TICKET.MD), required grilling and applicable repo skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](../closed/build_debug_framework_foundation.md) and shared identities
from [process/resource views](../closed/add_debug_process_resource_views.md).
See [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment). This ticket starts open;
analysis and implementation each retain their owner gate.

### Scope and internal milestones

1. Provide manager-owned Scheduler, SynchronizationManager and TimeManager stats
   headers. Maintain agreed publish/select/block/wake/requeue/failure and current
   wait/policy/timer values at their actual Resource/Logic/platform update sites.
   No Request-layer stats or registration. Define domains/units, operation success
   semantics and SMP publication without adding inspection-time target locks.
2. Show per-CPU accounting/execution and task affinity/placement, base/effective
   policy/priority, ready sequence, RR budget and deadline reservation state.
   Scheduler scans task slots; do not invent a conventional ready-list root.
   Distinguish stored queued/state flags from interpreted eligibility and explain
   known exclusion reasons. Do not claim to predict the next task from partial state.
3. Expose indexed sleep slots, per-CPU heads, next indices, sentinel, task/process,
   deadline/result, event sequence/expected value, waiting/dispatched and
   uninterruptible flags. Preserve slot identity and correlate with task state.
4. Show private/shared futex identity plus offset, queued/woken/requeue/conversion
   flags, backing retention, waiter sequence/bits, PI state/owner/waiters and
   effective priority donation/reschedule relationships. Report observed cycles
   or invalid owners as evidence, not proof of arbitrary lock deadlock.
5. Show clock/realtime availability, resolution/backend, selected source/scaling,
   last sampled epoch, stored per-CPU LAPIC calibration and process real/CPU timer
   deadlines/intervals/accounting. Label monotonic, realtime and CPU-time domains
   distinctly. No clock-read callback, port I/O, MMIO probe or fabricated current time.
6. Provide source-backed native-reader collection/selection semantics in records
   or documented view descriptions, not only GDB formatting. Adapters use common
   readers and process/task identities; update `docs/debugging/GdbCommands.md`.
   Task slot ownership/retirement readers remain in the dependency ticket.

### Proposed interface and illustrative output

```text
arx-debug scheduler tasks --cpu 1 --limit 32
  tid=12 policy=DEADLINE queued=true eligible=false reason=budget-exhausted
arx-debug scheduler sleeps --cpu 1
  slot=12 tid=12 next=<none> waiting=true deadline=5000000ns domain=monotonic
arx-debug sync waiters --tid 12
  key=(backing=<addr>,offset=4096) piOwner=7 requeuePriority=true
arx-debug time timers --pid 9
  real.armed=true deadline=5000000ns cpu.userDeadline=<value>
```

Output illustrates questions, not measured values or an approved final grammar.
Report incomplete/changed groups, invalid indices/keys, missing clocks/managers,
unreadable pointers and truncation. Bound arrays, chains and correlation depth;
no inferior execution or retries waiting for a stopped writer. Tracking compiles
out with release; historical scheduling/event streams and dump mechanics are excluded.

### Sources and unknowns

- [Scheduler.hpp](../../kernel/layers/Logic/Scheduler.hpp),
  [Scheduler.cpp](../../kernel/layers/Logic/Scheduler.cpp),
  [SchedulerPolicies.cpp](../../kernel/layers/Logic/SchedulerPolicies.cpp):
  `SleepQueue`, `SleepHeads`, execution/accounting arrays, policy selection.
- [TaskScheduling.hpp](../../kernel/layers/Resource/TaskScheduling.hpp):
  `task_scheduling_t`, deadline/RR/priority fields.
- [SynchronizationManager.hpp](../../kernel/layers/Logic/SynchronizationManager.hpp),
  [implementation](../../kernel/layers/Logic/SynchronizationManager.cpp): bounded
  `Waiters`, `PiStates`, `PiWaiters`, key and requeue semantics.
- [TimeManager.hpp](../../kernel/layers/Resource/TimeManager.hpp),
  [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp),
  [arch_time.c](../../arch/x86_64/arch_time.c), [clock.h](../../klib/clock.h),
  [lapic.c](../../arch/x86_64/lapic.c): clocks, process timers, sampled calibration.
- Unknowns: eligibility explanation fidelity, stable policy identifiers, timer
  freshness, SMP grouping, race-safe counter ownership, wake result semantics
  and native-reader representation. Confirm without changing scheduling policy.

## Acceptance criteria

- [x] Maintained debug stats and rich roots cover scheduler, synchronization and timer state with explicit success/units/domain semantics and release exclusion.
- [x] Per-CPU/task policy and wait views distinguish actual stored collections from derived eligibility and preserve index/sentinel bounds.
- [x] Futex keys, backing retention, PI owners/waiters and priority/requeue relationships correlate with valid task identities.
- [x] Clocks/timers expose configuration and samples without hardware callbacks or falsely current timestamps.
- [x] Uninitialized clocks, exhausted slots, partial updates, cycles and invalid indices/owners produce bounded evidence rather than hangs or unsupported deadlock claims.
- [x] Native-reader semantics and guide updates cover all rich collections; no Request instrumentation, target lock acquisition, history recorder or dump machinery is added.

## Verification approach

Use [SchedulerTests](../../kernel/selftests/logic/SchedulerTests.cpp),
[SynchronizationManagerTests](../../kernel/selftests/logic/SynchronizationManagerTests.cpp),
[TimeManagerTests](../../kernel/selftests/resource/TimeManagerTests.cpp),
[scheduler integration](../../kernel/selftests/poststartkernel/schedulertests.cpp)
and [wait integration](../../kernel/selftests/poststartkernel/waitintegration.cpp).
Observe known publish/block/wake, affinity, RR/deadline, private/shared futex,
priority donation/requeue and timer fixture states in x86_64 QEMU/GDB. Use a
deterministic backend fixture for clock availability/sampled values without
executing it during inspection. Cover SMP and pre-scheduler availability, invalid
sleep index/cycles and a stopped group writer via controlled debug records.
Verify changed code in debug/release builds and reuse foundation safety checks.

## Verification scope

Keep verification proportional to this ticket's changes. Select the smallest set
of checks that demonstrates the acceptance criteria and covers the affected
behavior. Run the relevant checks once after the final changes; stop when they
pass. Repeat or broaden checks only after a new change, a failure, or a concrete
unresolved risk, and record the reason. Do not add repeated QEMU boots, unrelated
suite runs, or baseline investigations merely for extra confidence. Record
incidental failures and their impact; investigate further only when evidence
makes them a blocker for this ticket. Preserve required acceptance checks and
report any checks that could not run.

## Work log

### 2026-10-18 — Created

P1: blocked/runnable task diagnosis is a primary use case. Combines the shared
policy, wait, wake and clock/deadline workload without duplicating task ownership.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Analysis started

The owner selected scheduler, waits, futexes and timers. Read the ticket and
required grilling skill and reviewed applicable repository skills. Delegated
read-only source fact finding for scheduler/policy/sleep roots and futex/PI/time
semantics as required by grilling. Human-readable output and focused verification
remain standing requirements. No implementation, builds or tests have started;
status remains open pending design confirmation.

### 2026-10-20 — Source findings and design round prepared

Resumed analysis and preserved the existing uncommitted work-log entry. Applied
`mattpocock-skills:grilling`; read repository `add-arx-code`,
`update-kernel-docs` and `write-kernel-docs` guidance. Read-only delegated source
inspection confirmed scheduler task scans and indexed sleep collections, existing
policy ABI values, synchronization key/PI collections and runtime locking.

Scheduler candidates are the 128 physical TaskManager slots, not a ready list.
Sleep chains use per-CPU heads and `size_t(-1)` termination. Static eligibility
must account for the awakened-slot exception, lifecycle/dispatch restrictions,
CPU placement, process stop state and policy budgets. Scheduling updates time
accounting and replenishes reservations before selecting; an inspection cannot
reproduce that transition or predict the next task. Proposed output therefore
explains observed exclusions at stored accounting values, retaining unknown and
partial states when dependencies cannot be validated.

Synchronization owns fixed 128-entry waiter, PI-state and PI-waiter arrays.
Private keys use address-space identity plus virtual address; shared keys use
retained backing identity plus offset, with separate destination retention during
requeue. Existing locks protect mutations; debug writer markers must cover actual
mutations without introducing inspection-time locks or claiming atomic snapshots.
Existing shared task/process generation identities remain the correlation source.

Prepared owner choices for command collections/selectors, eligibility wording and
counter success semantics. Clock configuration and runtime samples will remain
separate from current time; inspection performs no callbacks or hardware reads.
No implementation, builds or runtime checks performed. Status remains open until
the required design conversation and shared-understanding confirmation finish;
then record the complete analysis/verification plan and await execution instruction.

### 2026-10-20 — Analysis and plan; execution authorized

The owner answered yes to all three design choices and explicitly said “go”.
Shared understanding and execution are confirmed. Use scheduler cpus/tasks/sleeps,
sync waiters/pi and time clocks/timers with applicable bounded selectors. Explain
observed eligibility separately from stored state; invalid dependencies produce
unknown/partial results. Separate returned results from committed transitions and
numbers affected, including timer commit followed by output-copy failure.

Implementation plan: add manager-owned debug headers, direct collection roots,
mutation-site counters and existing-lock writer markers; publish through Dispatcher
composition; expose platform clock and calibration RAM through debug roots. Build
bounded adapters using shared readers and dependency-owned task/process identities.
Document native traversal, units, clock freshness and interpretation limitations in
headers and GdbCommands.md; update affected architecture descriptions. Scheduling
policy and runtime locking behavior remain unchanged.

Verification: focused host reader tests for normal/invalid/truncated collections,
identity and PI/cycle/clock cases; isolated x86_64 debug/release builds with release
symbol exclusion checks; one focused SMP QEMU/GDB session covering live providers
and controlled fixtures, reusing foundation safety checks. Run only affected
checks and broaden only for a concrete failure/risk. Main risks are cross-manager
partial observations, unmarked mutation sites and confusion between return success
and committed effects. Track those explicitly. No unresolved scope questions.

### 2026-10-20 — Ready for owner review

Implemented manager-owned SchedulerDebugStats, SynchronizationDebugStats and
TimeDebugStats with direct collection roots and source-backed native contracts.
Added debug-only atomic API-result/transition counters at runtime update sites,
queue/PI/timer writer markers using existing locks, and per-process timer groups.
Platform publication exposes selected clock RAM and stored LAPIC calibration;
custom backends remain distinct. Dispatcher owns composition publication.
Release erases tracking storage, registration and update arguments.

Added scheduler cpus/tasks/sleeps, sync waiters/pi and time clocks/timers adapters.
They reuse common bounded array/index readers and dependency-owned task/process
identities, distinguish stored flags from observed CPU eligibility, preserve sleep
slots/sentinels and futex retention/requeue semantics, and report observed PI
cycles/invalid owners without asserting arbitrary deadlock. CPU/slot combination
is accepted specifically for scheduler tasks/sleeps. Clock output never invokes
callbacks or touches hardware; process timer domains and charged accounting are
explicit. Headers and GdbCommands.md document traversal, counters, units,
selection, lifecycle, partial-state and freshness contracts. Updated KernelArch,
Logic, Resource and x86_64 architecture descriptions; the existing README link to
GdbCommands.md remains valid.

Verification kept proportional as directed:

- Eight focused host adapter fixture tests pass via
  `python3 -B debug/tests/test_scheduling.py`: physical slot versus TID, affinity,
  RR throttling and deadline donor budgets; sleep invalid indices/cycles;
  interrupted groups, stale identities and truncation; private/shared requeue
  keys; PI cycles/invalid owners/state indices; custom/unavailable and platform
  sampled clocks; timer domains. These are deterministic reader fixtures, not
  additional QEMU boots.
- Isolated x86_64 debug and release kernel builds pass. The release ELF has no
  DebugStats, PublishDebug, DebugResultOperation or arx_time_platform_debug symbols.
  Final incremental builds include the final runtime implementation; subsequent
  header edits only clarify visibility/layout comments and whitespace.
- One four-CPU q35 QEMU boot and GDB session covered pre-Dispatcher status, all
  production providers, an actual parked sleep stopped inside a marked queue
  writer, clock samples before calibration and stored calibration afterwards,
  timer/accounting domains and maintained counters. The boot's automatic suite
  completed 160/160 cases, zero failures/skips/blocked, including Scheduler,
  SynchronizationManager, TimeManager, scheduled wait/wake, shared requeue and SMP
  scheduler integration. Logs: `/tmp/arx-scheduling-boot.log` and
  `/tmp/arx-scheduling-gdb.log`.
- Live inspection found missing pointer-type lookup and parser rejection of
  combined CPU/task-slot selectors. Fixed both and reattached to the same VM
  (no second boot); CPU/task views and bounded truncation then passed. Evidence:
  `/tmp/arx-scheduling-recheck.log`. Later PI occupancy/owner correlation and
  invalid-bound refinements passed the focused host cases. No extra suite runs.
- QEMU's initial sandbox launch could not create its firmware snapshot in
  `/var/tmp`; GDB initially could not connect through the sandbox. Both checks
  succeeded with approved execution permissions. These are resolved environment
  restrictions, not remaining verification blockers.
- Reviewed dependencies, architecture boundaries and readable formatting;
  `git diff --check`, release exclusion and ticket-index link/inventory checks
  pass. No Request instrumentation, inspection locks, clock callbacks, history
  recorder or dump machinery was introduced.

All acceptance criteria are met. No deferred implementation TODOs. Observations
remain bounded sampled state, not coherent cross-manager snapshots or dispatch
predictions. Native-reader implementation itself remains outside this ticket;
matching header/selection semantics are provided. Live private/shared PI variants
were exercised by the boot self-tests and represented in controlled host readers;
no additional boots were used solely to stop each variant.

Generated build artifacts are preserved at `/tmp/arx-scheduling-artifacts-_5duete4`;
QEMU was stopped after verification.

Status stays in progress at the owner-review gate. Await explicit authorization
to close and move to `tickets/closed/add_debug_scheduling_wait_views.md`.

### 2026-10-20 — Completion

The owner authorized closure with “close”, accepting the proposed destination
`tickets/closed/add_debug_scheduling_wait_views.md`.
Completed maintained scheduler/wait/futex/PI/timer statistics and bounded debug
views, with native-reader contracts and updated guides. Verification passed as
recorded above; no further implementation work remains.
