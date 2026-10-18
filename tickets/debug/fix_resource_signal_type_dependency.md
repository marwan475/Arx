# Remove Resource's dependency on Logic-owned signal definitions

- Type: debug
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` found an upward
compile-time dependency: `TaskManager.hpp` includes Logic's `SignalManager.hpp`
to obtain signal records stored in Resource-owned tasks. `ProcessManager.hpp`
includes `TaskManager.hpp` and also stores these records; `ProcessManager.cpp`
uses signal constants declared in the Logic header.

Expected behavior: Resource defines or consumes these shared storage contracts
without depending on Logic. Logic may consume Resource definitions. Preserve
signal ABI layouts, values, task/process state, and signal behavior.

Follow [TICKET.MD](../TICKET.MD) and
[add-arx-code](../../docs/skills/add-arx-code/SKILL.md). Read the Resource and
Logic architecture guides before analysis. Creation does not authorize analysis
or implementation; use the required diagnosis skill when analysis is requested.

### Reproduce and inspect

- Inspect [TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp),
  initially line 3, and its signal fields.
- Inspect [SignalManager.hpp](../../kernel/layers/Logic/SignalManager.hpp) for
  `signal_action_t`, `signal_stack_t`, `signal_sender_t`, `signal_queue_t`, and
  signal constants, then trace their consumers in Resource and `arch/`.
- Inspect [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp)
  and [ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp).
- Reproduce with `rg -n 'layers/Logic|layers/Request' kernel/layers/Resource`.
  Review transitive dependencies as well as direct includes.

This is a source-level layering defect; no runtime failure was demonstrated by
the audit. The appropriate owning header and any include-cycle implications are
unknown until diagnosis. Moving the definitions is a candidate, not an approved
implementation plan; forward declarations alone cannot replace complete value
types. Keep this work separate from the
[runtime upward-call ticket](fix_resource_upward_runtime_calls.md).

## Acceptance criteria

- [ ] Diagnosis identifies the signal records/constants Resource requires and their consumers.
- [ ] Resource no longer includes or transitively depends on Logic for these definitions.
- [ ] Shared definitions have one authoritative owner below their consumers; signal ABI sizes and values remain intact.
- [ ] Affected declarations and consumers compile; relevant existing signal checks are selected during analysis and their results recorded.
- [ ] Changes follow `kernel.cpp` formatting, and documentation impact is reported; documentation edits require explicit instruction.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 for a confirmed
architecture-rule violation without a demonstrated runtime regression. Analysis
and execution have not started.
