# Add self-test progress and recorded boot-error debug views

- Type: feature
- Priority: P2
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose retained failure evidence through the new debug registry: the active
self-test phase/case and recorded results/details, plus published boot errors.
Diagnose a boot that stalls before its summary, an unsafe test abort or a failure
whose serial log is unavailable. Currently an investigator must manually locate
SelfTestManager private tables or the static boot error buffer.

Follow [the workflow](../TICKET.MD), required grilling and repo skills. Parent:
[planning](../closed/plan_gdb_extension_tickets.md). Depends on
[foundations](build_debug_framework_foundation.md). Reference:
[assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment) and
[self-test documentation ticket](../closed/document_self_tests.md).
This ticket starts open and requires its own analysis/execution gate.

### Scope and internal milestones

1. Define SelfTestManager and boot-diagnostics debug headers/records, with
   maintained summary values where useful and explicit roots to retained source
   records. Publish through Dispatcher DebugManager without assuming every
   provider is a layer manager. Isolated test buffers/manager fixtures must not
   replace production registrations.
2. Show initialized/uninitialized state, phase/context, group stack, root-mount
   readiness, active case name/index/start sample, case status/count-kind,
   passes/failures/skips/blocked results, failure details/truncation and finalization.
   Names are borrowed boot-lifetime strings; bound reads and preserve source
   units/status meaning. A completed suite does not imply QEMU should exit.
3. Show `boot_errors` reserved count, published ready entries in slot order,
   unpublished/reserved slots and capacity-exceeded accounting. Existing records
   store explicit diagnostic messages, not automatic syscall errno history,
   timestamps or CPU IDs. Do not fabricate missing attribution or reorder slots
   as if a global temporal trace existed.
4. Preserve collection bounds from actual arrays, separate summary stats from
   existing self-test/error evidence, and define publication/freshness/native-reader
   semantics. New debug-framework storage/update work compiles out in release;
   do not remove or disable pre-existing self-tests or production logging merely
   because their debug-registry adapters are disabled.
5. Add bounded GDB adapters and guide examples for pre-test, active, failed,
   truncated and finalized states. Never call Get/PrintSummary/error_logger_print
   in the inferior or acquire recorder locks. Dump mechanics remain deferred.

### Proposed interface and illustrative output

```text
arx-debug selftests active
  phase=post-start case=integration/wait/<name> finalized=false start=<sample>
arx-debug selftests results --failed --limit 20
  case=<name> status=FAIL countKind=assertions failures=1 detailTruncated=false
arx-debug errors records --limit 64
  slot=0 ready=true message="<recorded diagnostic>"
  slot=1 ready=false state=reserved-unpublished
  reserved=65 capacity=64 excess=1
```

Illustrative values only; settle grammar/status vocabulary during analysis.
Incomplete array entries, invalid lengths, unreadable strings and active writes
must remain distinguishable from a genuinely empty/successful result set.
No historical trace service, payload dump pipeline or Request-layer support.

### Sources and unknowns

- [SelfTestManager.hpp](../../kernel/selftests/SelfTestManager.hpp),
  [implementation](../../kernel/selftests/SelfTestManager.cpp),
  [selftests.h](../../kernel/selftests/selftests.h): singleton, context,
  `caseResults`, `caseFailDetails`, current case/group/result semantics.
- [error_logger.h](../../klib/error_logger.h), [klib.c](../../klib/klib.c):
  `error_log_buffer_t`, `boot_errors`, reserved/ready publication and reporting.
- [SelfTests.md](../../docs/architecture/selftests/SelfTests.md): execution phases,
  statuses and BSP/AP ownership.
- Unknowns: publication meaning of a partially completed case, sample clock
  interpretation, whether any extra debug summaries are necessary and production
  root visibility before/after Dispatcher. No extra recorder should duplicate
  retained messages merely to display them.

## Acceptance criteria

- [ ] Production self-test/error records are discoverable with staged availability and correct debug-only adapter lifecycle.
- [ ] Active/group/case/result/failure/finalization views preserve actual statuses, count kinds, detail truncation and sample units.
- [ ] Error records distinguish ready, unpublished and excess-capacity states without invented history/attribution or inferior printing calls.
- [ ] All records/strings are bounded; malformed/partial state is not rendered as empty, PASS or complete.
- [ ] Disabling the debug framework removes its own added work/storage while preserving existing self-test/logging behavior.
- [ ] Native-reader record rules and command guide are complete; no dump or Request machinery is added.

## Verification approach

Use [self-test infrastructure](../../kernel/selftests/SelfTestManager.cpp) and
[klib tests](../../kernel/selftests/platform/klibtests.c), including isolated
error buffers. Exercise empty/uninitialized, active and finalized test state,
PASS/FAIL/SKIP/BLOCKED, detail/name truncation, a reserved-but-unpublished error
slot and capacity overflow through controlled fixtures. Compare known fixture
results and serial evidence where available; inspect x86_64 QEMU before/after
phase boundaries and at a deliberate safe stop in an active case. Avoid causing
an unsafe kernel failure merely to test the display. Build debug/release and
verify no behavior change to the pre-existing recorders when adapters are absent.

## Work log

### 2026-10-18 — Created

P2: combines bounded published failure tables and boot evidence, providing useful
inspection even when normal test reporting has not completed.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
