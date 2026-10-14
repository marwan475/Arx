#include <arch/arch.h>
#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
#include <stddef.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

static volatile uint8_t* hpet;
static uint64_t          counter_origin;
static uint64_t          period_fs;
static uint8_t           cmos_index;

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
        return UINT32_MAX;
    return binary ? value : (value >> 4) * 10 + (value & 15);
}

static bool leap_year(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static bool read_rtc(uint64_t* seconds)
{
    uint8_t     century_index = 0;
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_FADT_SIGNATURE, &table) == UACPI_STATUS_OK)
    {
        if (table.hdr->length >= offsetof(struct acpi_fadt, century) + 1)
            century_index = ((struct acpi_fadt*) table.hdr)->century;
        uacpi_table_unref(&table);
    }
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
            current[i] = cmos_read(indices[i]);
        current[8] = century_index != 0 ? cmos_read(century_index) : 0;
        if (cmos_read(0x0a) & 0x80)
            continue;
        if (memcmp(previous, current, sizeof(current)) != 0)
        {
            memcpy(previous, current, sizeof(current));
            continue;
        }
        if (!(current[7] & 0x80) || (current[6] & 0x80))
            return false;
        bool     binary = (current[6] & 4) != 0;
        unsigned second = rtc_number(current[0], binary);
        unsigned minute = rtc_number(current[1], binary);
        unsigned hour   = rtc_number(current[2] & 0x7f, binary);
        if (!(current[6] & 2))
        {
            if (hour == 0 || hour > 12)
                return false;
            hour = hour % 12 + ((current[2] & 0x80) ? 12 : 0);
        }
        unsigned day     = rtc_number(current[3], binary);
        unsigned month   = rtc_number(current[4], binary);
        unsigned year    = rtc_number(current[5], binary);
        unsigned century = century_index != 0 ? rtc_number(current[8], binary) : 20;
        if (year > 99 || century > 99)
            return false;
        year += century * 100;
        static const unsigned days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > days_per_month[month - 1] + (month == 2 && leap_year(year)) || hour > 23 || minute > 59 || second > 59)
            return false;
        uint64_t days = 0;
        for (unsigned y = 1970; y < year; ++y)
            days += leap_year(y) ? 366 : 365;
        for (unsigned m = 1; m < month; ++m)
            days += days_per_month[m - 1] + (m == 2 && leap_year(year));
        *seconds = ((days + day - 1) * 24 + hour) * 3600 + minute * 60 + second;
        return true;
    }
    return false;
}

bool arch_time_init(uint64_t* realtimeSeconds, bool* realtimeAvailable, uint64_t* resolutionNanoseconds)
{
    uacpi_table table;
    if (uacpi_table_find_by_signature(ACPI_HPET_SIGNATURE, &table) != UACPI_STATUS_OK)
        return false;
    uint64_t address = 0;
    if (table.hdr->length >= sizeof(struct acpi_hpet))
    {
        struct acpi_hpet* descriptor = (struct acpi_hpet*) table.hdr;
        if (descriptor->address.address_space_id == 0)
            address = descriptor->address.address;
    }
    uacpi_table_unref(&table);
    if (address == 0 || (address & 7) || (address & (PAGE_SIZE - 1)) > PAGE_SIZE - 0xf8)
        return false;
    cpu_info_t* cpu  = &platform.cpus[arch_cpu_id()];
    phys_addr_t page = align_down(address, PAGE_SIZE);
    virt_addr_t base = vmm_map_direct_mmio_page(page, cpu->address_space, false);
    if (vmm_virt_to_phys(base, cpu->address_space) != page)
        return false;
    volatile uint8_t* registers    = (volatile uint8_t*) (uintptr_t) (base + (address - page));
    uint64_t          capabilities = REG(uint64_t, registers);
    uint64_t          period       = capabilities >> 32;
    if (!(capabilities & (1ULL << 13)) || period == 0 || period > 100000000)
        return false;
    REG(uint64_t, registers + 0x10) |= 1;
    uint64_t initial   = REG(uint64_t, registers + 0xf0);
    bool     advancing = false;
    for (unsigned i = 0; i < 100000; ++i)
    {
        if (REG(uint64_t, registers + 0xf0) != initial)
        {
            advancing = true;
            break;
        }
        arch_pause();
    }
    if (!advancing)
        return false;
    __asm__ volatile("inb $0x70, %0" : "=a"(cmos_index));
    // RTC is interpreted as UTC; no timezone-setting syscall is implemented.
    *realtimeAvailable = read_rtc(realtimeSeconds);
    __asm__ volatile("outb %0, $0x70" : : "a"(cmos_index));
    counter_origin         = REG(uint64_t, registers + 0xf0);
    period_fs              = period;
    hpet                   = registers;
    *resolutionNanoseconds = (period + 999999) / 1000000;
    return true;
}

bool arch_time_read_monotonic(uint64_t* nanoseconds)
{
    if (hpet == NULL || nanoseconds == NULL)
        return false;
    uint64_t ticks    = REG(uint64_t, hpet + 0xf0) - counter_origin;
    uint64_t whole    = ticks / 1000000;
    uint64_t fraction = (ticks % 1000000) * period_fs / 1000000;
    if (whole > (UINT64_MAX - fraction) / period_fs)
        return false;
    *nanoseconds = whole * period_fs + fraction;
    return true;
}
