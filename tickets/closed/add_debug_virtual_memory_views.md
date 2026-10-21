# Add virtual-memory ownership, backing and x86_64 translation debug views

- Type: feature
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose process and kernel mappings, backing-page relationships and selected
x86_64 page-table translations through the debug framework. Diagnose unexpected
permissions/data, memory retained after unmap, shared accounting mismatches or
failed writeback. Current `arx-vmm` reaches only CPU-active address spaces and
prints basic region bounds/type, requiring manual reads for inactive processes,
backing objects, dirty pages and hardware table entries.

Follow [the workflow](../TICKET.MD), required grilling and repo skills. Parent:
[planning](../closed/plan_gdb_extension_tickets.md). Depends on
[foundations](../closed/build_debug_framework_foundation.md); use process identities from
[process/resource views](../closed/add_debug_process_resource_views.md). Related:
[assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment),
[architecture documentation](../closed/document_architecture_layer.md).
This ticket is open and retains its own analysis/execution gate.

### Scope and internal milestones

1. Define manager-owned debug headers and maintained map/unmap/protect/backing/
   failure/current-resident statistics with clear page/byte and shared/private
   semantics. VMM is largely a C++ facade: publish appropriate platform roots,
   including kernel space, process/CPU spaces and pending writeback, without
   assuming useful state resides directly in the manager object.
   Include ElfMapper's parse/validation/load/map success/failure statistics in
   its own registered header. This does not authorize persistent segment/image
   history: production exec metadata is temporary. Extend ProcessController's
   memory counters in the record owned by the process/resource ticket; do not
   introduce duplicate records/registration for that manager.
2. Support process, CPU or explicit typed-address-space selection, including an
   inactive process. Show space type/PT/references/lock bytes and the four free/
   used region lists, metadata pool/counts, region permissions/max protection,
   shared flags, object/offset, resident/shared pages and owned backing blocks.
3. Walk reachable backing/shared-page relationships once, showing references,
   mappings/writable mappings, dirty/accounting flags, object size/context and
   pending-writeback state. Expose `pending_writeback` explicitly so failed
   writeback retained after the last mapping is not silently lost. Distinguish
   reachable objects from a nonexistent complete global backing catalogue.
4. Add selected-VA/range x86_64 page-table explanation using a chosen root and
   validated HHDM mapping: intermediate/leaf entries, presence, page size,
   permissions and physical result. Validate supported address width/canonicality,
   level/page-size formats and access path. Bound reads; fail clearly when page
   tables are not addressable at that stop. Never call target translation helpers
   or present this as a readout of hardware TLB contents.
5. Reuse shared identity readers to correlate backing with files, processes and
   CPUs without building a separate universal graph engine. Add view adapters,
   native-reader bounds/link/translation semantics and command-guide updates;
   preserve all useful old VMM output for [cutover](../in_progress/migrate_debug_commands_and_document_framework.md).

### Proposed interface and illustrative output

```text
arx-debug memory regions --pid 9 --limit 64
  [0] va=0x400000..0x401000 prot=R-X shared=false backing=<addr> offset=0
arx-debug memory backing <address> --limit 32
  references=2 mappings=1 writableMappings=0 page[0].dirty=true
arx-debug memory writeback --limit 32
  object=<addr> state=retained-for-retry
arx-debug memory translate --pid 9 --va 0x400123
  root=<pa> pageSize=4096 effective=R-X physical=<pa>
```

The final grammar/permissions vocabulary is settled in analysis. Counters and
records compile out of release. No target calls, locks, automatic hardware
probing, bulk page payload capture, new all-object ownership ledger, Request
instrumentation or dump mechanics. Partial state can explain a transition but
must not be declared corrupt solely because invariants do not yet match.

### Sources and unknowns

- [vmm.h](../../kernel/platform/memory/vmm.h), [vmm.c](../../kernel/platform/memory/vmm.c):
  `virt_addr_space_t`, `virt_region_t`, backing/shared-page records,
  `pending_writeback` and shared-resident accounting.
- [VirtualMemoryManager.hpp](../../kernel/layers/Resource/VirtualMemoryManager.hpp),
  [implementation](../../kernel/layers/Resource/VirtualMemoryManager.cpp),
  [ProcessMemoryManager.cpp](../../kernel/layers/Logic/ProcessMemoryManager.cpp):
  update/rollback sites, protection and file backing.
- [ElfMapper.hpp](../../kernel/layers/Logic/ElfMapper.hpp),
  [ElfMapper.cpp](../../kernel/layers/Logic/ElfMapper.cpp): manager-owned operation
  statistics; persistent segment arrays are not a production process catalogue.
- [arch_paging.c](../../arch/x86_64/arch_paging.c),
  [x86_64.h](../../arch/x86_64/x86_64.h), [pmm.h](../../kernel/platform/memory/pmm.h):
  table format, root/PA constraints and HHDM interpretation.
- Unknowns: canonicality/table-mode support, effective permission folding, counter
  ownership across retry/rollback, safe read limits, stale weak links and native
  reader representation. Keep architecture-specific interpretation local.

## Acceptance criteria

- [x] Debug-only maintained memory stats and roots expose kernel/CPU/process spaces, including inactive processes and pending writeback.
- [x] Regions, protection limits, backing/shared pages and observed ownership links are readable with explicit completeness and deduplication.
- [x] Selected x86_64 translations explain intermediate/leaf permissions and page size without inferior execution or claiming actual TLB contents.
- [x] Unknown/inaccessible roots, invalid/noncanonical addresses, cycles, excessive lengths and partial groups terminate with useful bounded status.
- [x] Useful old VMM information remains mapped to the new interface; roots/lifetime/translation semantics support a future native reader.
- [x] Guide, migration ledger and relevant architecture descriptions reflect verified behavior; release has no additional debug records/update work.

## Verification approach

Use [VirtualMemoryManagerTests](../../kernel/selftests/resource/VirtualMemoryManagerTests.cpp),
[memory integration](../../kernel/selftests/poststartkernel/memoryintegration.cpp),
[TLB integration](../../kernel/selftests/poststartkernel/tlbintegration.cpp)
and platform mapping fixtures. Inspect known anonymous/private/shared mappings,
protection changes, clone isolation, dirty pages and `dispatcher_backing_retry`
state. Compare a selected VA's expected fixture mapping with independently read
page-table entries; use controlled tables for supported large-page/missing-entry
cases absent in a normal boot. Include inaccessible HHDM, unmapped VA, malformed
links and inactive process state. Run x86_64 debug/release checks for changed code;
no privileged hardware register reads are required to verify the display.

## Verification scope

Keep verification proportional to this ticket's changes. Select the smallest set
of checks that demonstrates the acceptance criteria and covers the affected
behavior. Run the relevant checks once after the final changes; stop when they
pass. Repeat or broaden checks only after a new change, a failure, or a concrete
unresolved risk, and record the reason. Do not add repeated QEMU boots, unrelated
suite runs, or baseline investigations merely for extra confidence. Record
incidental failures and their impact; investigate further only when evidence
makes them a blocker for this ticket. Preserve required acceptance checks and
report any checks that could not run.

## Work log

### 2026-10-18 — Created

P1: mapping/backing and translation views share the same address/permission and
retention investigation workload. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Analysis started

The owner selected this ticket after closing process/resource views. Read the
required grilling skill, repository code/documentation skills and current ticket.
Read-only fact finding for VMM/backing roots and x86_64 translation constraints
was delegated as required by grilling. Analysis will preserve the owner's
human-readable output preference and proportional verification stopping rule.
Implementation has not started; status remains open pending design agreement.

### 2026-10-20 — Analysis and plan; owner design confirmation

The owner answered “yes to all” to full ticket scope, human-readable summaries,
focused verification, explicit address-space selection, four-level x86_64
translation and source-owned counters/relationships. Shared understanding is
confirmed. No implementation or builds have started. Execution awaits the
owner's separate instruction under the ticket workflow.

#### Source findings and agreed semantics

- Platform vmm.c owns the kernel address space and pending_writeback head.
  Process addressSpace pointers include inactive processes; CPU address_space
  pointers describe observed active associations. No global address-space or
  backing-object catalogue exists. Publish typed borrowed roots and reuse the
  completed process/task identity readers; do not create a shadow ownership DB.
- Address spaces already expose page-table roots, references, lock bytes, four
  free/used region lists and metadata pools/counts. Metadata counts are not live
  mapping counts. Free nodes may retain stale backing fields: interpret backing
  ownership only on used nodes. Bound each traversal and report partial state.
- Region backing is resident bytes; a null pointer may be an EOF or legacy
  reservation. object can retain ownership without resident bytes. shared page
  references differ from shared_mapping semantics. blocks own whole PMM
  allocations. Display current/max R/W/X, offsets, sizes and identities clearly.
- Backing mappings/writable_mappings count page-region relationships, not mmap
  syscalls. References include other owners and retained writeback. Dirty flags
  are conservative while writable aliases exist. Deduplicate reachable objects
  and pages; observed links cannot prove complete ownership or leaks.
- Failed final flush retains a reference and queues the object for retry. Retry
  temporarily detaches the pending list, so an empty head during an active retry
  is not proof that no writeback is outstanding. Expose pending state plus
  interrupted-update markers and retry/flush outcomes.
- Count high-level mmap/munmap/mprotect/mincore/madvise/brk attempts and outcomes
  in the existing ProcessController LifecycleDebugStats record, including early
  argument rejection. brk refusal returns the old break and needs explicit
  classification. Avoid duplicate lifecycle records and nested success totals.
- Lower-level map/unmap/protect, space/backing/page lifecycle and failures belong
  to their source owners. Distinguish request completion from actual allocation,
  reclamation and resident/shared byte changes. Destroy can drop a reference or
  refuse/defer reclamation; it must not count every call as a destroyed space.
  Count committed transitions once, retaining existing synchronization and
  rollback behavior. Unsigned lifetime counters use documented modular totals.
- ElfMapper gets its own parse/validation/load/map operation statistics and
  production registration. Its segment metadata is transient: retain no image
  history and do not infer a complete ELF catalogue from regions.
- Boot requests only four-level paging. The repository format is 48-bit canonical
  VA with 52-bit encoded PA; this is not a detected CPU capability claim. Explain
  4 KiB, 2 MiB and 1 GiB leaves, per-level raw entries and folded RW/US/NX
  permissions. These describe RAM page tables, not TLB contents or all CPU access
  controls. No register reads, target helper calls or locks are permitted.
- Read table entries only through a published valid HHDM path, checking presence,
  addition overflow, canonicality, alignment and target readability. No identity
  fallback when HHDM is unavailable. Validate root and huge-leaf formats, including
  PAT placement; stop on absent/unreadable entries or unsupported formats.

#### Implementation plan

1. Read affected architecture/platform, Resource, Logic and Dispatcher guides
   plus kernel.cpp before writing code. Follow repository code and documentation
   skills; preserve readable formatting and architecture boundaries.
2. Add debug-only owner records/native semantic contracts for platform VMM roots
   and maintained activity, VirtualMemoryManager and ElfMapper as appropriate.
   Extend the existing lifecycle record for process memory operations. Publish
   through normal Dispatcher composition; temporary test managers do not register.
   Keep storage, update arguments and registration absent from DEBUG=0.
3. Instrument actual owner-side completion/mutation sites in vmm.c,
   VirtualMemoryManager.cpp, ProcessMemoryManager.cpp and ElfMapper.cpp without
   changing memory ownership, locking, mapping policy or error behavior. Use
   grouped update markers to distinguish interrupted snapshots.
4. Add a dedicated virtual-memory adapter under debug/gdb/views, reusing bounded
   readers and shared identities. Proposed commands are memory space, regions,
   backing <numeric-address>, writeback and translate. Default selection is
   kernel; --pid, --cpu (logical ID), --slot and --space (typed numeric address)
   are mutually exclusive. Translate takes --va and optional bounded --size;
   numeric zero VA is valid. Reject expressions and incompatible options.
5. Lead output with ranges, permissions, sizes and ownership. Detailed backing
   links and translation steps appear in their selected views. Preserve legacy
   CPU count/slot/ID, space type/PT/locks, four region lists and metadata counts.
   Deduplicate reachable relationships, cap reads independently of stored counts,
   detect cycles and label unavailable, stale, partial and truncated information.
6. Keep x86_64 decoding local to its adapter/architecture contract. Selected-range
   traversal advances by leaf boundaries under shared entry/byte/depth budgets;
   never scan an entire address space by default. Document native layout, roots,
   lifetimes, permission folding, HHDM requirements and completeness limits.
7. Update GdbCommands.md, legacy VMM coverage ledger and affected existing
   architecture descriptions. Preserve old commands for the later cutover ticket.

#### Focused verification and stopping rule

- One focused host fixture suite covers region/backing deduplication, malformed
  links/counts, partial updates, pending writeback, inactive selection and parser
  limits. Translation cases cover three leaf sizes, intermediate restrictions,
  absent entries, noncanonical VA, overflow/invalid leaf, inaccessible HHDM and
  budget termination. Use controlled data for formats absent in a normal boot.
- Build one x86_64 debug image and one release image after final runtime changes.
  Perform focused compile-out/header checks for the added records and sites.
  Reuse existing tooling rather than adding a broad verification framework.
- Run one relevant QEMU/GDB session using known anonymous/private/shared and
  retained-writeback fixtures, including an inactive process, mapped/unmapped VA
  and independently read expected table entries. Check operation outcomes at
  their owning seam using existing relevant fixtures where possible.
- Stop when these checks pass. Repeat only after a new change, failed relevant
  check or concrete unresolved risk, recording the reason. Do not add repeated
  boots, unrelated suites or baseline investigations for additional confidence.
  Record incidental failures and investigate only if they block this ticket.

Remaining work is implementation, the bounded verification above, documentation
and owner review. No design decisions remain open. No historical ledger,
complete backing catalogue, payload capture, Request instrumentation, dump
mechanics or unrelated memory-management fixes are included. P1 is unchanged.

### 2026-10-20 — Execution authorized

The owner instructed “go”. Started the agreed implementation and moved this
ticket to in_progress. Existing workspace deletions under debug/tests and
generated bytecode changes are preserved; focused verification uses temporary
checks rather than restoring deleted files.

### 2026-10-20 — Ready for owner review

Implemented the agreed virtual-memory debug views and owner-side statistics.
Platform VMM publishes its kernel root, pending writeback, retained HHDM bounds,
paging-format contract and shared-resident accounting. Resource memory outcomes
and ElfMapper outcomes have their own records; process memory outcomes extend
LifecycleDebugStats without duplicate registration. Counters distinguish API
requests, completed operations, actual reclamation, brk no-ops/refusals and
writeback retry activity. No runtime ownership/locking policy was changed.

Added memory space/regions/backing/writeback/translate views, with explicit
kernel/process/CPU/typed-space selection, including inactive process slots.
Region and page relationships are bounded and deduplicated; private COW shared
pages remain visible even when absent from an object's page list. Backing refs,
page-region mapping counts, dirty semantics and opaque provider identities are
labeled. Process/task file views now show inode backing identity for correlation.
Translation validates the supported x86_64 four-level format and retained HHDM
access path, explains 4 KiB/2 MiB/1 GiB leaves and folded permissions, and never
calls the target or claims hardware TLB/CPU-control state.

Verification completed and stopped at the agreed scope:

- x86_64 debug and release image builds passed with no compiler warnings/errors.
  Build logs: /tmp/arx-virtual-debug-build.log and
  /tmp/arx-virtual-release-build.log.
- Ten focused host tests passed, including three page sizes, permissions,
  malformed/missing tables, HHDM/budget limits, inactive selection, malformed
  lists, shared-page/backing deduplication, retry state and parser/range limits.
  Harness: /tmp/arx-virtual-host.py. Rechecked the host fixtures after final
  shared-page and region-label changes.
- Focused preprocessing of changed runtime sites and release symbol inspection
  confirmed exclusion of added records/helpers/argument evaluation. Matching
  native-root headers compile. Harness: /tmp/arx-virtual-release-check.py.
- Focused four-CPU QEMU/GDB inspection passed:
  /tmp/arx-virtual-target-ej7tce9g/gdb.log and serial.log. Verified retained failed
  writeback, clone backing/pages, inactive process selection against running
  process records, known mapped/unmapped VA and independent expected physical
  address, memory/ELF/lifecycle counters and existing shared-writeback,
  protection and COW fixture results. Temporary harnesses are
  /tmp/arx-virtual-run.py and /tmp/arx-virtual-target.py.
- An earlier target harness assertion incorrectly treated stored CPU space
  pointers as definitive running-process ownership. Corrected the assertion to
  use RunningProcesses; the rerun passed. Output now explicitly labels CPU space
  associations as stored observations that may be stale. No baseline or
  unrelated-suite investigation was added.
- Reviewed changed code for readable formatting, release erasure, architecture
  helpers and layer boundaries. Updated GdbCommands.md, KernelArch.md, Resource,
  Logic, Dispatcher, Platform and x86_64 guides, plus the migration coverage
  ledger. Existing diagrams do not require changes. Whitespace checks passed.

Existing workspace deletions under debug/tests and generated bytecode changes
were preserved. Temporary verification files remain outside the repository;
this ticket did not restore deleted test tooling.

All acceptance criteria are met, with no required implementation or verification
remaining. Maintained shared-resident bytes and selected-space resident
observations have distinct meanings; neither is a complete physical-page census.
No global backing catalogue, retained history, payload capture, Request tracking
or dump mechanics was added. Legacy arx-vmm remains for the cutover ticket.
Status remains in progress pending owner authorization to close and move it.

### 2026-10-20 — Completion

The owner instructed “close” in response to the proposed closure and move to
`tickets/closed/`. Closed and moved the completed ticket. Virtual-memory regions,
backing/writeback, x86_64 page-table inspection, owner-side counters and relevant
documentation are complete; focused verification passed as recorded above.
