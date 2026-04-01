
BOOT_DIR ?= boot
BOOT_CFG ?= $(BOOT_DIR)/limine.conf

BUILD_DIR := build
BIN_DIR := bin
ISO_DIR := $(BUILD_DIR)/iso
ARCH_DIR := arch
SECTOR_SIZE ?= 512
ESP_SIZE_MB ?= 64
ESP_SECTORS = $(shell echo $$(( $(ESP_SIZE_MB) * 2048 )))
IMG_SECTORS = $(shell echo $$(( ($(ESP_SIZE_MB) + 2) * 2048 )))

KERNEL_SRC ?= kernel.c
KERNEL_X86_64_SRC ?= $(ARCH_DIR)/x86_64/arch_entry.c
KERNEL_AARCH64_SRC ?= $(ARCH_DIR)/aarch64/arch_entry.c
KERNEL_X86_64_COMMON_OBJ ?= $(BUILD_DIR)/kernel-common-x86_64.o
KERNEL_AARCH64_COMMON_OBJ ?= $(BUILD_DIR)/kernel-common-aarch64.o
KERNEL_X86_64_ARCH_OBJ ?= $(BUILD_DIR)/kernel-arch-x86_64.o
KERNEL_AARCH64_ARCH_OBJ ?= $(BUILD_DIR)/kernel-arch-aarch64.o
KERNEL_X86_64 ?= $(BIN_DIR)/kernel-x86_64.elf
KERNEL_AARCH64 ?= $(BIN_DIR)/kernel-aarch64.elf

KERNEL_X86_64_LD ?= $(ARCH_DIR)/x86_64/linker.ld
KERNEL_AARCH64_LD ?= $(ARCH_DIR)/aarch64/linker.ld

X86_64_CC ?= gcc
AARCH64_CC ?= aarch64-linux-gnu-gcc

ISO_X86_64 ?= $(BIN_DIR)/arx-x86_64.img
ISO_AARCH64 ?= $(BIN_DIR)/arx-aarch64.img

ISO_X86_64_ROOT := $(ISO_DIR)/x86_64
ISO_AARCH64_ROOT := $(ISO_DIR)/aarch64

BOOTX64_EFI := $(BOOT_DIR)/x86_64/BOOTX64.EFI
BOOTAA64_EFI := $(BOOT_DIR)/aarch64/BOOTAA64.EFI

.PHONY: all x86_64 aarch64 prepare-iso-tools clean qemu-x86_64 qemu-aarch64

KERNEL_X86_64_COMMON_DEP ?= $(KERNEL_X86_64_COMMON_OBJ:.o=.d)
KERNEL_AARCH64_COMMON_DEP ?= $(KERNEL_AARCH64_COMMON_OBJ:.o=.d)
KERNEL_X86_64_ARCH_DEP ?= $(KERNEL_X86_64_ARCH_OBJ:.o=.d)
KERNEL_AARCH64_ARCH_DEP ?= $(KERNEL_AARCH64_ARCH_OBJ:.o=.d)

-include $(KERNEL_X86_64_COMMON_DEP) $(KERNEL_AARCH64_COMMON_DEP) $(KERNEL_X86_64_ARCH_DEP) $(KERNEL_AARCH64_ARCH_DEP)

all: x86_64 aarch64

prepare-iso-tools:
	@command -v parted >/dev/null 2>&1 || { echo "Error: parted not found." >&2; exit 1; }
	@command -v mkfs.fat >/dev/null 2>&1 || { echo "Error: mkfs.fat not found (dosfstools)." >&2; exit 1; }
	@command -v mcopy >/dev/null 2>&1 || { echo "Error: mcopy not found (mtools)." >&2; exit 1; }
	@command -v mmd >/dev/null 2>&1 || { echo "Error: mmd not found (mtools)." >&2; exit 1; }
	@test -f "$(BOOTX64_EFI)" || { echo "Error: missing $(BOOTX64_EFI)" >&2; exit 1; }
	@test -f "$(BOOTAA64_EFI)" || { echo "Error: missing $(BOOTAA64_EFI)" >&2; exit 1; }
	@test -f "$(BOOT_CFG)" || { echo "Error: missing $(BOOT_CFG)" >&2; exit 1; }
	@test -f "$(KERNEL_X86_64_LD)" || { echo "Error: missing $(KERNEL_X86_64_LD)" >&2; exit 1; }
	@test -f "$(KERNEL_AARCH64_LD)" || { echo "Error: missing $(KERNEL_AARCH64_LD)" >&2; exit 1; }

$(KERNEL_X86_64_COMMON_OBJ): $(KERNEL_SRC)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	$(X86_64_CC) -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -nostdlib -MMD -MP -c -o $@ $<

$(KERNEL_X86_64_ARCH_OBJ): $(KERNEL_X86_64_SRC)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	$(X86_64_CC) -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -nostdlib -MMD -MP -c -o $@ $<

$(KERNEL_X86_64): $(KERNEL_X86_64_COMMON_OBJ) $(KERNEL_X86_64_ARCH_OBJ) $(KERNEL_X86_64_LD)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	$(X86_64_CC) -nostdlib -no-pie -Wl,-T,$(KERNEL_X86_64_LD) -Wl,-Map,$(BUILD_DIR)/kernel-x86_64.map -o $@ $(KERNEL_X86_64_COMMON_OBJ) $(KERNEL_X86_64_ARCH_OBJ)

$(KERNEL_AARCH64_COMMON_OBJ): $(KERNEL_SRC)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	@command -v $(AARCH64_CC) >/dev/null 2>&1 || { echo "Error: $(AARCH64_CC) not found. Set AARCH64_CC=<compiler>." >&2; exit 1; }
	$(AARCH64_CC) -ffreestanding -fno-stack-protector -fno-pic -fno-pie -nostdlib -MMD -MP -c -o $@ $<

$(KERNEL_AARCH64_ARCH_OBJ): $(KERNEL_AARCH64_SRC)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	@command -v $(AARCH64_CC) >/dev/null 2>&1 || { echo "Error: $(AARCH64_CC) not found. Set AARCH64_CC=<compiler>." >&2; exit 1; }
	$(AARCH64_CC) -ffreestanding -fno-stack-protector -fno-pic -fno-pie -nostdlib -MMD -MP -c -o $@ $<

$(KERNEL_AARCH64): $(KERNEL_AARCH64_COMMON_OBJ) $(KERNEL_AARCH64_ARCH_OBJ) $(KERNEL_AARCH64_LD)
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)
	$(AARCH64_CC) -nostdlib -no-pie -Wl,-T,$(KERNEL_AARCH64_LD) -Wl,-Map,$(BUILD_DIR)/kernel-aarch64.map -o $@ $(KERNEL_AARCH64_COMMON_OBJ) $(KERNEL_AARCH64_ARCH_OBJ)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

x86_64: $(ISO_X86_64)
aarch64: $(ISO_AARCH64)

$(ISO_X86_64): prepare-iso-tools $(KERNEL_X86_64) $(BOOT_CFG) $(BOOTX64_EFI)
	@mkdir -p $(ISO_X86_64_ROOT)/boot $(ISO_X86_64_ROOT)/EFI/BOOT $(BUILD_DIR) $(BIN_DIR)
	cp $(BOOTX64_EFI) $(ISO_X86_64_ROOT)/EFI/BOOT/BOOTX64.EFI
	cp $(KERNEL_X86_64) $(ISO_X86_64_ROOT)/boot/kernel.elf
	cp $(BOOT_CFG) $(ISO_X86_64_ROOT)/limine.conf
	rm -f $@
	dd if=/dev/zero of=$@ bs=$(SECTOR_SIZE) count=$(IMG_SECTORS) status=none
	parted $@ --script mklabel gpt
	parted $@ --script mkpart ESP fat32 1MiB $$((1 + $(ESP_SIZE_MB)))MiB
	parted $@ --script set 1 boot on
	parted $@ --script set 1 esp on
	@START=$$(parted -s $@ unit s print | awk '/^ 1/ {print $$2}' | sed 's/s//'); \
	ESP_TMP="$(BUILD_DIR)/esp-x86_64.fat"; \
	dd if=/dev/zero of=$$ESP_TMP bs=$(SECTOR_SIZE) count=$(ESP_SECTORS) status=none; \
	mkfs.fat -F32 $$ESP_TMP >/dev/null; \
	export MTOOLS_SKIP_CHECK=1; \
	mmd -i $$ESP_TMP ::/EFI; \
	mmd -i $$ESP_TMP ::/EFI/BOOT; \
	mmd -i $$ESP_TMP ::/EFI/limine; \
	mmd -i $$ESP_TMP ::/boot; \
	mmd -i $$ESP_TMP ::/boot/limine; \
	mcopy -i $$ESP_TMP $(ISO_X86_64_ROOT)/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI; \
	mcopy -i $$ESP_TMP $(ISO_X86_64_ROOT)/boot/kernel.elf ::/boot/kernel.elf; \
	mcopy -i $$ESP_TMP $(ISO_X86_64_ROOT)/limine.conf ::/limine.conf; \
	mcopy -i $$ESP_TMP $(ISO_X86_64_ROOT)/limine.conf ::/boot/limine/limine.conf; \
	mcopy -i $$ESP_TMP $(ISO_X86_64_ROOT)/limine.conf ::/EFI/limine/limine.conf; \
	dd if=$$ESP_TMP of=$@ bs=$(SECTOR_SIZE) seek=$$START conv=notrunc status=none; \
	rm -f $$ESP_TMP

$(ISO_AARCH64): prepare-iso-tools $(KERNEL_AARCH64) $(BOOT_CFG) $(BOOTAA64_EFI)
	@mkdir -p $(ISO_AARCH64_ROOT)/boot $(ISO_AARCH64_ROOT)/EFI/BOOT $(BUILD_DIR) $(BIN_DIR)
	cp $(BOOTAA64_EFI) $(ISO_AARCH64_ROOT)/EFI/BOOT/BOOTAA64.EFI
	cp $(KERNEL_AARCH64) $(ISO_AARCH64_ROOT)/boot/kernel.elf
	cp $(BOOT_CFG) $(ISO_AARCH64_ROOT)/limine.conf
	rm -f $@
	dd if=/dev/zero of=$@ bs=$(SECTOR_SIZE) count=$(IMG_SECTORS) status=none
	parted $@ --script mklabel gpt
	parted $@ --script mkpart ESP fat32 1MiB $$((1 + $(ESP_SIZE_MB)))MiB
	parted $@ --script set 1 boot on
	parted $@ --script set 1 esp on
	@START=$$(parted -s $@ unit s print | awk '/^ 1/ {print $$2}' | sed 's/s//'); \
	ESP_TMP="$(BUILD_DIR)/esp-aarch64.fat"; \
	dd if=/dev/zero of=$$ESP_TMP bs=$(SECTOR_SIZE) count=$(ESP_SECTORS) status=none; \
	mkfs.fat -F32 $$ESP_TMP >/dev/null; \
	export MTOOLS_SKIP_CHECK=1; \
	mmd -i $$ESP_TMP ::/EFI; \
	mmd -i $$ESP_TMP ::/EFI/BOOT; \
	mmd -i $$ESP_TMP ::/EFI/limine; \
	mmd -i $$ESP_TMP ::/boot; \
	mmd -i $$ESP_TMP ::/boot/limine; \
	mcopy -i $$ESP_TMP $(ISO_AARCH64_ROOT)/EFI/BOOT/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI; \
	mcopy -i $$ESP_TMP $(ISO_AARCH64_ROOT)/boot/kernel.elf ::/boot/kernel.elf; \
	mcopy -i $$ESP_TMP $(ISO_AARCH64_ROOT)/limine.conf ::/limine.conf; \
	mcopy -i $$ESP_TMP $(ISO_AARCH64_ROOT)/limine.conf ::/boot/limine/limine.conf; \
	mcopy -i $$ESP_TMP $(ISO_AARCH64_ROOT)/limine.conf ::/EFI/limine/limine.conf; \
	dd if=$$ESP_TMP of=$@ bs=$(SECTOR_SIZE) seek=$$START conv=notrunc status=none; \
	rm -f $$ESP_TMP

X86_64_UEFI ?= firmware/x86_64/X86_64_UEFI.fd
AARCH64_UEFI ?= firmware/aarch64/AARCH64_UEFI.fd
IMG ?=
IMG_X86_64 ?= $(ISO_X86_64)
IMG_AARCH64 ?= $(ISO_AARCH64)

QEMU_X86_64 ?= qemu-system-x86_64
QEMU_AARCH64 ?= qemu-system-aarch64
QEMU_COMMON ?= -m 1024 -serial stdio

qemu-x86_64: 
	GDK_BACKEND=x11 $(QEMU_X86_64) $(QEMU_COMMON) -display gtk,grab-on-hover=on -drive if=pflash,format=raw,readonly=on,file="$(X86_64_UEFI)" -drive file="$(or $(IMG),$(IMG_X86_64))",format=raw

qemu-aarch64: 
	GDK_BACKEND=x11 $(QEMU_AARCH64) -machine virt -cpu cortex-a72 $(QEMU_COMMON) -display gtk,grab-on-hover=on -device ramfb -device qemu-xhci -device usb-kbd -device usb-tablet -bios "$(AARCH64_UEFI)" -drive if=none,id=osdisk,file="$(or $(IMG),$(IMG_AARCH64))",format=raw -device virtio-blk-pci,drive=osdisk,bootindex=0