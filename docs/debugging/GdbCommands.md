# Kernel debug framework and GDB commands

The framework exposes maintained manager statistics and read-only collection
views in x86_64 debug kernels. Managers update their own records during runtime;
GDB reads them while all target threads are stopped. DebugManager owns discovery
metadata, not the managers or their records. Release kernels contain no framework
storage, initialization, registration, counters or update argument evaluation.

## Build and load

Use the existing integrated launcher from the repository root:

```sh
make x86_64-debug
```

This target rebuilds the image with `DEBUG=1`, starts QEMU paused with a GDB stub,
opens GDB in the current terminal with the matching kernel ELF and source layout,
connects, and continues to its `_start` hardware breakpoint. The script captures
serial output in `build/qemu-debug-serial.log` and cleans up QEMU when GDB exits.
No second terminal or separate GDB invocation is needed.

The x86_64 launcher automatically loads `debug/gdb/arx.py`. At the GDB prompt:

```text
hbreak kmain
continue
arx-debug list
```

At `kmain` entry the Dispatcher does not yet exist. Continue to a later breakpoint
before inspecting providers. The `GDB_EXTENSION` override can select a different
extension for one session, including the legacy commands during migration:

```sh
GDB_EXTENSION="$PWD/scripts/gdb_extention.py" make x86_64-debug
```

Legacy command removal belongs to the cutover work. The commands require the
normal image-building prerequisites, QEMU and GDB with Python and TUI support.

Separate output directories are useful when keeping debug and release builds
alongside each other:

```sh
make x86_64-debug BUILD_DIR=build/framework-debug BIN_DIR=bin/framework-debug
```

`x86_64-debug` forces a debug rebuild. Ordinary image targets do not track changed
compiler flags, so reuse of its output directory for a subsequent release build
can leave incompatible objects. Use separate directories or force a rebuild when
switching DEBUG values.

The reader compares the loaded ELF's GNU build-id note with target RAM before
using layouts, then checks the registry magic, format version and bounds.
It requires the matching unstripped ELF, including DWARF. The current address
model is the fixed x86_64 linker layout; relocation and other architectures are
unsupported. Before the kernel image is mapped, even its identity note may be
unreadable. A stripped ELF reports missing symbols, a debug ELF without DWARF
reports missing debug types, and a matching release ELF reports that the framework
was compiled out. A different ELF is rejected before reading records.

## Commands and limits

| Command | Behavior |
| --- | --- |
| `arx-debug status` | Diagnose early platform progress, Dispatcher/registry roots, provider publication and record-type availability. |
| `arx-debug list` | List reserved provider names, record types and availability. |
| `arx-debug <provider> stats` | Render a custom record's scalars, enums, nested structs and arrays from matching ELF types. |
| `arx-debug <provider> <view>` | Gather and render a named collection through an optional semantic adapter. |
| `arx-debug inspect <type> <address>` | Inspect a named ELF type at an explicit numeric address, without registry registration. Quote type names containing spaces. |

For example:

```text
arx-debug fixture-c stats
arx-debug fixture-cpp list --limit 40
arx-debug fixture-cpp graph --depth 2
arx-debug inspect "FoundationCppStats" 0xffffffff80288000
```

The address is illustrative; use the current symbol address or stopped target's
actual address. The command accepts decimal or hexadecimal integers and rejects
expressions, including function calls. Pointer fields print their numeric
addresses without expanding targets. Unions expose overlapping interpretations
with a partial-status warning; anonymous fields and base classes are reported as
unsupported rather than guessed. There are no target method calls, clock reads,
MMIO reads, lock acquisition, language changes or implicit stop/resume operations.
The extension retains no target-address/type cache between commands.

All commands accept the following options. Raising one does not raise the others.

| Option | Default | Maximum | Meaning |
| --- | --- | --- | --- |
| `--limit N` | 100 | 10,000 | Shared work budget for entries, fields and structural nodes. Empty hash buckets also consume work. |
| `--bytes N` | 65,536 | 16 MiB | Payload bytes copied from target RAM, including view roots. |
| `--depth N` | 8 | 32 | Struct expansion depth or graph depth. Graph roots have depth zero; depth 1 includes roots only. |

Registry discovery has its own fixed budget of 65,536 bytes and 10,000 string/work entries.
It copies the Dispatcher pointer from the large platform structure. `status`
uses the command budget for its staged diagnostic reads. Target
records are copied into local GDB values before rendering; pretty-printers do not
control traversal. A byte limit can reject a whole record before expansion.

Results preserve the source address, type, count interpretation and units.
`complete` means traversal finished within bounds; it does not establish global
consistency or object validity. `partial` includes truncation, cycles, shared graph
edges, invalid indices, unreadable memory, unsupported interpretations and marked
updates in progress. Partial results retain entries gathered before the problem.
Invalid top-level metadata can instead reject the command with a specific error.

## Kernel record interface

DebugRecords.h defines the shared C-compatible contract; DebugManager.hpp defines
the registry API. Dispatcher embeds DebugManager under `#if DEBUG`. The build
includes its implementation and fixture sources only with DEBUG=1.

Provider authors keep arbitrary custom stats structs in `debug/kernel/stats/`.
No inheritance, fixed field list or universal counter layout is required. Use
`ARX_DEBUG_ONLY(...)` for storage and declarations, `ARX_DEBUG_RUN(...)` for
registration or grouped operations, and `ARX_DEBUG_ADD`/`ARX_DEBUG_STORE` for
unsigned 64-bit counters. These macros erase their arguments in release.

```cpp
ARX_DEBUG_ONLY(MyStats debugStats{};)

void Manager::RecordAllocation(uint64_t bytes)
{
    ARX_DEBUG_ADD(debugStats.calls, 1);
    ARX_DEBUG_ADD(debugStats.bytes, bytes);
}
```

If the entire function exists solely for tracking, guard its declaration,
definition **and call** as well. An unconditional call to an empty function can
still impose cost in a release build. Conditional storage must include optional
group markers and rich-root descriptors. Keep raw helper calls inside debug-only
blocks; helper functions and types do not exist in release.

Composition wiring registers the production manager after construction. Do not
self-register inside a manager constructor: self-tests can construct temporary
instances. Lower managers do not locate upper layers to register themselves.
Early platform records can update before Dispatcher exists, then register later;
registry discovery remains unavailable before Dispatcher publication.

The registry has 64 lifetime slots and no allocations. Lifecycle mutations require
serialized bootstrap or quiescent wiring. This is separate from concurrent stats
updates, which never touch the registry. API results distinguish invalid metadata,
duplicates, full capacity, missing names and wrong withdrawal owners.

- `Register` reserves a fresh name and release-publishes initialized metadata.
- `DeclareUnavailable` reserves a name with a stable reason string. `Publish`
  transitions that reserved unavailable slot to a ready record.
- Duplicate names never replace records. Invalid metadata after reservation leaves
  an unavailable slot, which can be published correctly later.
- A stop during publication reports partial state. Unpublished pointers are not
  followed. Identity metadata is immutable after its initial publication.
- `Withdraw` checks the record owner, publishes a tombstone and clears borrowed
  pointers before the caller frees its storage. Withdrawn names/slots are never
  reused during that registry lifetime. `InvalidateAll` withdraws every entry;
  Dispatcher calls it before deleting factories. This is not a coordinated
  concurrent kernel-shutdown facility.

Names and type identities are copied into fixed arrays. Records, optional group
markers, view descriptor arrays, descriptor strings, unavailable reasons and all
borrowed roots must remain alive while published. Registration does not retain
objects or prevent collection slot reuse. Each provider must define reuse identity
and quiescent teardown rules for its own collections. Record versions are
provider-specific positive integers; registry format is version 1. The reader
checks record sizes against matching DWARF and adapters check their view versions.

Atomic helpers use relaxed operations on naturally aligned unsigned 64-bit
counters; do not pack atomic fields. Additions wrap modulo 2^64. Zero-initialize
group markers and never reset them while writers can access them. Selected related updates
can use an optional zero-initialized `arx_debug_group_t`, bracketing work with
`arx_debug_group_begin` and `arx_debug_group_end` inside `ARX_DEBUG_RUN`. Pair every
begin with exactly one end. Sequentially consistent marker operations bracket
payload updates without adding a lock. Each shared payload field still requires
atomics or existing provider synchronization.

A nonzero active-writer count means the stopped target interrupted a marked group;
the reader reports partial state. Multiple overlapping writers remain detectable,
including an even number of writers. Completed-update count is diagnostic; no
parity or seqlock claim is made. Zero active writers does not guarantee a coherent
snapshot of unmarked fields, different groups, providers or live collection links.

## Rich views and future native readers

Each arx_debug_view_t supplies a view name, typed root address/size, semantic ID
and version, count and capacity. These are explicit inputs, not generic reflection.
The provider must define whether counts describe used slots, bounds, reachable
objects or some other quantity. Dynamic counts can live in the typed root; the
adapter must validate them against physical bounds before traversal.

Each semantic contract must also document link/index fields, sentinel values,
ordering, ownership/weak references, slot identity/reuse, lifetime and partial-update
interpretation. Keep those rules in matching shared headers and this guide, rather
than only Python. A future native reader compiles against the exact build's layout
headers and implements the semantic ID/version with bounded memory reads. It also
needs the target architecture/address model, memory access and the descriptor's
roots/bounds. GNU build identity and matching headers are required inputs; DWARF
or executing Python in the kernel is not required. Native readers, serialization,
dump triggers, sinks and panic hooks are deferred.

GDB adapters live under `debug/gdb/views/`. Register their semantic ID/version, expected root type and gather function in
`views/__init__.py`; the command dispatcher selects that entry without provider
logic in the command parser. Framework readers return Result objects
with items, source/type/count/unit metadata and issues; gathering and field
expansion finish before presentation. New adapters reuse the shared bounded array,
pointer-list, index-list, string, hash-bucket and graph algorithms. They must not
print independently, invoke target methods, or invent an unbounded discovery pass.

## Platform, CPU and PCI views

```text
arx-debug status
arx-debug platform stats
arx-debug cpus stats
arx-debug cpus topology
arx-debug cpus topology --cpu 1
arx-debug cpus topology --slot 0
arx-debug pci stats
arx-debug pci devices --device 0000:00:01.0 --limit 20
```

`status` reads debug progress embedded in the global platform object before
Dispatcher exists. It preserves readable progress when a later root or type is
unavailable. A pre-Dispatcher result is partial because the registry is not yet
available. After publication it reports registry addresses, provider states and
whether each ready record's ELF type has the expected size. This checks type
availability, not the validity of every payload or rich-view descriptor.

The platform owns these records for the kernel lifetime. `PlatformStats.h` defines
their layout and numeric stage/outcome vocabulary. The platform stage follows
the BSP: not started, boot entered, CPU table ready, architecture initializing,
devices enumerating, waiting for CPUs, platform tests, kernel-stack barrier,
Dispatcher published, providers published and BSP handoff (values 0 through 10).
Each CPU independently records not started, architecture initializing,
architecture initialized and arrival on its permanent kernel stack (0 through 3).
`architectureInitialized` and `stackArrivals` count those separate milestones.
`bspHandoff` records reaching the scheduling handoff; it does not prove a task ran.
Progress updates are debug-only atomic observations, not coherent snapshots.

Dispatcher publishes the `platform`, `cpus` and `pci` providers during
`StartKernel`, before creating Resource. Generic `stats` commands render their
maintained records. CPU topology reads existing `cpu_info_t` state, including
preemption depth, context switching, pending reschedule, syscall activity, stack
bounds and mailbox payload/lock byte. It keeps array slot, logical ID, hardware
ID and ACPI/LAPIC IDs distinct. `--cpu` selects a logical ID; `--slot` selects an
array index. They are mutually exclusive. Shared IOAPIC state appears once;
NUMA and address-space relationships group CPU slots by borrowed source address.
Task correlation and richer address-space identities report unavailable until
their providers supply correlation adapters. Local CPU state stays readable.

A mailbox is a current observation. Scheduling IPIs can bypass it, so `NONE`
does not mean no scheduling work is pending. A lock byte of 1 alone does not
prove an unhandled request: a sender can hold it while checking completion.
Only an `INVALIDATE_TLB` mailbox interprets the stored TLB payload; cleared
mailboxes can retain stale bytes. TLB requests carry no address-space identity.
The CPU's current address-space pointer is separate. These views report neither
completed IPI history nor actual TLB contents, and do not discover saved frames.

PCI output uses the captured inventory, including segment-qualified BDF, IDs,
class/revision, command/status, header, interrupt and BAR fields. `--device`
requires hexadecimal `ssss:bb:dd.f` with an exact segment and BDF. Selectors are
valid only on their matching rich view and accept no expressions. Each entry
also reports its inventory index and source address. BAR counts describe register
slots (at most six), including absent entries and the empty upper slot of a
64-bit BAR. Captured BAR bases/raw words do not provide probed resource sizes.
PCI outcomes distinguish not started, enumerating, succeeded, no ECAM regions
and failed (0 through 4); capacity exhaustion marks a possibly truncated inventory.
No config-space, APIC, MSR, MMIO or device-register access is performed.

### Explicit discovery roots

`--platform <numeric-address>` resolves a typed platform's Dispatcher field;
`--registry <numeric-address>` names DebugManager directly. Both are accepted by
registry commands and `status`, apply to one command, and are mutually exclusive.
They accept positive decimal/hexadecimal integers, retain stopped-target, ELF
build-ID, type, format and bounds checks, and never persist across commands.
`status --registry` explicitly reports that it has no platform root. A missing
type cannot be repaired by guessing a member offset. `inspect` accepts its own
numeric address and rejects these root options.

### Native-reader contracts and migration

| Semantic ID/version | Root and traversal contract |
| --- | --- |
| `arx.platform.cpus` / 1 | `arx_cpu_debug_stats_t`; descriptor count=0 denotes dynamic count, capacity=`BOOT_SMP_MAX_CPUS` (8). Validate root size/capacity, table readiness, `source->cpu_count` and `source->cpus` before reading contiguous `cpu_info_t` slots in array order. Progress stages use the same slot index. |
| `arx.platform.pci` / 1 | `arx_pci_debug_stats_t`; descriptor count=0 denotes dynamic count, capacity=`MAX_PCI_DEVICES` (256). Validate root size/capacity, allocation/outcome and `source->pci_device_count` before reading contiguous `pci_device_t` entries in enumeration order. Validate BAR count against `MAX_PCI_BARS` (6). |

Roots, CPU slots and captured inventory have kernel lifetime; these views do not
define reuse generations. Pointer links are borrowed identities. Native readers
need matching headers/build identity and must implement these semantics with
bounded RAM reads. Never follow stored APIC, BAR or ECAM addresses. Unknown enums,
invalid counts, null nonempty inventories, unreadable slots and interrupted
publication produce explicit partial/unavailable results. Fixed capacities bound
selector searches as well as display traversal; byte limits cover every copied
slot. Each rich-view row consumes an entry; its field count is schema-bounded.
No target locks or CPU helper calls are permitted.

| Legacy coverage | Replacement | Preserved information and correction |
| --- | --- | --- |
| `arx-platform show/status/set/clear` | `status`, per-command `--platform` / `--registry` | Typed roots and availability; numeric overrides with no persistent state or expression evaluation. |
| `arx-cpus` | `cpus topology`, `cpus stats`, `platform stats` | Counts/IDs, NUMA/address-space and stored APIC identities plus runtime/progress state; explicit slot versus logical-ID selectors. |
| `arx-pci` | `pci devices`, `pci stats` | Captured BDF/vendor/device plus segment/class/BAR/configuration fields; actual allocation bounds replace the legacy 4096-entry guess. |
| Resource/manager discovery diagnostics | `status`, `list` | Dispatcher/DebugManager roots, named providers, staged availability and record types; no manager-map slot guesses or getter calls. Production Resource providers arrive in their own tickets. |

Legacy commands remain available through the explicit extension override until
the cutover ticket removes them. Their unsafe fallback discovery is not part of
this contract. The AArch64 launcher retains its existing extension default.

## Controlled foundation providers

These providers demonstrate the contract; they are not PMM, scheduler or other
real subsystem telemetry. Their global storage and borrowed roots last for the
kernel lifetime. Construction publishes unavailable names, then the C provider,
then the C++ provider after Resource creation/tests. Each CPU's scheduled post-init
entry updates both records once. C calls/bytes also include one publication sample;
bytes increase by a synthetic 64 per call. C++ runtimeCalls counts post-init
entries; publication.completedUpdates counts completed marked updates.

The C record demonstrates an enum, nested usage record, samples array and opaque
pointer. The C++ record owns a four-node fixed collection. All six descriptors use
`arx.foundation.collections` version 1, root type FoundationCollections, rootSize
`sizeof(FoundationCollections)` and count/capacity 4. The root's mutable count is
a used-array length bounded by 4; the fixture initializes it to 4. Node values are
synthetic sequence numbers, nextIndex is a signed array index, and pointers carry
no ownership. Nothing outside the fixture retains or reuses these nodes.

| View | Interpretation for Python and future compiled native readers |
| --- | --- |
| `array` | nodes[0..count), in increasing slot order. |
| `list` | head, then FoundationNode.next until null; visited-address cycle detection. |
| `indices` | indexHead, then nodes[index].nextIndex until -1; indices must be within count. |
| `string` | label points to UTF-8 bytes ending at NUL; stop at entry/byte limit. |
| `buckets` | Two buckets in array order, then each next chain; shared nodes appear per bucket. Bound empty buckets too. |
| `graph` | Depth-first head/next graph, deduplicated by address; repeated edges report cycle-or-sharing, not a deadlock proof. |

Null pointer-list/graph roots are empty; null strings are unavailable. Array roots
may be null only for length zero. Index walks accept -1 as empty; other negative
or out-of-range indices are invalid. Reads can stop mid-mutation, so damaged links
are evidence rather than proof of permanent corruption. The fixture adapter is
in foundation.py; FoundationStats.hpp and FoundationCStats.h define the layouts.
