import gdb


PAGE_SIZE = 4096


class ArxPmmCommand(gdb.Command):
    """Print Arx buddy allocator free lists and usage totals."""

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

    def invoke(self, arg, from_tty):
        del arg
        del from_tty

        try:
            zone_count = int(gdb.parse_and_eval("pmm_zone_count"))
            zones = gdb.parse_and_eval("pmm_zones")
        except gdb.error as err:
            try:
                zone = gdb.parse_and_eval("pmm_zone")
            except gdb.error as single_err:
                raise gdb.GdbError("Failed to read PMM symbols: {} / {}".format(err, single_err))

            print("Arx PMM state")
            print("=============")
            print("zones: 1")
            print("")
            self._print_zone(zone, "zone[0]")
            return

        if zone_count < 0:
            zone_count = 0

        print("Arx PMM state")
        print("=============")
        print("zones: {}".format(zone_count))
        print("")

        for index in range(zone_count):
            self._print_zone(zones[index], "zone[{}]".format(index))


ArxPmmCommand()


class ArxVmmCommand(gdb.Command):
    """Print Arx VMM init_kernel_address_space state."""

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

    def invoke(self, arg, from_tty):
        del arg
        del from_tty

        try:
            space = gdb.parse_and_eval("init_kernel_address_space")
        except gdb.error as err:
            raise gdb.GdbError("Failed to read VMM symbols: {}".format(err))

        space_type = int(space["type"])
        pt = int(space["pt"])
        lock = int(space["lock"])

        print("Arx VMM address space state")
        print("===========================")
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
