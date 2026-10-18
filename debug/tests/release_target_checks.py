"""Verify a matching release ELF reports erasure rather than missing debug types."""
import gdb

gdb.execute("source debug/gdb/arx.py")
from framework.commands import gather
from framework.results import ReadFailure

gdb.execute("thbreak kmain")
gdb.execute("continue")
try:
    gather("list")
except ReadFailure as error:
    assert "release kernel: debug framework compiled out" in str(error), error
else:
    raise AssertionError("release framework unexpectedly available")
print("PASS release target reports framework compiled out")
