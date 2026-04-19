#include <arch/arch.h>
#include <klib/klib.h>

void blue_screen(void)
{
    const kernel_framebuffer_t* fb = &dispatcher.framebuffer;

    if (fb->address == NULL || fb->width == 0 || fb->height == 0 || fb->pitch == 0)
    {
        return;
    }

    size_t bytes_per_pixel = fb->pitch / fb->width;
    if (bytes_per_pixel == 0)
    {
        bytes_per_pixel = 4;
    }

    uint32_t pixel = 0;
    if (fb->blue_mask_size >= 32)
    {
        pixel = 0xFFFFFFFFu;
    }
    else if (fb->blue_mask_size > 0)
    {
        pixel = ((1u << fb->blue_mask_size) - 1u) << fb->blue_mask_shift;
    }

    uint8_t* base = (uint8_t*) fb->address;

    for (size_t y = 0; y < fb->height; y++)
    {
        uint8_t* row = base + (y * fb->pitch);
        for (size_t x = 0; x < fb->width; x++)
        {
            uint8_t* px = row + (x * bytes_per_pixel);
            for (size_t b = 0; b < bytes_per_pixel; b++)
            {
                px[b] = (uint8_t) ((pixel >> (8 * b)) & 0xFFu);
            }
        }
    }
}

static void pagefault_debug(registers_t* reg)
{
    uint64_t cr2;
    uint64_t cr3;
    uint64_t error_code;

    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    error_code = reg->error_code;

    kprintf("Arx kernel: PAGE FAULT DEBUG\n");
    kprintf("  cr2 (fault addr): 0x%llx\n", (unsigned long long) cr2);
    kprintf("  cr3 (pt base):    0x%llx\n", (unsigned long long) cr3);
    kprintf("  rip:              0x%llx\n", (unsigned long long) reg->rip);
    kprintf("  rsp:              0x%llx\n", (unsigned long long) reg->rsp);
    kprintf("  rflags:           0x%llx\n", (unsigned long long) reg->rflags);
    kprintf("  cs:ss:            0x%llx:0x%llx\n", (unsigned long long) reg->cs, (unsigned long long) reg->ss);
    kprintf("  error_code:       0x%llx\n", (unsigned long long) error_code);
    kprintf("  error bits: P=%u W/R=%u U/S=%u RSVD=%u I/D=%u PK=%u SS=%u SGX=%u\n", (unsigned) ((error_code >> 0) & 1), (unsigned) ((error_code >> 1) & 1), (unsigned) ((error_code >> 2) & 1), (unsigned) ((error_code >> 3) & 1), (unsigned) ((error_code >> 4) & 1), (unsigned) ((error_code >> 5) & 1),
            (unsigned) ((error_code >> 6) & 1), (unsigned) ((error_code >> 15) & 1));

    kprintf("  regs: rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx\n", (unsigned long long) reg->rax, (unsigned long long) reg->rbx, (unsigned long long) reg->rcx, (unsigned long long) reg->rdx);
    kprintf("        rsi=0x%llx rdi=0x%llx rbp=0x%llx\n", (unsigned long long) reg->rsi, (unsigned long long) reg->rdi, (unsigned long long) reg->rbp);
    kprintf("        r8 =0x%llx r9 =0x%llx r10=0x%llx r11=0x%llx\n", (unsigned long long) reg->r8, (unsigned long long) reg->r9, (unsigned long long) reg->r10, (unsigned long long) reg->r11);
    kprintf("        r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n", (unsigned long long) reg->r12, (unsigned long long) reg->r13, (unsigned long long) reg->r14, (unsigned long long) reg->r15);
}

static void ipi_invalidate_tlb(const ipi_request_data_t* req)
{
    if (req->tlb_invalidation.tlb_invalidation_type == IPI_TLB_INVALIDATE_SINGLE_PAGE)
    {
        x86_64_invlpg(req->tlb_invalidation.va_start);
    }
    else if (req->tlb_invalidation.requires_page_flush)
    {
        const uint64_t range_end = req->tlb_invalidation.va_start + req->tlb_invalidation.size;
        for (virt_addr_t va = req->tlb_invalidation.va_start; va < range_end; va += PAGE_SIZE)
        {
            x86_64_invlpg(va);
        }
    }
    else
    {
        x86_64_flush_active_tlb_non_global();
    }
}

static void ipi_broadcast_exception(void)
{
    const uint8_t source_cpu_id = arch_cpu_id();

    for (uint8_t cpu_id = 0; cpu_id < dispatcher.cpu_count; cpu_id++)
    {
        if (cpu_id == source_cpu_id)
        {
            continue;
        }

        cpu_info_t* target_cpu_info = &dispatcher.cpus[cpu_id];
        if (!target_cpu_info->initialized || !target_cpu_info->arch_info.acpi_has_lapic)
        {
            continue;
        }

        send_ipi(cpu_id, (uint8_t) IPI_REQUEST_EXCEPTION, NULL);
    }
}

// cpus ipi is locked when this is called. so handler needs to unlock it
void handle_ipi(registers_t* reg)
{
    cpu_info_t*        cpu_info     = &dispatcher.cpus[arch_cpu_id()];
    ipi_request_type_t request_type = IPI_REQUEST_NONE;

    if (!cpu_info->arch_info.acpi_has_lapic)
    {
        return;
    }

    if (reg->interrupt_number != LAPIC_IPI_VECTOR)
    {
        return;
    }

    request_type = cpu_info->ipi_request_data.type;

    if (request_type == IPI_REQUEST_INVALIDATE_TLB)
    {
        const ipi_request_data_t* req = &cpu_info->ipi_request_data;
        ipi_invalidate_tlb(req);
    }
    else if (request_type == IPI_REQUEST_EXCEPTION)
    {
        cpu_info->ipi_request_data.type = IPI_REQUEST_NONE;
        memset(&cpu_info->ipi_request_data, 0, sizeof(cpu_info->ipi_request_data));
        spinlock_release(&cpu_info->ipi_lock);

        blue_screen();
        panic();
    }

    cpu_info->ipi_request_data.type = IPI_REQUEST_NONE;
    memset(&cpu_info->ipi_request_data, 0, sizeof(cpu_info->ipi_request_data));
    spinlock_release(&cpu_info->ipi_lock);
}

void ISRHANDLER(registers_t* reg)
{
    if (reg->interrupt_number >= 32)
    {
        lapic_eoi();
    }

    if (reg->interrupt_number == LAPIC_TIMER_VECTOR)
    {
        return;
    }

    if (reg->interrupt_number == LAPIC_IPI_VECTOR)
    {
        handle_ipi(reg);
        return;
    }

    if (reg->interrupt_number < 32)
    {
        ipi_broadcast_exception();
        blue_screen();
    }

    if (reg->interrupt_number == 14)
    {
        pagefault_debug(reg);
    }

    kprintf("Arx kernel: received interrupt: %llu\n", (unsigned long long) reg->interrupt_number);
    panic();
}
