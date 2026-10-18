"""Read stopped target memory with matching ELF layouts; never evaluate expressions."""
import struct
import gdb

from .results import ReadFailure

MAGIC = 0x4152584445425547
VERSION = 1


class Memory:
    def __init__(self):
        self.inferior = gdb.selected_inferior()
        selected = gdb.selected_thread()
        if selected is not None and selected.is_running():
            raise ReadFailure("unavailable: target running; stop it explicitly")
        if not self.inferior.is_valid() or self.inferior.pid == 0:
            raise ReadFailure("unavailable: no inferior")
        if any(thread.is_running() for thread in self.inferior.threads()):
            raise ReadFailure("unavailable: target running; stop it explicitly")

    def type(self, name):
        try:
            return gdb.lookup_type(name).strip_typedefs()
        except gdb.error as error:
            raise ReadFailure("missing ELF type %s: %s" % (name, error)) from error

    def sizeof(self, name):
        return self.type(name).sizeof

    def read_bytes(self, address, size, budget):
        budget.take_bytes(size)
        if address <= 0 or address + size > (1 << 64):
            raise ReadFailure("invalid address range")
        try:
            return bytes(self.inferior.read_memory(address, size))
        except (gdb.error, MemoryError) as error:
            raise ReadFailure("unreadable memory at %#x: %s" % (address, error)) from error

    def read(self, address, type_name, budget):
        value_type = self.type(type_name) if isinstance(type_name, str) else type_name
        data = self.read_bytes(address, value_type.sizeof, budget)
        return gdb.Value(data, value_type)

    def field_address(self, address, type_name, name):
        field = next((field for field in self.type(type_name).fields()
                      if field.name == name), None)
        if field is None or field.bitpos % 8:
            raise ReadFailure("missing/mismatched ELF field %s.%s" % (type_name, name))
        return address + field.bitpos // 8, field.type

    def field(self, address, type_name, name, budget):
        location, field_type = self.field_address(address, type_name, name)
        return self.read(location, field_type, budget)


def verify_elf(memory, budget):
    """Compare GNU build-id notes in the loaded ELF and target RAM.

    Kernel addresses are fixed by the x86_64 linker. No map offsets or cached
    addresses survive a command. Failure to verify is explicit, never a fallback.
    """
    filename = gdb.current_progspace().filename
    if not filename:
        raise ReadFailure("missing ELF: load the matching kernel file")
    try:
        with open(filename, "rb") as elf:
            header = elf.read(64)
            if header[:6] != b"\x7fELF\x02\x01":
                raise ReadFailure("unsupported ELF: requires little-endian ELF64")
            if struct.unpack_from("<H", header, 18)[0] != 62:
                raise ReadFailure("unsupported architecture: requires x86_64")
            offset = struct.unpack_from("<Q", header, 40)[0]
            entry_size, count, strings_index = struct.unpack_from("<HHH", header, 58)
            if entry_size != 64 or not 1 <= count <= 4096 or strings_index >= count:
                raise ReadFailure("invalid ELF section table")
            elf.seek(offset)
            sections = [elf.read(entry_size) for _ in range(count)]
            strings = sections[strings_index]
            strings_offset, strings_size = struct.unpack_from("<QQ", strings, 24)
            if strings_size > 16 * 1024 * 1024:
                raise ReadFailure("invalid ELF string table")
            elf.seek(strings_offset)
            names = elf.read(strings_size)
            for section in sections:
                name_offset = struct.unpack_from("<I", section)[0]
                name = names[name_offset:].split(b"\0", 1)[0]
                if name != b".note.gnu.build-id":
                    continue
                address, file_offset, size = struct.unpack_from("<QQQ", section, 16)
                if size <= 0 or size > 4096:
                    raise ReadFailure("invalid ELF build-id size")
                elf.seek(file_offset)
                expected = elf.read(size)
                if memory.read_bytes(address, size, budget) != expected:
                    raise ReadFailure("mismatched ELF: target build-id differs")
                has_symbols = any(struct.unpack_from("<I", item, 4)[0] == 2
                                  for item in sections)
                if not has_symbols:
                    raise ReadFailure("missing ELF symbols: stripped kernel")
                return
    except (OSError, struct.error) as error:
        raise ReadFailure("unreadable/invalid ELF: %s" % error) from error
    raise ReadFailure("missing ELF build-id: cannot verify target identity")


def require_debug(memory):
    # A minimal symbol survives even when release has no DWARF. This marker only
    # exists in debug ELFs, so missing debug types can be distinguished from release.
    marker = gdb.lookup_global_symbol("arx_debug_format_marker")
    if marker is None:
        try:
            # info address only resolves a symbol; it cannot execute target code.
            gdb.execute("info address arx_debug_format_marker", to_string=True)
        except gdb.error:
            raise ReadFailure("release kernel: debug framework compiled out")
        raise ReadFailure("missing ELF debug symbols/types for debug framework")
    dispatcher_type = memory.type("Dispatcher")
    if not any(field.name == "debugManager" for field in dispatcher_type.fields()):
        raise ReadFailure("release kernel: DebugManager compiled out")


def platform_address(memory, override=None):
    if override is not None:
        return override
    symbol = gdb.lookup_global_symbol("platform")
    if symbol is None:
        raise ReadFailure("missing symbols: platform")
    return int(symbol.value().address)


def registry(memory, budget, address):
    manager = memory.read(address, "DebugManager", budget)
    if int(manager["magic"]) != MAGIC or int(manager["formatVersion"]) != VERSION:
        raise ReadFailure("mismatched/uninitialized registry format")
    if int(manager["capacity"]) != 64 or not 0 <= int(manager["reserved"]) <= 64:
        raise ReadFailure("invalid registry bounds")
    return address, manager


def discover(memory, budget, platform_override=None, registry_override=None):
    verify_elf(memory, budget)
    require_debug(memory)
    if registry_override is not None:
        return registry(memory, budget, registry_override)
    root = platform_address(memory, platform_override)
    dispatcher_address = int(memory.field(root, "platform_t", "dispacher", budget))
    if dispatcher_address == 0:
        raise ReadFailure("unavailable: pre-Dispatcher")
    address, _ = memory.field_address(dispatcher_address, "Dispatcher", "debugManager")
    return registry(memory, budget, address)
