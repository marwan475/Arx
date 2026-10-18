# Add physical allocation, NUMA, heap and DMA debug views

- Type: feature
- Priority: P1
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Expose allocator statistics and rich allocation metadata through the new
DebugManager framework. Diagnose contiguous allocation failure despite free
pages, NUMA/DMA-zone imbalance, invalid heap addresses and DMA backing retained
after cleanup. Today these questions require walking zones, slab lists and DMA
records separately; old PMM/heap commands repeat per-CPU NUMA data and omit useful
DMA-zone/address details.

Follow [the workflow](../TICKET.MD), required grilling and repository code skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](build_debug_framework_foundation.md). This ticket is
open and does not authorize implementation. See the [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment).

### Scope and internal milestones

1. Define manager/provider-owned headers such as `PmmDebugStats.hpp`,
   `HeapDebugStats.hpp` and `DmaDebugStats.hpp` in `debug/kernel/stats/`. Maintain
   explicitly defined allocation/free/failure and current-usage statistics at
   real update sites. Distinguish pages, bytes, blocks, requested versus rounded
   sizes, and successful allocations versus rollback handles. Settle precise
   counters in analysis; do not equate used pages with allocated block count.
2. Track platform PMM/heap state from early initialization, then register its
   records when DebugManager exists. The C++ PMM manager is a facade, not the
   owner of all allocator storage. Expose NUMA-node identities/CPU association,
   ordinary and DMA zones, physical ranges, per-order buddy lists/metadata and
   HHDM interpretation. Do not duplicate a node for every CPU that references it.
3. Add per-node/zone/order summaries and bounded selected-PFN inspection. Explain
   fragmentation/largest available orders and validate zone membership; normal
   and DMA zones share metadata but have disjoint owned ranges/lists.
4. Expose per-node heap size caches, partial/full/empty slab lists, metadata pools,
   object arrays and allocation bitmaps. Add selected-address lookup and bounded
   bitmap/count consistency detail. No allocation stack traces or object-type
   ownership can be claimed from existing metadata.
5. Expose DmaManager live/releasing allocation list and backend identity, including
   CPU/physical/device addresses, IDs, device mask/alignment/boundary, requested/
   reserved/mapped sizes, cookie and retained rollback/cleanup state. Evaluate
   constraints from stored values without calling backend hooks. Do not imply
   an IOMMU translation table exists.
6. Add adapters under `debug/gdb/views/`, native-reader root/bound/ordering
   descriptions and updates to `docs/debugging/GdbCommands.md`. Preserve old
   `arx-pmm` and `arx-heap` information for [cutover](migrate_debug_commands_and_document_framework.md).

### Proposed interface and illustrative output

```text
arx-debug pmm nodes
  node=0 domain=0 cpus=2 normal.freePages=1024 dma.freePages=128
arx-debug pmm blocks --node 0 --zone dma --order 3 --limit 20
  order=3 blocks=4 pages=32 complete=true
arx-debug heap address 0xffff800000100000
  node=0 class=64 slab=<addr> slot=3 allocated=true
arx-debug dma allocations --limit 20
  id=7 cpu=<addr> physical=<pa> device=<dma> releasing=true
```

Grammar follows the foundation; example numbers are illustrative. Expensive PFN,
bitmap or list enumeration is opt-in and capped by bytes/entries. Missing early
boot roots, corrupt links, invalid addresses and interrupted counter groups must
produce partial/unavailable results. No inferior calls, target locks or payload
dump by default. All tracking/storage/update work is excluded under `DEBUG=0`.
Request providers and dump mechanics are excluded.

### Sources and design unknowns

- [pmm.h](../../kernel/platform/memory/pmm.h), [pmm.c](../../kernel/platform/memory/pmm.c),
  [platform.h](../../kernel/platform/platform.h): zones, pages, NUMA roots and updates.
- [heap.h](../../kernel/platform/memory/heap.h), [heap.c](../../kernel/platform/memory/heap.c),
  [metadata.h](../../kernel/platform/memory/metadata.h): caches/slabs/bitmaps.
- [PhysicalMemoryManager.hpp](../../kernel/layers/Resource/PhysicalMemoryManager.hpp),
  [DmaManager.cpp](../../kernel/layers/Resource/DmaManager.cpp): facade and `AllocationRecord`.
- [dma.c](../../kernel/platform/dma/dma.c): backend availability and address semantics.
- Context: [platform documentation ticket](../closed/document_platform_layer.md).
- Resolve per-zone update grouping, allocation-block counting semantics, debug
  storage placement, safe metadata bounds and DMA rollback identity in analysis.
  Registration must not introduce a direct upward manager dependency.

## Acceptance criteria

- [ ] Maintained stats cover agreed allocator operations and current state with explicit units/rounding/success semantics, and compile out in release.
- [ ] Unique NUMA nodes and ordinary/DMA zones, physical ranges, buddy orders and selected PFNs are inspectable without unlimited scans.
- [ ] Heap cache/slab totals and selected-address/bitmap ownership can be inspected; unavailable provenance is not invented.
- [ ] DMA records expose distinct CPU/physical/device addresses, constraints, backend and retained/releasing states without executing hooks.
- [ ] Old PMM/heap information is mapped to new views; malformed/partial data returns bounded status, not hangs or false completeness.
- [ ] Each rich collection exposes native-reader lifetime/bounds/semantics; guide and migration coverage ledger are updated.

## Verification approach

Use [platform memory tests](../../kernel/selftests/platform/memorytests.c),
[PhysicalMemoryManagerTests](../../kernel/selftests/resource/PhysicalMemoryManagerTests.cpp)
and [DmaManagerTests](../../kernel/selftests/resource/DmaManagerTests.cpp) to compare
maintained before/after counts with known allocation/free, fragmentation and
rollback state. Stop x86_64 QEMU in relevant fixture phases, including early boot
and populated NUMA/DMA/heap state. A single-node machine must not manufacture
multiple nodes; use controlled records for additional topology. Exercise corrupt
lists/bitmaps and invalid PFNs through safe fixtures, not a production allocator.
Run applicable foundation release-exclusion checks for added update sites. State
any unexercised backend/hardware behavior explicitly.

## Work log

### 2026-10-18 — Created

P1: allocator visibility is foundational for boot and memory-failure diagnosis.
Combines PMM, heap and DMA work sharing allocation/address-lifetime context.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
