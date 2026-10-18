# Add platform, CPU, IPI and PCI framework views

- Type: feature
- Priority: P1
- Status: open
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
Depends on [foundations](build_debug_framework_foundation.md). Reuse task/process
and address-space identities from [process](add_debug_process_resource_views.md)
and [memory](add_debug_virtual_memory_views.md), with local information readable
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

- [ ] Debug-only platform/CPU/PCI records preserve old useful information and expose staged boot availability through the shared registry.
- [ ] CPU slot/logical/hardware identities, preemption/switching/syscall/IPI state and memory/task links are unambiguous and bounded.
- [ ] PCI output uses captured fields with real count/capacity validation and no config-space access.
- [ ] Bootstrap/resource diagnostics explain unavailable roots/types without inferior calls or obsolete layout guesses.
- [ ] Missing CPUs, count corruption, unknown mailbox types and partial publication return explicit status; no hardware TLB/history claims are made.
- [ ] Native-reader descriptions, command guide and migration ledger cover the views, with no release work, Request registration or dump mechanics.

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
