# Cut over to the debug framework and document the replacement commands

- Type: feature
- Priority: P1
- Status: in progress
- Required skill: mattpocock-skills:grilling

## Task prompt

Replace the eleven legacy `arx-*` commands with the new framework, preserve their
useful inspection coverage, update launch/source integration and deliver a human
command guide. The owner explicitly chose removal of the existing script/command
set after coverage migration, not indefinite compatibility aliases. Do not erase
working investigation capabilities simply because the new entry point loads.

Follow [the workflow](../TICKET.MD), required grilling and repo skills. Parent:
[planning](../closed/plan_gdb_extension_tickets.md). The initial
[legacy inventory](migrate_debug_commands_and_document_framework.md#legacy-command-inventory) is the coverage
baseline; recheck it against the current code when starting. This ticket follows
its own analysis/execution gate. Old-tool defects are tracked in
[the separate debug ticket](../debug/resolve_legacy_gdb_discovery_defects.md).

### Dependencies and scope

Depends on [foundations](../closed/build_debug_framework_foundation.md) and the old-coverage
parts of [physical allocation](../closed/add_debug_physical_allocation_views.md),
[process/resources](../closed/add_debug_process_resource_views.md),
[memory](../closed/add_debug_virtual_memory_views.md),
[filesystems](../closed/add_debug_filesystem_views.md) and
[platform/CPU/PCI](../closed/add_debug_platform_cpu_pci_views.md).
Do not require unrelated new signal/socket/event enhancements before removing
legacy commands once all required migrated coverage passes. Provider tickets
update the common guide as they land; this ticket owns final integration and
reference completeness, not all documentation writing at the end.

### Milestones

1. Maintain a concrete old-to-new ledger including argument semantics, roots,
   useful fields/details and limitations. Cover platform override/status,
   PMM/heap node and CPU selection, VMM CPU spaces and four region lists, CPU/PCI
   identity, VFS cached paths/verbose mounts/explicit object inspection, initramfs
   listing/exact path, process list/detail, task/context and discovery/type status.
   Old syntax and erroneous raw pointer values need not be preserved; capabilities
   and changed semantics must be clearly mapped.
2. Verify the new generic family and registered production records expose that
   information at useful boot/runtime stops without inferior GetCaps/GetLogicLayerCaps
   calls, unbounded walks, stale offsets or resolver setting corruption. An early
   pre-Dispatcher stop reports unavailable per the agreed design; document this
   intentional difference rather than adding an unapproved second registry root.
3. Update `scripts/debug-kernel.sh`, Makefile and repository/editor references to
   load `debug/gdb/arx.py`; check matching ELF/debug build, headless agent workflows,
   path quoting, current working directory, attach/cleanup and isolated build paths.
   x86_64 is supported. Keep AArch64 launch behavior coherent: do not implicitly
   require new framework support on that target or break unrelated build targets.
4. After parity evidence, remove `scripts/gdb_extention.py` and old registrations
   and update stale references without keeping hidden active aliases. Record how
   each confirmed legacy defect is repaired by replacement or verified removal.
   Coordinate with the debug ticket without treating its owner closure as an
   extra feature dependency.
5. Complete `docs/debugging/GdbCommands.md`: loading/setup, debug/release and
   x86_64 requirements, discovery/status, selectors, stats and every implemented
   rich view, representative verified output, units/meaning, partial/unavailable/
   truncation behavior, limits, publication caveats and migration table. Include
   a maintainer section for headers, update sites, registration, typed roots and
   view adapters. Label future features clearly. Link it from README Documentation
   and affected existing guides; follow applicable documentation skills.
6. Review [agent-skill coordination](../feature/plan_kernel_agent_skill_tickets.md) and
   [authoring skill](../closed/create_debug_framework_authoring_skill.md): both reuse this
   reference rather than duplicating or claiming unimplemented command behavior.

### Illustrative user flow and output

```text
make x86_64-debug
# launcher loads debug/gdb/arx.py with the matching DEBUG=1 ELF
arx-debug status
  registry=ready target=x86_64 providers=<count>
arx-debug list
arx-debug pmm stats
arx-debug vfs cache
arx-debug initramfs contents --path /bin/test
```

Verify actual build/launch commands before documenting them; the example is a
proposed flow, not evidence that the current target builds a DEBUG=1 ELF. Final
view grammar follows the foundation contract. Documentation example values must
be labeled or captured from controlled verified state.

### Sources and unknowns

- `scripts/gdb_extention.py` (removed during cutover),
  [launcher](../../scripts/debug-kernel.sh), [Makefile](../../Makefile),
  [README](../../README.md); no .vscode directory is currently present.
- [Debug framework assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment) and
  [architecture guides](../../docs/architecture/KernelArch.md) for relevant references.
- Unknowns: existing user launch customizations, final grammar/override design,
  which milestone introduces each replacement and exact headless prerequisites.
  Preserve useful explicit-object/fixture inspection without target function calls.
- No Request providers, AArch64 framework expansion, independent new rich views,
  dump mechanics or general toolchain installation work.

## Acceptance criteria

- [x] Every old command's useful information/selection/discovery capability has a verified new equivalent or an explicitly owner-agreed change, with ledger evidence.
- [x] New discovery and views avoid the legacy resolver defects and never execute inferior getters; before-Dispatcher/release/missing-symbol behavior is documented.
- [ ] x86_64 launch/headless/source integration loads only the new entry point with matching symbols; other architecture/build paths are not inadvertently broken.
- [x] The old script and registrations are removed after parity, stale active references are corrected and confirmed legacy defects have a recorded disposition.
- [x] The command/maintainer guide covers implemented behavior, examples, limits and migration; README and affected guide links resolve.
- [x] No unrelated rich-view completion, Request-layer tracking, dump implementation or compatibility alias backlog is silently added.

## Verification approach

Use the ledger to compare typed source/fixture state with new outputs in x86_64
QEMU/GDB. Old broken fallback results are not an oracle. Cover normal populated,
empty/uninitialized, partial/corrupt and selected-object states, meaningful
CPU/node/process/path selectors and private fixtures. Verify script sourcing from
another working directory/isolated build and reinferior/reload behavior. Exercise
documented headless and interactive launch flows as available, recording any
unavailable environment dependency. Run relevant debug/release builds, link/source
reference checks and documentation verification. Review diffs for premature
deletion, stale aliases and unimplemented claims before owner review.

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

P1: preserves investigation coverage while completing the owner-requested command
replacement. Combines migration/integration and command-reference verification.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

## Legacy command inventory

Archived reference from the removed `tickets/analysis/` directory. Historical
commands and paths below describe the original investigation.

### Legacy GDB command inventory and replacement coverage

Inventory checked against `scripts/gdb_extention.py` (removed during cutover) on
2026-10-18 during [the authorized planning execution](../closed/plan_gdb_extension_tickets.md).
These are current source behaviors, not a claim that every legacy fallback works
in a live session. No kernel build/boot was performed for this planning task.

#### Commands, inputs, data and coverage owners

| Current command | Arguments and output | Source roots and replacement owner |
| --- | --- | --- |
| `arx-platform` | No argument defaults to show; `show`/`status`, `set <address_expr>`, `clear`. Reports override/resolution and CPU pointer/count. Override changes host-side state, not the kernel. | `ArxPlatform` resolves `platform`; replace through [foundation](../closed/build_debug_framework_foundation.md) bootstrap/status plus [platform views](../closed/add_debug_platform_cpu_pci_views.md). |
| `arx-pmm` | Optional CPU expression; regular-zone region/page/byte/PFN/HHDM accounting, per-order free blocks/pages and list-derived totals. | `platform.cpus[] -> numa_node.zone`, buddy free lists; [physical allocation](../closed/add_debug_physical_allocation_views.md). |
| `arx-vmm` | Optional CPU expression; address-space pointer/type/PT/lock, all four region lists with bounds/size/type, metadata counts. | `platform.cpus[].address_space`; [memory views](../closed/add_debug_virtual_memory_views.md). |
| `arx-cpus` | Optional CPU expression; count/slots/arch, platform IOAPIC details and CPU ID, NUMA/address space and LAPIC/ACPI identity. | `platform`, `cpu_info_t`, `arch_info_t`; [platform views](../closed/add_debug_platform_cpu_pci_views.md). |
| `arx-pci` | No supported selector; arguments are ignored. Arch/count/array pointer, x86 BDF/vendor/device summary. | `platform.pci_devices`, `pci_device_count`; [platform views](../closed/add_debug_platform_cpu_pci_views.md). |
| `arx-heap` | Optional CPU expression; NUMA heap lock, cache size, partial/full/empty slab/object totals and metadata-pool pointers/shape. | `platform.cpus[] -> numa_node.heap.caches[]`; [physical allocation](../closed/add_debug_physical_allocation_views.md). |
| `arx-vfs` | Optional leading `--verbose`/`-v`, then optional VFS expression (pointer or object). Default mount paths plus sorted unique cached dentry paths; verbose namespace/cache and detailed mounts/filesystems/root inodes. | Explicit object or Logic VFS discovery, mount list and occupied dentry hash buckets/parent chains; [filesystem views](../closed/add_debug_filesystem_views.md). |
| `arx-initramfs` | Optional exact path string. Manager/count/capacity, all archive path/size/data entries or one selected entry. | Resource InitRamFileSystemManager `Archives`/`ArchiveCount`; [filesystem views](../closed/add_debug_filesystem_views.md). |
| `arx-proc` | Optional PID expression. Lists allocated slots with PID/address-space/task head; selected process adds allocation state, task count and first 16 task IDs. | Resource ProcessManager `Processes`, process task links; [process/resources](../closed/add_debug_process_resource_views.md). |
| `arx-task` | Required TID expression; selected allocated slot's ID/allocation/stack/next/prev and raw architecture context. | Resource TaskManager `Tasks`; [process/resources](../closed/add_debug_process_resource_views.md), with policy/waits added by [scheduling](../closed/add_debug_scheduling_wait_views.md). |
| `arx-rsrc` | Arguments ignored. Dispatcher/factory/caps and labeled manager addresses plus type availability. | Unconditional raw Resource slot reads; preserve useful discovery/type diagnostics in [foundation](../closed/build_debug_framework_foundation.md)/[platform](../closed/add_debug_platform_cpu_pci_views.md), not wrong slot values. |

For PMM/VMM/CPU/heap, an omitted selector means all inferred CPU slots. A provided
selector first matches logical `cpu.id`, then falls back to an array slot.
Selectors and PID/TID/address expressions use GDB evaluation with numeric fallback.
The new grammar may clarify IDs versus slots; document changes and preserve useful
selection rather than ambiguous syntax or arbitrary function-call expressions.

#### Pointer and type discovery

- **Platform:** host override; cached address with plausibility check; symbol
  expressions including qualified spellings; temporary C language; global/static
  symbol lookup; `info address`; linker-map lookup and `_start` slide adjustment.
  Map candidates assume `build/kernel-*.map` and do not reliably cover isolated
  build paths. Plausibility reads CPU count/arch/pointer but is not ELF identity.
- **Resource:** Dispatcher from `platform.dispacher`/`dispatcher` spellings,
  typed factory/caps expressions/member access (including `GetCaps()` calls),
  then pointer-slot fallbacks and typed casts. Some failed paths return raw
  numeric values which do not establish a valid typed object.
- **VFS:** explicit expression, typed Logic getters/caps expressions (including
  `GetLogicLayerCaps()`), then raw Dispatcher/factory/Logic caps slots. Parent path
  reconstruction uses names/parent pointers; there is no automatic child tree.
- **Arrays:** prefer DWARF member arrays; process/task fallbacks guess first-member
  layout/capacity. Several guesses no longer match current declarations.

Calling the existing output inspection commands does not guarantee read-only
inferior behavior because discovery can evaluate target getters. Replacement
discovery must use stopped-target memory and the DebugManager registry only,
with clear pre-Dispatcher/missing-symbol states and explicit typed fixture access.

#### Current limitations and selected improvements

- PMM/heap enumerate CPU-linked nodes, repeating a shared NUMA node. PMM omits
  the separate DMA zone. Buddy, VMM, heap and process-task walks lack complete
  cycle/work bounds. Showing only 16 task IDs does not bound traversal work.
- VMM cannot directly select an inactive process/root and omits protection,
  backing and sharing relationships. It cannot explain translation/permission
  disagreement from region bounds alone.
- CPU views lack preemption/switching/reschedule/syscall/IPI progress information.
  PCI count guarding does not use the actual captured capacity consistently;
  richer stored metadata must be source-checked before adding fields.
- VFS cached paths are not complete backing contents. Mount traversal caps at
  256 and parent chains at 64, but the cache's 8192-result bound does not itself
  cap empty-bucket scanning. Some failures silently suppress useful evidence.
- Initramfs count validation is useful; exact-path string reads are less tolerant
  than list-mode reads. Mode/UID/GID and hierarchy are not displayed.
- Process/task output lacks lifecycle/retirement, parent tree, ownership,
  credentials, descriptor sharing, policy and blocking relationships.
- No existing command family covers futex PI, signals/events, socket/pipe queues,
  DMA records, timers, self-test progress or retained error records. The separate
  [rich-view assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment) identifies concrete roots
  and limitations. Request managers are excluded by owner instruction.

#### Confirmed legacy discovery defects

Recorded in [one debug ticket](../debug/resolve_legacy_gdb_discovery_defects.md):

1. `_extract_first_hex` lacks its successful return. Isolated AST evaluation of
   the class returned `None` for `Symbol at 0xffffffff80001000.`.
2. Temporary-language restoration matches `currently c` inside `currently c++`,
   losing auto/C++ selection. A stub GDB returning `auto; currently c++` observed
   `set language c` before evaluation and again during restoration.
3. Resource fallbacks/`arx-rsrc` use process/task/initramfs slots 1/3/4, while
   current Resource caps place them at 2/5/6. VFS fallback uses Logic slot 1
   (ProcessController), while VFS is slot 2.
4. Process/task array fallbacks assume manager base equals array base despite
   preceding current fields; task capacity fallback is 64 rather than 128.

The first two are pure-helper reproductions; the latter two are direct source
layout mismatches. Which live sessions use each fallback remains unverified.
Resolve by necessary verified interim repair or verified removal/safe replacement
during cutover; do not copy broken behavior for compatibility.

#### Migration completion and deferrals

[Cutover](../in_progress/migrate_debug_commands_and_document_framework.md) owns the
running coverage ledger and parity evidence. Source inventory alone is not
parity verification. New providers must document equivalents and preserve explicit
selection, useful fields and discovery diagnostics. Remove old commands/script
only once relevant migrated coverage works; unrelated later enhancements need
not delay cutover.

Deferred: dump formats/triggers/sinks/native dump execution; Request providers;
AArch64 framework support; historical event/syscall/IPI streams; raw spinlock
owner inference; automatic arbitrary-task unwinding/frame discovery; persistent
ELF image history; universal object/leak/deadlock proof. Keep existing unknowns
visible in child analysis rather than fill missing data with guessed semantics.

### 2026-10-18 — Launcher default changed at owner request

The owner explicitly requested automatic loading of the new extension during
platform-view work. The x86_64 launcher now defaults to the new entry point, with
the existing override retained and the AArch64 default preserved. This completes
only that default change; remaining migration/parity/removal work and this
ticket’s analysis gate remain open. Verification was not resumed.

### 2026-10-20 — PMM/heap migration coverage verified

[Physical allocation views](../closed/add_debug_physical_allocation_views.md)
provide `pmm nodes/zones/orders/blocks/pfn`, `heap caches/slabs/address` and
`dma allocations`, with generic stats. Legacy PMM normal-zone regions, counts,
bytes, PFNs/HHDM, lock and per-order blocks/pages map to node/zone/order/block
views. Legacy heap class/list totals, metadata-pool shape and lock map to
cache/slab views. Explicit node, logical CPU and slot selectors replace ambiguous
CPU expressions; shared nodes appear once and DMA zones are included.

Parity evidence: one-CPU and four-CPU x86_64 QEMU/GDB stops before Dispatcher and
before/after Resource tests inspected typed source fields and maintained totals,
bounded buddy/slab enumeration, metadata chunks, bitmaps and selected heap
addresses. Controlled host records cover an additional node, corrupt links/counts,
invalid PFNs and truncation. DMA mock fixture stops show retained rollback and
active release phases; production direct-address allocation/release and kernel
counter assertions pass. Release QEMU reports the framework compiled out.
`debug/tests/` contains repeatable checks. GdbCommands.md records replacement
coverage and native semantics. This does not authorize legacy removal or closure
of the cutover ticket; other providers and cutover work remain pending.

### 2026-10-20 — Process/task migration coverage verified

[Process/resource views](../closed/add_debug_process_resource_views.md)
map `arx-proc` to `process tree/details` and `arx-task` to `task list/details`,
with numeric PID/TID selection and generic typed context inspection. Allocated
slots, IDs, address spaces, task membership, names, state, stack/context and links
remain inspectable; real capacities replace legacy raw array guesses. Added
incarnation-safe parentage, CPU/dispatch/retirement state, process defaults and
task overrides, descriptor/file ownership correlation and credentials.

One-CPU and four-CPU QEMU/GDB checks exercise slot reuse, private task contexts,
retained child dispatch claims, wait-notification gating, real shared VFS files
and completed clone/exec/exit/wait syscall fixtures. Host fixtures cover malformed,
cyclic, stale and sparse data and independent scan/output limits. Release source,
symbol/disassembly/layout checks and the release guest verify exclusion. Command
and native-reader semantics are in GdbCommands.md; existing commands remain until
cutover. This updates coverage evidence without closing or expanding that ticket.

### 2026-10-20 — Owner-directed verification scope

The owner requested avoiding excessive verification on remaining tickets. Added
an explicit stopping rule and evidence-based limits on repeated or broader
checks. Acceptance criteria, implementation scope, status, and execution gates
are unchanged.

### 2026-10-20 — Virtual-memory migration coverage verified

[Virtual-memory views](../closed/add_debug_virtual_memory_views.md) preserve
legacy space pointer/type/PT/lock, explicit CPU ID/slot selection, four region
lists with bounds/size/type and metadata counts through memory space/regions.
The new views add inactive process selection, protection/max-protection, backing
and shared-page identities, pending writeback and selected-VA/range x86_64
translation. Process/task file output includes inode backing identity for
correlation. Existing arx-vmm remains until cutover.

Focused host fixtures cover three leaf sizes, permission folding, malformed and
bounded state, selection and writeback. Debug/release builds and focused source/
symbol exclusion/native-header checks pass. A QEMU/GDB run verifies retained
writeback, clone backing, inactive process selection, known mapped/unmapped VA,
and existing protection/shared-writeback/COW fixtures. GdbCommands.md documents
format and completeness limits. This records parity without expanding cutover.

### 2026-10-20 — VFS/initramfs migration coverage verified

[Filesystem views](../closed/add_debug_filesystem_views.md) preserve
arx-initramfs parsed-entry enumeration and exact-name size/data information,
adding raw mode/UID/GID and inferred hierarchy. VFS mounts/cache preserve mount,
root, filesystem and dentry identities, ancestry, flags and hash occupancy.
Visible contents additionally reconstruct uncached native cpio paths, overlay
precedence and mount shadow/bind/move behavior; callback-only backends explicitly
remain incomplete. Retired, unlinked and rename-forwarding records are labeled
separately. Existing legacy commands remain until cutover.

Thirteen focused host tests, x86_64 debug/release builds, release exclusion and
native-layout/header checks passed. Final one-CPU QEMU/GDB inspection passed
with known archive paths, rename/unlink and bind/move fixtures, verified backend
identity and unchanged cache occupancy. An earlier four-CPU VFS run passed;
a later four-CPU run was blocked before the VFS fixture by a task-cleanup failure,
recorded in the owning ticket without a claimed root cause or expanded scheduler
investigation. GdbCommands.md documents commands, bounds and native contracts.

### 2026-10-20 — Analysis started

Owner requested final cutover and old-command removal. Read the full ticket,
TICKET.MD, required grilling skill and applicable repository skills. Rechecking
the eleven-command ledger, archived provider evidence, current launch integration
and reference completeness before recording the implementation plan.

Current launcher defaults x86_64 to debug/gdb/arx.py but AArch64 to the legacy
script, so removal requires an explicit AArch64 default choice. The launcher
currently requires GTK and GDB TUI and has no integrated headless mode. Both
debug Make targets force DEBUG=1 builds. There is no current .vscode directory.
Implementation has not started; design confirmation and the subsequent execution
instruction remain pending under TICKET.MD.

### 2026-10-20 — Analysis and plan; owner narrowed scope to cleanup

Applied required grilling and the add-arx-code/update-kernel-docs repository
skills. The owner rejected adding opt-in features: "no opt in thing just clean
up". Keep the existing interactive workflow and existing customization surface;
do not introduce headless/batch modes, command-file options, new providers,
editor configuration or architecture support. Remove the obsolete commands and
stale integration/documentation references. AArch64 uses plain GDB by default
after legacy removal; do not automatically load the x86_64-only framework there.

Read-only recheck found replacement source coverage for every legacy command:

| Legacy | Existing replacement and relevant differences |
| --- | --- |
| arx-platform | status; numeric per-command --platform/--registry instead of persistent override state or expressions. |
| arx-pmm | pmm nodes/zones/orders/blocks/pfn; explicit logical CPU, slot and node selectors. |
| arx-heap | heap caches/slabs/address; shared NUMA nodes deduplicated. |
| arx-vmm | memory space/regions; kernel-space default, explicit --cpu/--slot for CPU-linked spaces, and --pid/--space for other roots. |
| arx-cpus | cpus topology/stats and platform stats; logical CPU IDs distinguished from array slots. |
| arx-pci | pci devices/stats; captured inventory within actual capacity. |
| arx-vfs | vfs mounts/cache/contents; numeric --vfs and typed inspect preserve explicit objects and verbose mount/cache detail. |
| arx-initramfs | initramfs contents/entry with --path/--initramfs; exact archive entries and listing. |
| arx-proc | process tree/details --pid; allocated slots, address spaces and bounded task membership. |
| arx-task | task list/details --tid and typed context inspection; actual 128-slot inventory. |
| arx-rsrc | status/list, provider stats source pointers and typed Dispatcher/factory/caps inspection; intentionally drops incorrect raw slot values. |

The closed physical allocation, process/resource, virtual-memory, filesystem and
platform tickets record debug/release builds and focused QEMU/GDB parity checks.
The platform ticket's owner-directed verification stop followed successful
single-/four-CPU snapshots, selector/corruption checks and release exclusion; it
does not mean those prior results were absent. Reuse that recorded evidence and
distinguish it from new cutover integration checks. Unfinished signal/event and
selftest/error enhancements are not removal dependencies.

Concrete cleanup plan:

1. Complete the guide's eleven-command migration ledger, citing existing provider
   parity records in this ticket. Document numeric selectors, kernel-space
   default, staged unavailable behavior and typed explicit-object alternatives.
2. Remove scripts/gdb_extention.py and its eleven registrations after reviewing
   the ledger against existing source and recorded provider evidence. Keep the
   new x86_64 entry point, existing interactive launch and existing custom-path
   override. Remove the AArch64 legacy default; source no default extension there.
   Fix source/path quoting and quote the existing Makefile launcher path only
   where needed by this cleanup. Add no new launcher options or build targets.
3. Correct active references in GdbCommands.md, architecture/arch/x86_64.md and
   aarch64.md, platform/Platform.md, layers/Dispatcher.md, Logic.md, Request.md,
   Resource.md and selftests/SelfTests.md. README already links the common guide.
   Preserve historical ticket evidence and clearly identify removed paths there.
4. Record the four legacy discovery defects as decommissioned through removal
   and typed replacement in the related debug ticket, without closing it or
   undertaking interim legacy repairs. Confirm coordination/authoring tickets
   continue to reference the common guide rather than duplicate command behavior.
5. Review the diff and verify the old script/registrations/active source paths
   are gone, replacement references resolve, and no compatibility aliases,
   unimplemented documentation claims or unrelated features were introduced.

Verification is proportional to cleanup: Git Bash syntax checking and focused
mocked launcher checks for x86_64 new-extension sourcing, AArch64 plain GDB,
existing explicit override, path spaces/other working directories and cleanup;
existing Python reader tests plus entry-point repeated-source/registration and
fresh-root behavior checks; source/reference/link and whitespace checks. Use
archived provider parity instead of rerunning unrelated kernel suites. A final
live matching-ELF launch/build check remains unverified if required tools are
unavailable; report that limit and leave the ticket in progress rather than
claiming full verification or closing it. General toolchain installation is out
of scope. Git Bash is available; make/compiler/QEMU/GDB and WSL are absent here.

Analysis is recorded. Await the owner's post-analysis execution instruction under
TICKET.MD, then move the ticket to tickets/in_progress/ before cleanup. No launcher,
legacy script, code or human-facing guide has been modified during analysis.

### 2026-10-20 — Execution authorized and parity ledger reviewed

Owner instructed "go" after the cleanup-only plan was recorded. Moved the ticket
to tickets/in_progress/ and updated the index before implementation. The eleven
replacement rows above were checked against current source before deleting the
legacy script. Existing live parity evidence is owned by:

- Platform/CPU/PCI/status: closed/add_debug_platform_cpu_pci_views.md records
  matching debug/release images, one-/four-CPU snapshots, roots/selectors/types,
  corruption cases and release exclusion before the owner's verification stop.
- PMM/heap: closed/add_debug_physical_allocation_views.md records normal-zone
  accounting, buddy/slab metadata, node/CPU selection and release evidence.
- Processes/tasks: closed/add_debug_process_resource_views.md records allocated
  slots, context/links, identities, task membership and typed shared resources.
- VMM: closed/add_debug_virtual_memory_views.md records all four region lists,
  CPU/space/PID selection, metadata and known translation/backing fixtures.
- VFS/initramfs: closed/add_debug_filesystem_views.md records archive list/exact
  paths, mounts/cache/explicit fixture objects and matching source-state checks.

This reuses the provider evidence already summarized in this ticket's prior work
log; it does not claim those archived runs were repeated in the current Windows
environment. Numeric-only selectors, distinct CPU IDs/slots, ephemeral overrides,
kernel-space VMM default and explicit typed roots are documented in the guide's
central eleven-command migration table. Bad raw slot values and inferior getter
evaluation are intentionally not preserved.

### 2026-10-20 — Progress and remaining work

Cleanup implemented:

- Removed scripts/gdb_extention.py (1,958 lines) and its eleven registrations.
  No compatibility aliases remain. x86_64 retains debug/gdb/arx.py as its default;
  AArch64 launches plain GDB. Existing GDB_EXTENSION customization is preserved.
- The existing script path is passed as one GDB -x argument, avoiding command
  interpolation/space splitting. Quoted the two existing Makefile launcher paths.
  Existing interactive/TUI behavior and DEBUG=1 rebuild targets are preserved;
  no opt-in mode, option, command-file feature or build target was added.
- Updated GdbCommands.md setup and the complete migration ledger; retained its
  existing provider/native/maintainer sections and illustrative-value caveat.
  Corrected the affected architecture/selftest guides and linked the common
  reference. README's existing Documentation link remains valid.
- Corrected moved-ticket links and removed dead hyperlinks to the deleted legacy
  source while preserving historical descriptions. The skill-coordination ticket
  now points to the current entry point, and the authoring ticket still reuses the
  shared guide. No new skill was implemented.
- Added all four defect dispositions to the separate debug ticket without
  closing it or starting an interim repair. New discovery uses matching symbols,
  typed fields and registry records, with no inferior calls or language changes.

Verification:

- Git Bash `bash -n scripts/debug-kernel.sh` passed.
- Bundled Python `python.exe -B -m unittest discover -s debug/tests -p
  'test_*.py' -v`: all 35 tests passed. Six new cutover checks cover x86_64 loading
  from another working directory with spaces in paths, AArch64 plain GDB,
  existing custom script override, missing-script rejection, cleanup after GDB
  failure, repeated-source single registration and fresh per-command discovery
  (the override and cleanup share one case). Existing 29 reader/parser tests pass.
- Initial Bash initialization/temp-file checks were blocked by the Windows
  sandbox, not by product assertions. The approved unsandboxed rerun passed.
  No further test repetition was needed after that passing run.
- Reviewed the launcher and guide diff, active source references, removed
  registrations, changed local links and ticket-index uniqueness. No editor
  configuration existed to migrate. No kernel/provider code was changed.

Remaining verification: actual kernel make/compiler, QEMU and GDB are absent in
this environment, and WSL is not installed. Mock launcher tests do not prove
live GDB -x Python script dispatch, matching-ELF attachment or debug/release build
integration. One supported-environment live x86_64 launcher/ELF check (including
an isolated build path and release refusal) remains required before marking the
launch integration criterion complete. Use prior provider parity rather than
rerunning unrelated subsystem suites. Do not add tooling installation or new
launcher features. Ticket remains in progress, awaiting that verification and
then the owner's review/closure instruction; no completion or closure is claimed.
