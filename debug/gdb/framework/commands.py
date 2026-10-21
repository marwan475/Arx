import re
import gdb

from .results import Budget, ReadFailure, Result
from .target import Memory, discover, verify_elf
from .status import gather_status
from . import readers, render
from views import ADAPTERS

STATES = {0: "empty", 1: "partial publication", 2: "ready",
          3: "unavailable", 4: "withdrawn"}


def inline_text(value):
    # Fixed-size metadata arrays already copied within the memory budget.
    low, high = value.type.range()
    data = bytearray()
    for index in range(low, high + 1):
        byte = int(value[index]) & 255
        if byte == 0:
            return data.decode("utf-8", "replace")
        data.append(byte)
    raise ReadFailure("invalid metadata: unterminated text")


def pointer_text(memory, address, budget):
    result = readers.string(memory, int(address), budget)
    if result.issues:
        raise ReadFailure("invalid metadata string: " + "; ".join(result.issues))
    return result.items[0]["text"]


def parse(arguments):
    words = gdb.string_to_argv(arguments)
    limits = {"--limit": 100, "--bytes": 65536, "--depth": 8}
    maxima = {"--limit": 10000, "--bytes": 16 * 1024 * 1024, "--depth": 32,
              "--vfs": (1 << 64) - 1, "--initramfs": (1 << 64) - 1,
              "--space": (1 << 64) - 1, "--va": (1 << 64) - 1, "--size": 16 * 1024 * 1024,
              "--pid": 63, "--tid": 127, "--scan": 1048576,
              "--cpu": 255, "--slot": 255, "--node": 7, "--order": 10, "--class": 131072,
              "--platform": (1 << 64) - 1, "--registry": (1 << 64) - 1}
    options = {}
    positional = []
    index = 0
    while index < len(words):
        word = words[index]
        if word.startswith("--"):
            if word == "--check-bitmap":
                if word in options:
                    raise ReadFailure("duplicate option: " + word)
                options[word] = True
                index += 1
                continue
            if word not in maxima and word not in ("--device", "--zone", "--path"):
                raise ReadFailure("invalid option: " + word)
            if word in options or index + 1 >= len(words):
                raise ReadFailure("missing or duplicate option: " + word)
            index += 1
            value = words[index]
            if word == "--path":
                if not value or len(value.encode("utf-8")) >= 512 or "\x00" in value:
                    raise ReadFailure("path is empty, excessive or contains NUL")
            elif word == "--zone":
                if value not in ("normal", "dma"):
                    raise ReadFailure("zone requires normal or dma")
            elif word == "--device":
                if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]", value):
                    raise ReadFailure("device requires ssss:bb:dd.f")
                segment, bus, devfun = value.split(":")
                device, function = devfun.split(".")
                value = tuple(int(part, 16) for part in (segment, bus, device, function))
                if value[2] > 31:
                    raise ReadFailure("device number out of range")
            else:
                if not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", value):
                    raise ReadFailure("option requires a numeric value; expressions forbidden")
                value = int(value, 16 if value.lower().startswith("0x") else 10)
                minimum = 0 if word in ("--cpu", "--slot", "--node", "--order", "--pid", "--tid", "--va") else 1
                if not minimum <= value <= maxima[word]:
                    raise ReadFailure("limit out of range: " + word)
            options[word] = value
        else:
            positional.append(word)
        index += 1
    if "--pid" in options and "--tid" in options:
        raise ReadFailure("--pid and --tid are mutually exclusive")
    if "--cpu" in options and "--slot" in options:
        raise ReadFailure("--cpu and --slot are mutually exclusive")
    if "--node" in options and ("--cpu" in options or "--slot" in options):
        raise ReadFailure("--node and CPU selectors are mutually exclusive")
    if "--platform" in options and "--registry" in options:
        raise ReadFailure("--platform and --registry are mutually exclusive")
    for name in limits:
        if name in options:
            limits[name] = options.pop(name)
    return positional, Budget(limits["--limit"], limits["--bytes"], limits["--depth"]), options


def gather(arguments):
    words, budget, options = parse(arguments)
    selectors = {name: value for name, value in options.items()
                 if name in ("--cpu", "--slot", "--device", "--node", "--zone", "--order", "--class", "--check-bitmap", "--pid", "--tid", "--scan", "--space", "--va", "--size", "--path", "--vfs", "--initramfs")}
    if len(words) >= 2 and not (words == ["memory", "backing"]) and tuple(words[:2]) in (("pmm", "pfn"), ("heap", "address"), ("memory", "backing")):
        if len(words) != 3 or not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", words[2]):
            raise ReadFailure("selected PFN/address requires one numeric value; expressions forbidden")
        value = int(words[2], 16 if words[2].lower().startswith("0x") else 10)
        if value >= 1 << 64:
            raise ReadFailure("selected PFN/address exceeds 64-bit range")
        selectors["--value"] = value
        words = words[:2]
    if not words:
        raise ReadFailure("usage: arx-debug status | list | <provider> stats | <provider> <view> | "
                          "inspect <type> <numeric-address> [--limit N --bytes N --depth N]")
    memory = Memory()
    # Discovery metadata has a separate fixed ceiling so --limit 1 still finds
    # records. Payload readers share the user-selected budget end to end.
    metadata = Budget(10000, 65536, 8)
    if words == ["status"]:
        if selectors:
            raise ReadFailure("selectors are not valid for status")
        return gather_status(memory, budget, options), budget
    if words[0] == "inspect":
        if options:
            raise ReadFailure("inspect does not accept roots or selectors")
        if len(words) != 3 or not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", words[2]):
            raise ReadFailure("inspect requires a quoted type and numeric address; expressions forbidden")
        verify_elf(memory, metadata)
        address = int(words[2], 16 if words[2].lower().startswith("0x") else 10)
        value = memory.read(address, words[1], budget)
        return render.gather(value, budget, address), budget

    if selectors and (len(words) != 2 or words[1] == "stats"):
        raise ReadFailure("selectors require a matching rich view")
    registry_address, manager = discover(memory, metadata, options.get("--platform"),
                                         options.get("--registry"))
    count = int(manager["reserved"])
    records = manager["records"]
    if words == ["list"]:
        result = Result(source=registry_address, type_name="DebugManager",
                        count_semantics="reserved registry slots")
        for index in range(count):
            try:
                budget.take_entry()
            except ReadFailure as error:
                result.fail(error)
                break
            record = records[index]
            state = int(record["state"])
            if state == 1:
                result.items.append({"path": "slot[%d]" % index, "type": "registry",
                                     "text": "partial publication"})
                result.fail("partial registry publication")
                continue
            name = inline_text(record["name"])
            text = STATES.get(state, "invalid state")
            if state == 3 and int(record["reason"]):
                text += ": " + pointer_text(memory, record["reason"], metadata)
            result.items.append({"path": name, "type": inline_text(record["typeName"]),
                                 "text": text})
            if state not in STATES:
                result.fail("invalid registry state")
        return result, budget
    if len(words) != 2:
        raise ReadFailure("expected <provider> stats or <provider> <view>")
    record = None
    for index in range(count):
        candidate = records[index]
        if int(candidate["state"]) == 1:
            continue
        if inline_text(candidate["name"]) == words[0]:
            record = candidate
            break
    if record is None:
        partial = any(int(records[i]["state"]) == 1 for i in range(count))
        raise ReadFailure("provider absent" + ("; registry partially published" if partial else ""))
    state = int(record["state"])
    if state != 2:
        reason = STATES.get(state, "invalid state")
        if state == 3 and int(record["reason"]):
            reason += ": " + pointer_text(memory, record["reason"], metadata)
        raise ReadFailure("provider " + reason)
    if int(record["version"]) == 0:
        raise ReadFailure("invalid record version")
    type_name = inline_text(record["typeName"])
    if memory.sizeof(type_name) != int(record["size"]):
        raise ReadFailure("mismatched ELF record size")
    if words[1] == "stats":
        address = int(record["address"])
        value = memory.read(address, type_name, budget)
        result = render.gather(value, budget, address)
    else:
        view_count = int(record["viewCount"])
        if not 0 <= view_count <= 64:
            raise ReadFailure("invalid view count")
        base = int(record["views"])
        if view_count and not base:
            raise ReadFailure("unavailable: null view descriptors")
        result = None
        for index in range(view_count):
            descriptor = memory.read(base + index * memory.sizeof("arx_debug_view_t"),
                                     "arx_debug_view_t", metadata)
            if pointer_text(memory, descriptor["name"], metadata) != words[1]:
                continue
            semantics = pointer_text(memory, descriptor["semanticsId"], metadata)
            root_type = pointer_text(memory, descriptor["rootType"], metadata)
            adapter = ADAPTERS.get((semantics, int(descriptor["version"])))
            if adapter is None:
                raise ReadFailure("unavailable: no adapter for " + semantics)
            expected_type, gather_view = adapter
            if root_type != expected_type:
                raise ReadFailure("mismatched view root type")
            if selectors.keys() - set(getattr(gather_view, "selectors", ())):
                raise ReadFailure("selector not supported by this view")
            if selectors:
                result = gather_view(words[1], memory, descriptor, budget, selectors)
            else:
                result = gather_view(words[1], memory, descriptor, budget)
            break
        if result is None:
            raise ReadFailure("view absent")
    if int(record["group"]):
        group = memory.read(int(record["group"]), "arx_debug_group_t", metadata)
        active = int(group["activeWriters"])
        if active:
            result.fail("interrupted selected grouped update: active writers=%d" % active)
    return render.expand(result, budget), budget


class Command(gdb.Command):
    def __init__(self):
        super().__init__("arx-debug", gdb.COMMAND_DATA)

    def invoke(self, arguments, from_tty):
        self.dont_repeat()
        try:
            result, budget = gather(arguments)
            render.present(result, budget)
        except (ReadFailure, gdb.error, ValueError, OverflowError) as error:
            gdb.write("arx-debug: %s\n" % error, gdb.STDERR)
