# Tori build system

SHELL := /bin/sh

BUILD_DIR := $(or $(TORI_BUILD_DIR),$(CURDIR)/build)

ifeq ($(origin CC),default)
CC := clang
endif
ifeq ($(origin CXX),default)
CXX := clang++
endif
ifeq ($(origin AS),default)
AS := $(CC)
endif
ifeq ($(origin LD),default)
LD := $(CXX)
endif
ifeq ($(origin AR),default)
AR := llvm-ar
endif
ifeq ($(origin NM),default)
NM := llvm-nm
endif

OVMF_CODE ?= $(or $(TORI_OVMF_CODE),/usr/share/edk2/x64/OVMF_CODE.4m.fd)
OVMF_VARS_TEMPLATE ?= $(or $(TORI_OVMF_VARS_TEMPLATE),/usr/share/edk2/x64/OVMF_VARS.4m.fd)
OVMF_VARS := $(BUILD_DIR)/OVMF_VARS.4m.fd

LIMINE_SRC := $(CURDIR)/third_party/limine
LIMINE_BUILD_DIR := $(BUILD_DIR)/third_party/limine
LIMINE_INSTALL_DIR := $(BUILD_DIR)/third_party/limine-install
LIMINE_STAMP := $(LIMINE_BUILD_DIR)/.tori-limine-installed

KERNEL_OUTPUT_DIR := $(BUILD_DIR)/kernel-build
KERNEL_ELF := $(KERNEL_OUTPUT_DIR)/boot/kernel.elf

LIBC_DIR := $(CURDIR)/libc
LIBC_OUT_DIR := $(BUILD_DIR)/libc
LIBC_OBJ_DIR := $(LIBC_OUT_DIR)/obj
LIBC_ARCHIVE := $(LIBC_OUT_DIR)/libtori_libc.a
CRT0_OBJ := $(LIBC_OUT_DIR)/crt0.o
LIBC_C_SOURCES := $(shell find $(LIBC_DIR)/src/syscall $(LIBC_DIR)/src/string $(LIBC_DIR)/src/stdlib $(LIBC_DIR)/src/errno -name '*.c' | LC_ALL=C sort)
LIBC_OBJS := $(patsubst $(LIBC_DIR)/src/%.c,$(LIBC_OBJ_DIR)/%.o,$(LIBC_C_SOURCES))
LIBC_DEPS := $(LIBC_OBJS:.o=.d)

LIBC_CFLAGS := --target=x86_64-unknown-none \
               -ffreestanding -fno-builtin -nostdlibinc \
               -I$(LIBC_DIR)/include \
               -I$(CURDIR)/shared/include \
               -std=gnu17
LIBC_ASFLAGS := --target=x86_64-unknown-none \
                -ffreestanding \
                -I$(LIBC_DIR)/include

INIT_DIR := $(CURDIR)/userspace/apps/init
INIT_OUT_DIR := $(BUILD_DIR)/init
INIT_BINARY := $(INIT_OUT_DIR)/init.elf
INIT_MAIN_OBJ := $(INIT_OUT_DIR)/main.o
INIT_CFLAGS := --target=x86_64-unknown-none \
               -ffreestanding -fno-builtin -nostdlibinc \
               -I$(LIBC_DIR)/include \
               -I$(CURDIR)/shared/include \
               -std=c17
INIT_LDFLAGS := --target=x86_64-unknown-none \
                -ffreestanding -nostdlib -static \
                -Wl,-z,max-page-size=0x1000 \
                -Wl,-e_start

SH_DIR := $(CURDIR)/userspace/apps/sh
SH_OUT_DIR := $(BUILD_DIR)/sh
SH_BINARY := $(SH_OUT_DIR)/sh.elf
SH_MAIN_OBJ := $(SH_OUT_DIR)/main.o
SH_CFLAGS := --target=x86_64-unknown-none \
             -ffreestanding -fno-builtin -nostdlibinc \
             -I$(LIBC_DIR)/include \
             -I$(CURDIR)/shared/include \
             -std=c17
SH_LDFLAGS := --target=x86_64-unknown-none \
              -ffreestanding -nostdlib -static -pie \
              -Wl,-z,max-page-size=0x1000 \
              -Wl,-e_start

ROOTFS_STAGING := $(BUILD_DIR)/rootfs-staging
ROOTFS_FAT := $(BUILD_DIR)/rootfs.fat
ISO_ROOT := $(BUILD_DIR)/iso-root
ISO_IMAGE := $(BUILD_DIR)/tori.iso
UEFI_DISK := $(BUILD_DIR)/tori-uefi.img

.PHONY: all kernel libc init sh rootfs limine iso iso-root run run-disk uefi-disk clean distclean

all: iso

kernel: $(KERNEL_ELF)

$(KERNEL_ELF):
	$(MAKE) -f $(CURDIR)/kernel/Makefile \
		KERNEL_OUTPUT_DIR=$(KERNEL_OUTPUT_DIR) \
		CC=$(CC) CXX=$(CXX) AS=$(AS) LD=$(LD) NM=$(NM)

libc: $(LIBC_ARCHIVE) $(CRT0_OBJ)

$(CRT0_OBJ): $(LIBC_DIR)/src/crt0/_start.S | $(LIBC_OUT_DIR)
	$(AS) $(LIBC_ASFLAGS) -c $< -o $@

$(LIBC_OBJ_DIR)/%.o: $(LIBC_DIR)/src/%.c | $(LIBC_OBJ_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(LIBC_CFLAGS) -MMD -MP -c $< -o $@

$(LIBC_ARCHIVE): $(LIBC_OBJS) | $(LIBC_OUT_DIR)
	$(AR) rcs $@ $(LIBC_OBJS)

init: $(INIT_BINARY)

sh: $(SH_BINARY)

$(INIT_MAIN_OBJ): $(INIT_DIR)/main.c | $(INIT_OUT_DIR)
	$(CC) $(INIT_CFLAGS) -MMD -MP -c $< -o $@

$(INIT_BINARY): $(INIT_MAIN_OBJ) $(CRT0_OBJ) $(LIBC_ARCHIVE) | $(INIT_OUT_DIR)
	$(CC) $(INIT_LDFLAGS) -o $@ $(CRT0_OBJ) $(INIT_MAIN_OBJ) -L$(LIBC_OUT_DIR) -ltori_libc

$(SH_MAIN_OBJ): $(SH_DIR)/main.c | $(SH_OUT_DIR)
	$(CC) $(SH_CFLAGS) -MMD -MP -c $< -o $@

$(SH_BINARY): $(SH_MAIN_OBJ) $(CRT0_OBJ) $(LIBC_ARCHIVE) | $(SH_OUT_DIR)
	$(CC) $(SH_LDFLAGS) -o $@ $(CRT0_OBJ) $(SH_MAIN_OBJ) -L$(LIBC_OUT_DIR) -ltori_libc

rootfs: $(ROOTFS_FAT)

$(ROOTFS_FAT): $(INIT_BINARY) $(SH_BINARY) $(CURDIR)/scripts/mkrootfs.sh
	rm -rf $(ROOTFS_STAGING)
	mkdir -p $(ROOTFS_STAGING)/sys
	cp -R $(CURDIR)/iso/rootfs/. $(ROOTFS_STAGING)/
	cp $(INIT_BINARY) $(ROOTFS_STAGING)/sys/init
	cp $(SH_BINARY) $(ROOTFS_STAGING)/sys/sh
	sh $(CURDIR)/scripts/mkrootfs.sh $@ $(ROOTFS_STAGING)

limine: $(LIMINE_STAMP)

$(LIMINE_STAMP):
	test -x $(LIMINE_SRC)/configure || (cd $(LIMINE_SRC) && ./bootstrap)
	mkdir -p $(LIMINE_BUILD_DIR) $(LIMINE_INSTALL_DIR)
	test -f $(LIMINE_BUILD_DIR)/Makefile || (cd $(LIMINE_BUILD_DIR) && $(LIMINE_SRC)/configure --prefix=$(LIMINE_INSTALL_DIR) --enable-uefi-x86-64 --enable-uefi-cd)
	$(MAKE) -C $(LIMINE_BUILD_DIR)
	$(MAKE) -C $(LIMINE_BUILD_DIR) install
	touch $@

iso-root: $(ISO_ROOT)/.staged

$(ISO_ROOT)/.staged: $(KERNEL_ELF) $(ROOTFS_FAT) $(LIMINE_STAMP)
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)
	cp -R $(CURDIR)/iso/. $(ISO_ROOT)/
	cp $(KERNEL_ELF) $(ISO_ROOT)/boot/kernel.elf
	cp $(ROOTFS_FAT) $(ISO_ROOT)/boot/rootfs.fat
	mkdir -p $(ISO_ROOT)/EFI/BOOT $(ISO_ROOT)/boot/limine
	cp $(LIMINE_INSTALL_DIR)/share/limine/BOOTX64.EFI $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI
	cp $(LIMINE_INSTALL_DIR)/share/limine/limine-uefi-cd.bin $(ISO_ROOT)/boot/limine/limine-uefi-cd.bin
	touch $@

iso: $(ISO_IMAGE)

$(ISO_IMAGE): $(ISO_ROOT)/.staged
	rm -f $@
	xorriso -as mkisofs \
		-R -r -J \
		-apm-block-size 2048 \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part \
		--efi-boot-image \
		--protective-msdos-label \
		$(ISO_ROOT) \
		-o $@

$(OVMF_VARS): $(OVMF_VARS_TEMPLATE) | $(BUILD_DIR)
	cp $< $@

run: $(ISO_IMAGE) $(OVMF_VARS)
	qemu-system-x86_64 \
		-machine q35 \
		-m $(or $(TORI_QEMU_MEMORY),2G) \
		-smp $(or $(TORI_QEMU_CPUS),4) \
		-serial stdio \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-cdrom $(ISO_IMAGE)

uefi-disk: $(UEFI_DISK)

$(UEFI_DISK): $(ISO_ROOT)/.staged
	rm -f $@
	qemu-img create -f raw $@ 64M
	mformat -i $@ -F ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
	mcopy -i $@ $(ISO_ROOT)/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(ISO_ROOT)/boot/kernel.elf ::/boot/kernel.elf
	mcopy -i $@ $(ISO_ROOT)/boot/limine.conf ::/boot/limine.conf
	mcopy -i $@ $(ISO_ROOT)/boot/rootfs.fat ::/boot/rootfs.fat
	mcopy -i $@ $(ISO_ROOT)/boot/limine/limine-uefi-cd.bin ::/boot/limine/limine-uefi-cd.bin

run-disk: $(UEFI_DISK) $(OVMF_VARS)
	qemu-system-x86_64 \
		-machine q35 \
		-m $(or $(TORI_QEMU_MEMORY),2G) \
		-smp $(or $(TORI_QEMU_CPUS),4) \
		-serial stdio \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(OVMF_VARS) \
		-drive format=raw,file=$(UEFI_DISK)

$(BUILD_DIR) $(LIBC_OUT_DIR) $(LIBC_OBJ_DIR) $(INIT_OUT_DIR) $(SH_OUT_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -f $(CURDIR)/kernel/Makefile KERNEL_OUTPUT_DIR=$(KERNEL_OUTPUT_DIR) clean

distclean: clean
	rm -rf $(CURDIR)/kernel/build

-include $(LIBC_DEPS) $(INIT_MAIN_OBJ:.o=.d) $(SH_MAIN_OBJ:.o=.d)
