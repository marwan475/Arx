#include <acpi/host.h>
#include <acpi/runtime.h>
#include <platform.h>
#include <selftests/TestCase.hpp>

struct AcpiHostFixture
{
    uacpi_handle mutex;
    uacpi_handle start;
    uacpi_handle finished;
    uacpi_thread_id caller;
    bool timedOut;
    bool acquired;
    bool independentThread;
    bool onBsp;
    bool selfDrainDenied;
    uint32_t executions;
};

static void exercise_host_wait(void* argument)
{
    AcpiHostFixture* fixture = static_cast<AcpiHostFixture*>(argument);
    fixture->independentThread = uacpi_kernel_get_thread_id() != fixture->caller;
    fixture->onBsp = arch_cpu_id() == platform.bsp_id;
    fixture->selfDrainDenied = uacpi_kernel_wait_for_work_completion() == UACPI_STATUS_DENIED;

    if (uacpi_kernel_wait_for_event(fixture->start, 1000))
    {
        fixture->timedOut = uacpi_kernel_acquire_mutex(fixture->mutex, 10) == UACPI_STATUS_TIMEOUT;
    }

    __atomic_add_fetch(&fixture->executions, 1, __ATOMIC_RELEASE);
    uacpi_kernel_signal_event(fixture->finished);
}

static void exercise_host_mutex(void* argument)
{
    AcpiHostFixture* fixture = static_cast<AcpiHostFixture*>(argument);
    fixture->acquired = uacpi_kernel_acquire_mutex(fixture->mutex, 1000) == UACPI_STATUS_OK;

    if (fixture->acquired)
    {
        uacpi_kernel_release_mutex(fixture->mutex);
    }

    __atomic_add_fetch(&fixture->executions, 1, __ATOMIC_RELEASE);
    uacpi_kernel_signal_event(fixture->finished);
}

void test_acpi_host(bool scheduled)
{
    TestCase test(scheduled ? "platform/acpi/scheduled_host" : "platform/acpi/bootstrap_host");

    if (platform_acpi_state().stage != ACPI_STAGE_READY)
    {
        test.Skip("runtime unavailable");
        return;
    }

    AcpiHostFixture fixture {};
    fixture.mutex = uacpi_kernel_create_mutex();
    fixture.start = uacpi_kernel_create_event();
    fixture.finished = uacpi_kernel_create_event();
    fixture.caller = uacpi_kernel_get_thread_id();

    if (!test.Check(fixture.mutex != nullptr && fixture.start != nullptr && fixture.finished != nullptr, "host synchronization objects allocated"))
    {
        uacpi_kernel_free_mutex(fixture.mutex);
        uacpi_kernel_free_event(fixture.start);
        uacpi_kernel_free_event(fixture.finished);
        return;
    }

    test.Check(uacpi_kernel_acquire_mutex(fixture.mutex, 0) == UACPI_STATUS_OK, "uncontended mutex acquired");
    uint64_t state = arch_save_and_disable_interrupts();
    uacpi_status status = uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, exercise_host_wait, &fixture);
    bool deferred = __atomic_load_n(&fixture.executions, __ATOMIC_ACQUIRE) == 0;
    arch_restore_interrupts(state);
    test.Check(status == UACPI_STATUS_OK, "GPE work can be queued with interrupts disabled");
    test.Check(deferred, "schedule_work never executes callbacks inline");
    uacpi_kernel_signal_event(fixture.start);
    test.Check(uacpi_kernel_wait_for_event(fixture.finished, 2000), "independent worker completes while caller waits");
    test.Check(fixture.timedOut, "a mutex held by another context times out");
    test.Check(fixture.independentThread && fixture.onBsp, "deferred work has independent BSP execution identity");
    test.Check(fixture.selfDrainDenied, "a worker cannot deadlock waiting for its own completion");
    uacpi_kernel_release_mutex(fixture.mutex);

    status = uacpi_kernel_schedule_work(UACPI_WORK_NOTIFICATION, exercise_host_mutex, &fixture);
    test.Check(status == UACPI_STATUS_OK, "notification work can be queued");
    test.Check(uacpi_kernel_wait_for_event(fixture.finished, 2000), "notification callback completes");
    test.Check(uacpi_kernel_wait_for_work_completion() == UACPI_STATUS_OK, "work completion drains active callbacks");
    test.Check(fixture.acquired, "released mutex can be acquired by a worker");
    test.Equal(fixture.executions, 2, "each deferred callback runs once");

    uacpi_kernel_signal_event(fixture.start);
    uacpi_kernel_signal_event(fixture.start);
    test.Check(uacpi_kernel_wait_for_event(fixture.start, 0), "first signal is consumed");
    test.Check(uacpi_kernel_wait_for_event(fixture.start, 0), "second signal is consumed");
    test.Check(!uacpi_kernel_wait_for_event(fixture.start, 0), "an empty event never succeeds");
    uacpi_kernel_signal_event(fixture.start);
    uacpi_kernel_reset_event(fixture.start);
    test.Check(!uacpi_kernel_wait_for_event(fixture.start, 0), "reset clears event count");

    uacpi_kernel_free_mutex(fixture.mutex);
    uacpi_kernel_free_event(fixture.start);
    uacpi_kernel_free_event(fixture.finished);

    uint8_t* large = static_cast<uint8_t*>(uacpi_kernel_alloc(HEAP_MAX_OBJECT_SIZE + 4096));

    if (test.Check(large != nullptr, "large interpreter allocations use virtual backing"))
    {
        large[0] = 0x5a;
        large[HEAP_MAX_OBJECT_SIZE + 4095] = 0xa5;
        test.Check(large[0] == 0x5a && large[HEAP_MAX_OBJECT_SIZE + 4095] == 0xa5, "large allocation is fully mapped");
        uacpi_kernel_free(large);
    }

    test.Check(uacpi_kernel_alloc(SIZE_MAX) == nullptr, "allocation overflow is rejected");

    uacpi_handle io = nullptr;
    test.Check(uacpi_kernel_io_map(0xffff, 2, &io) == UACPI_STATUS_INVALID_ARGUMENT, "port range overflow is rejected");
    test.Check(uacpi_kernel_io_map(0x80, 1, &io) == UACPI_STATUS_OK, "bounded SystemIO range can be represented");
    test.Check(uacpi_kernel_io_write16(io, 0, 0) == UACPI_STATUS_INVALID_ARGUMENT, "overwide access is rejected before touching hardware");
    uacpi_kernel_io_unmap(io);

    uacpi_pci_address address {};
    uacpi_handle pci = nullptr;
    test.Check(uacpi_kernel_pci_device_open(address, &pci) == UACPI_STATUS_OK, "PCI configuration handle can be opened");
    uint32_t dword = 0;
    uint16_t word = 0;
    uint8_t byte = 0;
    test.Check(uacpi_kernel_pci_read32(pci, 0, &dword) == UACPI_STATUS_OK, "PCI DWORD configuration read succeeds");
    test.Check(uacpi_kernel_pci_read16(pci, 0, &word) == UACPI_STATUS_OK && word == (dword & 0xffff), "PCI WORD access preserves width and offset");
    test.Check(uacpi_kernel_pci_read8(pci, 1, &byte) == UACPI_STATUS_OK && byte == ((dword >> 8) & 0xff), "PCI BYTE access preserves width and offset");
    test.Check(uacpi_kernel_pci_read16(pci, 4095, &word) == UACPI_STATUS_INVALID_ARGUMENT, "configuration boundary crossing is rejected");
    uacpi_kernel_pci_device_close(pci);
}

void test_acpi_event_queue(void)
{
    TestCase test("platform/acpi/event_queue");

    if (platform_acpi_state().stage != ACPI_STAGE_READY)
    {
        test.Skip("runtime unavailable");
        return;
    }

    acpi_event_t output {};
    uint64_t lost = 0;

    while (platform_acpi_next_event(&output, &lost, 0) == ACPI_SUCCESS)
    {
    }

    uint64_t originalLost = lost;

    for (uint64_t i = 0; i < 129; i++)
    {
        acpi_event_t input {};
        input.kind = ACPI_EVENT_NOTIFY;
        input.value = i;
        acpi_host_publish_event(&input);
    }

    for (uint64_t i = 0; i < 128; i++)
    {
        test.Check(platform_acpi_next_event(&output, &lost, 0) == ACPI_SUCCESS, "queued event is available");
        test.Equal(output.value, i, "notification order is preserved");
    }

    test.Equal(lost, originalLost + 1, "overflow is explicitly counted");
    test.Check(platform_acpi_next_event(&output, &lost, 0) == ACPI_TIMEOUT, "empty queue polling never blocks");

    for (size_t i = 0; i < platform_acpi_state().device_count; i++)
    {
        const acpi_device_t* device = platform_acpi_device(i);

        if (device->hardware_id == nullptr || device->path == nullptr || strlen(device->path) >= sizeof(output.path))
        {
            continue;
        }

        bool powerButton = strcmp(device->hardware_id, "PNP0C0C") == 0;
        bool sleepButton = strcmp(device->hardware_id, "PNP0C0E") == 0;

        if (!powerButton && !sleepButton)
        {
            continue;
        }

        acpi_event_t earlyNotification {};
        earlyNotification.kind = ACPI_EVENT_NOTIFY;
        earlyNotification.value = 0x80;
        memcpy(earlyNotification.path, device->path, strlen(device->path) + 1);
        acpi_host_publish_event(&earlyNotification);
        test.Check(platform_acpi_next_event(&output, &lost, 0) == ACPI_SUCCESS, "control-method button notification is retained");
        test.Equal(output.device, device->id, "early notification path resolves to the published identity");
        test.Check(output.kind == (powerButton ? ACPI_EVENT_POWER_BUTTON : ACPI_EVENT_SLEEP_BUTTON), "control-method buttons become typed events");
    }
}
