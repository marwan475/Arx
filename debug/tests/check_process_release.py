#!/usr/bin/env python3
"""Release exclusion and matching-header native contracts for process views."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
from check_memory_release import ROOT, INCLUDES, output


def layout(directory, baseline):
    headers = ("kernel/layers/Resource/ProcessManager.hpp", "kernel/layers/Resource/TaskManager.hpp",
               "kernel/layers/Logic/ProcessController.hpp", "kernel/layers/Logic/CredentialManager.hpp")
    if baseline:
        for name in headers:
            destination = directory / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(output(["git", "show", "HEAD:" + name]))
    source = directory / "layout.cpp"
    types = ("process_t", "task_t", "ProcessManager", "TaskManager", "ProcessController", "CredentialManager")
    source.write_text("\n".join('#include <%s>' % header for header in headers)
                      + '\n#include <stddef.h>\n'
                      + "\n".join("char layout_%s[sizeof(%s)];" % (name, name) for name in types)
                      + '\nchar layout_context[offsetof(task_t, taskContext) + 1];\n'
                      + 'char layout_parent[offsetof(process_t, parentId) + 1];\n')
    obj = directory / "layout.o"
    subprocess.run(["g++", "-DDEBUG=0", "-I" + str(directory), "-I" + str(directory / "kernel"), *INCLUDES,
                    "-c", str(source), "-o", str(obj)], cwd=ROOT, check=True)
    return {match[2]: int(match[1], 16) for line in output(["nm", "-S", str(obj)]).splitlines()
            if (match := re.match(r"\S+\s+(\S+)\s+\S\s+(layout_\w+)", line))}


def main():
    areas = ("Resource/ProcessManager", "Resource/TaskManager", "Logic/ProcessLifecycle",
             "Logic/CredentialManager", "Logic/Scheduler", "Dispatcher")
    for area in areas:
        source = "kernel/layers/" + area + ".cpp"
        text = output(["g++", "-E", "-P", "-DDEBUG=0", *INCLUDES, source])
        assert not re.search(r"\b(DebugStats|DebugOperation|ProcessSlotDebug|TaskSlotDebug|DebugParent|DebugExit|DebugPublished|debugOperation|debugUpdate)\b", text), area
        obj = ROOT / "build/process-release/x86_64/kernel/layers" / (area + ".o")
        assembly = output(["objdump", "-dr", str(obj)])
        assert not re.search(r"DebugOperation|DebugUpdate|DebugParent|DebugExit|DebugPublished|DebugStats", assembly), area
    symbols = output(["nm", "-C", str(ROOT / "bin/process-release/kernel-x86_64.elf")])
    assert not re.search(r"DebugManager|DebugParent|DebugAllocated|DebugPublished|DebugOperation|DebugUpdate|DebugStats", symbols)
    assert not (ROOT / "build/process-release/x86_64/debug/kernel/Process.o").exists()
    with tempfile.TemporaryDirectory(prefix="arx-process-release-") as temporary:
        root = Path(temporary)
        baseline, current = root / "baseline", root / "current"
        baseline.mkdir()
        current.mkdir()
        assert layout(baseline, True) == layout(current, False), "release process/task layout changed"
        source = root / "native.cpp"
        source.write_text('#include <kernel/layers/Resource/ProcessManager.hpp>\n'
                          '#include <kernel/layers/Resource/TaskManager.hpp>\n'
                          '#include <debug/kernel/stats/LifecycleDebugStats.hpp>\n'
                          '#include <debug/kernel/stats/CredentialDebugStats.hpp>\n'
                          '#include <stddef.h>\n'
                          'static_assert(offsetof(process_t, debug) > 0);\n'
                          'static_assert(offsetof(task_t, debug) > 0);\n'
                          'static_assert(sizeof(ProcessDebugStats::records) == sizeof(process_t*));\n'
                          'static_assert(sizeof(TaskDebugStats::records) == sizeof(task_t*));\n')
        subprocess.run(["g++", "-DDEBUG=1", *INCLUDES, "-c", str(source), "-o", str(root / "native.o")], cwd=ROOT, check=True)
    print("Process release source/symbol/disassembly/layout exclusion and native headers: PASS")


if __name__ == "__main__":
    main()
