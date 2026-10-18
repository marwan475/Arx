# Route necessary upward calls through Dispatcher

- Type: debug
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` found both Dispatcher
bridges and direct callbacks from Resource into Logic. The owner's revised
rule in [add-arx-code](../../docs/skills/add-arx-code/SKILL.md) permits necessary
upward access, but it MUST pass through Dispatcher. Direct upper-manager calls
and callbacks bypassing Dispatcher remain defects. Upper layers may access any
lower layer directly.

Expected behavior: preserve necessary upper-layer operations and route them
through Dispatcher-owned interfaces. Resource must not obtain an upper-layer
manager from Dispatcher and then call it directly. Preserve execution accounting,
scheduling-policy retirement, robust-futex/child-TID cleanup, descriptor retention
and release, and file-backed writeback semantics. Removing required work is not
a fix. A wholesale relocation of policy or resource ownership is not required
merely because an operation goes upward.

Follow [TICKET.MD](../TICKET.MD). Read the Resource, Logic, Dispatcher, and relevant
platform guides. Diagnosis uses `mattpocock-skills:diagnosing-bugs`; the latest
reassessment below supersedes the original blanket no-upward-call scope.
Execution is authorized; see the latest work-log entry.

### Reproduce and classify the current paths

| Path | Current evidence | Revised verdict |
| --- | --- | --- |
| TaskManager scheduling accounting → Dispatcher → Scheduler | `dispatcher_account_scheduling` in TaskManager and Dispatcher | Permitted necessary upward access; preserve this bridge and its timing. |
| TaskManager → SynchronizationManager priority cleanup callback | `SetPriorityCleanup`, `PriorityCleanup`, `CleanupPriority` | Bypasses Dispatcher; fix routing. |
| TaskManager → SynchronizationManager robust/child-TID cleanup callback | `retirementCleanup`, `CleanupRobust` | Bypasses Dispatcher; fix routing. |
| ProcessManager → VFS descriptor release | `entry.release`, `VirtualFileSystem::ReleaseFileHandle` | Bypasses Dispatcher; fix routing. |
| ProcessManager descriptor copy → Logic retain callback → VFS | `CopyDescriptorTable`, `retain_clone_file` | Bypasses Dispatcher; fix routing. |

Inspect [TaskManager.cpp](../../kernel/layers/Resource/TaskManager.cpp),
[TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp),
[ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp),
[ProcessLifecycle.cpp](../../kernel/layers/Logic/ProcessLifecycle.cpp),
[SynchronizationManager.cpp](../../kernel/layers/Logic/SynchronizationManager.cpp),
[EventManager.cpp](../../kernel/layers/Logic/EventManager.cpp), and
[Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp).

Reproduce the routing evidence with:

```sh
rg -n 'dispatcher_account_scheduling|AccountSchedulingCurrent' kernel/layers
rg -n 'SetPriorityCleanup|PriorityCleanup\(|retirementCleanup|cleanup\(context|CleanupRobust|CleanupPriority' kernel/layers/Resource kernel/layers/Logic
rg -n 'entry.release|retain\(context|retain_clone_file|ReleaseFileHandle' kernel/layers/Resource/ProcessManager.cpp kernel/layers/Logic
```

Follow both registration and invocation sites; an opaque pointer or a renamed
callback is not evidence of Dispatcher mediation. Confirm which production
operations actually need upper-layer work before specifying bridge interfaces.

### Platform/Klib scope and entry convention

The owner clarified that Platform and Klib are outside the layer-stack direction
rule. Their layer-stack entry is through Dispatcher routing into Request.
Do not classify an upward Platform/Klib operation as a defect solely because
of its direction, and do not redesign their storage ownership on that basis.

The audit found file-backing callbacks installed by
[VirtualFileSystem.cpp](../../kernel/layers/Logic/VirtualFileSystem.cpp), initially
lines 612 and 628, and invoked by
[platform VMM](../../kernel/platform/memory/vmm.c). These bypass Dispatcher and
therefore require assessment against that separate entry convention. Include
routing of these affected callbacks in the design inventory, preserving writeback,
retry, and release behavior; this is not a mandate to move VMM into Resource.
Identify any other entry points encountered without expanding into unrelated
kernel redesign.

### Documentation and unknowns

[Resource.md](../../docs/architecture/layers/Resource.md), section 4, permits
resource-event callbacks. [Logic.md](../../docs/architecture/layers/Logic.md) and
[Dispatcher.md](../../docs/architecture/layers/Dispatcher.md) describe the wiring.
Under the revised rule, the accounting bridge is legitimate. Update descriptions
of direct callbacks when they are mediated by Dispatcher; do not describe all
upward access as prohibited. Use `update-kernel-docs` after implementation as
required by the owner's standing instruction in `add-arx-code`.

No runtime failure was demonstrated by the audit. The typed bridge interfaces,
callback/registration ownership, early-boot availability, cleanup failure behavior,
locking, and affected runtime verification still need a design agreed with the
owner. Preserve existing manager-lock release, task pinning, and cleanup ordering.
A Dispatcher facade that merely returns a manager or hands back its callback
would still leave Resource invoking the upper layer directly.

Coordinate shared signal definitions with
[the header dependency ticket](fix_resource_signal_type_dependency.md). Keep
this ticket's existing filename and work-log history. Record the revised design
before requesting execution authorization; implementation is authorized by the latest work-log entry.

## Acceptance criteria

- [x] Diagnosis distinguishes compliant Dispatcher-mediated access from direct upper calls/callbacks and records why each upward operation is necessary.
- [x] The agreed design gives Dispatcher ownership of routing into upper managers; lower code does not receive those managers or invoke their callbacks directly.
- [x] Resource cleanup, descriptor retain/release, and other agreed upward operations pass through Dispatcher, with equivalent behavior preserved.
- [x] Existing scheduling-accounting routing and timing are preserved; upward direction alone does not trigger replacement of that bridge.
- [x] Affected Platform/Klib callbacks are assessed against Dispatcher → Request entry, without treating Platform/Klib as members of the layer stack.
- [x] Relevant accounting/scheduling, task retirement/robust cleanup, descriptor lifetime, and any affected backing-I/O checks are selected and their results recorded.
- [x] Relevant guides are updated through `update-kernel-docs` to describe the implemented Dispatcher routing and the owner's revised rule.
- [x] Changes follow `kernel.cpp` formatting and the architecture-helper rule.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 for confirmed policy
violations with no demonstrated runtime regression. Current callback wiring is
documented and behaviorally significant; the migration design remains open.
Analysis and execution have not started.

### 2026-10-17 — Priority raised; design requested

The owner raised this ticket to P1 and requested diagnosis and design alongside
the signal-definition dependency ticket before implementation. The owner also
authorized relevant documentation updates through `update-kernel-docs` as a
standing part of the code skill. Analysis is starting; implementation is not
authorized yet.

### 2026-10-17 — Reassessment under the Dispatcher exception

The owner revised the layer rule after pausing design: necessary upward access
is allowed only through Dispatcher. Platform/Klib are outside the layer-stack
direction rule; their entry goes through Dispatcher into Request. Raised priority
remains P1. Updated the task prompt and acceptance criteria to reflect the new
rule while preserving the earlier work-log entries as historical context.

Rechecked the source call sites and registrations. TaskManager's scheduling
accounting already passes through Dispatcher and is compliant; remove it from
the defect list. Task cleanup, robust/child-TID cleanup, descriptor final release,
and descriptor-copy retention invoke Logic-owned callbacks directly and remain
routing defects. The file-backing callback path is an ingress-routing concern,
not a Platform layering-direction violation.

The minimal source witnesses still identify direct callback calls and their
Logic registrations. The old temporary reproduction harness classified all
upward paths as failures, so it is obsolete and was removed. No runtime failure
has been shown and no kernel code was modified. The earlier ideas of moving
lifecycle ownership and introducing execution snapshots are superseded; neither
is required to fix a Dispatcher bypass.

Status is `analyzed` for this recorded reassessment. Design remains paused as
requested. Next, when the owner resumes design, specify the required Dispatcher
interfaces, registration ownership, early-boot behavior, and context/locking
contracts for the remaining bypasses. Implementation still awaits an agreed
plan and explicit execution instruction under TICKET.MD.

### 2026-10-17 — Execution authorized

The owner instructed “ok fix the issues” and requested documentation of necessary
Dispatcher-mediated layer exceptions in KernelArch.md. Execution is authorized
under the revised Dispatcher exception, superseding the paused-design state.

Implement common signal records below their consumers. Preserve runtime ownership
and timing while replacing raw Resource callbacks with opaque, typed Dispatcher
routes for task cleanup and file retain/release. Route affected Platform/Klib
entries through typed Request handlers, including file-backing I/O. Stateless
Request entry methods preserve early Resource-test availability without reordering
layer construction. Route providers retain their existing context lifetimes;
Resource can hold route handles but cannot access their callbacks.

Verify compiler dependencies and both target builds, then run the affected
in-kernel suites in headless x86_64 SMP boots. Record any existing target/runtime
limitations explicitly. Use update-kernel-docs for KernelArch.md and other affected
guides. Implementation and verification are starting; closure remains owner-gated.

### 2026-10-17 — Ready for owner review

Implemented opaque immutable Dispatcher routes for task priority/retirement
cleanup and descriptor retain/release. Provider callable fields are private and
absent from Resource headers; lower consumers invoke typed Dispatcher operations.
VFS's standard file route is stable and Dispatcher-owned; task-cleanup providers
remain owned by SynchronizationManager and file-backing providers retain their
existing context ownership. Source-table locking, partial-copy rollback, task
claims, cleanup ordering, and scheduling-accounting timing are preserved.

Platform backing writeback/release now enters Dispatcher then MemoryRequestManager.
Klib user-memory begin/end and the existing minor-fault, pending-reschedule,
mode-accounting, and signal-return bridges route through stateless Request entry
methods. Early Resource tests retain access without moving factory construction
or adding a generic request bus. Necessary Resource-to-Logic routes use Dispatcher
without imposing the Platform/Klib Request entry convention on Resource itself.

Adapted the existing task-cleanup-once and descriptor ownership/rollback tests to
the opaque interfaces. Added `resource/VirtualMemoryManager/dispatcher_backing_retry`
to verify error retention, a successful retry, unchanged byte range/data, and
exactly-once context release. It passed all eight checks in both boots. Existing
scheduling, priority-inheritance owner death, robust futex, clone/exec/exit, file,
event, socket, and user-copy tests also passed.

Documented necessary upward exceptions and their reasons in KernelArch.md's
“Necessary upward routes through Dispatcher” section. Updated Dispatcher.md,
Resource.md, Logic.md, Request.md, Platform.md, and x86_64.md to match implemented
interfaces and ownership. Main architecture diagrams remain valid as diagrams
of ordinary service dependencies; the new prose/table describes the explicit
upward exceptions. No implementation TODOs remain in this ticket.

#### Verification evidence

- Final debug builds passed for both targets:
  `make -j8 DEBUG=1 BUILD_DIR=build/dispatch-debug BIN_DIR=bin/dispatch-debug x86_64 aarch64`.
  Final optimized x86_64 build passed with
  `make -j8 BUILD_DIR=build/dispatch-release BIN_DIR=bin/dispatch-release x86_64`.
- Headless x86_64 Q35 boots used `-m 1024 -smp 4 -cpu max`, the repository
  x86_64 UEFI firmware, and the corresponding isolated debug/release disk images
  with `-snapshot -no-reboot`. Both reached the final self-test summary:
  **155 ran, 155 passed, zero failed, overall PASS**. QEMU was bounded by a
  90-second timeout because the kernel remains idle after the completed tests.
- Boot evidence: `/tmp/arx-dispatch-debug-boot.log` and
  `/tmp/arx-dispatch-release-boot.log`. Final build logs:
  `/tmp/arx-dispatch-final-debug-build.log` and
  `/tmp/arx-dispatch-final-release-build.log`. No new compiler warnings were
  reported; the initial AArch64 build still emitted the existing noreturn warning
  in its unchanged unsupported user-entry stub. AArch64 runtime was not claimed
  or tested; the port remains incomplete.
- Compiler dependency generation for Resource TaskManager/ProcessManager and
  production x86_64 signal code contains no Logic/Request headers or route
  provider definitions. Shared signal records compile as C; ABI size/offset
  assertions also pass in the target builds.
- Negative compilation probes confirmed that Resource/Platform headers expose
  incomplete route types: task, file, and backing consumers cannot invoke
  provider fields. `nm -C -u` on the Resource objects references the new
  Dispatcher operations and no Scheduler, SignalManager, SynchronizationManager,
  or VirtualFileSystem symbols.
- All 182 local links checked across README, architecture guides, skills, and
  tickets resolve, including cross-references after the ticket moves. Skill
  validation and `git diff --check` pass.

Required verification is complete. Status remains `in progress`, stored in
`tickets/in_progress/`, ready for owner review. Closure and a destination have
not been authorized; the usual proposed destination is `tickets/closed/`.

### 2026-10-17 — Completion

The owner authorized closing both tickets to the proposed `tickets/closed/`
destination (“yes you can close them”). Routed necessary upward cleanup and file-lifetime operations through opaque Dispatcher interfaces, preserved Platform/Klib entry through Request, and documented the exceptions in KernelArch.md. Both target builds and 155/155 cases in each x86_64 SMP configuration passed.
