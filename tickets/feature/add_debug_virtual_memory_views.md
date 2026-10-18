# Add virtual-memory ownership, backing and x86_64 translation debug views

- Type: feature
- Priority: P1
- Status: open
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
[foundations](build_debug_framework_foundation.md); use process identities from
[process/resource views](add_debug_process_resource_views.md). Related:
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
   preserve all useful old VMM output for [cutover](migrate_debug_commands_and_document_framework.md).

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

- [ ] Debug-only maintained memory stats and roots expose kernel/CPU/process spaces, including inactive processes and pending writeback.
- [ ] Regions, protection limits, backing/shared pages and observed ownership links are readable with explicit completeness and deduplication.
- [ ] Selected x86_64 translations explain intermediate/leaf permissions and page size without inferior execution or claiming actual TLB contents.
- [ ] Unknown/inaccessible roots, invalid/noncanonical addresses, cycles, excessive lengths and partial groups terminate with useful bounded status.
- [ ] Useful old VMM information remains mapped to the new interface; roots/lifetime/translation semantics support a future native reader.
- [ ] Guide, migration ledger and relevant architecture descriptions reflect verified behavior; release has no additional debug records/update work.

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

## Work log

### 2026-10-18 — Created

P1: mapping/backing and translation views share the same address/permission and
retention investigation workload. No implementation has started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
