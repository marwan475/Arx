#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

arch="${ARCH:-x86_64}"
kernel_elf="${KERNEL_ELF:-}"
img_path="${IMG_PATH:-}"
qemu_bin="${QEMU_BIN:-}"
gdb_bin="${GDB_BIN:-gdb}"
gdb_port="${QEMU_GDB_PORT:-1234}"
qemu_common="${QEMU_COMMON:-}"
x86_64_uefi="${X86_64_UEFI:-}"
aarch64_uefi="${AARCH64_UEFI:-}"
serial_log="${SERIAL_LOG:-${repo_root}/build/qemu-debug-serial.log}"
qemu_pid=""

cleanup() {
    if [[ -n "${qemu_pid}" ]] && kill -0 "${qemu_pid}" 2>/dev/null; then
        kill "${qemu_pid}" 2>/dev/null || true
        wait "${qemu_pid}" 2>/dev/null || true
    fi
}

wait_for_gdb_stub() {
    local port="$1"
    local attempt

    for attempt in $(seq 1 100); do
        if command -v ss >/dev/null 2>&1; then
            if ss -ltnH 2>/dev/null | awk '{print $4}' | grep -Eq "[.:]${port}$"; then
                return 0
            fi
        elif command -v netstat >/dev/null 2>&1; then
            if netstat -ltn 2>/dev/null | awk '{print $4}' | grep -Eq "[.:]${port}$"; then
                return 0
            fi
        fi

        if [[ -n "${qemu_pid}" ]] && ! kill -0 "${qemu_pid}" 2>/dev/null; then
            echo "QEMU exited before GDB could connect." >&2
            return 1
        fi

        sleep 0.1
    done

    echo "Timed out waiting for QEMU GDB stub on localhost:${port}." >&2
    return 1
}

trap cleanup EXIT INT TERM

[[ -n "${kernel_elf}" ]] || { echo "KERNEL_ELF is required." >&2; exit 1; }
[[ -n "${img_path}" ]] || { echo "IMG_PATH is required." >&2; exit 1; }
[[ -n "${qemu_bin}" ]] || { echo "QEMU_BIN is required." >&2; exit 1; }

command -v "${qemu_bin}" >/dev/null 2>&1 || { echo "QEMU binary not found: ${qemu_bin}" >&2; exit 1; }
command -v "${gdb_bin}" >/dev/null 2>&1 || { echo "GDB binary not found: ${gdb_bin}" >&2; exit 1; }
[[ -f "${kernel_elf}" ]] || { echo "Kernel ELF not found: ${kernel_elf}" >&2; exit 1; }
[[ -f "${img_path}" ]] || { echo "Image not found: ${img_path}" >&2; exit 1; }
mkdir -p "$(dirname "${serial_log}")"

if [[ "${arch}" == "x86_64" ]]; then
    [[ -f "${x86_64_uefi}" ]] || { echo "x86_64 UEFI firmware not found: ${x86_64_uefi}" >&2; exit 1; }

    echo "Starting x86_64 QEMU paused with GDB stub on localhost:${gdb_port}"
    GDK_BACKEND=x11 "${qemu_bin}" ${qemu_common} \
        -display gtk,grab-on-hover=on \
        -serial file:"${serial_log}" \
        -drive if=pflash,format=raw,readonly=on,file="${x86_64_uefi}" \
        -drive file="${img_path}",format=raw \
        -gdb tcp::"${gdb_port}" -S -no-reboot -no-shutdown &
    qemu_pid=$!

    wait_for_gdb_stub "${gdb_port}"

    "${gdb_bin}" -tui "${kernel_elf}" \
        -ex "set architecture i386:x86-64" \
        -ex "set breakpoint pending on" \
        -ex "layout src" \
        -ex "target remote localhost:${gdb_port}" \
        -ex "hbreak _start" \
        -ex "continue"
elif [[ "${arch}" == "aarch64" ]]; then
    [[ -f "${aarch64_uefi}" ]] || { echo "aarch64 UEFI firmware not found: ${aarch64_uefi}" >&2; exit 1; }

    echo "Starting aarch64 QEMU paused with GDB stub on localhost:${gdb_port}"
    GDK_BACKEND=x11 "${qemu_bin}" \
        -machine virt -cpu cortex-a72 ${qemu_common} \
        -display gtk,grab-on-hover=on \
        -serial file:"${serial_log}" \
        -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet \
        -bios "${aarch64_uefi}" \
        -drive if=none,id=osdisk,file="${img_path}",format=raw \
        -device virtio-blk-pci,drive=osdisk,bootindex=0 \
        -gdb tcp::"${gdb_port}" -S -no-reboot -no-shutdown &
    qemu_pid=$!

    wait_for_gdb_stub "${gdb_port}"

    "${gdb_bin}" -tui "${kernel_elf}" \
        -ex "set architecture aarch64" \
        -ex "set breakpoint pending on" \
        -ex "layout src" \
        -ex "target remote localhost:${gdb_port}" \
        -ex "hbreak _start" \
        -ex "continue"
else
    echo "Unsupported ARCH: ${arch}" >&2
    exit 1
fi
