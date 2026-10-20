# Add physical allocation, NUMA, heap and DMA debug views

- Type: feature
- Priority: P1
- Status: closed
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
Depends on [foundations](../closed/build_debug_framework_foundation.md). This ticket is
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
   `arx-pmm` and `arx-heap` information for [cutover](../feature/migrate_debug_commands_and_document_framework.md).

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

- [x] Maintained stats cover agreed allocator operations and current state with explicit units/rounding/success semantics, and compile out in release.
- [x] Unique NUMA nodes and ordinary/DMA zones, physical ranges, buddy orders and selected PFNs are inspectable without unlimited scans.
- [x] Heap cache/slab totals and selected-address/bitmap ownership can be inspected; unavailable provenance is not invented.
- [x] DMA records expose distinct CPU/physical/device addresses, constraints, backend and retained/releasing states without executing hooks.
- [x] Old PMM/heap information is mapped to new views; malformed/partial data returns bounded status, not hangs or false completeness.
- [x] Each rich collection exposes native-reader lifetime/bounds/semantics; guide and migration coverage ledger are updated.

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

### 2026-10-20 — Analysis started

Read `tickets/TICKET.MD`, the required `mattpocock-skills:grilling` skill,
and repository code/documentation skills. Analysis is underway; status remains
open pending the design conversation and owner confirmation. No implementation
has started.

Source inspection established these design constraints:

- `platform.numa_nodes[]` is the authoritative unique-node inventory, bounded
  by `MAX_NUMA_NODES`. CPU links are associations, not additional nodes.
- Normal and DMA zones share absolute-PFN metadata. Owned region membership,
  rather than broad min/max PFN bounds, must validate selected pages. Interior
  page metadata can be stale and does not establish allocation-head ownership.
- PMM fallback attempts across nodes differ from public allocation requests.
  PMM retains allocation order and heap retains size class, but neither retains
  original request sizes for exact live requested-byte accounting.
- Heap native list names differ from occupancy: `full_slabs` holds entirely
  free slabs; allocation does not currently move exhausted partial slabs to
  `empty_slabs`. Views must distinguish list membership from derived occupancy.
- DMA rollback can retain a tracked id-only cleanup handle after an allocation
  failure. This is retained backing, not a successful allocation. Effective
  alignment/boundary values are currently local variables and need debug-only
  retention to support later constraint inspection.

First design round will settle counter fidelity/storage cost, whether allocator
behavior fixes remain outside scope, and DMA diagnostic state retention. Command
selectors, early-root behavior, bounds and verification will be finalized from
those decisions before recording the analysis and execution plan. The ticket
index was checked; its existing open/P1 entry remains accurate.

### 2026-10-20 — First design round agreed

Owner accepted all three recommendations and emphasized human readability:

- Maintain operation/outcome counters, separate zone attempts from whole PMM
  requests, and distinguish live blocks/pages/rounded heap bytes. Retain
  cumulative requested bytes without adding per-allocation storage for exact
  live PMM/heap requested bytes.
- Expose actual allocator state without changing allocator behavior. Report
  discovered defects separately; distinguish native heap list names from
  occupancy derived from metadata.
- Retain debug-only DMA effective constraints, backend identity, cleanup phase
  and last cleanup result. Distinguish successful buffers from failed allocation
  cleanup handles and retained backing.

Human-readable output is a design requirement: descriptive state labels,
explicit units, short summaries before optional detail, and clear explanations
of unavailable or partial results. Inspection and command choices remain for
the next design round; implementation is not authorized yet.

### 2026-10-20 — Analysis and plan; owner design confirmation

Owner answered “yes to all” in both rounds, confirming the shared design below,
and explicitly requested human readability. Analysis is complete. Execution
awaits the separate instruction required by `tickets/TICKET.MD`; the ticket
remains in `tickets/feature/` until that instruction arrives.

#### Confirmed counters and ownership

- Platform owns early PMM/heap records and readiness state; initialize tracking
  before allocator activity and publish existing records when DebugManager is
  available. Use platform-owned C-compatible headers under `debug/kernel/stats/`
  and composition-owned publication, not a platform dependency on Resource or
  DebugManager. DmaManager owns its counters and allocation diagnostic fields.
- Count public PMM normal/DMA requests and terminal success/failure separately
  from each zone's attempts/successes/failures, since fallback can try multiple
  zones. Count accepted block frees and current allocated blocks explicitly.
  Report existing total/free/used pages separately, including initialization
  reservations rather than treating every used page as a public allocation.
- Count heap public allocation attempts/success/failure and free calls versus
  actual object releases; null, unknown or already-free pointers must not inflate
  successful frees. Maintain current objects and class-rounded live bytes,
  with per-cache operation/usage totals and slab/backing/metadata state.
- Accumulate successful requested bytes and rounded committed bytes for PMM and
  heap. Exact live requested bytes are unavailable because original sizes are
  not retained; do not add a per-allocation index. State units, rounding and
  counter reset/lifetime semantics in headers and the guide.
- DMA public Allocate/Free calls and outcomes distinguish invalid, unsupported,
  no-memory, mapping failure and busy outcomes. Allocation success means a valid
  buffer was returned. Track rollback-retained handles separately from successful
  buffers, release successes and failures, and current retained record/requested/
  mapped/reserved backing quantities. Cleanup-only releases are explicit.
- Retain debug-only requested/effective alignment and boundary, backend identity,
  allocation provenance, cleanup phase and last cleanup result in DMA records.
  A releasing record can remain linked after backing is returned: record the
  phase and update backing counters at actual ownership transitions. Backend
  identity includes a descriptive identity when known and opaque stored operation/
  context addresses; custom backends remain explicitly custom/unknown.
- Mark relevant update groups around real mutations and honor existing locking.
  Active writers mean partial/interrupted state. No coherent cross-provider
  snapshot or stronger heap synchronization guarantee is implied.
- Guard all added storage, types, registration, helpers and update expressions
  under `DEBUG`; `DEBUG=0` must add no tracking work or argument evaluation.

#### Confirmed commands and human-readable behavior

- Add `arx-debug pmm nodes`, `pmm zones`, `pmm orders`, `pmm blocks`, `pmm pfn`,
  `heap caches`, `heap slabs`, `heap address` and `dma allocations`, alongside
  each provider's generic `stats`. PFN and address views accept one numeric
  positional value; expressions and target calls remain forbidden.
- Use explicit `--node <index>`, `--zone normal|dma`, `--order <order>` and
  `--class <object-size-bytes>` selectors where applicable. Preserve explicit
  CPU association selection through existing `--cpu <logical-id>` or
  `--slot <index>` conventions, without legacy ID/slot ambiguity. Reject
  incompatible selectors and invalid values instead of silently ignoring them.
- Enumerate unique nodes from `platform.numa_nodes[]`, bounded by actual
  `MAX_NUMA_NODES` capacity; show proximity domains and CPU associations.
  Validate owned physical ranges for ordinary and DMA zones, and preserve
  region/page/byte/PFN/HHDM and observed lock information from legacy commands.
- Show compact summaries with explicit units, descriptive states, per-order
  availability and largest available block size. Maintain bounded per-order
  block counts at list update sites so summaries need no unbounded list scan;
  list-derived validation remains optional. Low free contiguous capacity is
  evidence of fragmentation, not proof of the sole cause of a request failure.
- PFN inspection validates global metadata allocation bounds and zone-owned
  regions before dereferencing. Distinguish raw metadata, validated heads and
  unknown allocation ownership; interior metadata may be stale. Do not infer
  block ownership solely from a stored flag/order.
- Heap views expose native list membership separately from occupancy derived
  from free/total object counts. Include metadata-pool shape/chunks and object
  array/bitmap locations. Address lookup reports containing slab, slot, offset,
  allocation bit and object-start versus interior-address status. It cannot
  claim object type, original request size, provenance or allocation stack.
- Full bitmap/count checks and long block/slab walks are opt-in and share entry,
  byte and depth ceilings. Add an explicit bitmap-check option for slab detail.
  Detect cycles, invalid counts/links, arithmetic overflow and budget exhaustion;
  an incomplete search cannot establish that an address is unowned.
- DMA output distinguishes CPU/backing, physical and device addresses; allocation
  ID, device constraints, requested/reserved/mapped sizes, cookie, backend and
  cleanup states remain readable even for id-only rollback handles. Evaluate
  stored constraints without backend calls. No IOMMU tables are implied.
- Default rich views lead with short summaries before optional detail. Explain
  partial/unavailable results in plain language and label units and uncertainty.
  Preserve framework result/presentation separation; avoid unrelated renderer
  rewrites. Before registry publication, status/typed inspection can expose early
  records; normal provider commands report unavailable rather than invent roots.

#### Implementation steps and affected code

1. Read the affected Platform, Resource and Dispatcher architecture sections and
   `kernel.cpp` before implementation. Add owner-specific stats/native-contract
   headers and conditional storage with early readiness and real capacities.
2. Instrument `pmm.c`, `heap.c` and necessary metadata backing transitions at
   real successful/failed mutation sites. Record global absolute-PFN metadata
   capacity explicitly; zone maxima do not bound this shared allocation.
   Preserve functional allocator behavior and existing allocation/free APIs.
3. Add DMA diagnostics in `DmaManager.hpp`/`.cpp` and publication through existing
   Dispatcher/Resource composition points. Do not call backends from readers or
   introduce direct upward manager dependencies.
4. Add PMM/heap/DMA adapters under `debug/gdb/views/`, register semantic versions,
   and extend the command parser for numeric arguments, applicable selectors and
   optional bitmap checks. Reuse shared readers, budgets, ELF/stopped-target
   validation and partial-result handling.
5. Define each native-reader contract beside its owning layout: actual root and
   capacity, count meaning, ordering, sentinels, ownership, ephemeral versus
   kernel-lifetime addresses, ID/slot reuse and partial-update interpretation.
   DMA records are ephemeral and newest-tracked-first; global IDs are not reused.
6. Update `docs/debugging/GdbCommands.md`, affected existing architecture guides
   and the migration ticket's PMM/heap coverage ledger. Preserve legacy commands
   until their separate cutover ticket. Review all changes against readable
   `kernel.cpp` styling and the repository documentation skills.
7. Report allocator defects separately with source evidence, without behavioral
   fixes in this ticket: misleading/incomplete heap list transitions and possible
   partial `slab_init` backing retention. Do not claim list totals account for
   every retained backing allocation. No Request providers or dump mechanics.

#### Verification, risks and remaining work

- Extend platform memory and PhysicalMemoryManager tests with before/after
  request, attempt, success/free, block/page and rounding checks, including
  fragmentation, fallback and heap object reuse. Add DMA counter/phase assertions
  to `resource/DmaManager/failure_rollback` and other applicable existing cases.
- Exercise bounded adapters/parser with controlled malformed roots, cycles,
  invalid ranges/PFNs, bitmap/count disagreement, missing early state, invalid
  selectors and interrupted groups. Corruption fixtures must not mutate the
  production allocator. Confirm human-readable output and explicit truncation.
- Stop x86_64 QEMU/GDB before Dispatcher and in populated allocator/DMA fixture
  phases; use one and multiple CPUs to verify unique nodes. Use controlled extra
  node records for topology not available in the machine. Compare stored fields
  and counters with known allocation/free/rollback states and legacy coverage.
- Build separate debug/release outputs, verify debug types and registration,
  and inspect release symbols/layout and changed update-site disassembly for
  exclusion, including an argument-side-effect erasure check where applicable.
  Reuse applicable foundation checks present in the checkout; historical tickets
  reference `debug/tests/`, which is absent here, so restore required meaningful
  coverage as part of verification rather than claiming those checks ran.
- Real IOMMU translation, unsupported architecture/backend behavior and physical
  multi-node hardware are not established by controlled fixtures. Clearly state
  what was exercised. Shared page metadata, heap caller synchronization and
  freeing DMA backing before record unlink are the principal interpretation risks.

No builds, runtime tests or implementation changes were performed during
analysis. The ticket/index metadata and whitespace will be checked before handoff.
No owner design decisions remain open; implementation and verification remain.

### 2026-10-20 — Execution authorized

Owner instructed “go” after analysis. Moved to `tickets/in_progress/` and began
implementing the agreed plan. Human-readable summaries remain a requirement.

### 2026-10-20 — Ready for owner review

Implemented the agreed PMM/NUMA, heap and DMA providers, maintained counters,
release exclusion and bounded GDB adapters. New commands provide readable node/
zone/order summaries, selected-PFN interpretation, cache/slab/address inspection,
optional bitmap consistency checks, and DMA address/constraint/cleanup detail.
Shared nodes are enumerated once. Rich native-reader contracts are defined in
owning stats headers; DmaManager.hpp exposes the allocation-record layout alias.
Legacy commands remain available for their separate cutover ticket.

Updated GdbCommands.md, KernelArch.md, Platform.md, Resource.md and Dispatcher.md,
and recorded PMM/heap parity evidence in the migration ticket's ledger. README
already links the command guide. Moved-ticket references and the current index
were corrected. Human-readable summaries, explicit units, native-list versus
occupancy distinctions and partial/unavailable explanations are retained.

Verification completed:

- Separate x86_64 debug and release ELF/image builds passed using
  `make -j4 DEBUG=1 BUILD_DIR=build/memory-debug BIN_DIR=bin/memory-debug bin/memory-debug/arx-x86_64.img`
  and the equivalent `DEBUG=0` / `memory-release` paths.
- `PYTHONDONTWRITEBYTECODE=1 python3 debug/tests/check_memory_views.py`: 15 host
  tests passed. Coverage includes four CPUs sharing one node, a controlled second
  node, overlapping ownership, invalid selectors/PFNs/links, stale interior-page
  metadata, cycles, interrupted groups, metadata bounds, heap address/bitmap
  interpretation, DMA retained/transition states and budget truncation.
- `PYTHONDONTWRITEBYTECODE=1 python3 debug/tests/check_memory_release.py`: passed
  preprocessor exclusion at all changed runtime areas, release symbol/disassembly
  checks, baseline-versus-current release layouts, argument-side-effect erasure
  and compilation of the native DMA layout alias.
- `python3 debug/tests/run_target_checks.py --cpus 1`: passed. Logs:
  `/tmp/arx-memory-debug-1-czbx8c3w/`.
- `python3 debug/tests/run_target_checks.py --cpus 4`: passed. Logs:
  `/tmp/arx-memory-debug-4-xv3amwjf/`.
- `python3 debug/tests/run_target_checks.py --release --cpus 4`: passed; matching
  release guest reports the framework compiled out. Logs:
  `/tmp/arx-memory-release-4-d9d5jfca/`.
- Debug target runs stop before Dispatcher, before Resource tests, at a retained
  rollback, inside an active unmap, and after Resource tests. Real zone/order,
  slab/pool/bitmap and selected-address reads passed. Existing foundation status,
  registry listing, repeated sourcing and C stats remain usable. Counter tests
  pass for platform physical/heap allocation, PMM request/block/page rounding and
  per-order list agreement, and DMA rollback/retry/busy/cleanup accounting.
  No kernel self-test failures were observed before the final stop.
- Final host/release checks and `git diff --check` passed. Documentation links
  and ticket inventory were verified. Generated bytecode changes were removed;
  build outputs remain available under `build/memory-*` and `bin/memory-*`.

The first GDB attempt had a test-script symbol-lookup error, and a later run
exposed use of Publish for previously undeclared provider names. Both were
corrected; final runs use Register for new providers and pass. No target calls,
allocator locks or buffer payload reads are performed by the inspection commands.

Separate allocator observations (behavior intentionally unchanged):

- Native heap full/empty names represent availability, and exhausted slabs can
  remain on partial lists. Readers report native membership and derived occupancy
  separately instead of claiming the lists are occupancy classifications.
- `slab_init` can retain the successful half of a payload/bitmap allocation pair
  if the other allocation fails. New counters expose retained initialization
  bytes, but this failure was established by source inspection, not forced in the
  live allocator. Repair remains outside this debug-view ticket.

Real multi-node hardware, IOMMU translation and unsupported architecture/backend
behavior were not exercised; controlled extra-node and DMA mock evidence is
explicitly separate from production direct-address DMA checks. No instrumentation
or ticket-workflow expansion is planned. All acceptance criteria are met; no
implementation or required verification remains. Status stays in progress pending
owner review and explicit authorization to close and move to the chosen destination.

### 2026-10-20 — Completion

Owner replied “close” to the proposal to close this ticket and move it to
`tickets/closed/`, authorizing closure at that destination. Completed readable
PMM/NUMA, heap and DMA views, maintained debug-only counters, bounded inspection,
native-reader contracts and documentation; verification evidence is recorded above.
