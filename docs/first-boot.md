# First Boot

Tori's first boot target is a higher-half x86_64 ELF kernel loaded through the Limine boot protocol.

## Build

Initialize submodules first:

```sh
git submodule update --init --recursive
```

```sh
make kernel
```

The kernel ELF is written to:

```text
build/kernel-build/boot/kernel.elf
```

## Stage ISO Root

```sh
make iso-root
```

The first `iso-root` build may run Limine's bootstrap step, then configure and build Limine with x86_64 UEFI support.

This creates:

```text
build/iso-root/
  boot/
    kernel.elf
    limine.conf
    limine/
      limine-uefi-cd.bin
  EFI/
    BOOT/
      BOOTX64.EFI
  rootfs/
```

Static files under `iso/boot` and `iso/rootfs` are copied into `build/iso-root`. The kernel ELF and Limine UEFI files are generated or built artifacts and are copied into the staged tree by Make.

## Create Bootable ISO

```sh
make iso
```

This creates:

```text
build/tori.iso
```

The ISO uses Limine's UEFI CD image and is generated with `xorriso`.

## Run In QEMU

```sh
make run
```

Or use the helper:

```sh
./scripts/run-qemu.sh
```

This boots `build/tori.iso` with QEMU and OVMF. By default the project expects:

```text
/usr/share/edk2/x64/OVMF_CODE.4m.fd
/usr/share/edk2/x64/OVMF_VARS.4m.fd
```

Override these with `TORI_OVMF_CODE` and `TORI_OVMF_VARS_TEMPLATE` if your distribution uses different paths.

The helper also accepts `TORI_BUILD_DIR`, `TORI_QEMU_MEMORY`, `CC`, and `CXX`, and forwards any extra arguments to `qemu-system-x86_64`.

There is also a FAT disk image path for UEFI testing:

```sh
make uefi-disk
make run-disk
```

## Current Kernel Behavior

On successful entry, the kernel:

- initializes COM1 serial output;
- initializes framebuffer output when Limine provides a 32-bit framebuffer;
- converts Limine responses into Tori's internal `BootInfo`;
- logs bootloader, kernel address, HHDM, RSDP, framebuffer, module, and memory map information;
- reclaims Limine bootloader-reclaimable pages into the physical page allocator;
- initializes a page allocator for 4 KiB physical pages;
- initializes a slice allocator with fixed-size classes backed by physical pages through HHDM;
- copies the boot memory map into allocator-owned kernel storage after allocator initialization;
- keeps ACPI reclaimable pages reserved until ACPI parsing exists;
- reserves physical page zero as a null-page trap;
- panics if the memory map is missing;
- runs small page and slice allocation/free smoke tests;
- halts after completing memory diagnostics.
