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
- Physical memory management init (pmm.c)
    - memory map is passed into pmm_init
    - using memory map we create region discriptors of usable memory regions
    - these regions are placed in a zone struct
    - we then create metadata for the memory zone which contains a page_t struct for every page in the memory zone
    - using the region discriptors and metadata we seed the zones buddy free lists
    - max order is currently 10 with a block size of 4096
    - zone is placed in a numa node, this allows use to support Non uniform memory access in the future
    - store pointer to numa node in each cpu in dispatcher
- Physical memory management allocations (pmm.c)
    - for allocations we have a buddy allocator
    - we request an allocation using an order buddy_alloc(order)
    - on alloc we find the smallest availble block to satisfy order
        - if the block is larger then request we split it
    - on free we place it back in free list and if its buddy exists we merge them
    - basic buddy allocator algorithm (Very paraphrased)
    - pmm_alloc and pmm_free are wrappers over buddy allocation functions
        - locks when accessing/editing memory zone 
        - auto bytes -> pages -> order conversion
        - auto pa -> hhdm and hhdm -> pa conversions
    - because of our dispatcher and zone buddy metadata allows the api to be pmm_alloc(size) and pmm_free(addr)
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
