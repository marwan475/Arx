# Restore blank-line separation in dense kernel functions

- Type: debug
- Priority: P2
- Status: open
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
[runtime layering migration](fix_resource_upward_runtime_calls.md).

## Acceptance criteria

- [ ] Analysis records the first-party functions/files needing spacing changes and the agreed execution batches.
- [ ] All three audited examples and the agreed inventory have readable blank-line separation matching `kernel.cpp`.
- [ ] Closely related statements stay grouped; no mechanical blank-line quota is substituted for readability review.
- [ ] Verification confirms changes are whitespace-only, preserve preprocessing/literal semantics, and leave vendor code untouched.
- [ ] Diff checks and a final readability review are recorded, along with the documentation-impact finding.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 because generous
newlines and human-readable formatting are explicit owner requirements.
Analysis and execution have not started.
