# Document self-test infrastructure, suites and execution

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Create the guide to Arx self-tests, including SelfTestManager internals and lifecycle,
registration/grouping/fixtures/assertion accounting, execution phases and dependencies.
Map platform, Resource, Logic, Request, post-start/post-init/SMP integration and
userspace fixtures. Explain which CPU records results, worker aggregation, scheduling
requirements, unsafe abort behavior and environmental prerequisites.

Document actual build/boot/run steps and how to capture and interpret PASS/FAIL/SKIP/
BLOCKED and final summaries. Explain how to distinguish a failure, a missing prerequisite,
a hang and an incomplete run. Verify what selection and automation mechanisms exist;
label unsupported capabilities explicitly. Provide a worked example for adding a test
and debugging a failing case using existing tooling. Correct stale self-test references
such as README's reference to a single selftest.c. Keep test-framework implementation
changes outside this ticket.

Write for an experienced kernel developer learning Arx's implementation, and for
agents gathering focused context or investigating failures. Explain repository-specific
mechanics and rationale supported by the code; distinguish verified behavior from
inferred intent, limitations, and unknowns. Avoid generic kernel tutorials.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to agree documentation placement, depth, diagrams, and
examples. Record the plan, set status to `analyzed`, and wait for the owner's
explicit execution instruction. This ticket authorizes documentation work; record
implementation defects discovered during research separately.

## Related work

Coordinate navigation with [the project overview](../closed/document_project_structure.md); component guides link to relevant suites rather than duplicating framework documentation.

## Source entry points

- [kernel/selftests](../../kernel/selftests)
- [kernel/selftests/SelfTestManager.hpp](../../kernel/selftests/SelfTestManager.hpp)
- [kernel/selftests/SelfTestManager.cpp](../../kernel/selftests/SelfTestManager.cpp)
- [kernel/selftests/selftests.h](../../kernel/selftests/selftests.h)
- [kernel/selftests/ManagerSuite.hpp](../../kernel/selftests/ManagerSuite.hpp)
- [kernel/selftests/TestCase.hpp](../../kernel/selftests/TestCase.hpp)
- [kernel/bootstrap.c](../../kernel/bootstrap.c)
- [kernel/kernel.cpp](../../kernel/kernel.cpp)
- [kernel/layers/Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp)
- [Makefile](../../Makefile)
- [scripts/build-dynamic-fixtures.sh](../../scripts/build-dynamic-fixtures.sh)
- [scripts/debug-kernel.sh](../../scripts/debug-kernel.sh)
- [scripts/gdb_extention.py](../../scripts/gdb_extention.py)
- [README.md](../../README.md)

These are starting points; inspect callers, dependencies, and tests before making claims.

## Acceptance criteria

- [ ] Test suite/source map and boot/runtime phase ordering identify concrete invocation points.
- [ ] SelfTestManager accounting, fixtures, grouping, BSP/AP responsibilities and lifecycle constraints are explained.
- [ ] Reproducible build/run/log-capture instructions state architecture, firmware, fixtures and scheduling prerequisites.
- [ ] Result interpretation covers PASS/FAIL/SKIP/BLOCKED, summaries, hangs, incomplete runs and unsafe aborts.
- [ ] A source-backed example explains adding a test and investigating failure with available tools.
- [ ] Existing selection/automation capabilities and limits are verified; stale documentation is reconciled and navigation links resolve.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.
