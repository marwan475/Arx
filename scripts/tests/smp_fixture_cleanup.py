#!/usr/bin/env python3
"""Repeat real QEMU boots to catch SMP fixture reclamation races."""

import argparse
import concurrent.futures
import pathlib
import subprocess
import tempfile
import time


def positive_int(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return number


def run_boot(index, args, root, log_directory):
    serial_path = log_directory / f"boot-{index}.log"
    stderr_path = log_directory / f"boot-{index}.stderr"
    command = [
        "qemu-system-x86_64", "-m", "1024", "-smp", str(args.cpus),
        "-cpu", "host" if args.kvm else "max", "-machine", "q35",
        "-display", "none", "-serial", f"file:{serial_path}",
        "-monitor", "none", "-drive",
        "if=pflash,format=raw,readonly=on,file=firmware/x86_64/X86_64_UEFI.fd",
        "-drive", f"file={args.image},format=raw", "-snapshot", "-no-reboot",
    ]
    if args.kvm:
        command.append("-enable-kvm")

    verdict = "TIMEOUT"
    with stderr_path.open("w") as stderr:
        process = subprocess.Popen(command, cwd=root, stdout=subprocess.DEVNULL, stderr=stderr)
        try:
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                serial = serial_path.read_text(errors="replace") if serial_path.exists() else ""
                if "SMP fixture cleanup failed" in serial:
                    verdict = "SMP_CLEANUP_FAIL"
                    break

                # QEMU can expose a partially written result line between reads.
                complete_lines = serial.split("\n")[:-1]
                if args.smp_only:
                    results = [
                        line.rstrip("\r") for line in complete_lines
                        if "[SELFTEST] RESULT poststartkernel/smp/scheduler_and_filesystem |" in line
                    ]
                    if results:
                        verdict = "PASS" if results[-1].endswith("=> PASS") else "OTHER_FAIL"
                        break

                summaries = [line for line in complete_lines if "overall     :" in line]
                if summaries:
                    verdict = "PASS" if "overall     : PASS" in summaries[-1] else "OTHER_FAIL"
                    break

                if process.poll() is not None:
                    verdict = "EXIT"
                    break
                time.sleep(0.1)
        finally:
            process.terminate()
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    print(f"{index}: {verdict} {serial_path}", flush=True)
    if verdict == "EXIT":
        print(stderr_path.read_text(), flush=True)
    return verdict


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="bin/arx-x86_64.img")
    parser.add_argument("--runs", type=positive_int, default=16)
    parser.add_argument("--jobs", type=positive_int, default=2)
    parser.add_argument("--cpus", type=positive_int, default=4)
    parser.add_argument("--timeout", type=positive_int, default=55)
    parser.add_argument("--kvm", action="store_true")
    parser.add_argument("--smp-only", action="store_true", help="stop after the SMP case")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    if not (root / args.image).is_file():
        parser.error(f"image does not exist: {args.image}; build x86_64 first")

    log_directory = pathlib.Path(tempfile.mkdtemp(prefix="arx-smp-cleanup-"))
    print(f"Logs: {log_directory}", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_boot, index, args, root, log_directory) for index in range(args.runs)]
        results = [future.result() for future in futures]

    print({result: results.count(result) for result in sorted(set(results))}, flush=True)
    return 0 if all(result == "PASS" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
