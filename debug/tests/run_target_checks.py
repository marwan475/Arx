"""Start an isolated snapshot QEMU, run GDB assertions, and always clean up.

Requires built bin/framework-debug and bin/framework-release kernels and debug
boot image (release image with --release), plus QEMU/GDB/objcopy. Run from repository root. All logs are under /tmp.
"""
import argparse
import os
import pathlib
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--release", action="store_true")
options = parser.parse_args()
configuration = "framework-release" if options.release else "framework-debug"

with tempfile.TemporaryDirectory(prefix="arx-framework-") as temporary:
    socket = pathlib.Path(temporary) / "gdb.sock"
    environment = dict(os.environ)
    for option, filename, key in (("--strip-debug", "no-dwarf.elf", "ARX_NO_DWARF_ELF"),
                                   ("--strip-all", "no-symbols.elf", "ARX_NO_SYMBOLS_ELF")):
        destination = pathlib.Path(temporary) / filename
        subprocess.run(["objcopy", option, "bin/framework-debug/kernel-x86_64.elf",
                        str(destination)], check=True)
        environment[key] = str(destination)
    command = ["qemu-system-x86_64", "-m", "1024", "-smp", "4", "-cpu", "max",
               "-machine", "q35", "-display", "none", "-monitor", "none",
               "-serial", "file:/tmp/arx-framework-check-%s-serial.log" % configuration,
               "-gdb", "unix:%s,server=on,wait=off" % socket, "-S",
               "-drive", "if=pflash,format=raw,readonly=on,file=firmware/x86_64/X86_64_UEFI.fd",
               "-drive", "file=bin/%s/arx-x86_64.img,format=raw" % configuration,
               "-snapshot", "-no-reboot"]
    with open("/tmp/arx-framework-check-%s-qemu.log" % configuration, "w") as output:
        qemu = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not socket.exists():
                if qemu.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("QEMU stub unavailable; inspect /tmp/arx-framework-check-qemu.log")
                time.sleep(0.05)
            checks = "release_target_checks.py" if options.release else "target_checks.py"
            gdb_command = ["gdb", "-nx", "-q", "-batch",
                           "-iex", "set debuginfod enabled off",
                           "bin/%s/kernel-x86_64.elf" % configuration,
                           "-ex", "set pagination off", "-ex", "set confirm off",
                           "-ex", "set debuginfod enabled off",
                           "-ex", "target remote " + str(socket),
                           "-ex", "source debug/tests/" + checks]
            if not options.release:
                gdb_command += ["-ex", "continue &", "-ex", "arx-debug list"]
            gdb = subprocess.run(gdb_command, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True, timeout=240,
                                 env=environment)
            log = "/tmp/arx-framework-check-%s-gdb.log" % configuration
            pathlib.Path(log).write_text(gdb.stdout)
            print(gdb.stdout)
            passed = "PASS release target" if options.release else "PASS target stages"
            if gdb.returncode or passed not in gdb.stdout:
                raise RuntimeError("GDB target checks failed")
            if not options.release and "target running; stop it explicitly" not in gdb.stdout:
                raise RuntimeError("running-target refusal failed")
        finally:
            qemu.terminate()
            try:
                qemu.wait(timeout=5)
            except subprocess.TimeoutExpired:
                qemu.kill()
                qemu.wait()
