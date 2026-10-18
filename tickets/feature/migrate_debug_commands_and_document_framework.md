# Cut over to the debug framework and document the replacement commands

- Type: feature
- Priority: P1
- Status: open
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
baseline; recheck it against the current code when starting. This open ticket
requires its own analysis/execution gate. Old-tool defects are tracked in
[the separate debug ticket](../debug/resolve_legacy_gdb_discovery_defects.md).

### Dependencies and scope

Depends on [foundations](../closed/build_debug_framework_foundation.md) and the old-coverage
parts of [physical allocation](add_debug_physical_allocation_views.md),
[process/resources](add_debug_process_resource_views.md),
[memory](add_debug_virtual_memory_views.md),
[filesystems](add_debug_filesystem_views.md) and
[platform/CPU/PCI](../in_progress/add_debug_platform_cpu_pci_views.md).
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
6. Review [agent-skill coordination](plan_kernel_agent_skill_tickets.md) and
   [authoring skill](create_debug_framework_authoring_skill.md): both reuse this
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

- [legacy script](../../scripts/gdb_extention.py),
  [launcher](../../scripts/debug-kernel.sh), [Makefile](../../Makefile),
  [README](../../README.md), [.vscode](../../.vscode).
- [Debug framework assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment) and
  [architecture guides](../../docs/architecture/KernelArch.md) for relevant references.
- Unknowns: existing user launch customizations, final grammar/override design,
  which milestone introduces each replacement and exact headless prerequisites.
  Preserve useful explicit-object/fixture inspection without target function calls.
- No Request providers, AArch64 framework expansion, independent new rich views,
  dump mechanics or general toolchain installation work.

## Acceptance criteria

- [ ] Every old command's useful information/selection/discovery capability has a verified new equivalent or an explicitly owner-agreed change, with ledger evidence.
- [ ] New discovery and views avoid the legacy resolver defects and never execute inferior getters; before-Dispatcher/release/missing-symbol behavior is documented.
- [ ] x86_64 launch/headless/source integration loads only the new entry point with matching symbols; other architecture/build paths are not inadvertently broken.
- [ ] The old script and registrations are removed after parity, stale active references are corrected and confirmed legacy defects have a recorded disposition.
- [ ] The command/maintainer guide covers implemented behavior, examples, limits and migration; README and affected guide links resolve.
- [ ] No unrelated rich-view completion, Request-layer tracking, dump implementation or compatibility alias backlog is silently added.

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

Inventory checked against [gdb_extention.py](../../scripts/gdb_extention.py) on
2026-10-18 during [the authorized planning execution](../closed/plan_gdb_extension_tickets.md).
These are current source behaviors, not a claim that every legacy fallback works
in a live session. No kernel build/boot was performed for this planning task.

#### Commands, inputs, data and coverage owners

| Current command | Arguments and output | Source roots and replacement owner |
| --- | --- | --- |
| `arx-platform` | No argument defaults to show; `show`/`status`, `set <address_expr>`, `clear`. Reports override/resolution and CPU pointer/count. Override changes host-side state, not the kernel. | `ArxPlatform` resolves `platform`; replace through [foundation](../closed/build_debug_framework_foundation.md) bootstrap/status plus [platform views](../in_progress/add_debug_platform_cpu_pci_views.md). |
| `arx-pmm` | Optional CPU expression; regular-zone region/page/byte/PFN/HHDM accounting, per-order free blocks/pages and list-derived totals. | `platform.cpus[] -> numa_node.zone`, buddy free lists; [physical allocation](../feature/add_debug_physical_allocation_views.md). |
| `arx-vmm` | Optional CPU expression; address-space pointer/type/PT/lock, all four region lists with bounds/size/type, metadata counts. | `platform.cpus[].address_space`; [memory views](../feature/add_debug_virtual_memory_views.md). |
| `arx-cpus` | Optional CPU expression; count/slots/arch, platform IOAPIC details and CPU ID, NUMA/address space and LAPIC/ACPI identity. | `platform`, `cpu_info_t`, `arch_info_t`; [platform views](../in_progress/add_debug_platform_cpu_pci_views.md). |
| `arx-pci` | No supported selector; arguments are ignored. Arch/count/array pointer, x86 BDF/vendor/device summary. | `platform.pci_devices`, `pci_device_count`; [platform views](../in_progress/add_debug_platform_cpu_pci_views.md). |
| `arx-heap` | Optional CPU expression; NUMA heap lock, cache size, partial/full/empty slab/object totals and metadata-pool pointers/shape. | `platform.cpus[] -> numa_node.heap.caches[]`; [physical allocation](../feature/add_debug_physical_allocation_views.md). |
| `arx-vfs` | Optional leading `--verbose`/`-v`, then optional VFS expression (pointer or object). Default mount paths plus sorted unique cached dentry paths; verbose namespace/cache and detailed mounts/filesystems/root inodes. | Explicit object or Logic VFS discovery, mount list and occupied dentry hash buckets/parent chains; [filesystem views](../feature/add_debug_filesystem_views.md). |
| `arx-initramfs` | Optional exact path string. Manager/count/capacity, all archive path/size/data entries or one selected entry. | Resource InitRamFileSystemManager `Archives`/`ArchiveCount`; [filesystem views](../feature/add_debug_filesystem_views.md). |
| `arx-proc` | Optional PID expression. Lists allocated slots with PID/address-space/task head; selected process adds allocation state, task count and first 16 task IDs. | Resource ProcessManager `Processes`, process task links; [process/resources](../feature/add_debug_process_resource_views.md). |
| `arx-task` | Required TID expression; selected allocated slot's ID/allocation/stack/next/prev and raw architecture context. | Resource TaskManager `Tasks`; [process/resources](../feature/add_debug_process_resource_views.md), with policy/waits added by [scheduling](../feature/add_debug_scheduling_wait_views.md). |
| `arx-rsrc` | Arguments ignored. Dispatcher/factory/caps and labeled manager addresses plus type availability. | Unconditional raw Resource slot reads; preserve useful discovery/type diagnostics in [foundation](../closed/build_debug_framework_foundation.md)/[platform](../in_progress/add_debug_platform_cpu_pci_views.md), not wrong slot values. |

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

[Cutover](../feature/migrate_debug_commands_and_document_framework.md) owns the
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
