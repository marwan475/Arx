# Build the debug-only kernel and GDB framework foundations

- Type: feature
- Priority: P1
- Status: closed
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
   [cutover](../feature/migrate_debug_commands_and_document_framework.md) owns final completeness.

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

- [x] A C record and a distinct C++ record register and render through the same small interface, without provider-specific commands or field tables.
- [x] DebugManager and staged availability are discoverable through Dispatcher without inferior calls; duplicate/full/unavailable registrations have defined behavior.
- [x] Debug-only helpers support correct concurrent counters and explicitly report interrupted selected grouped updates; collection reads never acquire target locks or retry forever.
- [x] Bounded readers report cycles, invalid indices/lengths, unreadable data and truncation; scalar stats and rich roots have documented lifetime and future-native-reader semantics.
- [x] `DEBUG=0` contains no DebugManager member, initialization/destruction, framework/record storage or update/registration work, including argument side effects, runtime checks or callbacks; debug symbols/types required for `DEBUG=1` remain inspectable.
- [x] Missing/mismatched ELF types, new inferiors and pre-Dispatcher stops fail clearly without stale pointers or GDB setting changes.
- [x] The guide documents only verified behavior and links the authoring interface; no old-command removal, Request support or dump mechanics are included.

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

### 2026-10-18 — Foundation analysis started

Read the ticket workflow, required `mattpocock-skills:grilling`, repository
`add-arx-code`, `update-kernel-docs` and `write-kernel-docs` skills, and the
parent's confirmed design. Source inspection confirms that `kmain` publishes
`platform.dispacher` after Dispatcher construction and before `StartKernel`
creates the layer managers. The existing build passes `DEBUG` to both C and C++,
uses `-O0 -ggdb3` for debug builds, and explicitly lists common kernel sources;
foundation source inclusion and storage must therefore be conditional.

Analysis will settle registry ownership/capacity/lifetime, multiwriter grouped
update semantics, the minimum future-native-reader contract, command grammar,
and controlled verification fixtures. A read-only fact-finding agent is checking
source constraints as required by the grilling skill. GDB, x86_64 QEMU, GCC,
G++ and NASM are present; no build or runtime verification has run.

Status remains `open` while owner design answers and shared-understanding
confirmation are pending. The current ticket index was checked and retains this
ticket once in Feature with matching metadata. No implementation is authorized
or started.

### 2026-10-18 — Analysis and plan; owner design confirmation

The owner answered yes to all four design recommendations and emphasized that
DebugManager must never be initialized or updated in release because of tracking
cost. This confirms shared understanding of the design below. Analysis is
complete; status is `analyzed`, awaiting the separate execution instruction
required by the ticket workflow. No implementation has started.

#### Confirmed interface and ownership

- Embed DebugManager by value in Dispatcher only under `#if DEBUG`, with 64
  fixed registry slots. Duplicate names and capacity exhaustion return explicit
  errors; neither replaces a production registration. Publish initialized slot
  metadata with release ordering and distinguish reserved/unpublished entries.
  Withdrawal invalidates the record before its storage is reclaimed. Retain the
  immutable identity as a tombstone; never reuse withdrawn slots or names in the
  same registry lifetime. Registration/withdrawal belong to serialized bootstrap
  or quiescent composition wiring, rather than allocator/scheduler update paths.
- Managers own their custom C/C++ records; production wiring publishes them only
  after construction. Ordinary constructors and isolated test instances do not
  register globally. Dispatcher registry teardown invalidates borrowed pointers
  before provider destruction. There is no concurrent kernel-shutdown protocol.
- Provide atomic counters and optional active-writer markers for selected groups.
  Begin/end ordering brackets updates; a nonzero marker reports interrupted or
  in-progress updates even with multiple overlapping writers. Zero is not a
  coherent snapshot guarantee. Providers still synchronize shared fields using
  atomics or their existing synchronization. Inspection never acquires target
  locks, retries indefinitely, or introduces a framework lock in hot paths.
- Native-reader contract: versioned view identity, typed roots, bounds/count
  semantics, ordering, links/indices, ownership/lifetime and interpretation.
  A future compiled per-view reader needs matching build/layout headers, target
  address model and architecture, memory access, roots/bounds and semantic
  version. Python is not the sole description of these rules. General reflection
  and all dump execution/serialization remain excluded.
- Commands: `arx-debug list`, `arx-debug <provider> stats`,
  `arx-debug <provider> <view>` and `arx-debug inspect <type> <address>`, with
  shared entry/byte/depth limits. The explicit address route accepts a numeric
  address, not arbitrary expressions that could call inferior functions.

#### Implementation steps and affected code

1. Add C-compatible record/publication helpers in `debug/kernel/`, DebugManager
   and controlled C/C++ stats headers under `stats/`. Define registry format and
   record/view version validation, publication states and withdrawal behavior.
2. Conditionally include debug sources in Makefile and guard Dispatcher includes,
   member declaration, initialization/destruction and registration wiring with
   preprocessing. Release helper macros discard their entire arguments; do not
   implement release erasure as empty functions or runtime `if (DEBUG)` checks.
   Guard provider storage and fixture declarations as well, so release contains
   no manager size overhead, constructor work, updates, branches or callbacks.
3. Publish controlled records at staged Dispatcher startup points to exercise
   early/partial/ready discovery without adding real subsystem instrumentation.
   Keep test data separate from production provider identities. Use isolated
   registries for duplicate/full/withdrawal checks; fixtures cannot replace
   production entries.
4. Implement `debug/gdb/arx.py`, common discovery/rendering/bounded readers under
   `framework/` and representative fixture adapters under `views/`. Use typed
   symbol/field reads through `platform.dispacher`, never getters or map offsets.
   Initially avoid persistent target-address/type caches. Enforce stopped-target
   reads and report pre-Dispatcher, release, missing/mismatched types, missing
   providers, unreadable memory and partial publication distinctly.
5. Separate reader results/status from presentation. Preserve addresses, types,
   units and count/truncation semantics. Exercise arrays, strings, pointer/index
   lists, buckets and bounded graphs with controlled adapters grounded in the
   selected provider collection shapes.
6. Start `docs/debugging/GdbCommands.md` for developers and agents: loading,
   verified commands, record/helper authoring, publication/lifetime constraints,
   reader results and native-reader inputs. Use concise prose and examples;
   no diagram is needed for this initial interface reference. Link it from README
   and update affected Dispatcher/architecture descriptions. Existing launch
   integration and legacy-command removal remain the cutover ticket's work.

#### Verification, risks and remaining unknowns

- Exercise C and C++ records through the same registry and generic renderer;
  verify actual emitted DWARF rather than assuming type-name strings retain it.
- Test duplicate/full/withdrawn/unavailable states, multiple concurrent counter
  writers and overlapping grouped updates, plus controlled null, cyclic,
  invalid-index/length, unreadable and truncated collections. Host checks may
  exercise reusable helpers; QEMU/GDB checks must cover real target discovery.
- Build x86_64 debug/release into separate output directories. Inspect release
  symbols, Dispatcher layout/size and relevant constructor/update disassembly.
  A side-effectful helper argument must never run in release. Check the source
  inclusion list too: binary inspection alone is insufficient proof of erasure.
- In headless QEMU/GDB stop before Dispatcher, at staged registration and after
  initialization. Verify inspection without target calls or setting changes,
  running-target refusal, ELF/inferior replacement and clear mismatch handling.
  Record exact commands/results; tool presence is not runtime verification.
- Main risks are incomplete type emission, misleading snapshot claims, stale
  roots during provider teardown and accidental release argument evaluation.
  Publication tests, lifetime/version checks and release binary checks directly
  address these. Known upstream legacy discovery defects must not be copied.
- No unresolved owner design choices remain. Build/boot prerequisites and
  feasibility of individual runtime stop checks still require execution-time
  verification. Required failures or unavailable checks keep the ticket in
  progress rather than qualifying it for closure.

### 2026-10-18 — Execution authorized

The owner clarified that managers update maintained counters/stats during normal
runtime in debug builds, then explicitly instructed “ok execute”. Execution of
the recorded plan has started. Moved this ticket to `tickets/in_progress/`;
release builds must erase all storage, initialization and update work.

### 2026-10-18 — Ready for owner review

Implemented the recorded foundation plan. DebugManager is a debug-only by-value
Dispatcher member with 64 bounded lifecycle slots, explicit unavailable/publishing/
ready/withdrawn states, owner-checked withdrawal and no slot/name reuse. Shared
C-compatible helpers erase storage/update expressions in release and support
atomic counters plus multiwriter active-group markers. Registry mutations are
serialized bootstrap/quiescent operations, not hot-path locks. Dispatcher
invalidates registrations before provider teardown.

Controlled C and C++ providers exercise staged publication and runtime updates
from scheduled post-init entries. The generic GDB command family renders fields
from matching ELF types without provider field tables, follows no pointer targets
by default and offers explicit numeric-address typed inspection. Shared bounded
collection readers return results/status before presentation; optional rich
adapters register semantic ID/version/root type independently of command parsing.
Readers verify the target GNU build ID, check registry/record/view layouts,
retain no target caches, preserve GDB language settings and refuse running targets.

Added `docs/debugging/GdbCommands.md` with loading, verified commands, limits,
publication/lifetime rules, the stats/helper authoring interface and future
compiled-native-reader inputs. Updated README navigation, KernelArch.md and
Dispatcher.md using repository documentation guidance. No diagram change was
needed for this interface addition. The guide labels fixtures as synthetic and
leaves production subsystem rollout, legacy command removal/launcher defaults
and dump mechanics to their existing scopes.

#### Verification evidence

- Built separate x86_64 debug and release ELFs/images successfully:
  `make -j4 DEBUG=1 BUILD_DIR=build/framework-debug BIN_DIR=bin/framework-debug bin/framework-debug/arx-x86_64.img`
  and `make -j4 DEBUG=0 BUILD_DIR=build/framework-release BIN_DIR=bin/framework-release bin/framework-release/arx-x86_64.img`.
  Build logs are `/tmp/arx-framework-debug-image.log` and
  `/tmp/arx-framework-release-image.log`.
- Historical host checks: all five tests passed, covering valid/null roots, array lengths, pointer
  and index cycles, unreadable nodes, invalid indices, entry/byte/depth limits,
  unterminated strings and bounded empty hash buckets.
- Historical registry/concurrency checks passed duplicate/full/unavailable/publish/
  withdrawal/tombstone/invalidation checks and four concurrent writers totaling
  400,000 updates. An even count of four simultaneously active writers remains
  marked in progress; completion yields zero active and four completed groups.
- Historical release-erasure probes ran with both GCC and G++ using
  `-DDEBUG=0 -I. -O2 -Wall -Wextra`: both passed without evaluating side effects.
  Probe disassembly contains only a load and return, no calls/updates/locks;
  neither probe binary contains debug record/helper symbols.
- Historical release-layout compilation: the static assertion passed and `nm -S` reports a 24-byte Dispatcher probe (exactly three pointers).
  Debug DWARF reports Dispatcher=16,432 and DebugManager=16,408 bytes, with all
  registry storage absent in release.
- `nm -C bin/framework-release/kernel-x86_64.elf` contains no DebugManager,
  DebugFoundation, arx_foundation or arx_debug symbols. Release Dispatcher
  constructor disassembly only allocates/constructs its three factories and
  writes their pointers. The `make -pn DEBUG=0` common source list excludes
  every `debug/kernel/` implementation file.
- Historical target checks passed real x86_64 QEMU/GDB stops
  before Dispatcher, with unavailable C/C++ providers, after C publication,
  after C++ publication and inside an interrupted runtime grouped update.
  Both records render through the same interface; all six collection adapters
  complete on valid fixtures. Controlled fixture mutations verify cycles,
  invalid indices/lengths, unreadable pointers, null lists, limits and partial
  registry state. Explicit typed inspection, missing types, numeric-address
  restriction, mismatched registry version, absent provider, changing inferiors,
  changing ELF, missing DWARF, stripped symbols and unchanged language settings
  all passed. Async continuation confirms explicit running-target refusal.
  Final output: `/tmp/arx-framework-check-framework-debug-gdb.log`.
- Historical release target checks booted the actual matching
  release image, stopped at kmain and confirmed the distinct “release kernel:
  debug framework compiled out” result. Output:
  `/tmp/arx-framework-check-framework-release-gdb.log`.
- Manual post-init inspection in the first snapshot run showed C calls=5,
  synthetic bytes=320 and C++ runtimeCalls/completedUpdates=4 with zero active
  writers, confirming one runtime update from each of four CPUs in addition to
  the single C publication sample.
- Python compilation, whitespace checks, local README/guide/ticket/index links
  and a complete unique ticket-index/metadata audit passed. Source/style and
  documentation reviews found no new upward dependencies or architecture-specific
  operations in shared code.

QEMU initially required escalation because the sandbox blocked `/var/tmp` scratch
files, and GDB's local connection was sandbox-blocked; authorized reruns passed.
The snapshot runs terminated their VMs. Their running-target refusal
check can produce an additional GDB batch-exit warning because GDB cannot detach
from an asynchronously running target; assertions pass, the command does not
implicitly stop it, and the isolated VM was terminated. Earlier verification
iterations exposed nested-source import handling and an unsafe running-thread
query order; both were corrected and the final checks passed. Early link checking
also found and corrected the moved ticket's cutover link.

No required check remains failing or unavailable. Full kernel self-test-suite
completion and real hardware behavior are not claimed; the required boot/runtime
and controlled foundation checks passed. There are no deferred foundation TODOs.
Status remains `in progress` in this directory pending the owner's authorization
to close and confirmation of destination, proposed
`tickets/closed/build_debug_framework_foundation.md`. No closure history has been
appended and no commit has been made.

Generated build/images and Python caches were moved out of the source tree to
`/tmp/arx-framework-artifacts-hz2r8tst` after verification, preserving both ELF/image sets and intermediate
objects for inspection. Rebuild using the recorded commands before performing future
target inspection, or restore its `build/` and `bin/` directories from this archive.
The original exploratory QEMU snapshot was also shut down after verification.

### 2026-10-18 — Integrated launch documentation correction

The owner clarified that interactive debugging already runs through
`make x86_64-debug`. Updated GdbCommands.md to lead with that existing integrated
QEMU/GDB/TUI workflow, loading the new framework at its GDB prompt or using the
existing `GDB_EXTENSION` override. Removed the separate-terminal interactive
launch instructions. Launcher defaults and legacy removal remain in cutover
scope; no launcher/kernel code changed. Checked the Make recipe and both script
launch arguments, plus documentation links and whitespace; no kernel rebuild is
needed for this correction. Ticket remains in progress awaiting owner review.

### 2026-10-18 — Completion

The owner instructed “ok close ticket”, authorizing closure to the previously
proposed `tickets/closed/build_debug_framework_foundation.md` destination.
Completed the debug-only kernel/GDB foundation, bounded inspection and publication
helpers, release-erasure checks and integrated-launch documentation; required
verification passed as recorded above. Updated incoming dependency links, the
current index and both required filename-only closure histories.

### 2026-10-18 — Owner-directed tooling removal

The owner requested deletion of the standalone debug verification sources and
references. Removed their command paths while preserving historical results.
