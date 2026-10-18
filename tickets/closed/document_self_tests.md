# Document self-test infrastructure, suites and execution

- Type: feature
- Priority: P2
- Status: closed
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
label unsupported capabilities explicitly. Explain test registration and available
failure-investigation tooling without worked examples. Reconcile stale self-test
references where they still exist. Keep test-framework implementation changes
outside this ticket.

Write for an experienced kernel developer learning Arx's implementation, and for
agents gathering focused context or investigating failures. Explain repository-specific
mechanics and rationale supported by the code; distinguish verified behavior from
inferred intent, limitations, and unknowns. Avoid generic kernel tutorials.

Follow [the ticket workflow](../TICKET.MD). During analysis use
`mattpocock-skills:grilling` to agree documentation placement and depth. The owner
excluded diagrams and examples during analysis. Record the plan, set status to
`analyzed`, and wait for the owner's
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

- [x] Test suite/source map and boot/runtime phase ordering identify concrete invocation points; layer-doc-style tables give one line describing each meaningful behavior/contract inside reported cases, including individual registered Request syscalls.
- [x] SelfTestManager accounting, fixtures, grouping, BSP/AP responsibilities and lifecycle constraints are explained.
- [x] Reproducible build/run/log-capture instructions state architecture, firmware, fixtures and scheduling prerequisites.
- [x] Result interpretation covers PASS/FAIL/SKIP/BLOCKED, summaries, hangs, incomplete runs and unsafe aborts.
- [x] Source-backed prose explains test registration and available failure-investigation tooling, without diagrams or worked examples.
- [x] Existing selection/automation capabilities and limits are verified; stale documentation is reconciled and navigation links resolve.

## Work log

### 2026-10-17 — Created

Assigned P2 as planned documentation work in the codebase understanding backlog.

### 2026-10-17 — Analysis and plan

The owner confirmed the proposed scope with two exclusions: no diagrams and no
examples. Create `docs/architecture/selftests/SelfTests.md` for experienced kernel
developers learning Arx and agents seeking focused context. Cover framework
internals, suite inventory, execution, and troubleshooting. Retain operational
build/run/log-capture commands; omit worked test-addition and debugging examples.
Use `docs/skills/write-kernel-docs/SKILL.md` and the ticket's required grilling
skill. No implementation changes are planned.

Initial source findings:

- `bootstrap.c` calls platform tests; `Dispatcher.cpp` calls Resource and Logic
  tests. `kernel.cpp` publishes the root-mount result and launches post-init tasks.
- `SelfTestManager::RunPostInitTests` runs post-start tests on participating CPUs,
  then Request tests and finalization on the BSP. The post-start group waits for
  SMP workers and checks cleanup before proceeding to other shared fixtures.
- `ManagerSuite.hpp` and `TestCase.hpp` define capability checks, grouping, RAII
  case reporting, assertion counts, and explicit SKIP/BLOCKED results. A case with
  no checks becomes BLOCKED; assertion failures override its requested status.
- `TestFixture.hpp` disables preemption and temporarily installs disposable task
  and process identity. Scheduling or sleeping while holding it is unsafe.
  Failed identity restoration or reclamation can invoke `AbortUnsafe`.
- `AbortUnsafe` records a blocked remaining-groups case, finalizes, disables
  interrupts, and halts. A printed summary therefore does not alone prove that
  every phase completed.
- `Makefile` explicitly lists self-test sources, builds musl-backed x86_64 dynamic
  fixtures, and exposes separate build, QEMU, and debugger targets.
  `debug-kernel.sh` captures serial output and loads `gdb_extention.py`.
- README already has no `selftest.c` reference; the original ticket's example of
  stale text is obsolete. Add the new guide to its Documentation list.

Execution plan:

1. Trace every group runner and its callers; inventory suites, fixture helpers,
   userspace artifacts, architecture conditions, and prerequisites. Inspect
   manager retention limits, accounting, summary generation, worker aggregation,
   and the existing debugger extension before documenting their contracts.
2. Write the guide in initialization/runtime order, using compact tables for the
   suite map, fixture responsibilities, and result meanings. Explain scheduling,
   BSP/AP ownership, cleanup, unsafe aborts, and incomplete-run detection.
3. Document actual build/boot/log-capture commands and debugger entry points.
   Verify available selection and automation mechanisms against source; identify
   unsupported mechanisms explicitly. Distinguish x86_64 support from experimental
   AArch64 behavior and separate verified observations from unknowns.
4. Add README navigation and correct any remaining stale references within scope.
   Record implementation defects separately rather than changing framework code.
5. Check claims against source, verify filenames and local links, review the guide
   for readability, and run `git diff --check`. Attempt representative x86_64 and
   AArch64 build/boot/log capture where tools and prerequisites are available;
   record actual outcomes and any blockers without claiming an unobserved pass.

Risks and remaining investigation: manager output has bounded retained detail;
streamed output may be needed for diagnosis. SMP waits and shared fixture cleanup
can stop later phases. Dynamic userspace fixtures have architecture/toolchain
constraints that need explicit verification. A timeout or summary alone must not
be treated as a complete passing run. Suite-selection and automated exit-status
support remain to be verified during the full source audit.

Analysis is complete. Await the owner's explicit execution instruction before
creating the guide; on execution, move this ticket to `tickets/in_progress/`.

### 2026-10-17 — Inventory and presentation refinement

The owner requested the style of the layer guides, with one line explaining what
each test tests, and asked to count cases first. Use the numbered-section and
compact-table style of `docs/architecture/layers/Logic.md`, with `Test case` and
`What it tests` columns grouped by suite. Keep the agreed exclusions of diagrams
and examples. This refines the inventory presentation within the analyzed scope.

Source inspection found 159 unique reportable case names: 155 regular cases and
four conditional diagnostic cases. Regular cases by phase are platform 17,
Resource 54, Logic 49, post-start 23, and Request 12. These include 19 automatically
reported manager-capability cases (eight Resource, ten Logic, one post-start),
leaving 136 regular behavior cases. Request suites use `InContract`, so their
group names do not create additional capability cases.

The four diagnostic names are `init/remaining_groups`, `poststartkernel_vfs_mount`,
`request/capabilities`, and `resource/DmaManager/reserved_memory`. They report
unsafe initialization or missing prerequisites, rather than four additional
normal-path tests. A particular boot need not report all 159 names.

Counted unique names from explicit `CaseBegin`, `selftest_case_begin`, `TestCase`,
and `ManagerSuite::Run` registrations; included default `ManagerSuite` capability
cases, the six table-driven klib cases, both executable-fixture names, and the
three shared-synchronization loop names. Deduplicated alternate branches such as
the heap-cache case. SMP workers contribute aggregate checks rather than one case
per CPU. This is a source inventory, not a runtime pass count or assertion count.


### 2026-10-17 — Execution authorized and section order confirmed

The owner authorized execution, briefly paused to refine presentation, and then
explicitly authorized continuation. Organize the guide around platform tests,
Resource unit tests immediately after Resource initialization, Logic unit tests
immediately after Logic initialization, post-start integration tests, and Request
kernel-side syscall tests grouped by manager. Provide one-line descriptions of
meaningful tests/contracts inside reporting groups, particularly each syscall
family and its validation paths. Distinguish handler coverage from actual
userspace syscall entry/return coverage. No diagrams or worked examples.

Moved this ticket to `tickets/in_progress/`. The earlier analysis pause is resolved.


### 2026-10-17 — Scope narrowed to x86_64

The owner excluded AArch64 content and verification from the current guide.
The initial combined architecture build and boot attempts had already started;
only x86_64 behavior and results are used for this ticket's completion evidence.
No architecture implementation changes were made.

### 2026-10-17 — Ready for owner review

Created [SelfTests.md](../../docs/architecture/selftests/SelfTests.md) in the layer
guides' numbered-section/compact-table style. The inventory follows platform,
Resource-after-init, Logic-after-init, live post-start integration, and BSP Request
syscall execution. It gives one-line descriptions for behavior cases and inner
contracts, including all 166 registered Request syscall subtests plus substantive
nested policy, readiness, synchronization, filesystem, and socket checks.
It distinguishes reported cases, syscall subtests, and assertions, and separates
kernel Dispatcher calls from actual userspace entry/return fixtures.

The guide also covers manager accounting/retention, fixture ownership and cleanup,
BSP/AP aggregation, prerequisites and musl artifacts, x86_64 build/run/log capture,
result interpretation, incomplete runs/unsafe abort, registration, selection
limits, and existing GDB tools. No diagrams or worked examples were added.
README navigation and the two open parent-ticket references were updated; no
stale `selftest.c` reference remained to remove.

Verification:

- Initial build command `make -j4 x86_64 aarch64` exited 0; the x86_64 image and
  its packaged userspace fixtures were built successfully. Build output:
  `/tmp/arx-selftests-doc-build.log`.
- Ran the documented headless x86_64 q35 / CPU max / four CPUs / 1 GiB / repository
  UEFI / snapshot configuration for 45 seconds. The sandbox initially rejected
  QEMU's `/var/tmp` snapshot creation; the authorized escalated rerun completed
  the suite. Serial evidence: `/tmp/arx-selftests-doc-x86.log`; emulator output:
  `/tmp/arx-selftests-doc-x86-qemu.log`.
- Observed 155 executed cases, all PASS; zero failed, skipped, blocked, or omitted
  manager cases; final overall PASS. Request emitted 166 syscall results: 165
  PASS and one expected SKIP for `getrandom`'s missing entropy subsystem. This
  nested skip is intentionally distinct from the manager-level skip count.
- QEMU subsequently exited 124 when the external timeout terminated it, matching
  the documented persistent kernel loop after the final summary.
- Checked every Resource/Logic behavior registration and each Request table entry
  against its guide section. All registered unit-case suffixes and 166 syscall
  names are covered. Verified source filenames, local navigation links, numbered
  section order, consistent table columns, and the absence of diagrams/AArch64
  content from the guide.
- `git diff --check` passed. Tracked implementation files are unchanged. Builds
  produced untracked `build/` and `bin/` outputs; they are not documentation
  deliverables. No implementation defects or deferred guide TODOs were identified.

All acceptance criteria are satisfied within the owner's final scope. Status
remains `in progress` pending explicit owner authorization to close and move to
`tickets/closed/document_self_tests.md`.


### 2026-10-17 — Remove changing totals from the guide

The owner requested README navigation and exclusion of dynamic pass/fail totals.
Verified the README's Kernel self-tests link resolves to the new guide. Removed
aggregate case counts and per-manager subtest counts from the guide so its
inventory describes contracts without freezing changing totals. Historical run
results remain in this work log as dated verification evidence. Result meanings
and instructions for reading current runtime totals remain documented.

Verified local README navigation and `git diff --check`. Status remains
`in progress` awaiting owner-authorized closure and destination.


### 2026-10-17 — Completion

The owner authorized closure after the proposed destination
`tickets/closed/document_self_tests.md` was stated. Created the x86_64 self-test
guide with phase-ordered inventories and one-line contract descriptions, linked
it from README, and excluded diagrams, worked examples, and changing test totals.
Source, navigation, build, runtime, and whitespace verification are recorded above.
