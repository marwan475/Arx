# Expand unbraced first-party control flow

- Type: debug
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` found 50 unbraced
control-flow sites across 22 first-party files. Examples include
`TaskManager::CountTasks`, EventRequestManager's user-output loops, and Klib
error-logger tests. Some loops have an empty line before an unbraced nested
`if`, making the actual body boundary especially difficult to follow.

Expected behavior: first-party control-flow bodies use explicit, expanded
braced blocks matching [kernel.cpp](../../kernel/kernel.cpp) and
[add-arx-code](../../docs/skills/add-arx-code/SKILL.md), with readable newlines
and unchanged behavior.

Follow [TICKET.MD](../TICKET.MD). Read the architecture guides for the affected
areas before changing them. Creation does not authorize analysis or
implementation; use the required diagnosis skill when analysis is requested.

### Reproduce and inspect

The following locations come from the audit; line numbers apply to `d057ba1`.
Re-find the surrounding symbols if intervening changes move them. These are
source-level formatting findings, not demonstrated runtime failures.

| File | Unbraced control-flow lines |
| --- | --- |
| [arch/x86_64/arch_entry.c](../../arch/x86_64/arch_entry.c) | 155, 157, 374, 407 |
| [arch/x86_64/arch_paging.c](../../arch/x86_64/arch_paging.c) | 542, 740, 1083, 1085 |
| [arch/x86_64/lapic.c](../../arch/x86_64/lapic.c) | 76 |
| [kernel/layers/Logic/CredentialManager.cpp](../../kernel/layers/Logic/CredentialManager.cpp) | 74, 179, 213 |
| [kernel/layers/Logic/InterProcessCommunicationManager.cpp](../../kernel/layers/Logic/InterProcessCommunicationManager.cpp) | 691 |
| [kernel/layers/Logic/Scheduler.cpp](../../kernel/layers/Logic/Scheduler.cpp) | 152, 371, 752 |
| [kernel/layers/Logic/SynchronizationManager.cpp](../../kernel/layers/Logic/SynchronizationManager.cpp) | 169, 171, 223, 225 |
| [kernel/layers/Logic/VirtualFileSystem.cpp](../../kernel/layers/Logic/VirtualFileSystem.cpp) | 3158 |
| [kernel/layers/Request/EventRequestManager.cpp](../../kernel/layers/Request/EventRequestManager.cpp) | 250, 273, 357, 366 |
| [kernel/layers/Request/SystemRequestManager.cpp](../../kernel/layers/Request/SystemRequestManager.cpp) | 243 |
| [kernel/layers/Request/VfsRequestManager.cpp](../../kernel/layers/Request/VfsRequestManager.cpp) | 430, 1985 |
| [kernel/layers/Resource/ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp) | 548, 1511, 1524 |
| [kernel/layers/Resource/TaskManager.cpp](../../kernel/layers/Resource/TaskManager.cpp) | 21, 497 |
| [kernel/layers/Resource/TimeManager.cpp](../../kernel/layers/Resource/TimeManager.cpp) | 49 |
| [kernel/platform/acpi/acpi.c](../../kernel/platform/acpi/acpi.c) | 61 |
| [kernel/selftests/platform/klibtests.c](../../kernel/selftests/platform/klibtests.c) | 253, 261, 269, 318, 325, 334, 346, 360 |
| [kernel/selftests/platform/memorytests.c](../../kernel/selftests/platform/memorytests.c) | 159 |
| [kernel/selftests/poststartkernel/schedulertests.cpp](../../kernel/selftests/poststartkernel/schedulertests.cpp) | 366 |
| [kernel/selftests/poststartkernel/smpfixtures.hpp](../../kernel/selftests/poststartkernel/smpfixtures.hpp) | 13 |
| [kernel/selftests/poststartkernel/smpselftests.cpp](../../kernel/selftests/poststartkernel/smpselftests.cpp) | 447 |
| [kernel/selftests/poststartkernel/taskcontexttests.cpp](../../kernel/selftests/poststartkernel/taskcontexttests.cpp) | 201 |
| [klib/klib.c](../../klib/klib.c) | 24, 261 |

During analysis, inspect each listed body, including nested `if`/`else`
associations. Re-scan first-party C/C++ sources under `arch/`, `kernel/`, `klib/`,
`boot/`, and `initramfs/` for equivalent sites; regex results alone are not a
complete parser. Mask comments and literals and balance multiline conditions.
Exclude vendor uACPI, Flanterm, printf, khashl/khashp, and `boot/limine.h`.
Treat macro continuations and `do { ... } while (0)` tails separately rather
than reporting them as ordinary missing-brace sites.

The initial scan's completeness beyond these 50 sites is unknown. Confirm the
inventory and execution scope during diagnosis. Insert braces around the existing
body without changing loop coverage, `else` association, early returns, or
variable scope/lifetime. Inspect those semantics explicitly: adding braces is
not always equivalent to a whitespace-only change. Preserve comments and add
blank lines where needed for the corrected block to match `kernel.cpp`.

Keep broader spacing work in
[its separate ticket](improve_dense_kernel_code_spacing.md), compressed lambdas
in [their ticket](expand_compressed_selftest_lambdas.md), and architecture changes
in the dependency tickets. Overlapping files should use the latest source, not
outdated audit line numbers. Select affected build/syntax checks during analysis;
no runtime failure or new test scenario was established by this audit.

## Acceptance criteria

- [x] All 50 audited sites are accounted for, including sites changed by other work.
- [x] The agreed first-party scope has explicit braced control-flow bodies; candidate scan results are reviewed for macro and syntax false positives.
- [x] Each change preserves statement ordering, branch association, loop coverage, and declaration lifetime.
- [x] Formatting follows `kernel.cpp`, with readable blocks and blank-line separation; bundled third-party code remains untouched.
- [x] Appropriate syntax/build verification and diff checks are selected and recorded, and documentation impact is reported without unsolicited guide edits.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 because expanded,
human-readable control flow is an explicit owner requirement. The initial
inventory is 50 sites in 22 files; analysis and execution have not started.

### 2026-10-18 — Analysis and plan

Owner requested work on all three debug tickets. Read TICKET.MD and the required
`mattpocock-skills:diagnosing-bugs` skill; applied repository `add-arx-code`
and reviewed `update-kernel-docs`. Read kernel.cpp and relevant architecture,
layer, platform and self-test guides. Priority remains P2.

See [shared diagnosis, inventories, bounded batches, and verification plan](improve_dense_kernel_code_spacing.md#readability-audit-inventory).
Archived discovery script source is in [readability_audit.py](improve_dense_kernel_code_spacing.md#archived-readability-audit-script).
This phase changes ticket records and analysis tooling only. Execution awaits
the owner's explicit instruction after review, as required by TICKET.MD.

Confirmed all 50 audited bodies in 22 files, accounting for shifted source lines.
The masked, balanced-condition scan reproduced 50 candidates in two runs; manual
body-context inspection confirmed each. No extra candidates were found within
the scanned first-party C/C++ source/header scope. This is not a formal parser
proof of completeness. Macros, vendor files and do/while tails are excluded.

Plan: execute the shared inventory in four bounded area batches, wrapping only
the existing body and preserving nested associations and subsequent unconditional
work. See the shared plan for specific loop/scope hazards. No declaration body
requires a changed lifetime at these sites. Review added-brace tokens and
semantics; compile affected units on both targets/configurations and link the
kernels. No new runtime case is justified by a source readability defect.
Documentation impact is expected to be none.

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

All 50 audited sites were wrapped, with exactly 100 inserted brace tokens and
no other non-whitespace token changes. The first-party rescan reports zero
unbraced candidates. Reviewed loop coverage, else associations, declaration
lifetimes and unconditional work following the new blocks. Local blank-line
separation around those blocks follows kernel.cpp; vendor source is untouched.

### 2026-10-18 — Completion

Owner confirmed “yes close” in response to the proposal to close all three
tickets in tickets/closed/. Added explicit braced bodies at all 50 audited sites and verified that branch associations, loop coverage and declaration lifetimes are preserved. Required source-preservation checks,
diff checks and debug/release kernel builds for x86_64 and AArch64 passed,
as recorded above.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
