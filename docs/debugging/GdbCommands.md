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

The launcher currently loads the legacy extension by default. Load the new
framework at the existing GDB prompt:

```text
source debug/gdb/arx.py
hbreak kmain
continue
arx-debug list
```

At `kmain` entry the Dispatcher does not yet exist. Continue to a later breakpoint
before inspecting providers. To load the new framework automatically instead of
the legacy extension for this session, use the launcher's existing override:

```sh
GDB_EXTENSION="$PWD/debug/gdb/arx.py" make x86_64-debug
```

Final launcher-default changes and legacy command removal belong to the cutover
work. The commands require the normal image-building prerequisites, QEMU and GDB
with Python and TUI support.

Separate output directories are useful when keeping debug and release builds
alongside each other:

```sh
GDB_EXTENSION="$PWD/debug/gdb/arx.py" make x86_64-debug BUILD_DIR=build/framework-debug BIN_DIR=bin/framework-debug
```

`x86_64-debug` forces a debug rebuild. Ordinary image targets do not track changed
compiler flags, so reuse of its output directory for a subsequent release build
can leave incompatible objects. Use separate directories or force a rebuild when
switching DEBUG values. The headless verification harness below launches its own
isolated snapshot QEMU/GDB session for automated checks.

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

Discovery has its own fixed budget of 65,536 bytes and 10,000 string/work entries.
Reads copy only the Dispatcher pointer from the large platform structure. Target
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

## Verification tools

Run the host corruption/bounds tests and concurrent registry contract check:

```sh
python3 -m unittest discover -s debug/tests -p 'test_*.py' -v
g++ -DDEBUG=1 -I. -pthread debug/tests/kernel_contract.cpp debug/kernel/DebugManager.cpp -o /tmp/arx-framework-contract
/tmp/arx-framework-contract
gcc -DDEBUG=0 -I. -O2 debug/tests/release_erasure.c -o /tmp/arx-framework-release-erasure
/tmp/arx-framework-release-erasure
objdump -d --disassemble=arx_release_probe /tmp/arx-framework-release-erasure
g++ -DDEBUG=0 -I. -Ikernel -Ikernel/platform -Ikernel/platform/acpi/uACPI/include -c debug/tests/dispatcher_layout.cpp -o /tmp/arx-framework-layout-release.o
nm -S /tmp/arx-framework-layout-release.o
```

For automated headless target checks, build both images without opening the
interactive launcher:

```sh
make -j4 DEBUG=1 BUILD_DIR=build/framework-debug BIN_DIR=bin/framework-debug bin/framework-debug/arx-x86_64.img
make -j4 DEBUG=0 BUILD_DIR=build/framework-release BIN_DIR=bin/framework-release bin/framework-release/arx-x86_64.img
python3 debug/tests/run_target_checks.py
python3 debug/tests/run_target_checks.py --release
```

The target harness starts an isolated snapshot QEMU, checks real boot stages and
matching types through GDB, and terminates its VM even on failure. It deliberately
mutates only controlled fixture/registry memory to verify corrupt and interrupted
state handling, then restores it. The inspection commands themselves are read-only.
Host checks do not substitute for runtime discovery checks. Real provider rollout,
legacy-command parity and final launch integration remain separate work.
