#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
default_image_x86_64="${repo_root}/bin/arx-x86_64.img"
default_image_aarch64="${repo_root}/bin/arx-aarch64.img"

show_help() {
    cat <<'EOF'
Write Arx boot image to a USB drive safely.

Usage:
  scripts/write-usb.sh --device /dev/sdX [options]

Options:
  -d, --device PATH     Target whole-disk device (required), e.g. /dev/sdb
    -a, --arch ARCH       Architecture: x86_64 or aarch64 (default: x86_64)
    -i, --image PATH      Image path override (default depends on --arch)
    -b, --build           Build image with: make <arch>
  -y, --yes             Skip interactive confirmation prompt
  -h, --help            Show this help

Notes:
  - This script writes a raw disk image and will destroy data on the target device.
  - For safety, it refuses to write to the disk backing the current root filesystem.
EOF
}

die() {
    echo "Error: $*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

device=""
arch="x86_64"
image=""
image_overridden=0
do_build=0
assume_yes=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -d|--device)
            [[ $# -ge 2 ]] || die "missing value for $1"
            device="$2"
            shift 2
            ;;
        -i|--image)
            [[ $# -ge 2 ]] || die "missing value for $1"
            image="$2"
            image_overridden=1
            shift 2
            ;;
        -a|--arch)
            [[ $# -ge 2 ]] || die "missing value for $1"
            arch="$2"
            shift 2
            ;;
        -b|--build)
            do_build=1
            shift
            ;;
        -y|--yes)
            assume_yes=1
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ -n "$device" ]] || {
    show_help
    die "--device is required"
}

case "$arch" in
    x86_64|aarch64)
        ;;
    *)
        die "unsupported arch: $arch (expected: x86_64 or aarch64)"
        ;;
esac

if [[ "$image_overridden" -ne 1 ]]; then
    if [[ "$arch" == "x86_64" ]]; then
        image="$default_image_x86_64"
    else
        image="$default_image_aarch64"
    fi
fi

need_cmd lsblk
need_cmd findmnt
need_cmd dd
need_cmd sync
need_cmd awk
need_cmd sed

if [[ "$do_build" -eq 1 ]]; then
    need_cmd make
    echo "Building ${arch} image..."
    make -C "$repo_root" "$arch"
fi

[[ -f "$image" ]] || die "image not found: $image"
[[ -b "$device" ]] || die "device is not a block device: $device"

# Require a whole-disk block device, not a partition.
device_type="$(lsblk -dn -o TYPE "$device" | awk 'NR==1 {print $1}')"
[[ "$device_type" == "disk" ]] || die "target must be a whole disk device (got type: $device_type)"

# Refuse to write to the host's root disk.
root_source="$(findmnt -n -o SOURCE / || true)"
if [[ -n "$root_source" ]]; then
    root_pkname="$(lsblk -no PKNAME "$root_source" 2>/dev/null | awk 'NR==1 {print $1}')"
    if [[ -n "$root_pkname" ]]; then
        root_disk="/dev/${root_pkname}"
        [[ "$device" != "$root_disk" ]] || die "refusing to write to root disk: $root_disk"
    fi
fi

echo "Target device details:"
lsblk -o NAME,SIZE,MODEL,TRAN,TYPE,MOUNTPOINT "$device"

echo ""
echo "Arch:   $arch"
echo "Image:  $image"
echo "Device: $device"
echo ""
echo "This will overwrite all data on $device."

if [[ "$assume_yes" -ne 1 ]]; then
    printf "Type the exact device path to continue: "
    read -r confirm
    [[ "$confirm" == "$device" ]] || die "confirmation did not match"
fi

run_as_root() {
    if [[ "${EUID}" -eq 0 ]]; then
        "$@"
    else
        need_cmd sudo
        sudo "$@"
    fi
}

# Unmount any mounted partitions from the target disk before writing.
mounted_parts="$(lsblk -ln -o PATH,MOUNTPOINT "$device" | awk '$2 != "" {print $1}')"
if [[ -n "$mounted_parts" ]]; then
    echo "Unmounting mounted partitions on target disk..."
    while IFS= read -r part; do
        [[ -n "$part" ]] || continue
        run_as_root umount "$part"
    done <<< "$mounted_parts"
fi

echo "Writing image to USB..."
run_as_root dd if="$image" of="$device" bs=4M conv=fsync status=progress
run_as_root sync

if command -v partprobe >/dev/null 2>&1; then
    run_as_root partprobe "$device" || true
fi

echo "Done. USB is ready to boot Arx."
