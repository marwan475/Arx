#include "layers/Resource/AcpiManager.hpp"
#include "layers/Resource/PciManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"

#include <selftests/ManagerSuite.hpp>
#include <platform.h>

void test_AcpiManager(ResourceLayerCaps* resources)
{
    ManagerSuite suite("resource/AcpiManager", resources != nullptr && resources->acpiManager != nullptr && resources->deviceManager != nullptr);

    suite.Run("resource/AcpiManager/explicit_startup", [&](TestCase& test)
    {
        AcpiManager* manager = resources->acpiManager;
        test.Check(manager->GetState().status == ACPI_NOT_READY, "construction does not execute firmware");
        test.Equal(manager->CountDevices(), 0, "inventory is empty before explicit initialization");
        test.Check(manager->GetDevice(0) == nullptr, "no unpublished device escapes");
        test.Equal(manager->GetDeviceId(0), INVALID_DEVICE_ID, "zero is not a device identity");
    });

    suite.Run("resource/AcpiManager/resource_templates", [&](TestCase& test)
    {
        // IRQ alternatives, translated DWORD address and an unknown vendor item.
        const uint8_t bytes[] = {
            0x31, 0x00,
            0x23, 0x00, 0x0c, 0x18,
            0x38,
            0x87, 23, 0, 1, 0x0c, 0,
            0xff, 0, 0, 0,
            0, 0x10, 0, 0,
            0xff, 0x1f, 0, 0,
            0, 0, 1, 0,
            0, 0x10, 0, 0,
            0x71, 0xab,
            0x79, 0
        };
        acpi_resources_t snapshot {};
        acpi_status_t status = platform_acpi_decode_resources(bytes, sizeof(bytes), &snapshot);

        if (!test.Check(status == ACPI_INCOMPLETE, "unknown descriptors make partial interpretation explicit"))
        {
            return;
        }

        test.Equal(snapshot.count, 6, "dependent boundaries and end descriptor are retained");
        test.Equal(snapshot.template_size, sizeof(bytes), "the complete original template is owned");
        test.Check(memcmp(snapshot.template_bytes, bytes, sizeof(bytes)) == 0, "raw descriptor flags and alternatives are preserved byte-for-byte");
        test.Equal(snapshot.entries[1].interrupt_count, 2, "multiple IRQ choices are preserved");
        test.Equal(snapshot.entries[1].interrupts[0], 10, "IRQ choice ten is decoded");
        test.Check(snapshot.entries[1].level_triggered && snapshot.entries[1].active_low && snapshot.entries[1].shared, "IRQ electrical and sharing flags survive");
        test.Equal(snapshot.entries[3].minimum, 0x1000, "address minimum is decoded");
        test.Equal(snapshot.entries[3].translation, 0x10000, "translation offset is preserved");
        test.Check(!snapshot.entries[4].understood, "unknown descriptor remains identifiable");
        AcpiManager::ReleaseResources(&snapshot);
        test.Check(snapshot.entries == nullptr && snapshot.template_bytes == nullptr, "release clears owned storage");
    });

    suite.Run("resource/AcpiManager/malformed_resources", [&](TestCase& test)
    {
        const uint8_t truncated[] = { 0x89, 10, 0, 0, 1, 0x79, 0 };
        const uint8_t unterminated[] = { 0x22, 0, 8 };
        acpi_resources_t snapshot {};
        test.Check(platform_acpi_decode_resources(truncated, sizeof(truncated), &snapshot) == ACPI_FIRMWARE_ERROR, "oversized descriptor rejected");
        test.Check(platform_acpi_decode_resources(unterminated, sizeof(unterminated), &snapshot) == ACPI_FIRMWARE_ERROR, "missing end tag rejected");
        test.Check(snapshot.entries == nullptr, "failed decoding publishes no partial snapshot");
    });
}

void test_AcpiManagerPlatform(ResourceLayerCaps* resources)
{
    ManagerSuite suite("resource/AcpiManager/live", resources != nullptr && resources->acpiManager != nullptr);

    suite.Run("resource/AcpiManager/live/namespace_and_registry", [&](TestCase& test)
    {
        AcpiManager* manager = resources->acpiManager;
        acpi_runtime_state_t state = manager->GetState();

        if (state.stage != ACPI_STAGE_READY)
        {
            test.Check(state.status != ACPI_SUCCESS, "runtime failure is explicit");
            test.Check(manager->GetDevice(0) == nullptr, "failed runtime publishes no interpreter identities");
            test.Skip("firmware runtime unavailable; table-based boot remains usable");
            return;
        }

        test.Check(state.status == ACPI_SUCCESS || state.status == ACPI_INCOMPLETE, "runtime readiness is explicit");
        test.Check(manager->CountDevices() != 0, "firmware namespace has devices");
        test.Check(manager->GetRegistrationStatus() == DeviceStatus::Success, "discovered devices reach the central registry");

        for (size_t i = 0; i < manager->CountDevices(); i++)
        {
            const acpi_device_t* identity = manager->GetDevice(i);
            Device registered;
            test.Check(identity != nullptr && identity->id == i + 1, "platform identities are stable");
            test.Check(resources->deviceManager->GetDevice(manager->GetDeviceId(identity->id), &registered) == DeviceStatus::Success, "ACPI identity is registered");
            test.Check(registered.bus == DeviceBus::ACPI && registered.identity == identity, "registry borrows the platform identity");
            test.Equal(registered.parent_id, manager->GetDeviceId(identity->parent), "firmware hierarchy is preserved");
        }

        const acpi_device_t* first = manager->GetDevice(0);
        manager->Initialize();
        test.Check(first == manager->GetDevice(0), "repeated initialization preserves identity");
        test.Equal(manager->CountDevices(), state.device_count, "initialization never rescans");
    });

    suite.Run("resource/AcpiManager/live/resources_and_routing", [&](TestCase& test)
    {
        AcpiManager* manager = resources->acpiManager;

        if (manager->GetState().stage != ACPI_STAGE_READY)
        {
            test.Skip("runtime unavailable");
            return;
        }

        size_t roots = 0;

        for (size_t i = 0; i < manager->CountDevices(); i++)
        {
            const acpi_device_t* device = manager->GetDevice(i);

            if (!device->pci_root)
            {
                continue;
            }

            roots++;
            uint32_t flags = 0;
            uint32_t firmwareError = 0;
            test.Check(manager->RefreshStatus(device->id, &flags, &firmwareError) == ACPI_SUCCESS, "PCI root status can be evaluated");
            acpi_resources_t resourcesSnapshot {};
            acpi_status_t status = manager->GetResources(device->id, false, &resourcesSnapshot);
            test.Check(status == ACPI_SUCCESS || status == ACPI_INCOMPLETE, "PCI root resources can be evaluated");
            test.Check(resourcesSnapshot.count != 0, "root resource descriptors are retained");
            AcpiManager::ReleaseResources(&resourcesSnapshot);
        }

        test.Check(roots != 0, "a PCI firmware root is discovered");
        size_t routes = 0;

        for (size_t i = 0; resources->pciManager != nullptr && i < resources->pciManager->CountDevices(); i++)
        {
            const pci_device_t* device = resources->pciManager->GetDevice(i);

            if (device->interrupt_pin == 0 || device->interrupt_pin > 4)
            {
                continue;
            }

            acpi_pci_route_t route {};
            acpi_status_t status = manager->ResolvePciInterrupt(device->segment, device->bus, device->device, device->function, device->interrupt_pin - 1, &route);
            test.Check(status == ACPI_SUCCESS || status == ACPI_NEEDS_CONFIGURATION, "PCI interrupt resolves through firmware");

            if (status == ACPI_SUCCESS)
            {
                routes++;
                test.Check(route.gsi != 0, "firmware selects a usable interrupt");

                if (route.link_device != 0)
                {
                    acpi_resources_t linkResources {};
                    acpi_status_t queried = manager->GetResources(route.link_device, false, &linkResources);

                    if (test.Check(queried == ACPI_SUCCESS && route.resource_index < linkResources.count, "link resources remain queryable"))
                    {
                        const acpi_resource_t& irq = linkResources.entries[route.resource_index];
                        test.Check(irq.level_triggered == route.level_triggered && irq.active_low == route.active_low && irq.shared == route.shared,
                                   "routing preserves the link's actual firmware electrical flags");
                    }

                    AcpiManager::ReleaseResources(&linkResources);
                    test.Check(manager->ConfigureInterruptLink(route.link_device, route.resource_index, route.gsi) == ACPI_SUCCESS,
                               "explicit link configuration accepts its already allocated interrupt");
                }
                else
                {
                    test.Check(route.level_triggered && route.active_low, "direct PCI GSI routes use PCI electrical defaults");
                }
            }
        }

        test.Check(routes != 0, "at least one PCI interrupt route is usable");
        uint64_t interruptState = arch_save_and_disable_interrupts();
        acpi_pci_route_t route {};
        acpi_status_t unsafe = manager->ResolvePciInterrupt(0, 0, 0, 0, 0, &route);
        arch_restore_interrupts(interruptState);
        test.Check(unsafe == ACPI_UNSAFE_CONTEXT, "firmware operations reject interrupt-disabled callers");
    });
}
