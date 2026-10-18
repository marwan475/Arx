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

See [shared diagnosis, inventories, bounded batches, and verification plan](../analysis/readability_inventory.md).
Reusable discovery commands are in [readability_audit.py](../analysis/readability_audit.py).
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
[execution results](../analysis/readability_inventory.md#2026-10-18--execution-results-and-review)
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
