# Clock sources and time semantics

On x86-64, boot selects an advancing HPET first, then an ACPI PM timer. Missing,
invalid and stuck candidates are skipped before publication. HPET accepts a
validated MMIO register block and a 32-bit or 64-bit main counter. The PM backend
accepts a legacy FADT port or an extended system-I/O descriptor with a 32-bit
access, and uses the FADT's 24-bit/32-bit counter flag. Unsupported extended
descriptors and hardware-reduced fixed registers are rejected. The fixed PM
frequency is 3,579,545 Hz, as specified by the
[ACPI hardware specification](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html).

Source configuration and the initial counter epoch are published together with
release/acquire ordering. Every CPU extends the same elapsed-tick epoch using
CAS, re-reading hardware after contention. A separate atomic maximum prevents a
delayed conversion from publishing an older timestamp. Unsigned modular deltas
handle 24-, 32- and 64-bit counter rollover. Conversion splits whole and fractional
ticks before multiplication; it retains the original rational frequency rather
than accumulating rounded nanoseconds per tick. Tick and conversion overflow
fail without overwriting the caller's output.

The BSP scheduler interrupt samples narrow counters even when no clock syscalls
or sleepers exist. Sampling must occur at least once per full counter period;
the shortest supported period is about 4.69 seconds for a 24-bit PM timer. The
ordinary scheduler timer is reprogrammed for 10 ms and its calibration uses the
selected source. Firmware stalls, disabled interrupts or VM pauses spanning an
entire hardware wrap cannot be reconstructed by a narrow counter alone.

RTC discovery is independent of monotonic source selection. The FADT no-CMOS-RTC
flag avoids CMOS probing; invalid/unavailable calendar data leaves UTC unavailable.
The existing UTC interpretation and second-resolution calendar anchor remain.
No UTC epoch is fabricated when the RTC is absent.

| Clock ID | Reads and resolution | Sleep deadlines |
| --- | --- | --- |
| 0, REALTIME | RTC epoch plus selected monotonic time; ENOSYS without RTC | Relative uses monotonic time without RTC; absolute requires RTC |
| 1, MONOTONIC | Selected source | Relative and absolute |
| 4, MONOTONIC_RAW | Same unadjusted hardware epoch; there is no frequency discipline | EINVAL |
| CPU clocks, coarse clocks, BOOTTIME, TAI, alarm clock IDs, dynamic IDs | EINVAL | EINVAL |

The supported read and sleep distinction follows the Linux
[clock interfaces](https://man7.org/linux/man-pages/man2/clock_gettime.2.html) and
[clock_nanosleep interface](https://man7.org/linux/man-pages/man2/clock_nanosleep.2.html).
CPU timer accounting already exists, but process totals do not provide a coherent
snapshot including live execution on all CPUs, and there are no per-thread totals.
CPU clock IDs remain rejected. BOOTTIME needs suspend accounting; TAI needs a
maintained UTC/TAI offset. These clocks are not aliases for elapsed wall time.

Request decoding and user copies remain in TimeRequestManager; TimeManager owns
clock validation, normalization, overflow checks and deadline conversion. The
existing scheduler consumes the same selected platform monotonic API for sleeping,
timeout wakeups and real timer expiration. Alarm replacement, cancellation and
periodic timer rearming use that epoch. Relative interrupted sleeps return a
normalized remaining interval; absolute sleeps leave the remainder untouched.
TimeManager's backend constructor allows isolated failure/deadline/alarm tests
without reinitializing or replacing the live platform clock. No manager was added.

## Regression coverage and validation

TimeManager resource tests cover absent/stuck candidates and fallback selection,
all-source failure, exact PM conversion, fractional HPET conversion, overflow,
24/32/64-bit rollover and a deterministic nested-reader CAS race. Isolated backend
tests cover missing RTC, failed reads, resolution, relative/absolute UTC conversion,
unsupported IDs and alarm arithmetic. Request tests cover RAW ABI access, missing
RTC output preservation, timer-produced EINTR for relative and absolute sleeps,
normalized remainders, and ignored absolute remainder pointers. Existing time and
CPU timer tests remain.

Only source review and `git diff --check` were performed. Builds, selftests and
hardware execution were not run under the local-execution restriction. An external
test run should boot HPET enabled/disabled, PM-only and no-counter configurations,
plus missing/invalid RTC fixtures; run the existing resource/request suites on
each viable source, and stress concurrent clock readers on multiple CPUs across
PM rollover. The synthetic race test does not replace that hardware SMP run.

## Architecture and hardware gaps

- AArch64 still reports ENOSYS: its architectural generic-timer backend and timer
  interrupt integration are not implemented in this tree.
- PM timer MMIO GAS descriptors are explicitly unsupported by this x86 backend.
- No TSC fallback is advertised without frequency discovery and cross-CPU
  synchronization validation. PIT and interrupt-count approximations are not
  substituted for a hardware clock.
- Selection occurs at boot. There is no runtime watchdog, source failover,
  hardware reset recovery, suspend/resume rebasing or chipset-specific PM timer
  erratum filtering. A counter that stops after selection can stall deadlines.
- Calendar accuracy remains limited by RTC sampling and firmware configuration;
  setting or disciplining realtime is not implemented.
