# Diagrams

Editable Mermaid sources for SVGs embedded in the kernel, platform, and
architecture guides.
The rendered SVG files remain in `docs/imgs/`. Render each block with Mermaid CLI,
using `platform-mermaid-config.json` and its listed SVG filename. Keep the guides
using the rendered SVGs rather than inline Mermaid blocks.
Preserve text spacing in exported SVGs with `xml:space="preserve"` on the root element.

## NUMA allocation structure

SVG: `NUMA Node Allocation Flow-2026-04-15-064600.svg`

```mermaid
flowchart TB
    accTitle: NUMA memory organization
    accDescr: CPU records reference their NUMA node. Each node owns Normal and DMA zones backed by buddy allocators.
    P[Platform CPU records] --> C[Current CPU]
    C --> N[Assigned NUMA node]
    P --> O[Other CPUs and NUMA nodes]
    N --> Z[Normal zone]
    N --> D[DMA zone]
    Z --> B[Buddy allocator]
    D --> DB[Buddy allocator]
    Z -. allocation fallback .-> R[Other nodes' Normal zones]
```

## Physical memory allocation

SVG: `pmmalloc-2026-04-15-065023.svg`

```mermaid
flowchart TB
    accTitle: Physical memory allocation
    accDescr: Try the local Normal zone, then remote zones, splitting buddy blocks as needed.
    A[pmm_alloc: requested bytes] --> B[Select local Normal zone]
    B --> C{Valid size and order?}
    C -- no --> X[Return NULL]
    C -- yes --> D[Lock zone]
    D --> E{Free block available?}
    E -- no --> F[Unlock zone]
    F --> G{Another node to try?}
    G -- yes --> H[Select next Normal zone]
    H --> D
    G -- no --> X
    E -- yes --> I[Split larger buddies if needed]
    I --> J[Mark block used and update counters]
    J --> K[Unlock zone]
    K --> L[Return HHDM pointer]
```

## Physical memory release

SVG: `pmmfree-2026-04-15-065114.svg`

```mermaid
flowchart TB
    accTitle: Physical memory release
    accDescr: Locate the owning Normal or DMA zone, validate the allocation, and merge free buddies.
    A[pmm_free: allocation pointer] --> B{Non-null pointer?}
    B -- no --> X[Return]
    B -- yes --> C[Find owning node and Normal or DMA zone]
    C --> D{Aligned address in a zone?}
    D -- no --> X
    D -- yes --> E[Lock zone]
    E --> F{Valid allocated block?}
    F -- no --> K[Unlock zone]
    F -- yes --> G[Record original block size]
    G --> H[Merge free buddies of the same order]
    H --> I[Insert resulting block into free list]
    I --> J[Update counters using original size]
    J --> K
    K --> X
```

## Platform CPU state

SVG: `dispatcher-2026-04-15-070216.svg`

```mermaid
flowchart TB
    accTitle: Platform CPU state
    accDescr: The current architecture CPU ID selects a platform CPU record containing machine and memory state.
    P[Platform state] --> T[CPU records]
    I[arch_cpu_id] --> C[Current CPU record]
    T --> C
    C --> N[NUMA node]
    C --> V[Active address space]
    C --> A[Architecture state]
    C --> S[Kernel stack]
    C --> R[Preemption and reschedule state]
```

## Virtual region reservation

SVG: `vmmreserve-2026-04-15-072825.svg`

```mermaid
flowchart TB
    accTitle: Virtual address reservation
    accDescr: Claim an aligned free address range without allocating or mapping physical pages.
    A[vmm_reserve_region] --> B[Select address space and align size]
    B --> C{Valid inputs?}
    C -- no --> X[Return 0]
    C -- yes --> D[Select kernel or user lists and metadata pool]
    D --> E[Lock address space]
    E --> F{Free range fits?}
    F -- no --> U[Unlock and return 0]
    F -- yes --> G{Exact fit?}
    G -- yes --> H[Move free record to used list]
    G -- no --> I{Metadata record available?}
    I -- no --> U
    I -- yes --> J[Create used record and shrink free range]
    H --> K[Unlock and return reserved address]
    J --> K
```

## Virtual region release

SVG: `vmmfree-2026-04-15-072859.svg`

```mermaid
flowchart TB
    accTitle: Virtual address reservation release
    accDescr: Return a reserved address range and merge adjacent free ranges; this does not unmap pages.
    A[vmm_free_region] --> B[Select address space]
    B --> C{Valid inputs?}
    C -- no --> X[Return]
    C -- yes --> D[Lock address space]
    D --> E{Used range starts at address?}
    E -- no --> K[Unlock address space]
    E -- yes --> F[Remove from used list]
    F --> G[Insert into sorted free list]
    G --> H[Merge adjacent left and right ranges]
    H --> I[Recycle merged metadata records]
    I --> K
    K --> X
```

## Virtual address-space structure

SVG: `vaddr-2026-04-15-074940.svg`

```mermaid
flowchart TB
    accTitle: Virtual address-space state
    accDescr: An address space holds page tables, locks, separate kernel and user region lists, and metadata pools.
    A[Address space] --> S[Page-table root and locks]
    A --> K[Kernel regions]
    A --> U[User regions]
    K --> KF[Free list]
    K --> KU[Used list]
    K --> KM[Kernel region metadata pool]
    U --> UF[Free list]
    U --> UU[Used list]
    U --> UM[User region metadata pool]
    KF --> R[Region record]
    KU --> R
    UF --> R
    UU --> R
    R --> I[Start, end, size, type and backing]
    R --> L[Previous and next links]
```

## Heap structure

SVG: `heap-2026-04-19-224905.svg`

```mermaid
flowchart TB
    accTitle: Per-node heap structure
    accDescr: A NUMA node owns a heap whose size-class caches track slabs and metadata.
    N[NUMA node] --> H[Kernel heap]
    H --> L[Heap lock]
    H --> C[Size-class caches]
    C --> O[Object size]
    C --> P[partial_slabs: in-use slabs]
    C --> F[full_slabs: all objects available]
    C --> E[empty_slabs: exhausted-slab list]
    C --> M[Slab metadata pool]
    P --> S[Slab record]
    F --> S
    E --> S
    S --> B[Object storage and allocation bitmap]
    S --> V[Object counts and list links]
```

## Heap allocation

SVG: `heapalloc-2026-04-19-225900.svg`

```mermaid
flowchart TB
    accTitle: Small-object heap allocation
    accDescr: Select a size class, reuse a slab or grow the cache, and return an object.
    A[heap_alloc: heap and size] --> B{Valid size class?}
    B -- no --> X[Return NULL]
    B -- yes --> C[Try partial_slabs]
    C --> D{Object allocated?}
    D -- yes --> R[Return object pointer]
    D -- no --> E[Try full_slabs: all objects available]
    E --> F{Object allocated?}
    F -- yes --> M[Move slab to partial_slabs]
    F -- no --> G[Grow cache with PMM storage and metadata]
    G --> H[Retry full_slabs]
    H --> I{Object allocated?}
    I -- no --> X
    I -- yes --> M
    M --> R
```

## Heap release

SVG: `heapfree-2026-04-19-225920.svg`

```mermaid
flowchart TB
    accTitle: Small-object heap release
    accDescr: Find the owning slab in each size-class cache and update the allocation bitmap and slab list.
    A[heap_free: heap and pointer] --> B{Valid inputs?}
    B -- no --> X[Return]
    B -- yes --> C[Select next size-class cache]
    C --> D{Owner in empty_slabs?}
    D -- yes --> E[Free object and move to partial_slabs if available]
    D -- no --> F{Owner in partial_slabs?}
    F -- yes --> G[Free object]
    G --> H{All objects available?}
    H -- yes --> I[Move slab to full_slabs]
    H -- no --> J{More caches?}
    I --> J
    E --> J
    F -- no --> J
    J -- yes --> C
    J -- no --> X
```

## VFS mount and open flow

```mermaid
flowchart TD
    subgraph VFS Mount and Open Flow
        A0[kmain] --> A1[MountRootFileSystem cpio source]
        A1 --> A2[Resource MountFilesystem]
        A2 --> A3[Resource GetRootNode]
        A3 --> A4[wrap backend root into inode]
        A4 --> A5[create slash dentry]
        A5 --> A6[create root mount]
        A6 --> A7[install namespace root mount]

        B0[Open start path flags] --> B1[ResolvePath]
        B1 --> B2{path component}
        B2 -->|dot| B1
        B2 -->|dotdot| B3[parent walk or mount backtrack]
        B3 --> B1
        B2 -->|name| B4[dentry cache lookup]
        B4 --> B5{cache hit}
        B5 -->|yes| B6[use cached dentry]
        B5 -->|no| B7[backend Lookup and cache]
        B6 --> B8[child mount check]
        B7 --> B8
        B8 --> B1

        B1 --> B9[resolved inode]
        B9 --> B10[allocate file struct]
        B10 --> B11[bind file ops]
        B11 --> B12[optional backend Open]
        B12 --> B13[Read Write Seek via backend]
        B13 --> B14[Retain Close refcount lifecycle]
    end
```

## Kernel architecture responsibilities

SVG: `kernel-architecture.svg`

```mermaid
flowchart TB
    accTitle: Arx architectural responsibilities
    accDescr: Dispatcher routes requests into Request. Request uses Logic and Resource, Logic uses Resource, and Resource uses platform services backed by Arch. Klib is shared throughout the kernel.
    D[Dispatcher: entry bridge] --> Q[Request: handle incoming requests]
    subgraph STACK[Layer stack]
        Q --> L[Logic: behavior and policy]
        Q --> R[Resource: objects and mechanisms]
        L --> R
    end
    R --> P[Platform: hardware interface]
    P --> A[Arch: machine-specific operations]
    K[Klib: shared utilities available to every kernel area]
```

## Kernel initialization order

SVG: `kernel-startup.svg`

```mermaid
flowchart TB
    accTitle: Arx initialization order
    accDescr: Platform setup and CPU startup precede Dispatcher and layer construction. Scheduling enables the final integration and Request tests.
    A[Arch entry reads boot information] --> P[Platform prepares hardware and memory]
    P --> S[Start other CPUs and run platform tests]
    S --> C[Kernel stacks and CPU rendezvous]
    C --> D[kmain creates Dispatcher]
    D --> R[Create Resource, then run its tests]
    R --> L[Create Logic using Resource, then run its tests]
    L --> Q[Create Request using Resource and Logic]
    Q --> F[Mount root and queue per-CPU tasks]
    F --> G[Release waiting CPUs and activate scheduling]
    G --> T[Scheduled post-init and Request tests]
```

## Kernel request flow

SVG: `kernel-request-flow.svg`

```mermaid
flowchart TB
    accTitle: Incoming kernel request flow
    accDescr: Architecture entry sends syscalls and scheduling requests through Dispatcher to Request handlers, which use Logic or Resource services.
    A[Arch entry: syscall or scheduling interrupt] --> D[Dispatcher selects a handler]
    D --> Q[Request manager interprets arguments]
    Q --> L[Logic coordinates kernel behavior]
    Q --> R[Resource operates on kernel objects]
    L --> R
    R --> P[Platform and Arch provide machine services]
```

## x86_64 initialization

SVG: `x86_64-initialization.svg`

```mermaid
flowchart TB
    accTitle: x86_64 architecture initialization
    accDescr: The bootstrap CPU establishes platform prerequisites and shared controller information before starting secondary CPUs and handing off to kernel startup.
    A[Arch entry: boot data and baseline floating-point state] --> P[Platform: CPU records, memory and early ACPI]
    P --> B[BSP arch_init: PAT check and MADT discovery]
    B --> C[Per-CPU GDT, TSS and syscall MSRs]
    C --> I[IDT, LAPIC and bootstrap timer; enable interrupts]
    I --> D[BSP: devices, DMA and AP launch]
    D --> AP[APs repeat per-CPU architecture setup]
    AP --> T[BSP waits for initialized CPUs and runs platform tests]
    T --> S[Kernel stacks and CPU rendezvous]
    S --> K[BSP enters kmain; APs wait for scheduling]
```

## x86_64 interrupt entry and return

SVG: `x86_64-interrupt-flow.svg`

```mermaid
flowchart TB
    accTitle: x86_64 interrupt entry and return
    accDescr: Interrupt stubs save registers and floating-point state, call the C handler, and restore the saved return frame. User return also considers signals.
    A[Interrupt or exception] --> B[CPU and ISR stub build vector, error and return state]
    B --> C[If entry was from userspace: swapgs]
    C --> D[Save general registers and aligned FXSAVE image]
    D --> E[ISRHANDLER: account time and handle event]
    E --> F{Handled successfully?}
    F -- no --> H[Log fatal event and halt]
    F -- yes --> U{Returning to userspace?}
    U -- yes --> S[Guard preemption; enable interrupts for signal work]
    S --> V[Prepare pending signals and restore interrupt guard]
    V --> R[Restore floating-point and general registers]
    U -- no --> R
    R --> G[If returning to userspace: swapgs]
    G --> X[iretq restores execution]
```

## x86_64 syscall entry and return

SVG: `x86_64-syscall-flow.svg`

```mermaid
flowchart TB
    accTitle: x86_64 syscall entry and return
    accDescr: Syscall entry changes to the kernel stack, saves a frame, dispatches the request, handles signals, and returns with iretq.
    A[Userspace executes syscall] --> B[Enter LSTAR with unsafe flags masked]
    B --> C[swapgs; save user RSP and select kernel stack]
    C --> D[Build syscall frame and save floating-point state]
    D --> E[Set syscall active; enable interrupts]
    E --> F[arch_syscall_dispatch]
    F --> G{Handled by fixture hook?}
    G -- yes --> R[Return fixture result]
    G -- no --> H[Dispatcher and Request manager handle syscall]
    H --> I[Account, service pending scheduling and deliver signals]
    I --> J[Disable interrupts and clear syscall active]
    R --> J
    J --> K[Restore floating-point and saved registers]
    K --> L[swapgs and iretq]
    L --> U[Resume user code or signal handler]
```

## AArch64 initialization boundary

SVG: `aarch64-initialization.svg`

```mermaid
flowchart TB
    accTitle: AArch64 existing startup code and runtime boundary
    accDescr: Existing boot and platform code can reach arch_init, but arch_init returns false and bootstrap panics. Normal kernel scheduling is not implemented by these stubs.
    A[Existing entry: MMU bit, FP access and PL011 serial] --> B[Convert Limine boot information]
    B --> C[Enter shared platform_init]
    C --> D[Attempt platform prerequisites]
    D --> E[If prerequisites succeed: call arch_init]
    E --> F[arch_init returns false]
    F --> G[Bootstrap reports architecture failure and panics]
    G -. normal runtime not reached .-> X[Layer construction, scheduling and user execution unavailable]
```
