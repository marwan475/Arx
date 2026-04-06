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

    def invoke(self, arg, from_tty):
        del arg
        del from_tty

        try:
            free_lists = gdb.parse_and_eval("buddy_free_lists")
            total_usable_pages = int(gdb.parse_and_eval("pmm_total_usable_pfns"))
        except gdb.error as err:
            raise gdb.GdbError("Failed to read PMM symbols: {}".format(err))

        order_count = self._array_len(free_lists, fallback=11)
        total_free_pages = 0
        total_blocks = 0

        print("Arx PMM buddy allocator state")
        print("============================")

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
            total_free_pages += list_free_pages

            print("order {}: blocks={}, free_pages={}".format(order, block_count, list_free_pages))

        used_pages = total_usable_pages - total_free_pages
        if used_pages < 0:
            used_pages = 0

        print("")
        print("Totals")
        print("------")
        print("free blocks:  {}".format(total_blocks))
        print("usable pages: {} ({} bytes)".format(total_usable_pages, total_usable_pages * PAGE_SIZE))
        print("free pages:   {} ({} bytes)".format(total_free_pages, total_free_pages * PAGE_SIZE))
        print("used pages:   {} ({} bytes)".format(used_pages, used_pages * PAGE_SIZE))


ArxPmmCommand()
