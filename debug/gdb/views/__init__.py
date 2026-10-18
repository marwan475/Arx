"""Semantic adapter table: (semantic ID, version) -> (root type, gather function).

Adding fields to stats never needs an adapter. Add an entry here only when a rich
collection needs interpretation beyond generic DWARF rendering.
"""
from . import foundation, platform

ADAPTERS = {
    (foundation.SEMANTICS, 1): ("FoundationCollections", foundation.gather),
    (platform.CPU_SEMANTICS, 1): ("arx_cpu_debug_stats_t", platform.cpus),
    (platform.PCI_SEMANTICS, 1): ("arx_pci_debug_stats_t", platform.pci),
}
