import gdb
import os
import re


PAGE_SIZE = 4096


class ArxPlatform:
    """Helpers for resolving the global platform symbol across GDB contexts."""

    _platform_addr_override = 0
    _auto_platform_addr_cache = 0

    @staticmethod
    def _extract_first_hex(text):
        match = re.search(r"0x[0-9a-fA-F]+", text or "")
        if match is None:
            return 0

    @staticmethod
    def _parse_and_eval_with_language(expr, language):
        """Evaluate an expression under a temporary GDB language mode."""
        previous = None
        try:
            previous = gdb.execute("show language", to_string=True)
        except Exception:
            previous = None

        try:
            gdb.execute("set language {}".format(language), to_string=True)
            return gdb.parse_and_eval(expr)
        finally:
            if previous is not None:
                lowered = previous.lower()
                if "currently c" in lowered:
                    target = "c"
                elif "currently c++" in lowered:
                    target = "c++"
                elif "currently auto" in lowered:
                    target = "auto"
                else:
                    target = "auto"
                try:
                    gdb.execute("set language {}".format(target), to_string=True)
                except Exception:
                    pass
        try:
            return int(match.group(0), 16)
        except ValueError:
            return 0

    @staticmethod
    def _is_plausible_platform_addr(addr):
        if addr == 0:
            return False
        try:
            platform = ArxPlatform._platform_from_address(addr)
            cpu_count = int(platform["cpu_count"])
            arch = int(platform["arch"])
            int(platform["cpus"])
            if cpu_count < 0 or cpu_count > 4096:
                return False
            if arch not in [0, 1]:
                return False
            return True
        except Exception:
            return False

    @staticmethod
    def _resolve_from_info_address():
        for cmd in ["info address 'platform'", "info address platform", "info address ::platform", "info address '::platform'"]:
            try:
                output = gdb.execute(cmd, to_string=True)
                addr = ArxPlatform._extract_first_hex(output)
                if addr != 0:
                    return addr
            except Exception:
                pass
        return 0

    @staticmethod
    def _candidate_map_paths():
        paths = []

        cwd = os.getcwd()
        paths.append(os.path.join(cwd, "build", "kernel-x86_64.map"))
        paths.append(os.path.join(cwd, "build", "kernel-aarch64.map"))

        # Derive repository-relative map paths from the loaded ELF when available.
        try:
            prog = gdb.current_progspace()
            prog_file = getattr(prog, "filename", None)
            if prog_file:
                elf_dir = os.path.dirname(os.path.abspath(prog_file))
                repo_root = os.path.abspath(os.path.join(elf_dir, ".."))
                paths.append(os.path.join(repo_root, "build", "kernel-x86_64.map"))
                paths.append(os.path.join(repo_root, "build", "kernel-aarch64.map"))
        except Exception:
            pass

        script_path = globals().get("__file__", "")
        if script_path:
            script_dir = os.path.dirname(os.path.abspath(script_path))
            repo_root = os.path.abspath(os.path.join(script_dir, ".."))
            paths.append(os.path.join(repo_root, "build", "kernel-x86_64.map"))
            paths.append(os.path.join(repo_root, "build", "kernel-aarch64.map"))

        # Preserve order but de-duplicate.
        seen = set()
        ordered = []
        for path in paths:
            if path in seen:
                continue
            seen.add(path)
            ordered.append(path)

        return ordered

    @staticmethod
    def _map_find_symbol_addr(map_path, symbol_name):
        try:
            with open(map_path, "r", encoding="utf-8", errors="ignore") as fp:
                for line in fp:
                    # Typical map line: 0xffffffff800480e0                platform
                    strict = r"^\s*(0x[0-9a-fA-F]+)\s+{}\s*$".format(re.escape(symbol_name))
                    match = re.match(strict, line)
                    if match is not None:
                        try:
                            return int(match.group(1), 16)
                        except ValueError:
                            pass
        except Exception:
            return 0
        return 0

    @staticmethod
    def _runtime_symbol_addr(symbol_name):
        for cmd in ["info address {}".format(symbol_name), "info address ::{}".format(symbol_name)]:
            try:
                output = gdb.execute(cmd, to_string=True)
                addr = ArxPlatform._extract_first_hex(output)
                if addr != 0:
                    return addr
            except Exception:
                pass
        return 0

    @staticmethod
    def _resolve_from_map_file():
        for map_path in ArxPlatform._candidate_map_paths():
            if not os.path.isfile(map_path):
                continue
            addr = ArxPlatform._map_find_symbol_addr(map_path, "platform")
            if addr != 0:
                return addr
        return 0

    @staticmethod
    def _resolve_from_map_with_slide():
        runtime_start = ArxPlatform._runtime_symbol_addr("_start")
        if runtime_start == 0:
            return 0

        for map_path in ArxPlatform._candidate_map_paths():
            if not os.path.isfile(map_path):
                continue

            map_start = ArxPlatform._map_find_symbol_addr(map_path, "_start")
            map_platform = ArxPlatform._map_find_symbol_addr(map_path, "platform")
            if map_start == 0 or map_platform == 0:
                continue

            slide = runtime_start - map_start
            candidate = map_platform + slide
            if candidate != 0:
                return candidate

        return 0

    @staticmethod
    def _parse_address(text):
        raw = (text or "").strip()
        if raw == "":
            raise gdb.GdbError("missing address expression")
        try:
            return int(gdb.parse_and_eval(raw))
        except gdb.error:
            try:
                return int(raw, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid address '{}': {}".format(raw, err))

    @staticmethod
    def _platform_from_address(addr):
        if addr == 0:
            raise gdb.GdbError("platform address override is NULL")
        try:
            platform_ptr_t = gdb.lookup_type("platform_t").pointer()
            return gdb.Value(addr).cast(platform_ptr_t).dereference()
        except Exception as err:
            raise gdb.GdbError("failed to decode platform_t at 0x{:x}: {}".format(addr, err))

    @staticmethod
    def set_override(addr):
        ArxPlatform._platform_addr_override = int(addr)

    @staticmethod
    def clear_override():
        ArxPlatform._platform_addr_override = 0

    @staticmethod
    def get_override():
        return ArxPlatform._platform_addr_override

    @staticmethod
    def resolve():
        if ArxPlatform._platform_addr_override != 0:
            return ArxPlatform._platform_from_address(ArxPlatform._platform_addr_override)

        if ArxPlatform._auto_platform_addr_cache != 0 and ArxPlatform._is_plausible_platform_addr(ArxPlatform._auto_platform_addr_cache):
            return ArxPlatform._platform_from_address(ArxPlatform._auto_platform_addr_cache)

        for expr in [
            "platform",
            "::platform",
            "'platform'",
            "'::platform'",
            "*(&platform)",
            "*(&::platform)",
            "*(&'platform')",
            "*(&'::platform')",
        ]:
            try:
                value = gdb.parse_and_eval(expr)
                return value
            except gdb.error:
                pass

        # If GDB parser treats platform as a type-name in auto/c++ mode, force C mode for symbol probe.
        for expr in ["'platform'", "::platform", "*(&'platform')", "*(&::platform)"]:
            try:
                value = ArxPlatform._parse_and_eval_with_language(expr, "c")
                return value
            except Exception:
                pass

        for lookup in [getattr(gdb, "lookup_global_symbol", None), getattr(gdb, "lookup_static_symbol", None)]:
            if lookup is None:
                continue
            try:
                sym = lookup("platform")
                if sym is not None:
                    return sym.value()
            except Exception:
                pass

        info_addr = ArxPlatform._resolve_from_info_address()
        if ArxPlatform._is_plausible_platform_addr(info_addr):
            ArxPlatform._auto_platform_addr_cache = info_addr
            return ArxPlatform._platform_from_address(info_addr)

        map_addr = ArxPlatform._resolve_from_map_file()
        if ArxPlatform._is_plausible_platform_addr(map_addr):
            ArxPlatform._auto_platform_addr_cache = map_addr
            return ArxPlatform._platform_from_address(map_addr)

        slid_map_addr = ArxPlatform._resolve_from_map_with_slide()
        if ArxPlatform._is_plausible_platform_addr(slid_map_addr):
            ArxPlatform._auto_platform_addr_cache = slid_map_addr
            return ArxPlatform._platform_from_address(slid_map_addr)

        raise gdb.GdbError(
            "Failed to resolve global 'platform' symbol/address automatically. Ensure correct kernel ELF symbols are loaded."
        )


class ArxPlatformCommand(gdb.Command):
    """Manage platform symbol/address resolution for Arx GDB commands."""

    def __init__(self):
        super().__init__("arx-platform", gdb.COMMAND_STATUS)

    def invoke(self, arg, from_tty):
        del from_tty

        parts = (arg or "").strip().split(None, 1)
        action = parts[0].lower() if len(parts) > 0 else "show"

        if action == "set":
            if len(parts) < 2:
                raise gdb.GdbError("usage: arx-platform set <address_expr>")
            addr = ArxPlatform._parse_address(parts[1])
            ArxPlatform.set_override(addr)
            print("platform override set to 0x{:016x}".format(addr))
            return

        if action == "clear":
            ArxPlatform.clear_override()
            print("platform override cleared")
            return

        if action in ["show", "status"]:
            override = ArxPlatform.get_override()
            if override != 0:
                print("platform override: 0x{:016x}".format(override))
            else:
                print("platform override: (not set)")

            try:
                platform = ArxPlatform.resolve()
                try:
                    cpus_ptr = int(platform["cpus"])
                    cpu_count = int(platform["cpu_count"])
                    print("platform resolved: yes")
                    print("platform.cpus:     0x{:016x}".format(cpus_ptr))
                    print("platform.cpu_count:{}".format(cpu_count))
                except Exception:
                    print("platform resolved: yes")
            except gdb.GdbError as err:
                print("platform resolved: no ({})".format(err))
            return

        raise gdb.GdbError("usage: arx-platform [show|status|set <address_expr>|clear]")


ArxPlatformCommand()


class ArxPmmCommand(gdb.Command):
    """Print Arx buddy allocator state from platform CPU context."""

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

        platform = ArxPlatform.resolve()

        cpu_count = int(platform["cpu_count"])
        cpu_slots = self._array_len(platform["cpus"], fallback=max(cpu_count, 1))
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
                cpu = platform["cpus"][i]
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
            cpu = platform["cpus"][cpu_index]
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
    """Print Arx VMM address_space state from platform CPU context."""

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

        platform = ArxPlatform.resolve()

        cpu_count = int(platform["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(platform["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)

        cpu_indices = []
        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = platform["cpus"][i]
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
            cpu = platform["cpus"][cpu_index]
            self._print_space(cpu_index, cpu)
            printed += 1

        if printed == 0 and requested_cpu is None:
            print("(no CPU slots found)")


ArxVmmCommand()


class ArxCpusCommand(gdb.Command):
    """Print Arx platform CPU info for all CPU slots."""

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

        platform = ArxPlatform.resolve()

        cpu_count = int(platform["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(platform["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)
        cpu_indices = []

        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = platform["cpus"][i]
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

        platform_arch = self._read_int_field(platform, "arch", default=-1)
        platform_arch_info = platform["arch_info"]

        print("Arx CPU state")
        print("=============")
        print("cpu_count: {}".format(cpu_count))
        print("cpu_slots: {}".format(cpu_slots))
        print("arch:      {}".format(self._arch_name(platform_arch)))
        print("")

        print("Platform")
        print("----------")
        if platform_arch == 0:
            ioapic_present = self._read_int_field(platform_arch_info, "acpi_has_ioapic", default=None)
            ioapic_id = self._read_int_field(platform_arch_info, "acpi_ioapic_id", default=None)
            ioapic_gsi_base = self._read_int_field(platform_arch_info, "acpi_ioapic_gsi_base", default=None)
            ioapic_base = self._read_int_field(platform_arch_info, "acpi_ioapic_base_addr", default=None)

            if ioapic_present is None:
                print("arch_info: unavailable in current debug symbols")
            else:
                print("acpi_has_ioapic:      {}".format(ioapic_present))
                print("acpi_ioapic_id:       {}".format(ioapic_id if ioapic_id is not None else 0))
                print("acpi_ioapic_gsi_base: {}".format(ioapic_gsi_base if ioapic_gsi_base is not None else 0))
                print("acpi_ioapic_base_addr: 0x{:016x}".format(ioapic_base if ioapic_base is not None else 0))
        else:
            print("arch_info: n/a for {}".format(self._arch_name(platform_arch)))
        print("")

        for i in cpu_indices:
            cpu = platform["cpus"][i]

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

            if platform_arch != 0:
                print("  arch_info: n/a for {}".format(self._arch_name(platform_arch)))
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
    """Print Arx PCI devices discovered in platform.pci_devices."""

    def __init__(self):
        super().__init__("arx-pci", gdb.COMMAND_STATUS)

    def invoke(self, arg, from_tty):
        del arg
        del from_tty

        platform = ArxPlatform.resolve()

        arch = int(platform["arch"])
        device_count = int(platform["pci_device_count"])
        devices_ptr = platform["pci_devices"]
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
    """Print Arx heap cache/slab state from platform CPU context."""

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

        platform = ArxPlatform.resolve()

        cpu_count = int(platform["cpu_count"])
        cpu_slots = ArxPmmCommand._array_len(platform["cpus"], fallback=max(cpu_count, 1))
        requested_cpu = self._parse_cpu_index(arg)

        cpu_indices = []
        if requested_cpu is not None:
            if requested_cpu < 0:
                raise gdb.GdbError("cpu selector must be non-negative")

            # Prefer matching logical cpu.id first, then fall back to slot index.
            for i in range(cpu_count):
                cpu = platform["cpus"][i]
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
            cpu = platform["cpus"][cpu_index]
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


class ArxResourceManagers:
    """Helpers for resolving ResourceLayer manager pointers from platform."""

    @staticmethod
    def _eval(expr):
        try:
            return gdb.parse_and_eval(expr)
        except gdb.error:
            return None

    @staticmethod
    def _read_ptr_at(base_addr, index):
        """Read pointer-sized slot at ((void**)base_addr)[index]."""
        if base_addr == 0:
            return 0
        try:
            void_pp = gdb.lookup_type("void").pointer().pointer()
            slot_ptr = gdb.Value(base_addr).cast(void_pp) + index
            return int(slot_ptr.dereference())
        except Exception:
            return 0

    @staticmethod
    def _cast_ptr(addr, type_name):
        if addr == 0:
            return None
        try:
            return gdb.Value(addr).cast(gdb.lookup_type(type_name).pointer())
        except Exception:
            return None

    @staticmethod
    def resolve_dispatcher_addr():
        """Resolve Dispatcher* pointer stored in global platform with spelling fallbacks."""
        try:
            platform = ArxPlatform.resolve()
        except gdb.GdbError:
            return 0

        # Prefer direct struct field access first.
        for field_name in ["dispacher", "dispatcher"]:
            try:
                value = int(platform[field_name])
                if value != 0:
                    return value
            except Exception:
                pass

        # Fall back to expression parsing if field indexing fails.
        for expr in ["platform.dispacher", "platform.dispatcher"]:
            value = ArxResourceManagers._eval(expr)
            if value is not None:
                try:
                    addr = int(value)
                    if addr != 0:
                        return addr
                except Exception:
                    pass

        return 0

    @staticmethod
    def _resolve_caps_address_from_platform():
        """
        Resolve ResourceLayerCaps* by object layout:
        platform.(dispacher|dispatcher) -> Dispatcher.resourceLayerFactory (first field)
        ResourceLayerFactory.ResourceLayerExportCaps (first field)
        """
        dispatcher_addr = ArxResourceManagers.resolve_dispatcher_addr()
        if dispatcher_addr == 0:
            return 0

        resource_factory_addr = ArxResourceManagers._read_ptr_at(dispatcher_addr, 0)
        if resource_factory_addr == 0:
            return 0

        return ArxResourceManagers._read_ptr_at(resource_factory_addr, 0)

    @staticmethod
    def resolve_caps():
        exprs = [
            "((ResourceLayerCaps*)((ResourceLayerFactory*)((Dispatcher*)platform.dispatcher)->resourceLayerFactory)->ResourceLayerExportCaps)",
            "((ResourceLayerFactory*)((Dispatcher*)platform.dispatcher)->resourceLayerFactory)->GetCaps()",
            "((ResourceLayerCaps*)((ResourceLayerFactory*)((Dispatcher*)platform.dispacher)->resourceLayerFactory)->ResourceLayerExportCaps)",
            "((ResourceLayerFactory*)((Dispatcher*)platform.dispacher)->resourceLayerFactory)->GetCaps()",
        ]

        for expr in exprs:
            value = ArxResourceManagers._eval(expr)
            if value is not None and int(value) != 0:
                return value

        caps_addr = ArxResourceManagers._resolve_caps_address_from_platform()
        if caps_addr == 0:
            return None

        typed_caps = ArxResourceManagers._cast_ptr(caps_addr, "ResourceLayerCaps")
        if typed_caps is not None:
            return typed_caps

        return gdb.Value(caps_addr)

        return None

    @staticmethod
    def resolve_process_manager():
        caps = ArxResourceManagers.resolve_caps()
        if caps is not None:
            try:
                manager = caps["processManager"]
                if int(manager) != 0:
                    return manager
            except Exception:
                pass

        caps_addr = ArxResourceManagers._resolve_caps_address_from_platform()
        if caps_addr == 0:
            return None

        manager_addr = ArxResourceManagers._read_ptr_at(caps_addr, 1)
        manager = ArxResourceManagers._cast_ptr(manager_addr, "ProcessManager")
        if manager is None:
            return gdb.Value(manager_addr)
        try:
            if int(manager) == 0:
                return None
            return manager
        except Exception:
            return None

    @staticmethod
    def resolve_task_manager():
        caps = ArxResourceManagers.resolve_caps()
        if caps is not None:
            try:
                manager = caps["taskManager"]
                if int(manager) != 0:
                    return manager
            except Exception:
                pass

        caps_addr = ArxResourceManagers._resolve_caps_address_from_platform()
        if caps_addr == 0:
            return None

        manager_addr = ArxResourceManagers._read_ptr_at(caps_addr, 3)
        manager = ArxResourceManagers._cast_ptr(manager_addr, "TaskManager")
        if manager is None:
            return gdb.Value(manager_addr)
        try:
            if int(manager) == 0:
                return None
            return manager
        except Exception:
            return None

    @staticmethod
    def resolve_initramfs_manager():
        caps = ArxResourceManagers.resolve_caps()
        if caps is not None:
            try:
                manager = caps["initRamFileSystemManager"]
                if int(manager) != 0:
                    return manager
            except Exception:
                pass

        caps_addr = ArxResourceManagers._resolve_caps_address_from_platform()
        if caps_addr == 0:
            return None

        manager_addr = ArxResourceManagers._read_ptr_at(caps_addr, 4)
        manager = ArxResourceManagers._cast_ptr(manager_addr, "InitRamFileSystemManager")
        if manager is None:
            return gdb.Value(manager_addr)
        try:
            if int(manager) == 0:
                return None
            return manager
        except Exception:
            return None


class ArxVfsCommand(gdb.Command):
    """Print VirtualFileSystem paths (or detailed state with --verbose)."""

    def __init__(self):
        super().__init__("arx-vfs", gdb.COMMAND_STATUS)

    @staticmethod
    def _eval(expr):
        try:
            return gdb.parse_and_eval(expr)
        except gdb.error:
            return None

    @staticmethod
    def _read_ptr_at(base_addr, index):
        if base_addr == 0:
            return 0
        try:
            void_pp = gdb.lookup_type("void").pointer().pointer()
            slot_ptr = gdb.Value(base_addr).cast(void_pp) + index
            return int(slot_ptr.dereference())
        except Exception:
            return 0

    @staticmethod
    def _cast_ptr(addr, type_name):
        if addr == 0:
            return None
        try:
            return gdb.Value(addr).cast(gdb.lookup_type(type_name).pointer())
        except Exception:
            return None

    @staticmethod
    def _safe_cstr(char_ptr):
        try:
            if int(char_ptr) == 0:
                return "<null>"
            return char_ptr.string(errors="replace")
        except Exception:
            return "<unreadable>"

    @staticmethod
    def _inode_type_name(raw_value):
        mapping = {
            0: "REGULAR",
            1: "DIRECTORY",
            2: "SYMLINK",
            3: "CHAR_DEVICE",
            4: "BLOCK_DEVICE",
        }
        return mapping.get(raw_value, "UNKNOWN({})".format(raw_value))

    @staticmethod
    def _dentry_path(dentry_ptr, max_depth=64):
        try:
            addr = int(dentry_ptr)
        except Exception:
            return "<invalid>"

        if addr == 0:
            return "<null>"

        parts = []
        seen = set()
        cur = dentry_ptr
        depth = 0

        while int(cur) != 0 and depth < max_depth:
            cur_addr = int(cur)
            if cur_addr in seen:
                return "<cycle>"
            seen.add(cur_addr)

            name = ArxVfsCommand._safe_cstr(cur["name"])
            parent = cur["parent"]

            if int(parent) == 0:
                if name == "/" or name == "":
                    break
                parts.append(name)
                break

            parts.append(name)
            cur = parent
            depth += 1

        if depth >= max_depth:
            return "<depth-limit>"

        if len(parts) == 0:
            return "/"

        parts.reverse()
        return "/" + "/".join(parts)

    @staticmethod
    def _resolve_vfs_from_arg(arg):
        text = (arg or "").strip()
        if text == "":
            return None

        try:
            value = gdb.parse_and_eval(text)
            if int(value) == 0:
                return None
            if value.type.code == gdb.TYPE_CODE_PTR:
                return value
            return value.address
        except Exception as err:
            raise gdb.GdbError("invalid VFS expression '{}': {}".format(text, err))

    @staticmethod
    def _resolve_vfs_default():
        exprs = [
            "((Dispatcher*)platform.dispacher)->GetLogicLayerCaps()->virtualFileSystem",
            "((Dispatcher*)platform.dispatcher)->GetLogicLayerCaps()->virtualFileSystem",
            "((LogicLayerCaps*)((LogicLayerFactory*)((Dispatcher*)platform.dispacher)->logicLayerFactory)->LogicLayerExportCaps)->virtualFileSystem",
            "((LogicLayerCaps*)((LogicLayerFactory*)((Dispatcher*)platform.dispatcher)->logicLayerFactory)->LogicLayerExportCaps)->virtualFileSystem",
        ]

        for expr in exprs:
            value = ArxVfsCommand._eval(expr)
            if value is None:
                continue
            try:
                if int(value) != 0:
                    return value
            except Exception:
                pass

        dispatcher_addr = ArxResourceManagers.resolve_dispatcher_addr()
        if dispatcher_addr == 0:
            return None

        logic_factory_addr = ArxVfsCommand._read_ptr_at(dispatcher_addr, 1)
        if logic_factory_addr == 0:
            return None

        logic_caps_addr = ArxVfsCommand._read_ptr_at(logic_factory_addr, 0)
        if logic_caps_addr == 0:
            return None

        vfs_addr = ArxVfsCommand._read_ptr_at(logic_caps_addr, 1)
        if vfs_addr == 0:
            return None

        casted = ArxVfsCommand._cast_ptr(vfs_addr, "VirtualFileSystem")
        if casted is not None:
            return casted

        return gdb.Value(vfs_addr)

    def _resolve_vfs(self, arg):
        from_arg = self._resolve_vfs_from_arg(arg)
        if from_arg is not None:
            return from_arg

        resolved = self._resolve_vfs_default()
        if resolved is None:
            raise gdb.GdbError(
                "Failed to resolve VirtualFileSystem pointer. Pass one explicitly: arx-vfs <vfs_expr>"
            )
        return resolved

    @staticmethod
    def _print_inode(prefix, inode_ptr):
        inode_addr = int(inode_ptr)
        print("{}inode: 0x{:016x}".format(prefix, inode_addr))
        if inode_addr == 0:
            return

        try:
            inode = inode_ptr.dereference()
            inode_num = int(inode["inodeNumber"])
            inode_type = int(inode["type"])
            inode_size = int(inode["size"])
            inode_fs = int(inode["filesystem"])
            inode_ops = int(inode["inodeOps"])
            file_ops = int(inode["fileOps"])

            print("{}  inodeNumber: {}".format(prefix, inode_num))
            print("{}  type:        {}".format(prefix, ArxVfsCommand._inode_type_name(inode_type)))
            print("{}  size:        {}".format(prefix, inode_size))
            print("{}  filesystem:  0x{:016x}".format(prefix, inode_fs))
            print("{}  inodeOps:    0x{:016x}".format(prefix, inode_ops))
            print("{}  fileOps:     0x{:016x}".format(prefix, file_ops))
        except Exception as err:
            print("{}  <failed to decode inode: {}>".format(prefix, err))

    def _print_mount(self, index, mount_ptr):
        mount_addr = int(mount_ptr)
        print("mount[{}] @ 0x{:016x}".format(index, mount_addr))

        try:
            mount = mount_ptr.dereference()
        except Exception as err:
            print("  <failed to decode mount: {}>".format(err))
            print("")
            return

        fs_ptr = mount["filesystem"]
        root_ptr = mount["root"]
        parent_mount_ptr = mount["parentMount"]
        mount_point_ptr = mount["mountPoint"]
        next_ptr = mount["next"]

        print("  filesystem:  0x{:016x}".format(int(fs_ptr)))
        print("  root:        0x{:016x}".format(int(root_ptr)))
        print("  parentMount: 0x{:016x}".format(int(parent_mount_ptr)))
        print("  mountPoint:  0x{:016x}".format(int(mount_point_ptr)))
        print("  next:        0x{:016x}".format(int(next_ptr)))

        if int(fs_ptr) != 0:
            try:
                fs = fs_ptr.dereference()
                fs_type_ptr = fs["type"]
                root_inode_ptr = fs["rootInode"]
                print("  fs.rootInode: 0x{:016x}".format(int(root_inode_ptr)))
                if int(fs_type_ptr) != 0:
                    fs_type = fs_type_ptr.dereference()
                    fs_name = self._safe_cstr(fs_type["name"])
                    print("  fs.type:      {}".format(fs_name))
                else:
                    print("  fs.type:      <null>")
            except Exception as err:
                print("  <failed to decode filesystem: {}>".format(err))

        if int(root_ptr) != 0:
            try:
                root = root_ptr.dereference()
                root_name = self._safe_cstr(root["name"])
                root_path = self._dentry_path(root_ptr)
                print("  root.name:    {}".format(root_name))
                print("  root.path:    {}".format(root_path))
                self._print_inode("  root.", root["inode"])
            except Exception as err:
                print("  <failed to decode root dentry: {}>".format(err))

        if int(mount_point_ptr) != 0:
            try:
                mount_path = self._dentry_path(mount_point_ptr)
                print("  mounted_at:   {}".format(mount_path))
            except Exception as err:
                print("  mounted_at:   <failed: {}>".format(err))

        print("")

    @staticmethod
    def _collect_paths_from_dentry_cache(vfs):
        paths = []

        try:
            dcache = vfs["DentryCache"]
            if int(dcache) == 0:
                return paths

            buckets = dcache["b"]
            used_words = dcache["used"]
            if int(buckets) == 0 or int(used_words) == 0:
                return paths

            bits = int(dcache["bits"])
            capacity = 1 << bits
            key_len = int(dcache["key_len"])
            val_len = int(dcache["val_len"])
            stride = key_len + val_len

            byte_ptr_t = gdb.lookup_type("unsigned char").pointer()
            u32_ptr_t = gdb.lookup_type("uint32_t").pointer()
            dentry_pp_t = gdb.lookup_type("dentry_t").pointer().pointer()

            buckets_base = buckets.cast(byte_ptr_t)
            used_base = used_words.cast(u32_ptr_t)

            max_entries = 8192
            scanned = 0

            for i in range(capacity):
                word = int((used_base + (i >> 5)).dereference())
                if ((word >> (i & 0x1F)) & 1) == 0:
                    continue

                bucket = buckets_base + (i * stride)
                value_ptr = (bucket + key_len).cast(dentry_pp_t)
                dentry_ptr = value_ptr.dereference()

                if int(dentry_ptr) == 0:
                    continue

                paths.append(ArxVfsCommand._dentry_path(dentry_ptr))

                scanned += 1
                if scanned >= max_entries:
                    break
        except Exception:
            pass

        return paths

    def _print_paths_only(self, vfs_ptr):
        vfs = vfs_ptr.dereference()

        try:
            mount_head = vfs["MountListHead"]
            root_mount = vfs["Namespace"]["rootMount"]
        except Exception as err:
            raise gdb.GdbError("failed to decode VFS namespace/mount list: {}".format(err))

        print("Arx VFS paths")
        print("=============")
        print("vfs: 0x{:016x}".format(int(vfs_ptr)))
        print("rootMount: 0x{:016x}".format(int(root_mount)))
        print("")

        print("Mount paths")
        print("-----------")
        if int(mount_head) == 0:
            print("(no registered mounts)")
        else:
            seen = set()
            current = mount_head
            index = 0
            max_mounts = 256

            while int(current) != 0 and index < max_mounts:
                cur_addr = int(current)
                if cur_addr in seen:
                    print("<cycle detected at mount 0x{:016x}>".format(cur_addr))
                    break

                seen.add(cur_addr)
                mount = current.dereference()

                mount_point = mount["mountPoint"]
                root_dentry = mount["root"]

                root_path = self._dentry_path(root_dentry) if int(root_dentry) != 0 else "<null>"
                if int(mount["parentMount"]) == 0:
                    print("[rootfs] {}".format(root_path))
                else:
                    at_path = self._dentry_path(mount_point) if int(mount_point) != 0 else "<null>"
                    print("[mount]  {} -> {}".format(at_path, root_path))

                current = mount["next"]
                index += 1

            if index >= max_mounts:
                print("<stopped after {} mounts (sanity limit)>".format(max_mounts))

        print("")
        print("Cached dentries")
        print("--------------")

        cached_paths = self._collect_paths_from_dentry_cache(vfs)
        if len(cached_paths) == 0:
            print("(no cached dentries)")
            return

        unique_paths = sorted(set(cached_paths))
        for p in unique_paths:
            print(p)

    def invoke(self, arg, from_tty):
        del from_tty

        raw = (arg or "").strip()
        verbose = False
        vfs_expr = raw

        if raw.startswith("--verbose"):
            verbose = True
            vfs_expr = raw[len("--verbose"):].strip()
        elif raw.startswith("-v"):
            verbose = True
            vfs_expr = raw[2:].strip()

        vfs_ptr = self._resolve_vfs(vfs_expr)
        vfs_addr = int(vfs_ptr)
        if vfs_addr == 0:
            raise gdb.GdbError("VirtualFileSystem pointer is NULL")

        if not verbose:
            self._print_paths_only(vfs_ptr)
            return

        try:
            vfs = vfs_ptr.dereference()
        except Exception as err:
            raise gdb.GdbError("failed to dereference VirtualFileSystem: {}".format(err))

        print("Arx VFS state")
        print("=============")
        print("vfs: 0x{:016x}".format(vfs_addr))

        try:
            namespace = vfs["Namespace"]
            root_mount = namespace["rootMount"]
        except Exception as err:
            raise gdb.GdbError("failed to decode Namespace/rootMount: {}".format(err))

        try:
            mount_head = vfs["MountListHead"]
        except Exception as err:
            raise gdb.GdbError("failed to decode MountListHead: {}".format(err))

        dcache_ptr = 0
        dcache_count = "<unknown>"
        dcache_capacity = "<unknown>"
        try:
            dcache = vfs["DentryCache"]
            dcache_ptr = int(dcache)
            if dcache_ptr != 0:
                dcache_count = int(dcache["count"])
                if int(dcache["b"]) != 0:
                    dcache_capacity = 1 << int(dcache["bits"])
                else:
                    dcache_capacity = 0
        except Exception:
            pass

        print("rootMount:     0x{:016x}".format(int(root_mount)))
        print("mountListHead: 0x{:016x}".format(int(mount_head)))
        print("dentryCache:   0x{:016x}".format(dcache_ptr))
        print("dentryCount:   {}".format(dcache_count))
        print("dentryCap:     {}".format(dcache_capacity))
        print("")

        if int(mount_head) == 0:
            print("(no registered mounts)")
            return

        print("Mounts")
        print("------")

        seen = set()
        current = mount_head
        index = 0
        max_mounts = 256

        while int(current) != 0 and index < max_mounts:
            cur_addr = int(current)
            if cur_addr in seen:
                print("mount[{}] @ 0x{:016x}".format(index, cur_addr))
                print("  <cycle detected in mount list>")
                print("")
                break

            seen.add(cur_addr)
            self._print_mount(index, current)

            try:
                current = current["next"]
            except Exception as err:
                print("mount[{}] next decode failed: {}".format(index, err))
                break
            index += 1

        if index >= max_mounts:
            print("<stopped after {} mounts (sanity limit)>".format(max_mounts))


ArxVfsCommand()


class ArxInitRamFsCommand(gdb.Command):
    """Print initramfs archive entries parsed by InitRamFileSystemManager."""

    def __init__(self):
        super().__init__("arx-initramfs", gdb.COMMAND_STATUS)

    @staticmethod
    def _get_archive_storage(manager):
        try:
            archives = manager["Archives"]
            count = int(manager["ArchiveCount"])
            capacity = ArxPmmCommand._array_len(archives, fallback=count)
            return archives, count, capacity
        except Exception as err:
            raise gdb.GdbError("Failed to decode InitRamFileSystemManager storage: {}".format(err))

    def invoke(self, arg, from_tty):
        del from_tty

        manager = ArxResourceManagers.resolve_initramfs_manager()
        if manager is None:
            raise gdb.GdbError(
                "Failed to resolve InitRamFileSystemManager. Ensure ResourceLayerFactory has been created and symbols are loaded."
            )

        requested_path = (arg or "").strip()
        archives, count, capacity = self._get_archive_storage(manager)

        if count < 0:
            raise gdb.GdbError("invalid ArchiveCount={} (negative)".format(count))
        if count > capacity:
            raise gdb.GdbError(
                "invalid ArchiveCount={} exceeds capacity={} (possibly stale/corrupt debug state)".format(count, capacity)
            )

        print("Arx initramfs state")
        print("==================")
        print("manager:  0x{:016x}".format(int(manager)))
        print("count:    {}".format(count))
        print("capacity: {}".format(capacity))
        print("")

        if count == 0:
            print("(no parsed initramfs archives)")
            return

        if requested_path != "":
            for i in range(count):
                archive = archives[i]
                path_ptr = int(archive["path"])
                if path_ptr == 0:
                    continue
                path = archive["path"].string(errors="replace")
                if path == requested_path:
                    print("archive[{}]".format(i))
                    print("  path: {}".format(path))
                    print("  size: {}".format(int(archive["size"])))
                    print("  data: 0x{:016x}".format(int(archive["data"])))
                    return

            raise gdb.GdbError("path '{}' not found".format(requested_path))

        for i in range(count):
            archive = archives[i]
            path_ptr = int(archive["path"])
            path = "<null>"
            if path_ptr != 0:
                try:
                    path = archive["path"].string(errors="replace")
                except Exception:
                    path = "<unreadable>"

            print(
                "[{}] path={} size={} data=0x{:016x}".format(
                    i,
                    path,
                    int(archive["size"]),
                    int(archive["data"]),
                )
            )


ArxInitRamFsCommand()


class ArxProcCommand(gdb.Command):
    """List allocated processes or print one process by id from ProcessManager."""

    def __init__(self):
        super().__init__("arx-proc", gdb.COMMAND_STATUS)

    @staticmethod
    def _parse_process_id(arg):
        text = (arg or "").strip()
        if text == "":
            return None
        try:
            return int(gdb.parse_and_eval(text))
        except gdb.error:
            try:
                return int(text, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid process id '{}': {}".format(text, err))

    @staticmethod
    def _iter_process_tasks(head):
        node = head
        while int(node) != 0:
            yield node
            node = node["next"]

    @staticmethod
    def _print_process(process, index):
        pid = int(process["id"])
        addr_space = int(process["addressSpace"])
        tasks_head = process["tasks"]

        task_ids = []
        task_count = 0
        for task in ArxProcCommand._iter_process_tasks(tasks_head):
            task_count += 1
            if task_count <= 16:
                task_ids.append(int(task["id"]))

        print("process[{}]".format(index))
        print("  allocated:    {}".format(int(process["allocated"])))
        print("  id:           {}".format(pid))
        print("  addressSpace: 0x{:016x}".format(addr_space))
        print("  tasks_head:   0x{:016x}".format(int(tasks_head)))
        print("  task_count:   {}".format(task_count))
        if task_count == 0:
            print("  task_ids:     (none)")
        elif task_count <= 16:
            print("  task_ids:     {}".format(", ".join(str(x) for x in task_ids)))
        else:
            print("  task_ids:     {} ...".format(", ".join(str(x) for x in task_ids)))
        print("")

    @staticmethod
    def _get_process_array(process_manager):
        # Preferred path: typed member access.
        try:
            processes = process_manager["Processes"]
            capacity = ArxPmmCommand._array_len(processes, fallback=64)
            return processes, capacity
        except Exception:
            pass

        # Fallback path: ProcessManager object starts with process_t Processes[MAX_PROCESSES].
        try:
            manager_addr = int(process_manager)
            process_ptr_type = gdb.lookup_type("process_t").pointer()
            processes = gdb.Value(manager_addr).cast(process_ptr_type)
            return processes, 64
        except Exception as err:
            raise gdb.GdbError("Failed to decode ProcessManager process array: {}".format(err))

    def invoke(self, arg, from_tty):
        del from_tty

        process_manager = ArxResourceManagers.resolve_process_manager()
        if process_manager is None:
            raise gdb.GdbError(
                "Failed to resolve ProcessManager. Ensure platform and ResourceLayerFactory debug symbols are available."
            )

        processes, capacity = self._get_process_array(process_manager)

        requested_pid = self._parse_process_id(arg)

        print("Arx process state")
        print("=================")
        print("process_manager: 0x{:016x}".format(int(process_manager)))
        print("capacity: {}".format(capacity))
        print("")

        allocated_count = 0
        matched = None

        for i in range(capacity):
            process = processes[i]
            if int(process["allocated"]) == 0:
                continue

            allocated_count += 1
            pid = int(process["id"])

            if requested_pid is None:
                print(
                    "process[{}]: id={} addressSpace=0x{:016x} tasks_head=0x{:016x}".format(
                        i,
                        pid,
                        int(process["addressSpace"]),
                        int(process["tasks"]),
                    )
                )
            elif pid == requested_pid:
                matched = (i, process)
                break

        if requested_pid is None:
            print("")
            print("allocated_processes: {}".format(allocated_count))
            if allocated_count == 0:
                print("(no allocated processes)")
            return

        if matched is None:
            print("allocated_processes: {}".format(allocated_count))
            raise gdb.GdbError("process id {} not found among allocated processes".format(requested_pid))

        self._print_process(matched[1], matched[0])


ArxProcCommand()


class ArxTaskCommand(gdb.Command):
    """Print task details by task id from TaskManager."""

    def __init__(self):
        super().__init__("arx-task", gdb.COMMAND_STATUS)

    @staticmethod
    def _parse_task_id(arg):
        text = (arg or "").strip()
        if text == "":
            raise gdb.GdbError("usage: arx-task <task_id>")
        try:
            return int(gdb.parse_and_eval(text))
        except gdb.error:
            try:
                return int(text, 0)
            except ValueError as err:
                raise gdb.GdbError("invalid task id '{}': {}".format(text, err))

    @staticmethod
    def _print_task(task, index):
        print("task[{}]".format(index))
        print("  allocated:   {}".format(int(task["allocated"])))
        print("  id:          {}".format(int(task["id"])))
        print("  stack:       0x{:016x}".format(int(task["stack"])))
        print("  next:        0x{:016x}".format(int(task["next"])))
        print("  prev:        0x{:016x}".format(int(task["prev"])))
        # Keep context dump generic so this works across arch-specific task context layouts.
        print("  taskContext: {}".format(task["taskContext"]))
        print("")

    @staticmethod
    def _get_task_array(task_manager):
        # Preferred path: typed member access.
        try:
            tasks = task_manager["Tasks"]
            capacity = ArxPmmCommand._array_len(tasks, fallback=64)
            return tasks, capacity
        except Exception:
            pass

        # Fallback path: TaskManager object starts with task_t Tasks[MAX_TASKS].
        try:
            manager_addr = int(task_manager)
            task_ptr_type = gdb.lookup_type("task_t").pointer()
            tasks = gdb.Value(manager_addr).cast(task_ptr_type)
            return tasks, 64
        except Exception as err:
            raise gdb.GdbError("Failed to decode TaskManager task array: {}".format(err))

    def invoke(self, arg, from_tty):
        del from_tty

        task_manager = ArxResourceManagers.resolve_task_manager()
        if task_manager is None:
            raise gdb.GdbError(
                "Failed to resolve TaskManager. Ensure platform and ResourceLayerFactory debug symbols are available."
            )

        tasks, capacity = self._get_task_array(task_manager)

        requested_tid = self._parse_task_id(arg)

        print("Arx task state")
        print("==============")
        print("task_manager: 0x{:016x}".format(int(task_manager)))
        print("capacity: {}".format(capacity))
        print("")

        allocated_count = 0
        matched = None

        for i in range(capacity):
            task = tasks[i]
            if int(task["allocated"]) == 0:
                continue
            allocated_count += 1
            if int(task["id"]) == requested_tid:
                matched = (i, task)
                break

        if matched is None:
            print("allocated_tasks: {}".format(allocated_count))
            raise gdb.GdbError("task id {} not found among allocated tasks".format(requested_tid))

        self._print_task(matched[1], matched[0])


ArxTaskCommand()


class ArxResourceDebugCommand(gdb.Command):
    """Debug resource manager resolution path for arx-proc/arx-task."""

    def __init__(self):
        super().__init__("arx-rsrc", gdb.COMMAND_STATUS)

    @staticmethod
    def _type_available(type_name):
        try:
            gdb.lookup_type(type_name)
            return True
        except Exception:
            return False

    def invoke(self, arg, from_tty):
        del arg
        del from_tty

        dispatcher_addr = ArxResourceManagers.resolve_dispatcher_addr()
        if dispatcher_addr == 0:
            raise gdb.GdbError(
                "Failed to resolve dispatcher pointer field from platform (tried fields: dispacher, dispatcher)."
            )

        resource_factory_addr = ArxResourceManagers._read_ptr_at(dispatcher_addr, 0)
        caps_addr = ArxResourceManagers._read_ptr_at(resource_factory_addr, 0)
        process_manager_addr = ArxResourceManagers._read_ptr_at(caps_addr, 1)
        task_manager_addr = ArxResourceManagers._read_ptr_at(caps_addr, 3)
        initramfs_manager_addr = ArxResourceManagers._read_ptr_at(caps_addr, 4)

        print("Arx resource resolution debug")
        print("=============================")
        print("platform.dispatcher_ptr:   0x{:016x}".format(dispatcher_addr))
        print("resourceLayerFactory addr: 0x{:016x}".format(resource_factory_addr))
        print("ResourceLayerCaps addr:    0x{:016x}".format(caps_addr))
        print("ProcessManager addr:       0x{:016x}".format(process_manager_addr))
        print("TaskManager addr:          0x{:016x}".format(task_manager_addr))
        print("InitRamFSManager addr:     0x{:016x}".format(initramfs_manager_addr))
        print("")
        print("Type availability")
        print("-----------------")
        print("ResourceLayerCaps: {}".format("yes" if self._type_available("ResourceLayerCaps") else "no"))
        print("ProcessManager:    {}".format("yes" if self._type_available("ProcessManager") else "no"))
        print("TaskManager:       {}".format("yes" if self._type_available("TaskManager") else "no"))
        print("InitRamFileSystemManager: {}".format("yes" if self._type_available("InitRamFileSystemManager") else "no"))
        print("process_t:         {}".format("yes" if self._type_available("process_t") else "no"))
        print("task_t:            {}".format("yes" if self._type_available("task_t") else "no"))


ArxResourceDebugCommand()
