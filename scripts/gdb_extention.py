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
            if requested_cpu < 0 or requested_cpu >= cpu_slots:
                raise gdb.GdbError("cpu index {} out of range [0, {})".format(requested_cpu, cpu_slots))
            cpu_indices = [requested_cpu]
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
            try:
                return int(gdb.parse_and_eval("arch_cpu_id()"))
            except gdb.error:
                return 0
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
        cpu_index = self._parse_cpu_index(arg)

        if cpu_index < 0 or cpu_index >= cpu_slots:
            raise gdb.GdbError("cpu index {} out of range [0, {})".format(cpu_index, cpu_slots))

        cpu = dispatcher["cpus"][cpu_index]
        space_ptr = cpu["address_space"]
        if int(space_ptr) == 0:
            raise gdb.GdbError("cpu[{}] address_space is NULL".format(cpu_index))

        space = space_ptr.dereference()

        space_type = int(space["type"])
        pt = int(space["pt"])
        lock = int(space["lock"])

        print("Arx VMM address space state")
        print("===========================")
        print("cpu:  {} (id={})".format(cpu_index, int(cpu["id"])))
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

    def invoke(self, arg, from_tty):
        del from_tty

        if (arg or "").strip() != "":
            raise gdb.GdbError("arx-cpus takes no arguments")

        try:
            dispatcher = gdb.parse_and_eval("dispatcher")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read dispatcher symbol: {}".format(err))

        cpu_count = int(dispatcher["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(dispatcher["cpus"], fallback=max(cpu_count, 1))

        print("Arx CPU state")
        print("=============")
        print("cpu_count: {}".format(cpu_count))
        print("cpu_slots: {}".format(cpu_slots))
        print("")

        for i in range(cpu_slots):
            cpu = dispatcher["cpus"][i]

            cpu_id = self._read_int_field(cpu, "id", default=0)
            acpi_has_lapic = self._read_int_field(cpu, "acpi_has_lapic", default=None)
            acpi_uid = self._read_int_field(cpu, "acpi_processor_uid", default=None)
            acpi_lapic_id = self._read_int_field(cpu, "acpi_lapic_id", default=None)
            acpi_lapic_flags = self._read_int_field(cpu, "acpi_lapic_flags", default=None)

            numa_node = int(cpu["numa_node"])
            address_space = int(cpu["address_space"])

            print("cpu[{}]".format(i))
            print("  id: {}".format(cpu_id))
            print("  numa_node:     0x{:016x}".format(numa_node))
            print("  address_space: 0x{:016x}".format(address_space))

            if acpi_has_lapic is None:
                print("  acpi: unavailable in current debug symbols")
            else:
                print("  acpi_has_lapic:   {}".format(acpi_has_lapic))
                print("  acpi_processor_uid: {}".format(acpi_uid))
                print("  acpi_lapic_id:      {}".format(acpi_lapic_id))
                print("  acpi_lapic_flags:   0x{:08x}".format(acpi_lapic_flags if acpi_lapic_flags is not None else 0))

            print("")


ArxCpusCommand()
