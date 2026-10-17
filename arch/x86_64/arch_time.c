#include <arch/arch.h>
#include <klib/clock.h>
#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
#include <stddef.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

static clock_source_t  sources[2];
static clock_source_t* active_source;
static uint8_t         cmos_index;

static uint64_t hpet_read_counter(void* context)
{
    volatile uint8_t* registers = context;
    return REG(uint64_t, registers + 0xf0);
}

static uint64_t hpet_read_counter32(void* context)
{
    volatile uint8_t* registers = context;
    return REG(uint32_t, registers + 0xf0);
}

static uint64_t pm_read_counter(void* context)
{
    uint32_t       value;
    const uint16_t port = (uint16_t) (uintptr_t) context;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t cmos_read(uint8_t index)
{
    uint8_t value;
    uint8_t selector = (index & 0x7f) | (cmos_index & 0x80);
    __asm__ volatile("outb %0, $0x70" : : "a"(selector));
    __asm__ volatile("inb $0x71, %0" : "=a"(value));
    return value;
}

static unsigned rtc_number(uint8_t value, bool binary)
{
    if (!binary && ((value & 15) > 9 || (value >> 4) > 9))
    {
        return UINT32_MAX;
    }

    return binary ? value : (value >> 4) * 10 + (value & 15);
}

static bool leap_year(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static bool read_rtc_calendar(uint64_t* seconds, uint8_t century_index)
{
    // Two identical snapshots outside the update window avoid torn calendar reads.
    const uint8_t indices[]   = {0, 2, 4, 7, 8, 9, 0x0b, 0x0d};
    uint8_t       previous[9] = {0};
    uint8_t       current[9];

    for (unsigned attempt = 0; attempt < 100000; ++attempt)
    {
        if (cmos_read(0x0a) & 0x80)
        {
            arch_pause();
            continue;
        }

        for (unsigned i = 0; i < 8; ++i)
        {
            current[i] = cmos_read(indices[i]);
        }

        current[8] = century_index != 0 ? cmos_read(century_index) : 0;

        if (cmos_read(0x0a) & 0x80)
        {
            continue;
        }

        if (memcmp(previous, current, sizeof(current)) != 0)
        {
            memcpy(previous, current, sizeof(current));
            continue;
        }

        if (!(current[7] & 0x80) || (current[6] & 0x80))
        {
            return false;
        }

        bool     binary = (current[6] & 4) != 0;
        unsigned second = rtc_number(current[0], binary);
        unsigned minute = rtc_number(current[1], binary);
        unsigned hour   = rtc_number(current[2] & 0x7f, binary);

        if (!(current[6] & 2))
        {
            if (hour == 0 || hour > 12)
            {
                return false;
            }

            hour = hour % 12 + ((current[2] & 0x80) ? 12 : 0);
        }

        unsigned day     = rtc_number(current[3], binary);
        unsigned month   = rtc_number(current[4], binary);
        unsigned year    = rtc_number(current[5], binary);
        unsigned century = century_index != 0 ? rtc_number(current[8], binary) : 20;

        if (year > 99 || century > 99)
        {
            return false;
        }

        year += century * 100;
        static const unsigned days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

        if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > days_per_month[month - 1] + (month == 2 && leap_year(year)) || hour > 23 || minute > 59 || second > 59)
        {
            return false;
        }

        uint64_t days = 0;

        for (unsigned y = 1970; y < year; ++y)
        {
            days += leap_year(y) ? 366 : 365;
        }

        for (unsigned m = 1; m < month; ++m)
        {
            days += days_per_month[m - 1] + (m == 2 && leap_year(year));
        }

        *seconds = ((days + day - 1) * 24 + hour) * 3600 + minute * 60 + second;
        return true;
    }

    return false;
}

static bool read_rtc(uint64_t* seconds)
{
    uint8_t     century_index = 0;
    uacpi_table table;

    if (uacpi_table_find_by_signature(ACPI_FADT_SIGNATURE, &table) == UACPI_STATUS_OK)
    {
        if (table.hdr->length >= offsetof(struct acpi_fadt, iapc_boot_arch) + sizeof(uint16_t) && (((struct acpi_fadt*) table.hdr)->iapc_boot_arch & ACPI_IA_PC_NO_CMOS_RTC) != 0)
        {
            uacpi_table_unref(&table);
            return false;
        }

        if (table.hdr->length >= offsetof(struct acpi_fadt, century) + 1)
        {
            century_index = ((struct acpi_fadt*) table.hdr)->century;
        }

        uacpi_table_unref(&table);
    }

    __asm__ volatile("inb $0x70, %0" : "=a"(cmos_index));
    const bool available = read_rtc_calendar(seconds, century_index);
    __asm__ volatile("outb %0, $0x70" : : "a"(cmos_index));
    return available;
}

static bool prepare_hpet(clock_source_t* source)
{
    uacpi_table  table;
    uacpi_status status = uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &table);

    if (status != UACPI_STATUS_OK)
    {
        error_logger("Arx kernel: HPET unavailable: ACPI table lookup status=%u\n", (unsigned) status);
        return false;
    }

    uint64_t address = 0;

    if (table.hdr->length >= sizeof(struct acpi_hpet))
    {
        struct acpi_hpet* descriptor = (struct acpi_hpet*) table.hdr;

        if (descriptor->address.address_space_id == 0)
        {
            address = descriptor->address.address;
        }
    }

    uacpi_table_unref(&table);

    if (address == 0 || (address & 7) || (address & (PAGE_SIZE - 1)) > PAGE_SIZE - 0xf8)
    {
        error_logger("Arx kernel: HPET unavailable: missing/unsupported register address\n");
        return false;
    }

    cpu_info_t* cpu  = &platform.cpus[arch_cpu_id()];
    phys_addr_t page = align_down(address, PAGE_SIZE);
    virt_addr_t base = vmm_map_direct_mmio_page(page, cpu->address_space);

    if (vmm_virt_to_phys(base, cpu->address_space) != page)
    {
        error_logger("Arx kernel: HPET unavailable: MMIO mapping failed\n");
        return false;
    }

    volatile uint8_t* registers    = (volatile uint8_t*) (uintptr_t) (base + (address - page));
    uint64_t          capabilities = REG(uint64_t, registers);
    uint64_t          period       = capabilities >> 32;

    if (period == 0 || period > 100000000)
    {
        error_logger("Arx kernel: HPET unavailable: counter64=%u period_fs=%llu\n", (unsigned) ((capabilities >> 13) & 1), (unsigned long long) period);
        return false;
    }

    const bool wide = (capabilities & (1ULL << 13)) != 0;
    REG(uint64_t, registers + 0x10) |= 1;
    source->read        = wide ? hpet_read_counter : hpet_read_counter32;
    source->context     = (void*) registers;
    source->mask        = wide ? UINT64_MAX : UINT32_MAX;
    source->numerator   = (uint32_t) period;
    source->denominator = 1000000;
    return true;
}

static bool prepare_pm_timer(clock_source_t* source)
{
    uacpi_table table;

    if (uacpi_table_find_by_signature(ACPI_FADT_SIGNATURE, &table) != UACPI_STATUS_OK)
    {
        return false;
    }

    const struct acpi_fadt* fadt     = (const struct acpi_fadt*) table.hdr;
    uint64_t                address = 0;
    bool                    wide    = false;
    bool                    valid   = table.hdr->length >= offsetof(struct acpi_fadt, flags) + sizeof(fadt->flags);

    if (valid)
    {
        address = fadt->pm_tmr_blk;
        wide    = (fadt->flags & ACPI_TMR_VAL_EXT) != 0;
        valid   = fadt->pm_tmr_len == 4 && (fadt->flags & ACPI_HW_REDUCED_ACPI) == 0;

        if (table.hdr->length >= offsetof(struct acpi_fadt, x_pm_tmr_blk) + sizeof(fadt->x_pm_tmr_blk) && fadt->x_pm_tmr_blk.address != 0)
        {
            const struct acpi_gas gas = fadt->x_pm_tmr_blk;
            address                  = gas.address;
            // This backend implements 32-bit system-I/O accesses only. Never
            // reinterpret a firmware MMIO address as a port or use legacy data
            // when a supplied extended descriptor is unsupported.
            valid = valid && gas.address_space_id == 1 && gas.register_bit_offset == 0 && gas.register_bit_width == 32 && (gas.access_size == 0 || gas.access_size == 3);
        }
    }

    uacpi_table_unref(&table);

    if (!valid || address == 0 || address > UINT16_MAX - 3)
    {
        error_logger("Arx kernel: ACPI PM timer unavailable: unsupported register descriptor\n");
        return false;
    }

    source->read        = pm_read_counter;
    source->context     = (void*) (uintptr_t) address;
    source->mask        = wide ? UINT32_MAX : 0xffffff;
    source->numerator   = 1000000000;
    source->denominator = 3579545;
    return true;
}

bool arch_time_init(uint64_t* realtimeSeconds, bool* realtimeAvailable, uint64_t* resolutionNanoseconds)
{
    // Initialization runs once on the BSP. AP interrupt readers see either no
    // source or the completely initialized selection, never a partial epoch.
    *realtimeSeconds       = 0;
    *realtimeAvailable     = false;
    *resolutionNanoseconds = 0;
    (void) prepare_hpet(&sources[0]);
    (void) prepare_pm_timer(&sources[1]);

    // Read the UTC calendar before anchoring the monotonic epoch, regardless
    // of which counter was discovered. RTC absence never disables deadlines.
    *realtimeAvailable = read_rtc(realtimeSeconds);
    clock_source_t* selected = clock_source_select(sources, 2);

    if (selected == NULL)
    {
        error_logger("Arx kernel: neither HPET nor ACPI PM timer advances\n");
        return false;
    }

    *resolutionNanoseconds = ((uint64_t) selected->numerator + selected->denominator - 1) / selected->denominator;
    __atomic_store_n(&active_source, selected, __ATOMIC_RELEASE);
    kprintf("Arx kernel: selected clock source %s\n", selected == &sources[0] ? "HPET" : "ACPI PM timer");
    return true;
}

bool arch_time_read_monotonic(uint64_t* nanoseconds)
{
    return clock_source_read(__atomic_load_n(&active_source, __ATOMIC_ACQUIRE), nanoseconds);
}
