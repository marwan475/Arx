"""Load with `source debug/gdb/arx.py` in GDB's Python interpreter."""
import os
import sys

_directory = os.path.dirname(os.path.abspath(sys._getframe().f_code.co_filename))
if _directory not in sys.path:
    sys.path.insert(0, _directory)

from framework.commands import Command

# Sourcing twice does not accumulate command registrations. All target state is
# gathered afresh per invocation; ELF/inferior replacement cannot retain roots.
if not globals().get("_arx_debug_registered", False):
    _arx_debug_command = Command()
    _arx_debug_registered = True
