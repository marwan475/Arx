import re
import gdb

from .results import Budget, ReadFailure, Result
from .target import Memory, discover, verify_elf
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
    options = {"--limit": 100, "--bytes": 65536, "--depth": 8}
    positional = []
    index = 0
    while index < len(words):
        word = words[index]
        if word.startswith("--"):
            if word not in options or index + 1 >= len(words):
                raise ReadFailure("invalid option: " + word)
            index += 1
            try:
                value = int(words[index], 0)
            except ValueError as error:
                raise ReadFailure("limits must be integers") from error
            maxima = {"--limit": 10000, "--bytes": 16 * 1024 * 1024, "--depth": 32}
            if not 1 <= value <= maxima[word]:
                raise ReadFailure("limit out of range: " + word)
            options[word] = value
        else:
            positional.append(word)
        index += 1
    return positional, Budget(options["--limit"], options["--bytes"], options["--depth"])


def gather(arguments):
    words, budget = parse(arguments)
    if not words:
        raise ReadFailure("usage: arx-debug list | <provider> stats | <provider> <view> | "
                          "inspect <type> <numeric-address> [--limit N --bytes N --depth N]")
    memory = Memory()
    # Discovery metadata has a separate fixed ceiling so --limit 1 still finds
    # records. Payload readers share the user-selected budget end to end.
    metadata = Budget(10000, 65536, 8)
    if words[0] == "inspect":
        if len(words) != 3 or not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", words[2]):
            raise ReadFailure("inspect requires a quoted type and numeric address; expressions forbidden")
        verify_elf(memory, metadata)
        address = int(words[2], 16 if words[2].lower().startswith("0x") else 10)
        value = memory.read(address, words[1], budget)
        return render.gather(value, budget, address), budget

    registry_address, manager = discover(memory, metadata)
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
