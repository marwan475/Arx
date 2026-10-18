# Expand compressed self-test lambdas into readable blocks

- Type: debug
- Priority: P2
- Status: closed
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` identified four self-test
lambdas with their bodies compressed onto the call or assignment line. This
violates [add-arx-code](../../docs/skills/add-arx-code/SKILL.md), which requires
expanded, human-readable code matching [kernel.cpp](../../kernel/kernel.cpp).

Expected behavior: each affected lambda has a readable multiline body, braces
and spacing consistent with `kernel.cpp`, and unchanged captures, return values,
assertions, and invocation behavior.

Follow [TICKET.MD](../TICKET.MD). Read the relevant Logic and Request guides.
Creation does not authorize analysis or implementation; use the required
diagnosis skill when analysis is requested.

### Reproduce and inspect

Inspect the following locations; line numbers describe the audited revision:

| File | Line | Expression |
| --- | --- | --- |
| [ElfMapperTests.cpp](../../kernel/selftests/logic/ElfMapperTests.cpp) | 186 | Assignment to `backend.operations.Read`. |
| [EventRequestManagerTests.cpp](../../kernel/selftests/request/EventRequestManagerTests.cpp) | 2199 | Lambda passed to `with_syscall_peer`. |
| [SignalRequestManagerTests.cpp](../../kernel/selftests/request/SignalRequestManagerTests.cpp) | 278 | Interrupted-futex lambda passed to `signal_with_peer`. |
| [SignalRequestManagerTests.cpp](../../kernel/selftests/request/SignalRequestManagerTests.cpp) | 296 | Robust-task-exit lambda passed to `signal_with_peer`. |

Reproduce candidate discovery with `rg -n '\{ return .*;.*\}' kernel/selftests`;
inspect results rather than treating regex matches as proof. During analysis,
confirm the four sites still exist and check the affected files for equivalent
compressed lambdas. Additional sites outside those files are follow-up scope.

This is a readability defect; no runtime failure was demonstrated. No new test
behavior or test cases are requested. Determine the necessary verification from
the final syntactic changes and existing build configuration during analysis.
Keep brace insertion in ordinary control flow in
[its separate ticket](expand_unbraced_first_party_control_flow.md).

## Acceptance criteria

- [x] The four audited sites are accounted for and compressed lambdas in the affected files use expanded bodies.
- [x] Captures, statements, return types, assertions, and call ordering are unchanged.
- [x] The diff follows `kernel.cpp` formatting and contains no unrelated test or implementation changes.
- [x] Appropriate syntax/build verification and diff checks are recorded; documentation impact is reported without unsolicited guide edits.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 because readable,
expanded formatting is an explicit owner requirement. Analysis and execution
have not started.

### 2026-10-18 — Analysis and plan

Owner requested work on all three debug tickets. Read TICKET.MD and the required
`mattpocock-skills:diagnosing-bugs` skill; applied repository `add-arx-code`
and reviewed `update-kernel-docs`. Read kernel.cpp and relevant architecture,
layer, platform and self-test guides. Priority remains P2.

See [shared diagnosis, inventories, bounded batches, and verification plan](improve_dense_kernel_code_spacing.md#readability-audit-inventory).
Archived discovery script source is in [readability_audit.py](improve_dense_kernel_code_spacing.md#archived-readability-audit-script).
This phase changes ticket records and analysis tooling only. Execution awaits
the owner's explicit instruction after review, as required by TICKET.MD.

Confirmed all four audited sites at current lines 186, 2199, 278 and 296 in the
three listed files. The ticket's `rg` command reproduced exactly those four
compressed bodies; reviewing other lambda candidates in those files found no
additional site. Diagnosis: formatting defects remain; no runtime failure.

Plan: expand the four bodies using existing multiline lambda style, preserving
all tokens, captures, return types, assertions, ordering, and invocation syntax.
No new test cases. Verify token identity, diff checks, and compilation of all
three translation units on both architectures in debug and release. Coordinate
spacing in EventRequestManagerTests.cpp after lambda expansion. No meaningful
architecture-guide update is expected; review impact at completion.

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

The four audited bodies are multiline. Source-token comparisons preserve all
lambda captures, statements, return types, assertions and invocation order.
The original rg query has no remaining matches. Build coverage includes all
three affected self-test translation units on both targets/configurations.

### 2026-10-18 — Completion

Owner confirmed “yes close” in response to the proposal to close all three
tickets in tickets/closed/. Expanded the four audited lambda bodies while preserving captures, assertions, returns and invocation order. Required source-preservation checks,
diff checks and debug/release kernel builds for x86_64 and AArch64 passed,
as recorded above.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
