#!/usr/bin/env python3
"""Run allocator inspection checks with a disposable QEMU snapshot and GDB."""
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, choices=(1, 4), default=4)
    parser.add_argument("--release", action="store_true")
    parser.add_argument("--suite", choices=("memory", "process"), default="memory")
    parser.add_argument("--artifact-dir", type=Path)
    parser.add_argument("--full-boot", action="store_true")
    args = parser.parse_args()
    mode = "release" if args.release else "debug"
    output = Path(tempfile.mkdtemp(prefix="arx-%s-%s-%d-" % (args.suite, mode, args.cpus)))
    artifacts = args.artifact_dir or Path("bin/%s-%s" % (args.suite, mode))
    elf = (artifacts / "kernel-x86_64.elf").resolve()
    image = (artifacts / "arx-x86_64.img").resolve()
    if not elf.exists() or not image.exists():
        parser.error("build the matching bin/memory-%s ELF and image first" % mode)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    script = output / "checks.gdb"
    script.write_text("set remotetimeout 30\ntarget remote 127.0.0.1:%d\n"
                      "python\nimport sys\nsys.dont_write_bytecode = True\n"
                      "sys.path.insert(0, 'debug/tests')\n"
                      "from %s_target import run\nrun(%r)\nend\nquit\n" % (port, "boot" if args.full_boot else args.suite, args.release))
    command = ["qemu-system-x86_64", "-m", "1024", "-smp", str(args.cpus), "-cpu", "max",
               "-machine", "q35", "-display", "none", "-serial", "file:" + str(output / "serial.log"),
               "-monitor", "none", "-drive", "if=pflash,format=raw,readonly=on,file=firmware/x86_64/X86_64_UEFI.fd",
               "-drive", "file=%s,format=raw" % image, "-snapshot", "-no-reboot", "-S",
               "-gdb", "tcp:127.0.0.1:%d" % port]
    with (output / "qemu.log").open("w") as qemu_log, (output / "gdb.log").open("w") as gdb_log:
        guest = subprocess.Popen(command, stdout=qemu_log, stderr=subprocess.STDOUT)
        try:
            # GDB's TCP retry handles listener startup; all target work is capped.
            result = subprocess.run(["gdb", "-q", "-batch", str(elf), "-x", str(script)],
                                    stdout=gdb_log, stderr=subprocess.STDOUT, timeout=240)
        finally:
            guest.terminate()
            guest.wait(timeout=10)
    text = (output / "gdb.log").read_text()
    serial = (output / "serial.log").read_text(errors="replace")
    marker = "BOOT TARGET COMPLETE" if args.full_boot else "%s TARGET %s PASS" % (args.suite.upper(), mode.upper())
    assert result.returncode == 0 and marker in text, "target checks failed: " + str(output / "gdb.log")
    if not args.release:
        cases = ("platform/memory/physical_allocation", "platform/memory/heap_cache",
                 "resource/PhysicalMemoryManager/debug_counters", "resource/DmaManager/failure_rollback")
        if args.suite == "process" and not args.full_boot:
            cases += ("resource/ProcessManager/debug_incarnations", "resource/TaskManager/debug_incarnations",
                      "logic/CredentialManager/debug_outcomes", "logic/ProcessController/debug_failures")
        for case in cases:
            lines = [line for line in serial.splitlines() if "RESULT " + case + " |" in line]
            assert len(lines) == 1 and "=> PASS" in lines[0], (case, lines, str(output))
        assert "=> FAIL" not in serial, "kernel self-test failure: " + str(output / "serial.log")
    print("%s; cpus=%d; logs=%s" % (marker, args.cpus, output))


if __name__ == "__main__":
    main()
