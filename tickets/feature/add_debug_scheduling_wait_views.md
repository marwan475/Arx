# Add scheduling, blocked-task, futex and timer debug views

- Type: feature
- Priority: P1
- Status: open
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

- [ ] Maintained debug stats and rich roots cover scheduler, synchronization and timer state with explicit success/units/domain semantics and release exclusion.
- [ ] Per-CPU/task policy and wait views distinguish actual stored collections from derived eligibility and preserve index/sentinel bounds.
- [ ] Futex keys, backing retention, PI owners/waiters and priority/requeue relationships correlate with valid task identities.
- [ ] Clocks/timers expose configuration and samples without hardware callbacks or falsely current timestamps.
- [ ] Uninitialized clocks, exhausted slots, partial updates, cycles and invalid indices/owners produce bounded evidence rather than hangs or unsupported deadlock claims.
- [ ] Native-reader semantics and guide updates cover all rich collections; no Request instrumentation, target lock acquisition, history recorder or dump machinery is added.

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
