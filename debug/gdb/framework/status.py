"""Staged typed discovery diagnostics, including before Dispatcher publication."""
from .results import ReadFailure, Result
from .target import platform_address, registry, require_debug, verify_elf
from . import render

BOOT_STAGES = ("not-started", "boot-entered", "CPU-table-ready", "architecture-initializing",
               "devices-enumerating", "waiting-for-CPUs", "platform-selftests", "stack-barrier",
               "Dispatcher-published", "providers-published", "BSP-handoff")


def gather_status(memory, budget, options):
    verify_elf(memory, budget)
    result = Result(type_name="platform discovery",
                    count_semantics="staged availability; stored RAM observations")

    try:
        _gather_status(memory, budget, options, result)
    except ReadFailure as error:
        if str(error).startswith("release kernel:"):
            raise
        result.fail(error)
    return result


def _gather_status(memory, budget, options, result):
    def item(name, text, type_name="status"):
        budget.take_entry()
        result.items.append({"path": name, "type": type_name, "text": str(text)})

    try:
        require_debug(memory)
    except ReadFailure as error:
        if str(error).startswith("release kernel:"):
            raise
        result.fail(error)

    registry_address = options.get("--registry")
    if registry_address is not None:
        item("platform", "unavailable: explicit registry root has no platform root")
    else:
        try:
            address = platform_address(memory, options.get("--platform"))
            result.source = address
            item("platform", "%#x" % address, "platform_t*")
            # Keep boot progress if a later Dispatcher read/type is unavailable.
            try:
                stats = memory.field(address, "platform_t", "debugStats", budget)
                location, _ = memory.field_address(address, "platform_t", "debugStats")
                fields = render.gather(stats, budget, location)
                result.items.extend(fields.items)
                for issue in fields.issues:
                    result.fail(issue)
                stage = int(stats["stage"])
                if 0 <= stage < len(BOOT_STAGES):
                    item("bootPhase", BOOT_STAGES[stage])
                else:
                    item("bootPhase", "unknown(%d)" % stage)
                    result.fail("unknown platform boot stage")
            except ReadFailure as error:
                result.fail(error)
            dispatcher = int(memory.field(address, "platform_t", "dispacher", budget))
            item("dispatcher", "%#x" % dispatcher, "Dispatcher*")
            if dispatcher:
                registry_address, _ = memory.field_address(dispatcher, "Dispatcher", "debugManager")
            else:
                result.fail("unavailable: pre-Dispatcher; early progress remains readable")
        except ReadFailure as error:
            result.fail(error)

    if registry_address is not None:
        item("registry", "%#x" % registry_address, "DebugManager*")
        try:
            _, manager = registry(memory, budget, registry_address)
            item("registry.state", "ready")
            item("registry.providers", int(manager["reserved"]))
            from .commands import inline_text, pointer_text, STATES
            for index in range(int(manager["reserved"])):
                record = manager["records"][index]
                state = int(record["state"])
                if state == 1:
                    item("slot[%d]" % index, "partial publication")
                    result.fail("partial registry publication")
                    continue
                name = inline_text(record["name"])
                item("provider." + name, STATES.get(state, "invalid state"))
                if state not in STATES or state == 0:
                    result.fail("invalid reserved registry state")
                if state == 3 and int(record["reason"]):
                    item(name + ".reason", pointer_text(memory, record["reason"], budget))
                if state == 2:
                    type_name = inline_text(record["typeName"])
                    try:
                        if memory.sizeof(type_name) != int(record["size"]):
                            raise ReadFailure("mismatched ELF record size: " + type_name)
                        item(name + ".elfType", type_name + " available")
                    except ReadFailure as error:
                        item(name + ".elfType", str(error))
                        result.fail(error)
        except ReadFailure as error:
            result.fail(error)
