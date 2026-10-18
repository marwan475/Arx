# Restore blank-line separation in dense kernel functions

- Type: debug
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` found long stretches of
kernel code without blank lines separating distinct operations. In particular,
`SynchronizationManager::PriorityFutex`, initially lines 1045–1399, has no blank
lines despite containing validation, setup, lookup, retry/wait work, cleanup,
and return. Expanded braces alone do not make this code readable.

Expected behavior: follow [kernel.cpp](../../kernel/kernel.cpp) and
[add-arx-code](../../docs/skills/add-arx-code/SKILL.md), using generous blank lines
between logical phases and around control-flow blocks. Keep related declarations
and assignments together. Use human judgment rather than inserting a blank line
after every statement or enforcing an arbitrary line-count threshold.

Follow [TICKET.MD](../TICKET.MD). Read the guides for the layers and subsystems
being reformatted. Creation does not authorize analysis or implementation; use
the required diagnosis skill when analysis is requested.

### Reproduce and inspect

Start with these audited examples, using symbols to relocate stale line numbers:

- [SynchronizationManager.cpp](../../kernel/layers/Logic/SynchronizationManager.cpp):
  `PriorityFutex`, initially lines 1045–1399.
- [ProcessLifecycle.cpp](../../kernel/layers/Logic/ProcessLifecycle.cpp):
  exec commit/cleanup work, initially lines 1311–1348.
- [SchedulerPolicies.cpp](../../kernel/layers/Logic/SchedulerPolicies.cpp):
  policy-handling sequence, initially lines 54–169.

Inspect the rest of first-party code for the same missing separation, including
platform and self-tests where applicable. Record an explicit file/function
inventory and bounded batches during analysis before execution is authorized.
The three examples are starting evidence, not a claim that all other functions
are readable. Contiguous record initialization, tables, and simple switch cases
may reasonably stay grouped.

This is a readability defect; no runtime failure was demonstrated. The complete
affected inventory remains unknown. Limit the fix to whitespace; expression
rewrites, function extraction, and behavior changes need a separately agreed
scope. Preserve macro continuation semantics, literals, and assembly. Exclude
bundled third-party code and avoid the repository-wide `make format` target,
which also touches vendor files and cannot supply logical blank-line grouping.

Coordinate overlapping files with
[brace insertion](expand_unbraced_first_party_control_flow.md),
[lambda expansion](expand_compressed_selftest_lambdas.md), and the
[runtime layering migration](../closed/fix_resource_upward_runtime_calls.md).

## Acceptance criteria

- [x] Analysis records the first-party functions/files needing spacing changes and the agreed execution batches.
- [x] All three audited examples and the agreed inventory have readable blank-line separation matching `kernel.cpp`.
- [x] Closely related statements stay grouped; no mechanical blank-line quota is substituted for readability review.
- [x] Verification confirms changes are whitespace-only, preserve preprocessing/literal semantics, and leave vendor code untouched.
- [x] Diff checks and a final readability review are recorded, along with the documentation-impact finding.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 because generous
newlines and human-readable formatting are explicit owner requirements.
Analysis and execution have not started.

### 2026-10-18 — Analysis and plan

Owner requested work on all three debug tickets. Read TICKET.MD and the required
`mattpocock-skills:diagnosing-bugs` skill; applied repository `add-arx-code`
and reviewed `update-kernel-docs`. Read kernel.cpp and relevant architecture,
layer, platform and self-test guides. Priority remains P2.

See [shared diagnosis, inventories, bounded batches, and verification plan](improve_dense_kernel_code_spacing.md#readability-audit-inventory).
Archived discovery script source is in [readability_audit.py](improve_dense_kernel_code_spacing.md#archived-readability-audit-script).
This phase changes ticket records and analysis tooling only. Execution awaits
the owner's explicit instruction after review, as required by TICKET.MD.

Confirmed the three audited examples at current anchors PriorityFutex:1053–1407,
ConfigurePolicy:54–169, and Execute's commit/cleanup section:1308–1344. Their
named regions contain 355, 116, and 37 nonblank lines respectively with no blank
line separation. The broader first-party discovery scan reproduced 133 candidate
regions in 46 files, including grouped declarations, records, simple switch
cases and tables that should remain grouped.

Plan: use the explicit file/function candidate inventory and six proposed
execution batches in the shared analysis. The three examples are confirmed;
manual phase-boundary review throughout the wider candidate functions remains
an execution investigation step. Discovery is complete for this bounded scan,
not proof that every short function outside it is readable. Inspect each listed
function before edits, record no-change decisions, and pause for a revised plan
if materially broader scope is necessary. Keep this ticket whitespace-only,
using the post-brace/lambda snapshot for comparisons. Preserve grouped related
statements, preprocessing, literals, assembly and vendor files. Verify whitespace
and token identity, target/configuration compilation and links, diff checks,
and final human readability. No meaningful documentation edits are expected.

### 2026-10-18 — Execution started

Owner explicitly instructed “apply fixes” after analysis. Executing the recorded
plan for all three readability tickets. Moving this ticket to in_progress;
source snapshots for independent lambda/brace/spacing verification are retained
under /tmp/arx-readability-baseline and subsequent phase snapshots.

### 2026-10-18 — Ready for owner review

Implemented the recorded scope and completed verification. See the shared
[execution results](improve_dense_kernel_code_spacing.md#2026-10-18--execution-results-and-review)
for build commands, source-preservation checks, reviewed exclusions and evidence.
All acceptance criteria below are satisfied; no implementation TODOs remain.

Full kernel compile/link checks pass for x86_64 and AArch64 with DEBUG=0 and
DEBUG=1, including a final rerun after the last source edits. No runtime tests
were selected or run for these source-formatting defects. Reviewed affected
guides using update-kernel-docs: behavior, interfaces, architecture and diagrams
are unchanged, so no meaningful guide or README update is needed.

Owner previously authorized closure (“you can close it”), then explicitly
instructed application of the fixes. Closure destination remains unconfirmed.
Status stays in progress pending confirmation of tickets/closed/ or another
owner-selected destination; no closure histories have been written.

Completed all six planned review batches. Added phase separation in 29 candidate
files; six additional files have local blank-line separation belonging to the
brace ticket. All three named examples are readable, and remaining dense scan
regions were reviewed as cohesive records, initialization, switches or branch
chains. The spacing diff against the post-brace/lambda snapshot is whitespace-only,
with identical lexical tokens, complete literals, comments and directive lines.
The shared report records grouped no-change decisions and manual phase review.

### 2026-10-18 — Completion

Owner confirmed “yes close” in response to the proposal to close all three
tickets in tickets/closed/. Added readable phase separation across the agreed inventory while preserving grouped statements and all source tokens. Required source-preservation checks,
diff checks and debug/release kernel builds for x86_64 and AArch64 passed,
as recorded above.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

## Readability audit inventory

Archived reference from the removed `tickets/analysis/` directory. Historical
commands and paths below describe the original investigation.

### Readability diagnosis and proposed execution inventory

Analysis date: 2026-10-18. Baseline: `dbba84f`.
This file supports the three P2 debug tickets. The analysis below records the
approved execution scope. Fixes and verification are complete; see the execution
results at the end. All three tickets are closed in tickets/closed/ with owner
authorization.

#### Feedback loops and diagnosis

- `rg -n '\{ return .*;.*\}' kernel/selftests` reproduced all four
  audited single-line lambdas: ElfMapperTests.cpp:186,
  EventRequestManagerTests.cpp:2199, SignalRequestManagerTests.cpp:278,296.
  Reviewing lambda/brace candidates throughout those three files found no
  additional compressed lambda bodies. Array initializers are false positives.
- `python3 tickets/analysis/readability_audit.py braces` returned exit 1:
  **50 candidates in 22 files**. A second run returned the same inventory.
  Every candidate was inspected with its existing body/branch context and
  corresponds to an audited site. Current anchors are in the table below.
- `python3 tickets/analysis/readability_audit.py spacing` returned exit 1:
  **133 candidate regions in 46 files**. A second run returned the same inventory.
  This is discovery, not a readability verdict or a requirement to eliminate
  every long run. The 25-line threshold only narrows manual inspection.
- `python3 tickets/analysis/readability_audit.py examples` returned exit 1
  and reproduced the three spacing examples: the complete
  `PriorityFutex` region has 355 consecutive nonblank lines; `ConfigurePolicy`
  has 116; the exec commit/cleanup section beginning at
  `tasks->CleanupCurrentTask()` has 37. Inspecting these smallest named regions
  confirms validation, work, cleanup, and return phases lack visual separation.
  Recheck the specific phase boundaries after edits, not a blank-line quota.

Ranked hypotheses (shown before body inspection):

1. Existing source still contains the audit defects. Prediction: balanced
   condition scanning and direct body inspection retain the reported sites.
2. Intervening changes already resolved some findings. Prediction: relocated
   symbols have expanded bodies or phase separation and need no edit.
3. Discovery is confusing macros, literals, loop tails, or nested branches with
   defects. Prediction: masking and body inspection remove those candidates.

The first explanation is supported for all four lambdas, all 50 unbraced sites,
and the three named spacing examples. The second explains shifted anchors,
not resolved sites. The third explains several broad spacing candidates
(declaration tables and initialization sequences); none of the 50 brace
candidates proved to be a macro or loop-tail false positive.

These are source readability defects, with no established runtime failure.
Debugger probes, runtime minimization, and new behavioral regression cases are
therefore inapplicable. The source scans and direct region review are the
feedback loop. No theory about a functional kernel failure is being proposed.
The scanner masks comments, string/character/raw literals, directives and their
continuations, balances multiline conditions, accepts `else if`, and excludes
braced `do/while` tails. It is not a complete C/C++ parser; final manual review
and compiler verification remain necessary.

#### Proposed order and bounded batches

First expand the four lambdas, then brace the 50 bodies, then apply spacing
against the latest source. Do not run `make format`. Braces/lambdas and spacing
must remain independently reviewable, including overlapping files.

Brace batches: (1) Arch, (2) Logic, (3) Request and Resource,
(4) platform, self-tests, and Klib. The inventory below bounds all four batches.
Wrap only the existing statement/body. Nested loops in arch_entry, arch_paging,
EventRequestManager, ProcessManager, TaskManager, and CredentialManager require
explicit scope review. In particular, `pmm_free(words)` remains outside
`if (depth > 1)`; CPU selection publication remains outside its duplicate-check
loop; user-output returns remain outside their loops. The other sites wrap
expressions, returns, or calls: no existing declaration body needs a lifetime
change. Preserve each `else` association and every following unconditional
operation, including Klib copy-pointer advancement and test pass accounting.

Spacing batches (proposed, pending owner execution instruction):

1. Logic synchronization and scheduling: SynchronizationManager.cpp,
   SchedulerPolicies.cpp, Scheduler.cpp; all named candidate functions below.
2. Logic lifecycle, events, and signals: ProcessLifecycle.cpp, EventManager.cpp,
   EventNotifications.cpp, SignalManager.cpp; preserve grouped ABI fields.
3. Logic files and IPC: VirtualFileSystem.cpp, SocketManager.cpp,
   InterProcessCommunicationManager.cpp.
4. Request and Resource: SchedulerPolicyRequests.cpp, SchedulerRequestManager.cpp,
   SignalRequestManager.cpp, VfsRequestManager.cpp, and ProcessManager::FreeProcess.
5. Architecture and platform: arch/aarch64/arch_selftest.cpp (separation between
   function definitions), arch/x86_64/arch_paging.c, arch_signal.cpp, lapic.c,
   kernel/platform/memory/numa.c and pmm.c.
6. Self-tests: SocketFileSystemFixture.hpp::Mknod, logic/SignalManagerTests.cpp,
   platform/platforminvariants.c, request/EventRequestManagerTests.cpp,
   SchedulerRequestManagerTests.cpp, SocketRequestManagerTests.cpp,
   SyncRequestManagerTests.cpp, and resource/TimeManagerTests.cpp.

The complete candidate list is below. For each proposed function, inspect its
whole body before editing and use human judgment to separate logical operations.
The named three examples are confirmed; detailed grouping throughout the wider
candidate inventory remains an execution investigation step. A listed function
can require no edit after that review; record that result. Do not silently expand
to unlisted functions or architectural refactors. If review reveals a materially
different scope, update the plan and pause under TICKET.MD.

Retain grouped record initialization in both architectures' `gather_boot_info`,
ProcessManager::ResetProcessSlot and TaskManager::ResetTaskSlot. Retain the simple
Dispatcher syscall switch, VFS test registration table, header declarations,
ISR declaration lists, and ABI structures. These are scan exclusions, not a
claim that blank-line counts prove readability. No vendor files are in scope.

#### Verification selected for execution

- Review every changed block against kernel.cpp and rerun source discovery;
  manually confirm each audited site is resolved. Existing self-test behavior,
  assertions, captures, call order, types and expressions stay identical.
- For the spacing batch alone, require `git diff -w` to be empty for source
  changes relative to the post-brace/lambda snapshot. Also compare lexical
  tokens including literal contents and preprocessor directive/continuation
  contents: whitespace-only changes can still damage literals or macros.
- For lambdas, require the same tokens before and after. For braces, require
  that the only added non-whitespace tokens are the planned braces; inspect
  body boundaries, declarations and `else` associations separately.
- Compile every affected translation unit with the Makefile's actual flags for
  x86_64 and AArch64, DEBUG=0 and DEBUG=1, into separate output directories.
  Header edits must be covered by their including translation units. Link both
  kernels for both configurations to catch build integration problems. Record
  pre-existing failures rather than treating them as ticket regressions.
- `make -n -B build/x86_64/kernel/selftests/logic/ElfMapperTests.o DEBUG=1`
  and the equivalent `build/aarch64/...` command were run successfully during
  analysis to verify the object rules and flags. gcc/g++, clang/clang-format,
  and aarch64-linux-gnu-gcc/g++ are available. These dry runs are not builds.
- `git diff --check`, source-only diff review, and explicit vendor exclusion
  checks. No new runtime scenarios are requested; booting QEMU is not needed
  for these formatting-only changes unless compilation or scope review finds
  a behavioral concern.
- Apply update-kernel-docs after implementation verification. Expected impact:
  none, since behavior, interfaces, architecture, source names and diagrams
  are unchanged. Report the review finding without cosmetic guide edits.

Existing untracked `bin/` and `build/` were present before analysis; preserve them.
No AGENTS.md, GLOSSARY.md, or ADR inventory was found in this repository.
Read kernel.cpp, add-arx-code, update-kernel-docs, KernelArch, Logic, Request,
Resource, Dispatcher, Platform, SelfTests, x86_64, and AArch64 guides for scope
and verification. No implementation or runtime checks have run in this phase.

#### Confirmed brace inventory

| File | Current lines | Count |
| --- | --- | --- |
| `arch/x86_64/arch_entry.c` | 155, 157, 374, 407 | 4 |
| `arch/x86_64/arch_paging.c` | 542, 740, 1083, 1085 | 4 |
| `arch/x86_64/lapic.c` | 76 | 1 |
| `kernel/layers/Logic/CredentialManager.cpp` | 74, 179, 213 | 3 |
| `kernel/layers/Logic/InterProcessCommunicationManager.cpp` | 691 | 1 |
| `kernel/layers/Logic/Scheduler.cpp` | 153, 372, 753 | 3 |
| `kernel/layers/Logic/SynchronizationManager.cpp` | 179, 181, 233, 235 | 4 |
| `kernel/layers/Logic/VirtualFileSystem.cpp` | 3160 | 1 |
| `kernel/layers/Request/EventRequestManager.cpp` | 250, 273, 357, 366 | 4 |
| `kernel/layers/Request/SystemRequestManager.cpp` | 243 | 1 |
| `kernel/layers/Request/VfsRequestManager.cpp` | 430, 1985 | 2 |
| `kernel/layers/Resource/ProcessManager.cpp` | 548, 1511, 1524 | 3 |
| `kernel/layers/Resource/TaskManager.cpp` | 21, 498 | 2 |
| `kernel/layers/Resource/TimeManager.cpp` | 49 | 1 |
| `kernel/platform/acpi/acpi.c` | 61 | 1 |
| `kernel/selftests/platform/klibtests.c` | 253, 261, 269, 318, 325, 334, 346, 360 | 8 |
| `kernel/selftests/platform/memorytests.c` | 159 | 1 |
| `kernel/selftests/poststartkernel/schedulertests.cpp` | 366 | 1 |
| `kernel/selftests/poststartkernel/smpfixtures.hpp` | 13 | 1 |
| `kernel/selftests/poststartkernel/smpselftests.cpp` | 447 | 1 |
| `kernel/selftests/poststartkernel/taskcontexttests.cpp` | 201 | 1 |
| `klib/klib.c` | 24, 261 | 2 |

#### Spacing candidate inventory

Line ranges identify baseline source regions, not automatic edit boundaries.
Function labels are anchors; some regions span multiple small functions or
lambda bodies. Declaration-only rows and the explicit exclusions above remain
unchanged. Other rows bound the proposed six spacing batches.

| File | Candidate regions | Function/region anchors |
| --- | --- | --- |
| `arch/aarch64/arch_entry.c` | 245–276 | `gather_boot_info` |
| `arch/aarch64/arch_selftest.cpp` | 10–62 | `arch_selftest_elf_machine`; `arch_selftest_machine_name`; `arch_selftest_invalid_addresses`; `arch_selftest_page_permissions`; `arch_selftest_user_frame`; `arch_selftest_signal_restorer`; `arch_selftest_syscall_instruction`; `arch_selftest_syscall_probe`; `arch_selftest_load_child`; `arch_selftest_load_resource_child`; `arch_selftest_signal_return`; `arch_selftest_timer_signal`; `arch_selftest_event_signal` |
| `arch/x86_64/arch_entry.c` | 242–273 | `gather_boot_info` |
| `arch/x86_64/arch_paging.c` | 1135–1191 | `arch_user_page_accessible`; `arch_boot_map_physical` |
| `arch/x86_64/arch_signal.cpp` | 86–111, 215–239, 241–285 | `arch_signal_prepare`; `arch_signal_restore`; `arch_signal_interrupt` |
| `arch/x86_64/lapic.c` | 226–252 | `arch_program_scheduler_timer` |
| `arch/x86_64/x86_64.h` | 111–140, 326–583 | `Grouped declarations/records; excluded` |
| `kernel/layers/Dispatcher.cpp` | 93–130, 187–308, 310–345, 372–403, 424–455 | `Dispatcher::DispatchSyscall` |
| `kernel/layers/Logic/EventManager.cpp` | 194–253 | `EventManager::Poll` |
| `kernel/layers/Logic/EventManager.hpp` | 73–115 | `Grouped declarations/records; excluded` |
| `kernel/layers/Logic/EventNotifications.cpp` | 23–52, 136–179, 200–304, 306–343, 345–411, 413–450, 452–488, 531–555, 563–676 | `EventManager::InstallNotification`; `EventManager::AddWatch`; `EventManager::RemoveWatch`; `EventManager::NotifyVfs`; `EventManager::SignalDescriptor`; `EventManager::NotifySignals`; `EventManager::NotificationIoctl`; `EventManager::NotificationReadVector` |
| `kernel/layers/Logic/InterProcessCommunicationManager.cpp` | 140–212, 214–243, 307–359, 380–521, 523–562, 630–658 | `InterProcessCommunicationManager::CreatePipe`; `InterProcessCommunicationManager::NotifyPipe`; `InterProcessCommunicationManager::PipeControl`; `InterProcessCommunicationManager::PipeTransfer`; `InterProcessCommunicationManager::CreateMemoryFile`; `InterProcessCommunicationManager::MemoryWrite` |
| `kernel/layers/Logic/InterProcessCommunicationManager.hpp` | 16–41 | `Grouped declarations/records; excluded` |
| `kernel/layers/Logic/ProcessLifecycle.cpp` | 715–742, 1307–1344 | `ProcessController::Wait`; `ProcessController::Execute` |
| `kernel/layers/Logic/Scheduler.cpp` | 811–860, 931–1090, 1092–1159, 1177–1204, 1494–1535 | `Scheduler::SetAffinity`; `Scheduler::RunSchedulingAlgorithm`; `Scheduler::RunNextReadyProcess`; `Scheduler::ServicePendingReschedule`; `Scheduler::AcquireTask` |
| `kernel/layers/Logic/SchedulerPolicies.cpp` | 54–169, 265–312, 351–387, 389–437, 439–516 | `Scheduler::ConfigurePolicy`; `Scheduler::AccountSchedulingLocked`; `Scheduler::MoreUrgent`; `Scheduler::NextSchedulingEventLocked`; `Scheduler::UserPriority` |
| `kernel/layers/Logic/SignalManager.cpp` | 597–640, 642–683, 685–713, 732–773 | `SignalManager::Queue`; `SignalManager::Peek`; `SignalManager::Consume`; `SignalManager::ReserveExit` |
| `kernel/layers/Logic/SocketManager.cpp` | 213–250, 406–432, 467–575, 653–730, 732–771, 949–982, 1086–1110, 1479–1519, 1678–1717, 1781–1858 | `copy_vectors`; `SocketManager::AutobindLocked`; `SocketManager::ParseControl`; `SocketManager::DropReference`; `SocketManager::Bind`; `SocketManager::Connect`; `SocketManager::GetOption`; `SocketManager::Transfer` |
| `kernel/layers/Logic/SocketManager.hpp` | 72–104 | `Grouped declarations/records; excluded` |
| `kernel/layers/Logic/SynchronizationManager.cpp` | 911–955, 957–1043, 1053–1407, 1409–1473, 1511–1692, 1694–1721 | `SynchronizationManager::BestPiWaiter`; `SynchronizationManager::RefreshPrioritiesLocked`; `SynchronizationManager::PriorityFutex`; `SynchronizationManager::CleanupPriority`; `SynchronizationManager::RequeuePriority`; `SynchronizationManager::LockSchedulingChange` |
| `kernel/layers/Logic/SynchronizationManager.hpp` | 33–90 | `Grouped declarations/records; excluded` |
| `kernel/layers/Logic/VirtualFileSystem.cpp` | 1235–1261, 1682–1734, 2171–2196, 2208–2242, 2263–2288, 2315–2346, 2373–2424, 2451–2551, 2585–2643, 2677–2708, 2820–2845, 3021–3080, 3462–3566, 3568–3748 | `VirtualFileSystem::RegisterFileSystemType`; `VirtualFileSystem::BuildAbsolutePath`; `VirtualFileSystem::CheckInodeAccess`; `VirtualFileSystem::CheckDirectoryMutation`; `VirtualFileSystem::InvalidateDentry`; `VirtualFileSystem::Mkdir`; `VirtualFileSystem::Unlink`; `VirtualFileSystem::Rename`; `VirtualFileSystem::Link`; `VirtualFileSystem::Mknod`; `VirtualFileSystem::SocketNode`; `VirtualFileSystem::Open`; `VirtualFileSystem::ChangeMetadata`; `VirtualFileSystem::Mount` |
| `kernel/layers/Logic/VirtualFileSystem.hpp` | 266–311 | `Grouped declarations/records; excluded` |
| `kernel/layers/Request/RequestLayerFactory.hpp` | 25–207 | `Grouped declarations/records; excluded` |
| `kernel/layers/Request/SchedulerPolicyRequests.cpp` | 12–132 | `SchedulerRequestManager::HandleSchedulingPolicyRequest` |
| `kernel/layers/Request/SchedulerRequestManager.cpp` | 160–189 | `SchedulerRequestManager::HandleSched_setaffinityRequest` |
| `kernel/layers/Request/SignalRequestManager.cpp` | 176–211 | `SignalRequestManager::HandleQueuedSignalRequest` |
| `kernel/layers/Request/VfsRequestManager.cpp` | 2954–2989, 2991–3051, 3143–3170, 3172–3198 | `VfsRequestManager::MetadataRequest`; `VfsRequestManager::HandleMountRequest` |
| `kernel/layers/Request/VfsRequestManager.hpp` | 14–74 | `Grouped declarations/records; excluded` |
| `kernel/layers/Resource/ProcessManager.cpp` | 362–425, 665–696 | `ProcessManager::ResetProcessSlot`; `ProcessManager::FreeProcess` |
| `kernel/layers/Resource/ProcessManager.hpp` | 109–172 | `Grouped declarations/records; excluded` |
| `kernel/layers/Resource/TaskManager.cpp` | 142–202 | `TaskManager::ResetTaskSlot` |
| `kernel/layers/Resource/TaskManager.hpp` | 25–55, 57–85 | `Grouped declarations/records; excluded` |
| `kernel/layers/Resource/VirtualMemoryManager.hpp` | 11–36 | `Grouped declarations/records; excluded` |
| `kernel/platform/memory/numa.c` | 34–147, 178–226 | `numa_parse_srat`; `numa_boot_topology` |
| `kernel/platform/memory/pmm.c` | 242–331 | `pmm_init` |
| `kernel/selftests/SocketFileSystemFixture.hpp` | 55–85 | `SocketFileSystemFixture::Mknod` |
| `kernel/selftests/logic/SignalManagerTests.cpp` | 48–86, 88–129 | `test_SignalManager` |
| `kernel/selftests/platform/platforminvariants.c` | 103–178 | `test_numa_topology` |
| `kernel/selftests/request/EventRequestManagerTests.cpp` | 13–58, 80–116, 118–181, 183–263, 265–322, 324–404, 1813–1859, 1867–1913, 2148–2195 | `create_readiness_source`; `readiness_action_entry`; `with_readiness_action`; `test_readiness_reuse`; `test_readiness_producers`; `test_readiness_interruption`; `test_epoll_exclusive`; `test_inotify_unlinked`; `test_signalfd_framework` |
| `kernel/selftests/request/SchedulerRequestManagerTests.cpp` | 53–175, 209–238 | `test_scheduling_policies`; `validate_sched_getaffinity` |
| `kernel/selftests/request/SocketRequestManagerTests.cpp` | 423–461, 463–501, 503–542, 544–593, 622–662, 683–707, 732–785, 799–845, 847–916, 1348–1376 | `test_socket_ancillary`; `test_socket_credentials`; `test_socket_reference_cycles`; `test_socket_pathnames`; `test_socket_sender_exit`; `validate_socket` |
| `kernel/selftests/request/SyncRequestManagerTests.cpp` | 411–483, 485–557, 559–641, 669–708 | `test_pi_cleanup`; `test_shared_pi`; `test_priority_futex`; `test_requeue_pi` |
| `kernel/selftests/request/VfsRequestManagerTests.cpp` | 3545–3609 | `test_vfs_manager` |
| `kernel/selftests/resource/TimeManagerTests.cpp` | 181–225 | `test_TimeManager` |
| `boot/boot.h` | 41–70 | `Grouped declarations/records; excluded` |

#### 2026-10-18 — Execution results and review

The owner instructed “apply fixes” after the recorded analysis. All three
readability fixes are implemented. No commit was requested or created.

- Expanded the four audited lambda bodies without changing captures, signatures,
  assertions, return expressions or invocation order.
- Added 50 braced bodies in the 22 audited files. Current masked/balanced scan:
  **0 candidates in 0 files**, exit 0. Reviewed nested scopes and following
  unconditional statements against the original source, including CPU selection,
  page-table freeing, EventRequest output loops, process/task loops, Klib copy
  advancement and pass-counting branches.
- Completed the six spacing batches: 29 files in the agreed spacing inventory
  received blank-line separation, plus six brace-ticket files received local
  separation around newly braced bodies. All three named examples now separate
  phases; source-region checks report 61 blank lines in PriorityFutex, 32 in
  ConfigurePolicy and 10 in the exec publication/cleanup section. These counts
  are evidence of change, not readability targets.
- Reviewed remaining discovery regions: the final scan reports 33 candidates
  in 22 files, retained as grouped declarations/ABI fields, record initialization,
  register-image copying, simple switches, or cohesive if/else dispatch chains.
  In particular, SocketManager's endpoint structure and option switch, the
  SynchronizationManager ownership/CAS branch chains, and validate_socket's
  request dispatch remain grouped. The long register copies in arch_signal
  remain grouped; the dispatch operation between input/output copies is separated.
  Both gather_boot_info initialization sequences, ResetProcessSlot,
  ResetTaskSlot, declaration headers, ISR list, Dispatcher switch, VFS test
  registration table and boot_info are intentional no-change decisions.
- Additional human-selected boundaries separate exec context publication from
  old-resource release and image replacement; scheduler policy state from deadline
  state; realtime queue enqueue/consume scenarios; RR/deadline/inheritance tests;
  individual malformed ancillary-message cases; readiness fixture setup, action,
  result inspection and cleanup; and CPU timer/limit state transitions. Related
  initializers and assertion groups remain together.

##### Source preservation checks

`python3 /tmp/arx-verify-readability.py` passed against the baseline and the
post-brace/lambda snapshot. It compares C/C++ lexical tokens, keeping comments
and complete string/character/raw literals as tokens, and checks directive and
continuation lines separately. Baseline → post-brace/lambda differs only by
**100 inserted brace tokens**; post-brace/lambda → final has identical tokens
and exact preprocessor lines. All 35 files with additional spacing also pass
`git diff --no-index -w --ignore-blank-lines --exit-code` against that snapshot.
The blank-line-ignore flag is necessary for an accurate whitespace-only check;
plain `git diff -w` still displays added blank lines.

Final source inventory: **45 changed first-party files**, with no changed vendor
source or boot/limine.h. The four-site `rg` lambda query returns no matches
(exit 1, as expected); the brace scan returns exit 0. The examples command
returns exit 0 with REVIEW for the three regions, followed by human phase-boundary
review. `git diff --check` passes. No temporary DEBUG instrumentation was added.
Snapshots and diagnostic/build logs are kept under clearly named
`/tmp/arx-readability-*` paths; no throwaway implementation prototypes were added.

##### Build checks

All four complete kernel compile/link checks pass:

| Target | DEBUG=1 | DEBUG=0 |
| --- | --- | --- |
| x86_64 | PASS | PASS |
| AArch64 | PASS | PASS |

Commands use the existing Makefile flags and isolated output trees, preserving
pre-existing repository bin/ and build/ contents:

```sh
make -j3 BUILD_DIR=/tmp/arx-readability-build/debug/build BIN_DIR=/tmp/arx-readability-build/debug/bin DEBUG=1 /tmp/arx-readability-build/debug/bin/kernel-x86_64.elf /tmp/arx-readability-build/debug/bin/kernel-aarch64.elf
make -j3 BUILD_DIR=/tmp/arx-readability-build/release/build BIN_DIR=/tmp/arx-readability-build/release/bin DEBUG=0 /tmp/arx-readability-build/release/bin/kernel-x86_64.elf /tmp/arx-readability-build/release/bin/kernel-aarch64.elf
```

Both commands were rerun after the final source edits and returned exit 0.
Initial target logs: `/tmp/arx-readability-build-{debug,release}-{x86,arm}.log`.
Final logs: `/tmp/arx-readability-build-{debug,release}-final.log`.
`file` confirms all four outputs are statically linked ELFs for the correct
machine. Full builds cover header consumers, including the SMP and socket fixtures.
Initial AArch64 builds emit the existing warning in unchanged
arch/aarch64/arch_helper.c:29 that a noreturn function returns; builds still pass.
This work does not complete the AArch64 runtime port. No QEMU/runtime self-tests
were run, as selected in the formatting-only verification plan.

##### Documentation and ticket handoff

Applied the update-kernel-docs impact review to the existing layer, Dispatcher,
platform, architecture and self-test guides. Behavior, contracts, ownership,
capabilities, source references and diagrams are unchanged, so no guide or README
edit is meaningful. No deferred implementation TODOs remain within this scope.

All acceptance criteria are met and the tickets are ready for owner review.
The owner previously said “you can close it”; the closure destination still needs
confirmation under TICKET.MD. Keep all three tickets in tickets/in_progress until
the owner confirms tickets/closed/ or another destination. Do not append closure
histories or mark closed before that confirmation.

#### 2026-10-18 — Closure

Owner confirmed “yes close” after the explicit proposal to use tickets/closed/
for all three tickets. All three files now have closed status at that destination;
TICKET_LOG.md and both originating Debug and Closed closure histories are updated.

## Archived readability audit script

Historical source used by the completed readability tickets. This is evidence,
not an installed executable; its original path-based root calculation is retained
as it was used during that audit.

```python
"""Candidate discovery for the October 2026 readability tickets.

This is analysis tooling, not a C/C++ parser or a readability acceptance test.
Comments, literals, and preprocessor continuations are masked for control scans.
Spacing spans require human review; grouped records and tables may stay dense.
Exit 1 means candidates exist, and exit 0 means none were found.
"""

import bisect
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXCLUDED = ('/uACPI/', '/flanterm/', '/printf/', '/khashl/')
LITERALS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\n]{0,16})\([\s\S]*?\)\1"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
TOKEN = re.compile(r'\b[a-zA-Z_]\w*\b|[^\s]')


def mask(source):
    source = LITERALS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), source)
    lines = source.splitlines(keepends=True)
    continuation = False
    for index, line in enumerate(lines):
        directive = continuation or line.lstrip().startswith('#')
        continuation = directive and line.rstrip().endswith('\\')
        if directive:
            lines[index] = re.sub(r'[^\n]', ' ', line)
    return ''.join(lines)


def sources():
    for directory in ('arch', 'kernel', 'klib', 'boot', 'initramfs'):
        for path in sorted((ROOT / directory).rglob('*')):
            relative = path.relative_to(ROOT).as_posix()
            if path.suffix not in ('.c', '.cpp', '.h', '.hpp'):
                continue
            if any(part in '/' + relative for part in EXCLUDED) or relative == 'boot/limine.h':
                continue
            yield path


def controls(path):
    source = path.read_text()
    clean = mask(source)
    tokens = list(TOKEN.finditer(clean))
    values = [token.group() for token in tokens]
    newlines = [match.start() for match in re.finditer('\n', source)]
    for index, value in enumerate(values):
        if value not in ('if', 'for', 'while', 'else', 'do'):
            continue
        end = index + 1
        if value in ('if', 'for', 'while'):
            if values[end] == 'constexpr':
                end += 1
            if values[end] != '(':
                continue
            depth = 1
            end += 1
            while end < len(values) and depth:
                depth += (values[end] == '(') - (values[end] == ')')
                end += 1
            if value == 'while' and values[end] == ';' and values[index - 1] == '}':
                continue
        if values[end] == '{' or (value == 'else' and values[end] == 'if'):
            continue
        line = bisect.bisect_right(newlines, tokens[index].start()) + 1
        yield line, source.splitlines()[line - 1].strip()


def dense(path):
    lines = path.read_text().splitlines()
    start = 0
    for index, line in enumerate(lines + ['']):
        if not line.strip():
            if index - start >= 25:
                yield start + 1, index
            start = index + 1


def examples():
    cases = (
        (
            'kernel/layers/Logic/SynchronizationManager.cpp',
            'uint64_t SynchronizationManager::PriorityFutex',
            '\nvoid SynchronizationManager::CleanupPriority',
        ),
        (
            'kernel/layers/Logic/SchedulerPolicies.cpp',
            'uint64_t Scheduler::ConfigurePolicy',
            '\nuint64_t Scheduler::ReadPolicy',
        ),
        (
            'kernel/layers/Logic/ProcessLifecycle.cpp',
            '    // Siblings have retired.',
            '\n    {\n        SignalDispositionGuard guard(current);',
        ),
    )
    failures = 0

    for filename, start, end in cases:
        source = (ROOT / filename).read_text()
        region = source.split(start, 1)[1].split(end, 1)[0].strip('\n')
        lines = region.splitlines()
        boundaries = sum(not line.strip() for line in lines)
        failures += boundaries == 0
        verdict = 'FAIL' if boundaries == 0 else 'REVIEW'
        print(f'{verdict}: {filename}: {len(lines)} lines, {boundaries} blank lines')

    # Separation existing is necessary but insufficient; a human reviews phases.
    return 1 if failures else 0


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ('braces', 'spacing', 'examples'):
        raise SystemExit('usage: readability_audit.py braces|spacing|examples')

    mode = sys.argv[1]

    if mode == 'examples':
        return examples()
    count = 0
    files = 0

    for path in sources():
        found = list(controls(path) if mode == 'braces' else dense(path))

        if found:
            files += 1
            count += len(found)
            print(path.relative_to(ROOT), found)

    print(f'{mode}: {count} candidates in {files} files')
    return 1 if count else 0


if __name__ == '__main__':
    raise SystemExit(main())
```
