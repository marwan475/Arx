#!/usr/bin/env python3
"""Check release erasure, baseline layouts and native-reader header usability."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
INCLUDES = ["-I.", "-Ikernel", "-Ikernel/platform", "-Ikernel/selftests",
            "-Ikernel/platform/terminal/flanterm", "-Ikernel/platform/acpi/uACPI/include"]


def output(command):
    return subprocess.check_output(command, cwd=ROOT, text=True)


def layout(directory, baseline):
    headers = ("kernel/platform/platform.h", "kernel/platform/memory/pmm.h",
               "kernel/platform/memory/heap.h", "kernel/layers/Resource/DmaManager.hpp")
    if baseline:
        for name in headers:
            destination = directory / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(output(["git", "show", "HEAD:" + name]))
    source = directory / "layout.cpp"
    source.write_text('#include <kernel/platform/platform.h>\n'
                      '#include <kernel/layers/Resource/DmaManager.hpp>\n'
                      + "\n".join("char layout_%s[sizeof(%s)];" % (name.replace("_t", ""), name)
                                  for name in ("zone_t", "cache_t", "kernel_heap_t", "numa_node_t", "platform_t", "DmaManager")))
    object_file = directory / "layout.o"
    subprocess.run(["g++", "-DDEBUG=0", "-I" + str(directory), "-I" + str(directory / "kernel/platform"),
                    *INCLUDES, "-c", str(source), "-o", str(object_file)], cwd=ROOT, check=True)
    return {match[2]: int(match[1], 16) for line in output(["nm", "-S", str(object_file)]).splitlines()
            if (match := re.match(r"\S+\s+(\S+)\s+\S\s+(layout_\w+)", line))}


def main():
    for source in ("kernel/platform/memory/pmm.c", "kernel/platform/memory/heap.c",
                   "kernel/layers/Resource/DmaManager.cpp", "kernel/layers/Dispatcher.cpp"):
        compiler = "g++" if source.endswith(".cpp") else "gcc"
        text = output([compiler, "-E", "-P", "-DDEBUG=0", *INCLUDES, source])
        assert not re.search(r"\b(debugStats|debugPmm|debugHeap|DebugStats|debugGroup|DebugDmaOperation|arx_debug_group_begin|arx_allocation_success)\b", text), source
    elf = ROOT / "bin/memory-release/kernel-x86_64.elf"
    symbols = output(["nm", "-C", str(elf)])
    assert not re.search(r"arx_debug_|DebugManager|DebugPlatform|DebugDmaOperation|DmaManager::Debug", symbols)
    for source in ("kernel/platform/memory/pmm", "kernel/platform/memory/heap", "kernel/layers/Resource/DmaManager"):
        assembly = output(["objdump", "-dr", str(ROOT / "build/memory-release/x86_64" / (source + ".o"))])
        assert not re.search(r"arx_debug_|DebugStats|DebugOutcome|DebugDmaOperation", assembly), source
    with tempfile.TemporaryDirectory(prefix="arx-memory-release-") as temporary:
        base = Path(temporary)
        baseline, current = base / "baseline", base / "current"
        baseline.mkdir()
        current.mkdir()
        assert layout(baseline, True) == layout(current, False), "release layout differs from baseline"
        probe = base / "probe.c"
        probe.write_text('#include <debug/kernel/DebugRecords.h>\n'
                         'extern void side_effect(void);\nvoid probe(void)\n{\n'
                         'ARX_DEBUG_ONLY(side_effect();)\nARX_DEBUG_RUN(side_effect());\n'
                         'ARX_DEBUG_ADD(nonexistent.counter, side_effect());\n'
                         'ARX_DEBUG_STORE(nonexistent.counter, side_effect());\n}\n')
        assembly = output(["gcc", "-S", "-O2", "-DDEBUG=0", *INCLUDES, str(probe), "-o", "-"])
        assert "side_effect" not in assembly, "release evaluated a debug argument"
        native = base / "native.cpp"
        native.write_text('#include <kernel/layers/Resource/DmaManager.hpp>\n'
                          '#include <stddef.h>\n'
                          'static_assert(sizeof(DmaManager::DebugAllocationRecord) > 0);\n'
                          'static_assert(offsetof(DmaManager::DebugAllocationRecord, debug) > 0);\n')
        subprocess.run(["g++", "-DDEBUG=1", *INCLUDES, "-c", str(native), "-o", str(base / "native.o")], cwd=ROOT, check=True)
    print("Release erasure, disassembly, baseline layouts, argument side effects and native headers: PASS")


if __name__ == "__main__":
    main()
