"""Run inside batch GDB attached to a fresh paused debug QEMU.

Only this test mutates controlled fixture memory; arx-debug itself is read-only.
"""
import os
import pathlib
import gdb

root = pathlib.Path.cwd()
gdb.execute("source debug/gdb/arx.py")
from framework.commands import gather
from framework.results import ReadFailure


def expect_error(arguments, text):
    try:
        gather(arguments)
    except ReadFailure as error:
        assert text in str(error), str(error)
        return
    raise AssertionError("expected error: " + text)


def stop_at(location):
    gdb.execute("thbreak " + location)
    gdb.execute("continue")


def check_fields(result, expected):
    fields = {item["path"]: item["text"] for item in result.items}
    for name, text in expected.items():
        assert fields[name] == text, fields


language = gdb.parameter("language")
stop_at("kmain")
expect_error("list", "pre-Dispatcher")
stop_at("DebugFoundationPublishC")
result, _ = gather("list")
assert len(result.items) == 2
assert all("unavailable" in item["text"] for item in result.items)
expect_error("fixture-c stats", "not published")
stop_at("DebugFoundationPublishCpp")
result, _ = gather("fixture-c stats")
assert result.status == "complete"
check_fields(result, {"calls": "1", "usage.bytes": "64"})
expect_error("fixture-cpp stats", "not published")
stop_at("DebugFoundationRuntimeUpdate")
result, _ = gather("fixture-cpp stats")
assert result.status == "complete"
for name in ("array", "list", "indices", "string", "buckets", "graph"):
    result, _ = gather("fixture-cpp " + name)
    assert result.status == "complete", (name, result)
    assert result.items

# Stop after begin, before a writer completes, without calling any target helper.
source = (root / "debug/kernel/Foundation.cpp").read_text().splitlines()
line = next(i + 1 for i, text in enumerate(source) if "ARX_DEBUG_ADD(" in text)
stop_at("debug/kernel/Foundation.cpp:%d" % line)
result, _ = gather("fixture-cpp stats")
assert any("interrupted selected" in issue for issue in result.issues)

symbol = gdb.lookup_global_symbol("arx_foundation_cpp_stats")
address = int(symbol.value().address)
size = gdb.lookup_type("FoundationCppStats").sizeof
inferior = gdb.selected_inferior()
saved = bytes(inferior.read_memory(address, size))
prefix = "arx_foundation_cpp_stats.collections."
try:
    result, _ = gather('inspect "FoundationCppStats" %#x' % address)
    assert result.items
    expect_error('inspect "FoundationCppStats" bad()', "expressions forbidden")
    expect_error('inspect "NoSuchType" %#x' % address, "missing ELF type")
    expect_error('inspect "FoundationCppStats" 1', "unreadable")

    gdb.execute("set variable " + prefix + "nodes[3].next = &" + prefix + "nodes[0]")
    result, _ = gather("fixture-cpp list")
    assert any("cycle" in issue for issue in result.issues)
    result, _ = gather("fixture-cpp graph")
    assert any("cycle or shared" in issue for issue in result.issues)
    gdb.execute("set variable " + prefix + "nodes[3].next = 0")
    gdb.execute("set variable " + prefix + "nodes[3].nextIndex = 4")
    result, _ = gather("fixture-cpp indices")
    assert any("invalid index" in issue for issue in result.issues)
    gdb.execute("set variable " + prefix + "nodes[3].nextIndex = -1")
    gdb.execute("set variable " + prefix + "head = 0")
    result, _ = gather("fixture-cpp list")
    assert len(result.items) == 0
    gdb.execute("set variable " + prefix + "head = (FoundationNode*) 1")
    result, _ = gather("fixture-cpp list")
    assert any("unreadable" in issue for issue in result.issues)
    gdb.execute("set variable " + prefix + "head = &" + prefix + "nodes[0]")
    gdb.execute("set variable " + prefix + "count = 5")
    expect_error("fixture-cpp array", "invalid collection length")
    gdb.execute("set variable " + prefix + "count = 4")
    for command, text in (("list --limit 1", "entry limit"),
                          ("array --bytes 160", "byte limit"),
                          ("graph --depth 1", "depth limit")):
        result, _ = gather("fixture-cpp " + command)
        assert any(text in issue for issue in result.issues), result
finally:
    inferior.write_memory(address, saved)

# Corrupt only the debug registry's format, then restore it.
manager = "((Dispatcher*)platform.dispacher)->debugManager"
gdb.execute("set variable " + manager + ".formatVersion = 99")
expect_error("list", "registry format")
gdb.execute("set variable " + manager + ".formatVersion = 1")
gdb.execute("set variable " + manager + ".records[1].state = 1")
result, _ = gather("list")
assert any("partial registry" in issue for issue in result.issues)
expect_error("fixture-cpp stats", "registry partially published")
gdb.execute("set variable " + manager + ".records[1].state = 2")
expect_error("absent stats", "provider absent")
assert gdb.parameter("language") == language

# No persistent pointer/type caches: new inferior and object-file replacement.
gdb.execute("add-inferior")
gdb.execute("inferior 2")
expect_error("list", "no inferior")
gdb.execute("inferior 1")
result, _ = gather("fixture-c stats")
assert result.items
gdb.execute("symbol-file bin/framework-release/kernel-x86_64.elf")
expect_error("list", "mismatched ELF")
gdb.execute("symbol-file " + os.environ["ARX_NO_DWARF_ELF"])
expect_error("list", "missing ELF debug symbols/types")
gdb.execute("symbol-file " + os.environ["ARX_NO_SYMBOLS_ELF"])
expect_error("list", "missing ELF symbols")
gdb.execute("symbol-file bin/framework-debug/kernel-x86_64.elf")
result, _ = gather("fixture-c stats")
assert result.items
assert gdb.parameter("language") == language

print("PASS target stages, typed records, bounded views, corruption, publication, ELF/inferior changes and settings")
