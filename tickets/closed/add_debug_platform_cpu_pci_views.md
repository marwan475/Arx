# Add platform, CPU, IPI and PCI framework views

- Type: feature
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:grilling

## Task prompt

Migrate useful platform/CPU/PCI information and add stored progress/IPI state to
the new debug framework. Diagnose incomplete CPU initialization, deferred
preemption/rescheduling, outstanding TLB mailboxes and device inventory issues.
Current commands expose hardware identity but omit the runtime flags that help
explain SMP stalls; platform and resource discovery diagnostics also need a safe
replacement.

Follow [the workflow](../TICKET.MD), required grilling and applicable repo skills.
Parent: [planning](../closed/plan_gdb_extension_tickets.md).
Depends on [foundations](../closed/build_debug_framework_foundation.md). Reuse task/process
and address-space identities from [process](../closed/add_debug_process_resource_views.md)
and [memory](../feature/add_debug_virtual_memory_views.md), with local information readable
when those adapters are unavailable. See [assessment](../closed/plan_gdb_extension_tickets.md#rich-view-assessment)
and [platform documentation](../closed/document_platform_layer.md).
This open ticket retains its own analysis and execution gate.

### Scope and internal milestones

1. Provide platform-owned debug stats headers for boot readiness, CPU/IPI and
   stored PCI inventory; agree maintained counters/current values with exact
   semantics. Track necessary early platform data, publish via DebugManager
   afterward, and exclude all records/update work under `DEBUG=0`.
2. Preserve old platform-resolution diagnostics: bootstrap status, Dispatcher/
   DebugManager address, registry/provider availability and useful type status.
   A host-only explicit root/address override may be needed for fixtures or symbol
   problems; settle safe behavior in foundation analysis. It must not mutate the
   kernel, call methods or reintroduce obsolete raw member-slot guesses.
3. Preserve CPU counts/IDs, NUMA/address-space pointers, initialized state and
   x86 LAPIC/IOAPIC/ACPI identities/configuration. Add per-CPU preemption depth,
   switching/reschedule/syscall-active flags, kernel-stack bounds, IPI mailbox
   type and TLB request range/flags, plus observed mailbox lock bytes. Distinguish
   logical CPU IDs, slots and hardware IDs. Deduplicate topology relationships.
4. Preserve PCI BDF/vendor/device output and add stored segment/BAR/class/flags
   only where the captured `pci_device_t` data supports it. Validate actual
   capacity/count and root pointer; define a bounded selector and stable address
   vocabulary. Do not perform fresh config-space or device-register reads.
5. Correlate CPU/task/address-space progress via shared identities and report
   outstanding mailbox state as a current observation, not a completed IPI history
   or readout of actual TLB contents. Add adapters, native-reader data descriptions,
   guide updates and platform/CPU/PCI/resource-discovery migration coverage.

### Proposed interface and illustrative output

```text
arx-debug status
  dispatcher=<addr> registry=ready providers=12 elfTypes=available
arx-debug platform stats
  cpusInitialized=4 bspKmainExited=true
arx-debug cpus --cpu 1
  slot=1 id=1 hardwareId=2 preemptionDepth=1 reschedulePending=true
  ipi=INVALIDATE_TLB start=<va> size=4096 mailboxLock=1
arx-debug pci devices --limit 64
  segment=0 bdf=00:01.0 vendor=1234 device=1111
```

Illustrative values only; settle grammar with foundation. Never acquire target
locks or invoke CPU helpers, MSR/APIC/MMIO/config-space reads. Bounded counts,
unknown enums, partial initialization and unreadable CPU slots must report useful
status. No Request-layer provider (including InterruptRequestManager counters),
hardware mutation, architecture expansion, historical tracing or dump mechanics.

### Sources and unknowns

- [platform.h](../../kernel/platform/platform.h),
  [cpu.h](../../kernel/platform/cpu/cpu.h),
  [cpu.c](../../kernel/platform/cpu/cpu.c): topology, boot flags, IPI and ownership.
- [x86_64.h](../../arch/x86_64/x86_64.h),
  [interrupt_handler.c](../../arch/x86_64/interrupt_handler.c),
  [arch_syscall.c](../../arch/x86_64/arch_syscall.c),
  [lapic.c](../../arch/x86_64/lapic.c): stored architecture/syscall/mailbox state.
- [device.h](../../kernel/platform/device/device.h),
  [device.c](../../kernel/platform/device/device.c),
  [pci.c](../../arch/x86_64/pci.c): captured PCI schema/capacity.
- Unknowns: captured BAR/segment availability, per-CPU counter update ownership,
  bootstrap override grammar, registry diagnostics and partial type retention.
  Automatic syscall/exception frame discovery/history is not implied by active flags.

## Acceptance criteria

- [x] Debug-only platform/CPU/PCI records preserve old useful information and expose staged boot availability through the shared registry.
- [x] CPU slot/logical/hardware identities, preemption/switching/syscall/IPI state and memory/task links are unambiguous and bounded.
- [x] PCI output uses captured fields with real count/capacity validation and no config-space access.
- [x] Bootstrap/resource diagnostics explain unavailable roots/types without inferior calls or obsolete layout guesses.
- [x] Missing CPUs, count corruption, unknown mailbox types and partial publication return explicit status; no hardware TLB/history claims are made.
- [x] Native-reader descriptions, command guide and migration ledger cover the views, with no release work, Request registration or dump mechanics.

## Verification approach

Use [platform invariants](../../kernel/selftests/platform/platforminvariants.c),
[SMP fixtures](../../kernel/selftests/poststartkernel/smpfixtures.hpp),
[SMP tests](../../kernel/selftests/poststartkernel/smpselftests.cpp)
and [TLB integration](../../kernel/selftests/poststartkernel/tlbintegration.cpp).
Inspect x86_64 QEMU stops before/after Dispatcher and CPU initialization, with
single and multiple CPUs and a known PCI inventory. Observe real reschedule/TLB
request fixture states where reproducible; use controlled records for unknown
enums, invalid counts and rare partial states. Compare legacy identity data via
typed source reads rather than trusting its broken fallback diagnostics. Build
debug/release and verify that inspection reads stored RAM only.

## Work log

### 2026-10-18 — Created

P1: migrates platform/CPU/PCI coverage needed for safe command cutover and adds
source-backed SMP progress information without Request-layer support.

### 2026-10-18 — Evidence reference cleanup

Shared analysis references were consolidated into their owning tickets before
removing `tickets/analysis/`. Current links point to the archived evidence; ticket
scope, implementation authorization and verification requirements are preserved.

### 2026-10-18 — Analysis started

Read `tickets/TICKET.MD`, the required `mattpocock-skills:grilling`, and repository
`add-arx-code`, `update-kernel-docs` and `write-kernel-docs` skills. Analysis is
underway; status remains `open` pending owner design answers and confirmation of
shared understanding. No implementation or execution authorization is claimed.
A read-only fact-finding agent is inspecting source constraints as required by
the grilling skill.

Initial source findings: `cpu_info_t` already stores preemption, switching,
reschedule, stack and mailbox/TLB fields; x86 syscall activity is stored in
`arch_info.syscall_ctx.active`. `initialized` marks completion of per-CPU arch
initialization, whereas `platform.cpus_initialized` counts arrival on the kernel
stack at `platform_init_complete`; these must remain distinct observations.
Reschedule IPIs can bypass the mailbox, so `IPI_REQUEST_NONE` cannot imply no
pending reschedule. PCI inventory captures segment, class, command/status and
BAR fields with real capacity `MAX_PCI_DEVICES` (256). `bar_count` counts BAR
register slots, including absent entries and the high half of 64-bit BARs.

The foundation supports named provider stats/views and numeric typed inspection,
but has no `status`, selectors or root override yet. Design questions concern
explicit selector grammar, maintained debug summaries versus direct stored-state
views, safe per-command numeric root overrides, and optional cross-provider
correlation while process/memory providers remain unimplemented. Existing useful
legacy output will be compared against typed source data, not its fallback
discovery logic. The ticket index still contains this ticket once with matching
P1/feature/open metadata. No builds or runtime checks have run during analysis.

### 2026-10-18 — Analysis and plan; owner design confirmation

The owner answered “yes to all” four design recommendations, confirming the
shared design below. Analysis is complete. Status is `analyzed`; execution has
not started and awaits the separate instruction required by `tickets/TICKET.MD`.

#### Confirmed behavior and command contract

- `arx-debug status` reads early platform-owned debug records before Dispatcher
  exists, then reports Dispatcher/DebugManager addresses, registry availability,
  provider states and required ELF-type availability as those become readable.
  Preserve partial diagnostics when a later root/type fails. A registry cannot
  explain the CPU initialization barriers by itself: both precede `kmain`.
- Register `platform`, `cpus` and `pci` providers from Dispatcher composition
  wiring. Commands are `platform stats`, `cpus stats`, `cpus topology` and
  `pci devices`, under `arx-debug`. Keep generic scalar stats rendering and shared
  entry/byte/depth budgets; rich adapters return results separately from rendering.
- `--cpu <logical-id>` and `--slot <index>` select CPU topology entries with no
  logical-to-slot fallback; reject their simultaneous use. `--device
  <ssss:bb:dd.f>` selects an exact segment-qualified PCI identity. Validate numeric
  syntax, ranges and command applicability; a missing identity is explicit.
- Numeric `--platform <address>` and `--registry <address>` overrides apply only
  to the current command. Reject simultaneous overrides. A platform override
  resolves its typed Dispatcher field; a registry override identifies DebugManager
  directly and does not pretend to recover a platform root. Retain stopped-target
  and matching ELF/build-ID checks, format/bounds validation and type requirements.
  No persistent roots, arbitrary expressions, target calls or raw member offsets.
- Read existing CPU flags, mailbox payloads and captured PCI data from RAM.
  Maintain debug-only boot stages, per-CPU progress, enumeration outcomes and
  trusted collection bounds; do not mirror all hot-path runtime flags. All new
  storage, initialization, registration and update expressions disappear at
  `DEBUG=0`, while existing functional CPU/PCI state remains ordinary kernel state.
- Publish stable source addresses for CPU, NUMA, address-space and available task
  relationships. Optional process/memory adapters may enrich these when present;
  absent adapters produce explicit unavailable correlation without suppressing
  local state. Do not invent task IDs, follow unsupported ownership paths or
  assign an address-space identity to a TLB payload that contains none.

#### Implementation steps and affected code

1. Add C-compatible platform-owned stats/root headers under `debug/kernel/stats/`
   and conditional implementation sources as needed. Records have static kernel
   lifetime and semantic versions. Store actual CPU allocation capacity
   (`BOOT_SMP_MAX_CPUS`, currently 8) and PCI allocation capacity
   (`MAX_PCI_DEVICES`, currently 256), with staged root readiness. Validate counts
   against these bounds before any traversal; never infer allocation from a
   corrupted count. Static debug metadata must be inspectable before heap setup.
2. Instrument boot progress in `kernel/bootstrap.c`, `kernel/platform/cpu/cpu.c`
   and `kernel/kernel.cpp` using debug-only guards/helpers. Distinguish CPU-table
   readiness, per-CPU arch initialization, kernel-stack barrier arrival,
   Dispatcher publication, provider publication and BSP handoff. Use per-CPU
   ownership/atomic updates for concurrent progress rather than a shared stage
   overwritten by secondary CPUs. Preserve existing boot control flow.
3. Capture device enumeration started/succeeded/failed/no-regions and inventory
   capacity exhaustion where observable in `device.c`/`arch/x86_64/pci.c`.
   The scan currently returns success when capacity stops traversal, so a
   full/possibly truncated inventory must not be described as exhaustive.
   Add debug observations only; preserve functional enumeration behavior.
4. Declare/publish providers in `kernel/layers/Dispatcher.cpp` after platform
   data becomes usable, preserving unavailable/partial states. Keep Platform
   ownership and Dispatcher publication separate; add no Request providers or
   lower-layer discovery of upper managers. Retain emitted C record/root types
   and verify actual DWARF, including pre-Dispatcher discovery types.
5. Extend `debug/gdb/framework/commands.py` and `target.py` for diagnostic status,
   numeric per-command overrides and typed selectors. Keep provider-specific
   interpretation in new adapters under `debug/gdb/views/`; preserve existing
   foundation command behavior, build-ID verification and running-target refusal.
   Report missing types, malformed roots/metadata, unknown enums, absent/unreadable
   slots, partial publication and truncation without unlimited retries.
6. CPU topology output includes slot/logical/hardware identities, initialized and
   boot-progress state, preemption/switch/reschedule/syscall flags, stack bounds,
   NUMA/address-space pointers, mailbox type/TLB range/flags and observed lock byte.
   Show shared x86 LAPIC/IOAPIC/ACPI topology once per identity, with references
   from CPUs. Do not follow LAPIC/IOAPIC/ECAM addresses into hardware memory.
7. PCI output includes inventory index, source address, segment-qualified BDF,
   captured IDs/class/revision/configuration flags and validated BAR slots.
   `bar_count` counts register slots, not present resources; 64-bit BARs consume
   two slots. Captured bases/raw values have no probed resource-size guarantee.
   Inspection performs no config-space, MSR, APIC, MMIO or device-register reads.
8. Extend `docs/debugging/GdbCommands.md` with commands, native-reader semantic
   contracts, roots/bounds/lifetime, selectors, stages, correlation and a migration
   ledger for legacy platform/CPU/PCI/resource discovery. Update affected existing
   architecture guides concisely. Legacy command removal/launcher cutover stays
   in its own ticket; no new guide or diagram is currently needed.

#### Verification and interpretation risks

- Build separate x86_64 debug/release ELFs and images. Check new type retention,
  registry wiring and debug-only records in debug; verify source exclusion,
  symbols/layout and relevant update-site disassembly in release. Extend the
  side-effect erasure probe where useful. Release must incur no new tracking work.
- Run existing host foundation tests and add meaningful adapter/parser fixtures
  for corrupt counts, null/unreadable roots and slots, unknown mailbox enums,
  invalid BAR counts, selectors, limits, deduplication and partial availability.
- Extend snapshot QEMU/GDB verification for single and multiple CPUs, stops before
  CPU-table readiness, during per-CPU/stack initialization, before Dispatcher,
  during provider publication and after handoff. Compare real stored identities
  and known PCI inventory using typed reads, not the broken legacy fallbacks.
- Observe real reschedule/TLB fixture states where reproducible using existing
  SMP/TLB integration fixtures. Use explicitly controlled records for rare states
  and corruption; keep those results distinct from real runtime observations.
  Verify numeric override scope, missing-type diagnostics, unchanged GDB settings,
  foundation compatibility and refusal of running/mismatched targets.
- Treat observations as potentially interrupted multi-field state, never a
  coherent historical snapshot. Reschedule IPIs can bypass the mailbox; lock=1
  may be sender ownership while checking completion. A residual TLB payload with
  mailbox NONE is not an outstanding request. No actual TLB contents, completed
  IPI history or automatic syscall/exception frame discovery is promised.
- Check style against `kernel.cpp`, dependency/architecture boundaries, docs,
  local links, whitespace and ticket-index uniqueness before handoff. Record exact
  build/runtime evidence and any unavailable checks; failures keep execution in
  progress. No build or runtime verification is claimed by this analysis.

No unresolved owner design choices remain. Exact fixture stop locations and
runtime reproducibility will be established during execution. P1 is unchanged.

### 2026-10-18 — Execution authorized

The owner instructed “execute”. Moved the ticket to `tickets/in_progress/` and
started the recorded plan. Existing unrelated workspace changes are preserved.

### 2026-10-18 — Ready for owner review; verification stopped by owner

Implemented debug-only early boot progress and platform/CPU/PCI publication,
stored-RAM CPU/IPI/topology and captured PCI views, explicit selectors, per-command
numeric roots, staged discovery/type diagnostics and native-reader/migration docs.
Updated GdbCommands.md, KernelArch.md, Platform.md and x86_64.md. Task/address-space
enrichment remains explicitly unavailable until its owning providers implement it,
as agreed; local identities remain readable. No Request providers or hardware
inspection operations were added.

Before the owner stopped verification, both debug/release images built, single-
and four-CPU snapshot checks passed, real TLB/reschedule paths were observed,
foundation regressions and concurrent counter checks passed, and release erasure,
layout, source/symbol/disassembly checks passed. Corruption fixtures covered counts,
roots, slots, enums, BARs, publication, selectors, overrides and missing types.
The first SMP verification attempt timed out due to breakpoint ordering; the
corrected run passed. Logs are /tmp/arx-platform-target-1.log,
/tmp/arx-platform-target-4.log, /tmp/arx-platform-foundation-target.log,
/tmp/arx-platform-release-target.log and /tmp/arx-platform-release-audit.txt.
Full kernel self-test completion and real hardware behavior are not claimed.

The owner instructed “stop verification” and requested deletion of standalone
verification sources and all references. Removed those sources and their guide
commands; retained historical results without references to removed files.
No further verification was run. Status remains in progress pending owner review
and authorization to close to tickets/closed/add_debug_platform_cpu_pci_views.md.
No commit was made. Generated builds/images and Python caches were moved to
/tmp/arx-platform-artifacts-psai6y1x.

### 2026-10-18 — Owner-directed automatic loading

The owner explicitly requested automatic loading of the new extension. Changed
the x86_64 launcher default to the new entry point and updated GdbCommands.md.
The explicit extension override remains supported; AArch64 keeps its existing
default. Verification remains stopped per the owner’s instruction.

### 2026-10-18 — Completion

The owner instructed “close ticket”, authorizing closure to the previously
proposed tickets/closed/add_debug_platform_cpu_pci_views.md destination.
Completed platform/CPU/IPI/PCI debug records and views, early discovery diagnostics,
selectors, native-reader documentation and automatic x86_64 extension loading.
Verification results and the subsequent owner-directed verification stop/tooling
removal are recorded above. No additional verification was run for closure.
