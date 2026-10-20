"""Semantic adapter table: (semantic ID, version) -> (root type, gather function).

Adding fields to stats never needs an adapter. Add an entry here only when a rich
collection needs interpretation beyond generic DWARF rendering.
"""
from . import foundation, platform, memory

ADAPTERS = {
    (memory.PMM_SEMANTICS, 1): ("arx_pmm_debug_stats_t", memory.pmm),
    (memory.HEAP_SEMANTICS, 1): ("arx_heap_debug_stats_t", memory.heap),
    (memory.DMA_SEMANTICS, 1): ("DmaDebugStats", memory.dma),
    (foundation.SEMANTICS, 1): ("FoundationCollections", foundation.gather),
    (platform.CPU_SEMANTICS, 1): ("arx_cpu_debug_stats_t", platform.cpus),
    (platform.PCI_SEMANTICS, 1): ("arx_pci_debug_stats_t", platform.pci),
}
