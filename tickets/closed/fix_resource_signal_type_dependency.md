# Remove Resource's dependency on Logic-owned signal definitions

- Type: debug
- Priority: P1
- Status: closed
- Required skill: mattpocock-skills:diagnosing-bugs

## Task prompt

The 2026-10-17 code-skill audit at revision `d057ba1` found an upward
compile-time dependency: `TaskManager.hpp` includes Logic's `SignalManager.hpp`
to obtain signal records stored in Resource-owned tasks. `ProcessManager.hpp`
includes `TaskManager.hpp` and also stores these records; `ProcessManager.cpp`
uses signal constants declared in the Logic header.

Expected behavior: Resource defines or consumes these shared storage contracts
without directly depending on a Logic manager header. Necessary upward runtime
access is permitted through Dispatcher under the owner's revised rule, but
that does not justify this direct header dependency. Logic may consume shared
definitions. Preserve signal ABI layouts, values, task/process state, and signal
behavior. Use the latest reassessment entry below for the current scope.

Follow [TICKET.MD](../TICKET.MD) and
[add-arx-code](../../docs/skills/add-arx-code/SKILL.md). Read the Resource and
Logic architecture guides before analysis. Creation does not authorize analysis
or implementation; use the required diagnosis skill when analysis is requested.

### Reproduce and inspect

- Inspect [TaskManager.hpp](../../kernel/layers/Resource/TaskManager.hpp),
  initially line 3, and its signal fields.
- Inspect [SignalManager.hpp](../../kernel/layers/Logic/SignalManager.hpp) for
  `signal_action_t`, `signal_stack_t`, `signal_sender_t`, `signal_queue_t`, and
  signal constants, then trace their consumers in Resource and `arch/`.
- Inspect [ProcessManager.hpp](../../kernel/layers/Resource/ProcessManager.hpp)
  and [ProcessManager.cpp](../../kernel/layers/Resource/ProcessManager.cpp).
- Reproduce with `rg -n 'layers/Logic|layers/Request' kernel/layers/Resource`.
  Review transitive dependencies as well as direct includes.

This is a source-level layering defect; no runtime failure was demonstrated by
the audit. The appropriate owning header and any include-cycle implications are
unknown until diagnosis. Moving the definitions is a candidate, not an approved
implementation plan; forward declarations alone cannot replace complete value
types. Keep this work separate from the
[runtime upward-call ticket](fix_resource_upward_runtime_calls.md).

## Acceptance criteria

- [x] Diagnosis identifies the signal records/constants Resource requires and their consumers.
- [x] Resource no longer includes or transitively depends on Logic for these definitions.
- [x] Shared definitions have one authoritative owner below their consumers; signal ABI sizes and values remain intact.
- [x] Affected declarations and consumers compile; relevant existing signal checks are selected during analysis and their results recorded.
- [x] Changes follow `kernel.cpp` formatting; relevant documentation is updated through `update-kernel-docs` as required by `add-arx-code`.

## Work log

### 2026-10-17 — Created

Created from the repository code-skill audit. Assigned P2 for a confirmed
architecture-rule violation without a demonstrated runtime regression. Analysis
and execution have not started.

### 2026-10-17 — Priority raised; design requested

The owner raised this ticket to P1 and requested diagnosis and design alongside
the runtime upward-call ticket before implementation. The owner also authorized
relevant documentation updates through `update-kernel-docs` as a standing part
of the code skill. Analysis is starting; implementation is not authorized yet.

### 2026-10-17 — Analysis and plan

#### Diagnosis and evidence

No glossary or ADR files were found. Read the kernel overview and the Resource,
Logic, Dispatcher, platform, and architecture guides. This defect is structural:
the compiler's include dependency graph is the reproduction seam. A kernel boot
cannot demonstrate compliance with dependency direction, so runtime reproduction,
instrumentation, and an implementation regression run are deferred to execution.

Ran this deterministic compiler query:

```sh
g++ -MM -x c++ -I. -Ikernel -Ikernel/platform -Ikernel/platform/acpi/uACPI/include kernel/layers/Resource/TaskManager.hpp
```

Its dependency list includes `kernel/layers/Logic/SignalManager.hpp`. This is
already a minimal witness: one Resource header reaches one upper-layer header.
The combined source-witness check also reproduced this edge twice, alongside
the four runtime edges recorded in the related ticket. No implementation changed.

The ranked causes considered were Logic ownership of shared records, Resource
methods mixing mechanism and policy, and callbacks hiding that ownership. For
this ticket, source inspection confirms the first cause: `task_t` embeds a
complete `signal_stack_t`, and `process_t` embeds `signal_action_t` records.
Forward declarations cannot replace those value definitions.

An additional production consumer is `arch/x86_64/arch_signal.cpp`, which also
includes Logic's header for these records. Placing every definition in Resource
would simply move that architecture dependency up to Resource. Use a common
data-only location below both consumers instead.

#### Proposed interface and ownership

1. Add `klib/signal_types.h` for the shared signal contract:
   `signal_action_t`, `signal_stack_t`, `signal_sender_t`, and the existing
   signal-number masks. Include only the standard integer/layout definitions
   needed by the records. Keep this header usable from C and C++; give it no
   dependency on Arch, Resource, Logic, Request, or manager implementations.
2. Add `kernel/layers/Resource/SignalState.hpp` for `signal_queue_t` and its
   Resource-owned queue storage. Preserve `CAPACITY = 128`, `count`, `reserved`,
   and the current record layout. Logic continues to decide queuing/delivery
   policy while Resource owns task/process storage.
3. `TaskManager.hpp` and `ProcessManager.hpp` include their storage definitions
   directly. `SignalManager.hpp` consumes the common records and Resource queue
   definition without owning a duplicate. `arch.h` exposes the common record
   definitions or compatible declarations; production `arch_signal.cpp` includes
   only the lower common contract and existing architecture helpers.
4. Preserve the ABI field order and widths: action is 32 bytes; alternate stack
   is 24 bytes; sender layout and mask values remain unchanged. Keep/add meaningful
   size and offset assertions in the owning definitions and independent target
   fixtures. Keep target register/frame details in `arch/`.

This follows the existing `TaskScheduling.hpp` separation between stored state
and Logic policy, while placing records also needed by Arch below Resource.
Moving all of SignalManager into Resource, copying definitions, or replacing
value fields with allocated pointers would add responsibilities or lifetime
changes unrelated to the defect and is rejected.

#### Execution sequence and verification

1. After execution authorization, move this ticket to `tickets/in_progress/`.
2. Extract the definitions without changing their values or runtime operations;
   update direct consumers and source dependencies.
3. Re-run compiler dependency generation for TaskManager, ProcessManager, and
   production architecture signal code. Resource must not reach Logic/Request;
   target signal implementation must not reach layer-manager headers.
4. Compile affected x86_64 and AArch64 translation units to check both consumers
   of the common contract; preserve AArch64's existing unsupported behavior.
   Establish any pre-existing compiler/environment failures separately.
5. Run relevant existing SignalManager and Request signal/ABI checks once the
   paired runtime migration is integrated; existing target fixtures cover actual
   signal frames. Record exact results and unavailable checks rather than treating
   compiler dependency output as runtime validation.
6. Use `update-kernel-docs` to correct signal-definition ownership in Resource,
   Logic, and architecture guides wherever the changed contract affects a claim.
   Review `KernelArch.md` and the relevant diagrams for meaningful impact.

The runtime ticket owns retirement and release interface changes. This extraction
can land first and is not dependent on the separate Platform/Klib scope decision.
No kernel behavior is intentionally changed in this ticket. Formatting must
follow `kernel.cpp`, including expanded blocks and generous blank lines.

#### State and remaining work

Design is recorded and status is `analyzed`; the owner requested design first.
No kernel implementation, build, boot, or runtime test was performed. Execution
awaits an explicit instruction under TICKET.MD. The source-level violation remains
present until implementation and verification are completed.

### 2026-10-17 — Reassessment under the Dispatcher exception

The owner revised the rule: necessary upward access is allowed, but MUST pass
through Dispatcher. Platform and Klib are outside the layer-stack direction
rule and may enter through Dispatcher routing into Request. The earlier blanket
prohibition on upward runtime calls is superseded.

This ticket remains a P1 defect: `TaskManager.hpp` directly includes Logic's
`SignalManager.hpp` to obtain embedded records. Compiler dependency generation
still reports that edge; no Dispatcher bridge mediates a header include.
Sharing those records through a common header remains the appropriate scope.
The previously proposed data extraction is still a candidate design, not an
instruction to move signal policy or all upper-layer access out of Resource.

Reassessed the current source without changing implementation. The runtime
ticket must now preserve the existing compliant scheduling-accounting bridge
and address callbacks that bypass Dispatcher. Broader lifecycle redesign and
new execution snapshots are not prerequisites for this header fix. The owner
paused design work before this reassessment; no further implementation design
or kernel changes were made. Status remains `analyzed`, pending agreement on
the revised plan and a later execution instruction.

### 2026-10-17 — Execution authorized

The owner instructed “ok fix the issues” and requested documentation of necessary
Dispatcher-mediated layer exceptions in KernelArch.md. Execution is authorized
under the revised Dispatcher exception, superseding the paused-design state.

Implement common signal records below their consumers. Preserve runtime ownership
and timing while replacing raw Resource callbacks with opaque, typed Dispatcher
routes for task cleanup and file retain/release. Route affected Platform/Klib
entries through typed Request handlers, including file-backing I/O. Stateless
Request entry methods preserve early Resource-test availability without reordering
layer construction. Route providers retain their existing context lifetimes;
Resource can hold route handles but cannot access their callbacks.

Verify compiler dependencies and both target builds, then run the affected
in-kernel suites in headless x86_64 SMP boots. Record any existing target/runtime
limitations explicitly. Use update-kernel-docs for KernelArch.md and other affected
guides. Implementation and verification are starting; closure remains owner-gated.

### 2026-10-17 — Ready for owner review

Implemented the common signal contract in `klib/signal_types.h` and Resource
queue storage in `SignalState.hpp`. Resource and production architecture signal
code no longer include Logic's SignalManager header. Preserved record widths,
offsets, masks, and queue capacity; added explicit SignalManager includes to the
Logic consumers that previously depended on the upward transitive include.

Updated KernelArch.md, Resource.md, Logic.md, and x86_64.md for the definition
ownership. The related runtime ticket records the Dispatcher route migration.
Existing signal-manager, Request signal, and user-frame roundtrip checks passed
in both x86_64 runs. No implementation TODOs remain in this ticket.

#### Verification evidence

- Final debug builds passed for both targets:
  `make -j8 DEBUG=1 BUILD_DIR=build/dispatch-debug BIN_DIR=bin/dispatch-debug x86_64 aarch64`.
  Final optimized x86_64 build passed with
  `make -j8 BUILD_DIR=build/dispatch-release BIN_DIR=bin/dispatch-release x86_64`.
- Headless x86_64 Q35 boots used `-m 1024 -smp 4 -cpu max`, the repository
  x86_64 UEFI firmware, and the corresponding isolated debug/release disk images
  with `-snapshot -no-reboot`. Both reached the final self-test summary:
  **155 ran, 155 passed, zero failed, overall PASS**. QEMU was bounded by a
  90-second timeout because the kernel remains idle after the completed tests.
- Boot evidence: `/tmp/arx-dispatch-debug-boot.log` and
  `/tmp/arx-dispatch-release-boot.log`. Final build logs:
  `/tmp/arx-dispatch-final-debug-build.log` and
  `/tmp/arx-dispatch-final-release-build.log`. No new compiler warnings were
  reported; the initial AArch64 build still emitted the existing noreturn warning
  in its unchanged unsupported user-entry stub. AArch64 runtime was not claimed
  or tested; the port remains incomplete.
- Compiler dependency generation for Resource TaskManager/ProcessManager and
  production x86_64 signal code contains no Logic/Request headers or route
  provider definitions. Shared signal records compile as C; ABI size/offset
  assertions also pass in the target builds.
- Negative compilation probes confirmed that Resource/Platform headers expose
  incomplete route types: task, file, and backing consumers cannot invoke
  provider fields. `nm -C -u` on the Resource objects references the new
  Dispatcher operations and no Scheduler, SignalManager, SynchronizationManager,
  or VirtualFileSystem symbols.
- All 182 local links checked across README, architecture guides, skills, and
  tickets resolve, including cross-references after the ticket moves. Skill
  validation and `git diff --check` pass.

Required verification is complete. Status remains `in progress`, stored in
`tickets/in_progress/`, ready for owner review. Closure and a destination have
not been authorized; the usual proposed destination is `tickets/closed/`.

### 2026-10-17 — Completion

The owner authorized closing both tickets to the proposed `tickets/closed/`
destination (“yes you can close them”). Moved shared signal records below their consumers, removed the upward Logic-header dependency, and preserved ABI layouts. Both target builds and the debug/optimized x86_64 SMP suites passed.
