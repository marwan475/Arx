import gdb


PAGE_SIZE = 4096


class ArxPmmCommand(gdb.Command):
    """Print Arx buddy allocator state from dispatcher CPU context."""

    def __init__(self):
        super().__init__("arx-pmm", gdb.COMMAND_STATUS)

    @staticmethod
    def _array_len(value, fallback):
        array_type = value.type.strip_typedefs()
        if array_type.code == gdb.TYPE_CODE_ARRAY:
            bounds = array_type.range()
            if bounds is not None:
                return int(bounds[1] - bounds[0] + 1)
        return fallback

    @staticmethod
    def _print_zone(zone, zone_label):
        free_lists = zone["buddy_free_lists"]
        order_count = ArxPmmCommand._array_len(free_lists, fallback=11)

        total_free_pages_from_lists = 0
        total_blocks = 0

        print(zone_label)
        print("=" * len(zone_label))
        print("region_count:  {}".format(int(zone["region_count"])))
        print("total_pages:   {} ({} bytes)".format(int(zone["total_pages"]), int(zone["total_pages"]) * PAGE_SIZE))
        print("free_pages:    {} ({} bytes)".format(int(zone["free_pages"]), int(zone["free_pages"]) * PAGE_SIZE))
        print("used_pages:    {} ({} bytes)".format(int(zone["used_pages"]), int(zone["used_pages"]) * PAGE_SIZE))
        print("total_memory:  {} bytes".format(int(zone["total_memory"])))
        print("min_pfn:       {}".format(int(zone["min_pfn"])))
        print("max_pfn:       {}".format(int(zone["max_pfn"])))
        print("hhdm_present:  {}".format(int(zone["hhdm_present"])))
        print("hhdm_offset:   0x{:016x}".format(int(zone["hhdm_offset"])))
        print("")
        print("Buddy Free Lists")
        print("----------------")

        for order in range(order_count):
            node = free_lists[order]["head"]
            block_count = 0
            list_free_pages = 0

            while int(node) != 0:
                node_order = int(node["order"])
                block_pages = 1 << node_order

                block_count += 1
                list_free_pages += block_pages

                node = node["next"]

            total_blocks += block_count
            total_free_pages_from_lists += list_free_pages

            print("order {}: blocks={}, free_pages={}".format(order, block_count, list_free_pages))

        print("")
        print("List-derived totals")
        print("-------------------")
        print("free blocks:  {}".format(total_blocks))
        print("free pages:   {} ({} bytes)".format(total_free_pages_from_lists, total_free_pages_from_lists * PAGE_SIZE))
        print("")

    @staticmethod
    def _parse_cpu_index(arg):
        text = (arg or "").strip()
        if text == "":
            return None
        try:
            return int(gdb.parse_and_eval(text))
        except gdb.error:
            try:
                return int(text, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid CPU index '{}': {}".format(text, err))

    def invoke(self, arg, from_tty):
        del from_tty

        try:
            dispatcher = gdb.parse_and_eval("dispatcher")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read dispatcher symbol: {}".format(err))

        cpu_count = int(dispatcher["cpu_count"])
        cpu_slots = self._array_len(dispatcher["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)

        print("Arx PMM state")
        print("=============")
        print("cpu_count: {}".format(cpu_count))
        print("cpu_slots: {}".format(cpu_slots))
        print("")

        cpu_indices = []
        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = dispatcher["cpus"][i]
                if int(cpu["id"]) == requested_cpu:
                    cpu_indices = [i]
                    break

            if len(cpu_indices) == 0 and requested_cpu < cpu_slots:
                cpu_indices = [requested_cpu]

            if len(cpu_indices) == 0:
                raise gdb.GdbError(
                    "cpu selector {} did not match any cpu.id or cpu slot [0, {})"
                    .format(requested_cpu, cpu_slots)
                )
        else:
            cpu_indices = list(range(cpu_slots))

        printed = 0
        for cpu_index in cpu_indices:
            cpu = dispatcher["cpus"][cpu_index]
            numa_node = cpu["numa_node"]
            if int(numa_node) == 0:
                if requested_cpu is not None:
                    print("cpu[{}] numa_node: NULL".format(cpu_index))
                continue

            print("cpu[{}] id={}".format(cpu_index, int(cpu["id"])))
            print("")
            self._print_zone(numa_node["zone"], "cpu[{}].numa_node.zone".format(cpu_index))
            printed += 1

        if printed == 0 and requested_cpu is None:
            print("(no CPUs with initialized numa_node pointer)")


ArxPmmCommand()


class ArxVmmCommand(gdb.Command):
    """Print Arx VMM address_space state from dispatcher CPU context."""

    def __init__(self):
        super().__init__("arx-vmm", gdb.COMMAND_STATUS)

    @staticmethod
    def _iter_regions(head):
        node = head
        while int(node) != 0:
            yield node
            node = node["next"]

    @staticmethod
    def _print_region_list(title, head):
        print(title)
        print("-" * len(title))

        count = 0
        total_size = 0

        for region in ArxVmmCommand._iter_regions(head):
            start = int(region["start"])
            end = int(region["end"])
            size = int(region["size"])
            rtype = int(region["type"])

            count += 1
            total_size += size

            print(
                "[{}] start=0x{:016x} end=0x{:016x} size=0x{:x} ({}) type={}"
                .format(count - 1, start, end, size, size, "KERNEL" if rtype == 0 else "USER")
            )

        if count == 0:
            print("(empty)")

        print("regions: {}, total_size=0x{:x} ({})".format(count, total_size, total_size))
        print("")

    @staticmethod
    def _parse_cpu_index(arg):
        text = (arg or "").strip()
        if text == "":
            return None
        try:
            return int(gdb.parse_and_eval(text))
        except gdb.error:
            try:
                return int(text, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid CPU index '{}': {}".format(text, err))

    def _print_space(self, cpu_index, cpu):
        space_ptr = cpu["address_space"]

        print("cpu:  {} (id={})".format(cpu_index, int(cpu["id"])))
        print("address_space: 0x{:016x}".format(int(space_ptr)))

        if int(space_ptr) == 0:
            print("(address_space is NULL)")
            print("")
            return

        space = space_ptr.dereference()

        space_type = int(space["type"])
        pt = int(space["pt"])
        lock = int(space["lock"])

        print("type: {}".format("KERNEL" if space_type == 0 else "USER"))
        print("pt:   0x{:016x}".format(pt))
        print("lock: {}".format(lock))
        print("")

        self._print_region_list("kernel_free_regions", space["kernel_free_regions"])
        self._print_region_list("kernel_used_regions", space["kernel_used_regions"])
        self._print_region_list("user_free_regions", space["user_free_regions"])
        self._print_region_list("user_used_regions", space["user_used_regions"])

        print("Metadata")
        print("--------")
        print("kernel_regions_count: {}".format(int(space["kernel_regions_count"])))
        print("user_regions_count:   {}".format(int(space["user_regions_count"])))
        print("")

    def invoke(self, arg, from_tty):
        del from_tty

        try:
            dispatcher = gdb.parse_and_eval("dispatcher")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read dispatcher symbol: {}".format(err))

        cpu_count = int(dispatcher["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(dispatcher["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)

        cpu_indices = []
        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = dispatcher["cpus"][i]
                if int(cpu["id"]) == requested_cpu:
                    cpu_indices = [i]
                    break

            if len(cpu_indices) == 0 and requested_cpu < cpu_slots:
                cpu_indices = [requested_cpu]

            if len(cpu_indices) == 0:
                raise gdb.GdbError(
                    "cpu selector {} did not match any cpu.id or cpu slot [0, {})"
                    .format(requested_cpu, cpu_slots)
                )
        else:
            cpu_indices = list(range(cpu_slots))

        print("Arx VMM address space state")
        print("===========================")
        print("cpu_count: {}".format(cpu_count))
        print("cpu_slots: {}".format(cpu_slots))
        print("")

        printed = 0
        for cpu_index in cpu_indices:
            cpu = dispatcher["cpus"][cpu_index]
            self._print_space(cpu_index, cpu)
            printed += 1

        if printed == 0 and requested_cpu is None:
            print("(no CPU slots found)")


ArxVmmCommand()


class ArxCpusCommand(gdb.Command):
    """Print Arx dispatcher CPU info for all CPU slots."""

    def __init__(self):
        super().__init__("arx-cpus", gdb.COMMAND_STATUS)

    @staticmethod
    def _read_int_field(value, field_name, default=None):
        try:
            return int(value[field_name])
        except Exception:
            return default

    @staticmethod
    def _read_nested_int_field(value, field_names, default=None):
        try:
            current = value
            for name in field_names:
                current = current[name]
            return int(current)
        except Exception:
            return default

    @staticmethod
    def _arch_name(arch_value):
        if arch_value == 0:
            return "x86_64"
        if arch_value == 1:
            return "aarch64"
        return "unknown({})".format(arch_value)

    @staticmethod
    def _parse_cpu_index(arg):
        text = (arg or "").strip()
        if text == "":
            return None
        try:
            return int(gdb.parse_and_eval(text))
        except gdb.error:
            try:
                return int(text, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid CPU index '{}': {}".format(text, err))

    def invoke(self, arg, from_tty):
        del from_tty

        try:
            dispatcher = gdb.parse_and_eval("dispatcher")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read dispatcher symbol: {}".format(err))

        cpu_count = int(dispatcher["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(dispatcher["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)
        cpu_indices = []

        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = dispatcher["cpus"][i]
                if int(cpu["id"]) == requested_cpu:
                    cpu_indices = [i]
                    break

            if len(cpu_indices) == 0 and requested_cpu < cpu_slots:
                cpu_indices = [requested_cpu]

            if len(cpu_indices) == 0:
                raise gdb.GdbError(
                    "cpu selector {} did not match any cpu.id or cpu slot [0, {})"
                    .format(requested_cpu, cpu_slots)
                )
        else:
            cpu_indices = list(range(cpu_slots))

        dispatcher_arch = self._read_int_field(dispatcher, "arch", default=-1)
        dispatcher_arch_info = dispatcher["arch_info"]

        print("Arx CPU state")
        print("=============")
        print("cpu_count: {}".format(cpu_count))
        print("cpu_slots: {}".format(cpu_slots))
        print("arch:      {}".format(self._arch_name(dispatcher_arch)))
        print("")

        print("Dispatcher")
        print("----------")
        if dispatcher_arch == 0:
            ioapic_present = self._read_int_field(dispatcher_arch_info, "acpi_has_ioapic", default=None)
            ioapic_id = self._read_int_field(dispatcher_arch_info, "acpi_ioapic_id", default=None)
            ioapic_gsi_base = self._read_int_field(dispatcher_arch_info, "acpi_ioapic_gsi_base", default=None)
            ioapic_base = self._read_int_field(dispatcher_arch_info, "acpi_ioapic_base_addr", default=None)

            if ioapic_present is None:
                print("arch_info: unavailable in current debug symbols")
            else:
                print("acpi_has_ioapic:      {}".format(ioapic_present))
                print("acpi_ioapic_id:       {}".format(ioapic_id if ioapic_id is not None else 0))
                print("acpi_ioapic_gsi_base: {}".format(ioapic_gsi_base if ioapic_gsi_base is not None else 0))
                print("acpi_ioapic_base_addr: 0x{:016x}".format(ioapic_base if ioapic_base is not None else 0))
        else:
            print("arch_info: n/a for {}".format(self._arch_name(dispatcher_arch)))
        print("")

        for i in cpu_indices:
            cpu = dispatcher["cpus"][i]

            cpu_id = self._read_int_field(cpu, "id", default=0)
            acpi_has_lapic = self._read_nested_int_field(cpu, ["arch_info", "acpi_has_lapic"], default=None)
            acpi_lapic_base_addr = self._read_nested_int_field(cpu, ["arch_info", "acpi_lapic_base_addr"], default=None)
            acpi_uid = self._read_nested_int_field(cpu, ["arch_info", "acpi_processor_uid"], default=None)
            acpi_lapic_id = self._read_nested_int_field(cpu, ["arch_info", "acpi_lapic_id"], default=None)
            acpi_lapic_flags = self._read_nested_int_field(cpu, ["arch_info", "acpi_lapic_flags"], default=None)

            numa_node = int(cpu["numa_node"])
            address_space = int(cpu["address_space"])

            # Skip empty slots beyond configured CPU count.
            if i >= cpu_count and numa_node == 0 and address_space == 0:
                continue

            print("cpu[{}]".format(i))
            print("  id: {}".format(cpu_id))
            print("  numa_node:     0x{:016x}".format(numa_node))
            print("  address_space: 0x{:016x}".format(address_space))

            if dispatcher_arch != 0:
                print("  arch_info: n/a for {}".format(self._arch_name(dispatcher_arch)))
            elif acpi_has_lapic is None:
                print("  arch_info: unavailable in current debug symbols")
            else:
                print("  acpi_has_lapic:      {}".format(acpi_has_lapic))
                print("  acpi_lapic_base_addr: 0x{:016x}".format(acpi_lapic_base_addr if acpi_lapic_base_addr is not None else 0))
                print("  acpi_processor_uid:   {}".format(acpi_uid if acpi_uid is not None else 0))
                print("  acpi_lapic_id:        {}".format(acpi_lapic_id if acpi_lapic_id is not None else 0))
                print("  acpi_lapic_flags:     0x{:08x}".format(acpi_lapic_flags if acpi_lapic_flags is not None else 0))

            print("")


ArxCpusCommand()


class ArxPciCommand(gdb.Command):
    """Print Arx PCI devices discovered in dispatcher.pci_devices."""

    def __init__(self):
        super().__init__("arx-pci", gdb.COMMAND_STATUS)

    def invoke(self, arg, from_tty):
        del arg
        del from_tty

        try:
            dispatcher = gdb.parse_and_eval("dispatcher")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read dispatcher symbol: {}".format(err))

        arch = int(dispatcher["arch"])
        device_count = int(dispatcher["pci_device_count"])
        devices_ptr = dispatcher["pci_devices"]
        devices_ptr_int = int(devices_ptr)

        print("Arx PCI devices")
        print("===============")
        print("arch:      {}".format("x86_64" if arch == 0 else "aarch64" if arch == 1 else "unknown({})".format(arch)))
        print("count:     {}".format(device_count))
        print("array_ptr: 0x{:016x}".format(devices_ptr_int))
        print("")

        if arch != 0:
            print("PCI device enumeration output is currently expected on x86_64.")
            return

        if device_count <= 0:
            print("(no PCI devices discovered)")
            return

        if devices_ptr_int == 0:
            print("(pci_device_count is non-zero but pci_devices is NULL)")
            return

        # Guard against invalid/corrupt values in a halted debug session.
        if device_count > 4096:
            raise gdb.GdbError("refusing to print {} PCI entries (sanity limit 4096)".format(device_count))

        print("idx  bdf      vendor  device")
        print("---  -------  ------  ------")
        for i in range(device_count):
            dev = devices_ptr[i]
            bus = int(dev["bus"])
            device = int(dev["device"])
            function = int(dev["function"])
            vendor_id = int(dev["vendor_id"])
            device_id = int(dev["device_id"])

            print(
                "{:3d}  {:02x}:{:02x}.{}  {:04x}    {:04x}".format(
                    i, bus, device, function, vendor_id, device_id
                )
            )


ArxPciCommand()


class ArxHeapCommand(gdb.Command):
    """Print Arx heap cache/slab state from dispatcher CPU context."""

    def __init__(self):
        super().__init__("arx-heap", gdb.COMMAND_STATUS)

    @staticmethod
    def _iter_slabs(head):
        node = head
        while int(node) != 0:
            yield node
            node = node["next"]

    @staticmethod
    def _list_stats(head):
        slab_count = 0
        free_objects = 0
        total_objects = 0

        for slab in ArxHeapCommand._iter_slabs(head):
            slab_count += 1
            free_objects += int(slab["free_objects"])
            total_objects += int(slab["total_objects"])

        used_objects = total_objects - free_objects
        return slab_count, total_objects, free_objects, used_objects

    @staticmethod
    def _print_cache(cache_index, cache):
        object_size = int(cache["object_size"])
        metadata_pool = cache["slab_metadata_pool"]
        metadata_free_list = int(metadata_pool["free_list"])
        metadata_chunks = int(metadata_pool["chunks"])
        metadata_elem_size = int(metadata_pool["element_size"])
        metadata_elems_per_chunk = int(metadata_pool["elements_per_chunk"])

        partial_stats = ArxHeapCommand._list_stats(cache["partial_slabs"])
        full_stats = ArxHeapCommand._list_stats(cache["full_slabs"])
        empty_stats = ArxHeapCommand._list_stats(cache["empty_slabs"])

        total_slabs = partial_stats[0] + full_stats[0] + empty_stats[0]
        total_objects = partial_stats[1] + full_stats[1] + empty_stats[1]
        total_free = partial_stats[2] + full_stats[2] + empty_stats[2]
        total_used = partial_stats[3] + full_stats[3] + empty_stats[3]

        print("cache[{}] object_size={}".format(cache_index, object_size))
        print("  slab_metadata_pool.chunks:            0x{:016x}".format(metadata_chunks))
        print("  slab_metadata_pool.free_list:         0x{:016x}".format(metadata_free_list))
        print("  slab_metadata_pool.element_size:      {}".format(metadata_elem_size))
        print("  slab_metadata_pool.elements_per_chunk:{}".format(metadata_elems_per_chunk))
        print("  partial_slabs: count={} total_objects={} free_objects={} used_objects={}".format(
            partial_stats[0], partial_stats[1], partial_stats[2], partial_stats[3]
        ))
        print("  full_slabs:    count={} total_objects={} free_objects={} used_objects={}".format(
            full_stats[0], full_stats[1], full_stats[2], full_stats[3]
        ))
        print("  empty_slabs:   count={} total_objects={} free_objects={} used_objects={}".format(
            empty_stats[0], empty_stats[1], empty_stats[2], empty_stats[3]
        ))
        print("  totals:        slabs={} total_objects={} free_objects={} used_objects={}".format(
            total_slabs, total_objects, total_free, total_used
        ))
        print("")

    @staticmethod
    def _parse_cpu_index(arg):
        text = (arg or "").strip()
        if text == "":
            return None
        try:
            return int(gdb.parse_and_eval(text))
        except gdb.error:
            try:
                return int(text, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid CPU index '{}': {}".format(text, err))

    def invoke(self, arg, from_tty):
        del from_tty

        try:
            dispatcher = gdb.parse_and_eval("dispatcher")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read dispatcher symbol: {}".format(err))

        cpu_count = int(dispatcher["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(dispatcher["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)

        cpu_indices = []
        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = dispatcher["cpus"][i]
                if int(cpu["id"]) == requested_cpu:
                    cpu_indices = [i]
                    break

            if len(cpu_indices) == 0 and requested_cpu < cpu_slots:
                cpu_indices = [requested_cpu]

            if len(cpu_indices) == 0:
                raise gdb.GdbError(
                    "cpu selector {} did not match any cpu.id or cpu slot [0, {})"
                    .format(requested_cpu, cpu_slots)
                )
        else:
            cpu_indices = list(range(cpu_slots))

        print("Arx heap state")
        print("==============")
        print("cpu_count: {}".format(cpu_count))
        print("cpu_slots: {}".format(cpu_slots))
        print("")

        printed = 0
        for cpu_index in cpu_indices:
            cpu = dispatcher["cpus"][cpu_index]
            numa_node = cpu["numa_node"]

            if int(numa_node) == 0:
                if requested_cpu is not None:
                    print("cpu[{}] numa_node: NULL".format(cpu_index))
                continue

            heap = numa_node["heap"]
            cache_count = ArxPmmCommand._array_len(heap["caches"], fallback=8)

            print("cpu[{}] id={}".format(cpu_index, int(cpu["id"])))
            print("heap.lock: {}".format(int(heap["lock"])))
            print("cache_count: {}".format(cache_count))
            print("")

            for cache_index in range(cache_count):
                self._print_cache(cache_index, heap["caches"][cache_index])

            printed += 1

        if printed == 0 and requested_cpu is None:
            print("(no CPUs with initialized numa_node pointer)")


ArxHeapCommand()


class ArxRustCommand(gdb.Command):
    """Enable Rust-friendly output and show rust_entry symbol status."""

    def __init__(self):
        super().__init__("arx-rust", gdb.COMMAND_STATUS)

    def invoke(self, arg, from_tty):
        del from_tty

        gdb.execute("set print demangle on", to_string=True)
        gdb.execute("set print asm-demangle on", to_string=True)

        # Some GDB builds support explicit Rust demangle style; keep this optional.
        try:
            gdb.execute("set demangle-style rust", to_string=True)
        except gdb.error:
            pass

        print("Rust debug settings enabled")

        rust_symbol = gdb.lookup_global_symbol("rust_entry")
        if rust_symbol is None:
            print("rust_entry: not found in loaded symbols")
            return

        print("rust_entry: found")

        if (arg or "").strip() == "break":
            gdb.execute("hbreak rust_entry")
            print("hardware breakpoint set at rust_entry")


ArxRustCommand()
