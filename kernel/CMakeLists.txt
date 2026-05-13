# Kernel workspace.
#
# This workspace builds the freestanding Tori kernel. It must not depend on
# the userspace libc workspace.

# Kernel is built via its own Makefile which discovers sources via wildcards.
# This avoids CMake re-configuration when adding/removing source files.
set(KERNEL_MAKE_OUTPUT_DIR ${CMAKE_BINARY_DIR}/kernel-build)
set(KERNEL_MAKE_ELF ${KERNEL_MAKE_OUTPUT_DIR}/boot/kernel.elf)

add_custom_target(tori_kernel ALL
    COMMAND make -f Makefile
        KERNEL_OUTPUT_DIR=${KERNEL_MAKE_OUTPUT_DIR}
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        AS=${CMAKE_C_COMPILER}
        LD=${CMAKE_CXX_COMPILER}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    BYPRODUCTS ${KERNEL_MAKE_ELF}
    COMMENT "Building kernel via kernel/Makefile"
    USES_TERMINAL
)

set(TORI_ISO_ROOT ${CMAKE_BINARY_DIR}/iso-root)
set(TORI_ISO_IMAGE ${CMAKE_BINARY_DIR}/tori.iso)
set(TORI_UEFI_DISK ${CMAKE_BINARY_DIR}/tori-uefi.img)
set(TORI_OVMF_VARS ${CMAKE_BINARY_DIR}/OVMF_VARS.4m.fd)
set(TORI_LIMINE_BUILD_DIR ${CMAKE_BINARY_DIR}/third_party/limine)
set(TORI_LIMINE_INSTALL_DIR ${CMAKE_BINARY_DIR}/third_party/limine-install)
set(TORI_LIMINE_STAMP ${TORI_LIMINE_BUILD_DIR}/.tori-limine-installed)
set(TORI_OVMF_CODE /usr/share/edk2/x64/OVMF_CODE.4m.fd CACHE FILEPATH "OVMF code firmware image")
set(TORI_OVMF_VARS_TEMPLATE /usr/share/edk2/x64/OVMF_VARS.4m.fd CACHE FILEPATH "OVMF vars firmware template")

add_custom_command(
    OUTPUT ${TORI_LIMINE_STAMP}
    COMMAND /bin/sh -c "test -x '${PROJECT_SOURCE_DIR}/third_party/limine/configure' || (cd '${PROJECT_SOURCE_DIR}/third_party/limine' && ./bootstrap)"
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TORI_LIMINE_BUILD_DIR}
    COMMAND /bin/sh -c "test -f '${TORI_LIMINE_BUILD_DIR}/Makefile' || (cd '${TORI_LIMINE_BUILD_DIR}' && '${PROJECT_SOURCE_DIR}/third_party/limine/configure' --prefix='${TORI_LIMINE_INSTALL_DIR}' --enable-uefi-x86-64 --enable-uefi-cd)"
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${TORI_LIMINE_BUILD_DIR}
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${TORI_LIMINE_BUILD_DIR} install
    COMMAND ${CMAKE_COMMAND} -E touch ${TORI_LIMINE_STAMP}
    COMMENT "Building Limine bootloader artifacts"
    VERBATIM
)

add_custom_target(tori_limine DEPENDS ${TORI_LIMINE_STAMP})

# Init binary built from userspace
set(TORI_INIT_BINARY ${CMAKE_BINARY_DIR}/init/init.elf)

# Staging rootfs directory in the build tree
set(TORI_ROOTFS_STAGING ${CMAKE_BINARY_DIR}/rootfs-staging)
set(TORI_ROOTFS_FAT ${CMAKE_BINARY_DIR}/rootfs.fat)

add_custom_command(
    OUTPUT ${TORI_ROOTFS_FAT}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${TORI_ROOTFS_STAGING}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/iso/rootfs ${TORI_ROOTFS_STAGING}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TORI_ROOTFS_STAGING}/sys
    COMMAND ${CMAKE_COMMAND} -E copy ${TORI_INIT_BINARY} ${TORI_ROOTFS_STAGING}/sys/init
    COMMAND sh ${PROJECT_SOURCE_DIR}/scripts/mkrootfs.sh ${TORI_ROOTFS_FAT} ${TORI_ROOTFS_STAGING}
    DEPENDS tori_init
    COMMENT "Creating rootfs FAT image with init binary"
)

add_custom_target(tori_iso_root
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${TORI_ISO_ROOT}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TORI_ISO_ROOT}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/iso ${TORI_ISO_ROOT}
    COMMAND ${CMAKE_COMMAND} -E copy ${KERNEL_MAKE_ELF} ${TORI_ISO_ROOT}/boot/kernel.elf
    COMMAND ${CMAKE_COMMAND} -E copy ${TORI_ROOTFS_FAT} ${TORI_ISO_ROOT}/boot/rootfs.fat
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TORI_ISO_ROOT}/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TORI_LIMINE_INSTALL_DIR}/share/limine/BOOTX64.EFI ${TORI_ISO_ROOT}/EFI/BOOT/BOOTX64.EFI
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TORI_ISO_ROOT}/boot/limine
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TORI_LIMINE_INSTALL_DIR}/share/limine/limine-uefi-cd.bin ${TORI_ISO_ROOT}/boot/limine/limine-uefi-cd.bin
    DEPENDS tori_kernel ${TORI_ROOTFS_FAT}
    COMMENT "Staging ISO root at ${TORI_ISO_ROOT}"
    VERBATIM
)

add_dependencies(tori_iso_root tori_limine)

add_custom_target(tori_limine_root DEPENDS tori_iso_root)

add_custom_target(tori_iso
    COMMAND ${CMAKE_COMMAND} -E rm -f ${TORI_ISO_IMAGE}
    COMMAND xorriso -as mkisofs
        -R
        -r
        -J
        -apm-block-size 2048
        --efi-boot boot/limine/limine-uefi-cd.bin
        -efi-boot-part
        --efi-boot-image
        --protective-msdos-label
        ${TORI_ISO_ROOT}
        -o ${TORI_ISO_IMAGE}
    DEPENDS tori_iso_root
    COMMENT "Creating UEFI ISO image at ${TORI_ISO_IMAGE}"
    VERBATIM
)

add_custom_target(tori_uefi_disk
    COMMAND ${CMAKE_COMMAND} -E rm -f ${TORI_UEFI_DISK}
    COMMAND qemu-img create -f raw ${TORI_UEFI_DISK} 64M
    COMMAND mformat -i ${TORI_UEFI_DISK} -F ::
    COMMAND mmd -i ${TORI_UEFI_DISK} ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
    COMMAND mcopy -i ${TORI_UEFI_DISK} ${TORI_ISO_ROOT}/EFI/BOOT/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
    COMMAND mcopy -i ${TORI_UEFI_DISK} ${TORI_ISO_ROOT}/boot/kernel.elf ::/boot/kernel.elf
    COMMAND mcopy -i ${TORI_UEFI_DISK} ${TORI_ISO_ROOT}/boot/limine.conf ::/boot/limine.conf
    COMMAND mcopy -i ${TORI_UEFI_DISK} ${TORI_ISO_ROOT}/boot/rootfs.fat ::/boot/rootfs.fat
    COMMAND mcopy -i ${TORI_UEFI_DISK} ${TORI_ISO_ROOT}/boot/limine/limine-uefi-cd.bin ::/boot/limine/limine-uefi-cd.bin
    DEPENDS tori_iso_root
    COMMENT "Creating FAT UEFI disk image at ${TORI_UEFI_DISK}"
    VERBATIM
)

add_custom_target(tori_run_qemu
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TORI_OVMF_VARS_TEMPLATE} ${TORI_OVMF_VARS}
    COMMAND qemu-system-x86_64
        -machine q35
        -m 512M
        -serial stdio
        -drive if=pflash,format=raw,readonly=on,file=${TORI_OVMF_CODE}
        -drive if=pflash,format=raw,file=${TORI_OVMF_VARS}
        -cdrom ${TORI_ISO_IMAGE}
    DEPENDS tori_iso
    USES_TERMINAL
    COMMENT "Running Tori under QEMU/OVMF"
    VERBATIM
)

add_custom_target(tori_run_qemu_disk
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TORI_OVMF_VARS_TEMPLATE} ${TORI_OVMF_VARS}
    COMMAND qemu-system-x86_64
        -machine q35
        -m 512M
        -serial stdio
        -drive if=pflash,format=raw,readonly=on,file=${TORI_OVMF_CODE}
        -drive if=pflash,format=raw,file=${TORI_OVMF_VARS}
        -drive format=raw,file=${TORI_UEFI_DISK}
    DEPENDS tori_uefi_disk
    USES_TERMINAL
    COMMENT "Running Tori FAT disk image under QEMU/OVMF"
    VERBATIM
)
