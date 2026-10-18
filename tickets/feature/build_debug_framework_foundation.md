# Build the debug-only kernel and GDB framework foundations

- Type: feature
- Priority: P1
- Status: open
- Required skill: mattpocock-skills:grilling

## Task prompt

Create the shared producer/consumer interface for the owner-approved x86_64 debug
framework. Existing commands independently discover objects, decode fields and
walk collections, making new inspection work repetitive and fragile. A developer
should be able to add a field to a subsystem-owned stats struct and inspect it
through generic GDB rendering without writing another command.

Follow [the ticket workflow](../TICKET.MD) and applicable repository skills. This
ticket starts open: complete its own grilling/analysis and execution gate before
implementation. Parent: [framework planning](../closed/plan_gdb_extension_tickets.md).
Source-backed rich-view requirements: [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment).

### Agreed scope and milestones

1. Create root `debug/kernel/` and `debug/gdb/`. Kernel files include
   `DebugManager.hpp`/`.cpp` and per-subsystem headers under `stats/`; GDB entry is
   `arx.py`, with common code under `framework/` and optional adapters under
   `views/`. Use controlled provider records to exercise the contract; real
   subsystem rollouts belong to the other feature tickets.
2. Store DebugManager in Dispatcher and expose a bounded named registry of record
   pointers/type identities and rich-view information. Managers own arbitrary
   custom stats structs: no universal field vocabulary, inheritance requirement
   or strict statistics layout. Include C platform records as well as C++ records.
   Production construction wiring registers one instance per manager/provider;
   isolated test instances must not replace production entries. Define duplicate,
   capacity, initialization, teardown and stale-record behavior.
3. Compile records, updates, registration, registry storage and framework work out
   under `DEBUG=0`. No release evaluation of update arguments, allocation, hidden
   branches or callbacks. Provide debug-only concurrent counter helpers and a
   way to identify interrupted selected grouped updates. Choose publication and
   ownership semantics that remain valid with multiple writers; do not hold a
   new framework lock in allocator/scheduler hot paths or promise a global snapshot.
4. GDB discovers `platform.dispacher` then DebugManager, uses the matching ELF's
   type information to render scalars/enums/nested structs/arrays, and leaves
   arbitrary pointer targets unexpanded by default. Define an explicit typed
   record/address inspection route for controlled fixtures without registering
   them as production managers. Report pre-Dispatcher, absent provider, missing
   symbols, release kernel and partial publication distinctly.
5. Supply bounded array, pointer-list, index-list, string, hash-bucket and graph
   reading helpers as justified by actual adapters. Enforce entry/byte/depth
   limits and cycle/invalid-index handling. Separate gathering from presentation
   so a view returns inspectable results/status and does not duplicate printing.
   Preserve source address, units/type, count semantics and truncation status.
6. Define the rich-view contract beyond scalar stats: typed roots, lengths or
   bounds, ordering/link/index rules, lifetime and interpretation. Optional GDB
   adapters can interpret a manager-specific collection. A future native dump
   reader must be able to recover equivalent information without Python/DWARF
   kernel reflection; specify the required native-reader inputs or descriptors.
   Select that minimal contract during analysis. Do not implement dump triggers,
   sinks, serialization, panic hooks or general-purpose reflection.
7. Start `docs/debugging/GdbCommands.md` with the contract, loading instructions,
   extension points and implemented commands. Per-provider tickets extend it;
   [cutover](migrate_debug_commands_and_document_framework.md) owns final completeness.

### Proposed interface and illustrative output

Settle the grammar in analysis and keep all providers on one family, for example:

```text
source debug/gdb/arx.py
arx-debug list
  pmm  PmmDebugStats  ready
  vfs  VfsDebugStats  unavailable: provider not initialized
arx-debug pmm stats
  allocationCalls: 120
  nodes[0].normal.freePages: 1024
  publication: complete; collection not a cross-provider snapshot
arx-debug vfs contents --limit 100
  <partial: unreadable root; entries=0>
```

The numbers are examples, not current kernel measurements. Consider a registry
format/version guard for mismatched readers, not a persistent public ABI. Reset
cached addresses/types on ELF/inferior changes. Do not retain stale map-file offsets.
Never invoke target methods, clock callbacks or inferior functions, including
through discovery expressions. If GDB language/settings are changed, restore
their actual previous setting. Inspect stopped targets; report a running target
as unavailable rather than silently stopping/resuming it.

### Sources, dependencies and open design points

- [Dispatcher.hpp](../../kernel/layers/Dispatcher.hpp),
  [Dispatcher.cpp](../../kernel/layers/Dispatcher.cpp),
  [kernel.cpp](../../kernel/kernel.cpp): construction and publication order.
- [platform.h](../../kernel/platform/platform.h),
  [ResourceLayerFactory.cpp](../../kernel/layers/Resource/ResourceLayerFactory.cpp),
  [LogicLayerFactory.cpp](../../kernel/layers/Logic/LogicLayerFactory.cpp): C roots,
  staged manager creation and partial availability.
- [Makefile](../../Makefile), [x86_64 linker](../../arch/x86_64/linker.ld): debug
  flags, source lists, symbol retention and C/C++ build constraints.
- [spinlock.h](../../klib/spinlock.h), [error_logger.h](../../klib/error_logger.h):
  locking limitations and existing bounded publication precedent.
- [legacy extension](../../scripts/gdb_extention.py): migration reference only;
  [legacy defects](../debug/resolve_legacy_gdb_discovery_defects.md) must not be copied.
- No implementation dependency. Remaining choices include by-value versus owned
  pointer storage, registry capacity, registration linkage, C-compatible helper
  syntax, type-name emission/retention, publication states and rich-view adapters.
  Route any necessary upward access through Dispatcher; prefer composition wiring
  over lower managers locating upper-layer objects. No Request-layer providers.

## Acceptance criteria

- [ ] A C record and a distinct C++ record register and render through the same small interface, without provider-specific commands or field tables.
- [ ] DebugManager and staged availability are discoverable through Dispatcher without inferior calls; duplicate/full/unavailable registrations have defined behavior.
- [ ] Debug-only helpers support correct concurrent counters and explicitly report interrupted selected grouped updates; collection reads never acquire target locks or retry forever.
- [ ] Bounded readers report cycles, invalid indices/lengths, unreadable data and truncation; scalar stats and rich roots have documented lifetime and future-native-reader semantics.
- [ ] `DEBUG=0` contains no framework/record storage or update/registration work, including argument side effects; debug symbols/types required for `DEBUG=1` remain inspectable.
- [ ] Missing/mismatched ELF types, new inferiors and pre-Dispatcher stops fail clearly without stale pointers or GDB setting changes.
- [ ] The guide documents only verified behavior and links the authoring interface; no old-command removal, Request support or dump mechanics are included.

## Verification approach

Exercise the public registry/GDB interface with two different record types and
controlled valid, null, cyclic, invalid-index and interrupted-publication data.
Use meaningful concurrent update checks and a side-effectful update expression
that must not run in release. Build x86_64 debug and release separately; inspect
symbols, sizes and relevant disassembly to demonstrate removal, not merely a
macro's spelling. In headless QEMU/GDB, stop before Dispatcher, during staged
registration and after initialization; inspect without executing target code.
Change the loaded ELF/inferior and verify cache invalidation. Record exact commands
and limitations. Future provider/runtime checks do not need to be duplicated here.

## Work log

### 2026-10-18 — Created

P1: every provider and cutover depends on the shared interface and release-mode
guarantees. Scope follows the owner's confirmed combined kernel/GDB foundation
workload. Analysis and implementation have not started.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.
