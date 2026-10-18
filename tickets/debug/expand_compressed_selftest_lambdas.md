# Expand compressed self-test lambdas into readable blocks

- Type: debug
- Priority: P2
- Status: open
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

- [ ] The four audited sites are accounted for and compressed lambdas in the affected files use expanded bodies.
- [ ] Captures, statements, return types, assertions, and call ordering are unchanged.
- [ ] The diff follows `kernel.cpp` formatting and contains no unrelated test or implementation changes.
- [ ] Appropriate syntax/build verification and diff checks are recorded; documentation impact is reported without unsolicited guide edits.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 because readable,
expanded formatting is an explicit owner requirement. Analysis and execution
have not started.
