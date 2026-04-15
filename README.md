# Arx
Multiarch 64 bit Kernel in development

Targets:
- x86_64
- aarch64

## Design
Design in order of initialization

### Setup
- Limine Bootloader loads kernel and sets entry point to _start in arch_entry.c
- arch_entry processes Limine requests, formats it to fit Arx boot protocol in boot.h then passes it to kmain in kernel.c
- Arx boot protocol needs 
    - Memory map
    - Framebuffer 
    - Higher half direct memory
    - Paging with no user access and RWX 
    - ACPI rsdp address
    - SMP setup 
- kmain will initialize kernel subsystems

### Dispatcher
- Global dispatcher variable which stores important global and per cpu data structures
- Defined in klib.h and accessiable from anypart of the kernel
- Array of cpu structs defined in cpu.h which is indexed by cores id 
- Core id comes from arch_cpu_id() 
- This allows easy managment of smp cores

### Memory Management

Physical memory managment

```mermaid
graph TD

    subgraph Dispatcher
        CPU1[CPU 1]
        CPU2[CPU 2]
        CPU3[CPU 3]
        CPUN[CPU N]
    end

    CPU1 --> NUMA
    CPU2 --> NUMA
    CPU3 --> NUMA
    CPUN --> NUMA

    NUMA[NUMA Node]
    NUMA --> ZONE[Zone]
    ZONE --> BUDDY[Buddy Allocator]
```
- Zone is built from boot memory map
- Currently only one Numa node and zone but gives us space to scale into suporting Non Uniform Memory Access and different memory zones

Allocations
```mermaid
flowchart TD
    subgraph Alloc
        K[bytes to pages to order] --> L[lock zone]
        L --> M[buddy alloc]
        M --> N{higher order block}
        N -- yes --> O[split until requested order]
        N -- no --> P[use block]
        O --> Q[update free pages and used pages]
        P --> Q
        Q --> R[unlock zone]
        R --> S[return HHDM virtual address]
    end
```
- pmm_alloc(size)

Frees
```mermaid
flowchart TD
    subgraph Free
        T[HHDM VA to PA to PFN] --> U[lock zone]
        U --> V[buddy free]
        V --> W{buddy free and same order}
        W -- yes --> X[merge and retry]
        W -- no --> Y[insert block in free list]
        X --> W
        Y --> Z[update free pages and used pages]
        Z --> AA[unlock zone]
    end
```
- pmm_free(addr)
_
- Virtual memory managment (vmm.c)
    - page table managment
    - maping and unmaping pages
    - changing page flags
    - each arch implements paging api in arch_paging.c
    - also implement arch_map/unmap/protect_range functions
        - optimization to avoid redundant page table walks , cache flushes and batching flushes
        - instead of calling arch_map/unmap/protect_page on each page in a range which would be much slower
    - vmm_map/unmap/protect_page/range are wrappers over arch specific implmentation that lock on address space
    - pagetable is stored in virt_addr_space defined in vmm.h
- Virtual address space management (vmm.c)
    - uses canonical addressing
    - address space stores free lists of usable virt_region
    - user and kernel free lists 
    - on vmm init we init the initial address space by subtracting hhdm, framebuffer, and kernel form the kernel address space
    - expose vmm_reserve_region and vmm_free_region api which allows you to reserve a virtual memory region to map physical pages
        - locks on virtual address space access/editing
    - store initial address space in each cpu in dispatcher
- Klib allocations
    - vmalloc and vfree
    - uses pmm and vmm api to allocate large contiguous virtual memory chunks
    - slow due to needing to map pages

### Arch Init
ran on each smp core
- x86_64
    - build and install 64 bit GDT for kernel and user segments including a TSS
    - build and install IDT all isrs call a common isr handler and jumps to c code ISRHANDLER passing regs
    - get mdat from acpi using uacpi
    - get per core lapic info from mdat
    - init lapic per core
    - init per core lapic timer
    - init global ioapic (masking all entries)
    - get iso overides from mdat
    - route legacy irqs to bsp lapic
    - expose api to mask/unmask vectors, register new vectors, and route vectors

## Third Party
- [Limine](https://github.com/limine-bootloader/limine) - Bootloader/protocol used to load the kernel and provide boot info.
- [printf](https://github.com/mpaland/printf) - Small freestanding printf implementation used for kernel logging output.
- [uACPI](https://github.com/uACPI/uACPI) - ACPI implementation used for table parsing and ACPI support.

All third-party components retain their original licenses.

## AI Usage
- Build system and scripts are AI generated
- selftest.c is AI generated testing of kernel subsystems
