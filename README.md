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

![Dispatcher Access Flow](docs/dispatcher-2026-04-15-070216.svg)
- Global dispatcher variable which stores important global and per cpu data structures
- index by arch cpu id

### Memory Management

Physical memory managment

![NUMA Node Allocation Flow](docs/NUMA%20Node%20Allocation%20Flow-2026-04-15-064600.svg)
- Zone is built from boot memory map
- Currently only one Numa node and zone but gives us space to scale into suporting Non Uniform Memory Access and different memory zones

Allocations | pmm_alloc(size)

![PMM Allocation Flow](docs/pmmalloc-2026-04-15-065023.svg)


Frees | pmm_free(addr)

![PMM Free Flow](docs/pmmfree-2026-04-15-065114.svg)


Virtual memory managment
- page table managment apis (map/unmap/protect)
- each arch implements paging api
- also implement arch_map/unmap/protect_range functions
    - optimization to avoid redundant page table walks , cache flushes and batching flushes
    - instead of calling arch_map/unmap/protect_page on each page in a range which would be much slower
- vmm paging apis are wrappers over arch specific implmentation that lock on address space

Virtual address space management
- uses canonical addressing
- on vmm init we init the initial address space by subtracting hhdm, framebuffer, and kernel form the kernel address space

Virtual address space structure

![VMM addrspace](docs/vaddr-2026-04-15-074940.svg)

Reserve region in address space

![VMM reserve](docs/vmmreserve-2026-04-15-072825.svg)

Free region in address space

![VMM free](docs/vmmfree-2026-04-15-072859.svg)

    
Klib allocations
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
