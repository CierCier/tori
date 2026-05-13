# AGENTS.md

## Project

Tori is a freestanding 64-bit hobby kernel for modern UEFI systems. The first implemented target is `x86_64 + Limine + UEFI PC`, but the codebase must be structured so boot protocols, CPU architectures, and platform assumptions can be replaced later.

The kernel is written in freestanding C++ with C and assembly at ABI-sensitive boundaries. The default toolchain is Clang, LLD, and CMake.

The repository is organized as a CMake superproject with separate workspaces:

- `kernel`: freestanding kernel sources and kernel-only headers.
- `libc`: future userspace C library; the kernel must not link against it.
- `userspace`: future applications, services, and userspace libraries.
- `shared`: ABI-safe declarations intentionally shared across kernel and userspace.
- `toolchain`: CMake helpers, target files, and cross-build configuration.
- `third_party`: imported external projects, including Limine.
- `iso`: static files copied into the staged ISO root.
- `docs`: design notes and project documentation.

## Architecture Rules

- Keep generic kernel code independent of Limine, UEFI, and x86_64 details.
- Treat Limine as the first boot adapter, not as the kernel's internal boot model.
- Keep CPU-specific code in architecture-owned areas.
- Keep UEFI PC machine assumptions in platform-owned areas.
- Route all boot data through Tori-owned types before entering generic kernel code.
- Prefer typed internal interfaces over passing raw protocol structures across subsystem boundaries.

Expected responsibility split:

- `kernel/src/boot/limine`: Limine request/response handling and conversion into Tori boot structures.
- `kernel/src/arch/x86_64`: CPU primitives, assembly stubs, port I/O, halt, control registers, and architecture-specific serial support.
- `kernel/src/platform/uefi_pc`: PC/UEFI platform assumptions, firmware handoff expectations, framebuffer and ACPI availability.
- `kernel/src/kernel`: architecture-independent kernel initialization and core runtime flow.
- `kernel/src/log`: kernel logging implementation.
- `kernel/src/memory`: kernel memory map modeling and later allocators.
- `kernel/targets/x86_64-limine`: linker script, target CMake configuration, Limine config, boot image layout, and QEMU helpers.
- `iso/boot`: bootloader configuration and static boot files included in the ISO.
- `iso/rootfs`: initial root filesystem payload included in the ISO.

## Coding Constraints

- Build as freestanding code.
- Do not depend on the hosted C or C++ standard library.
- Do not link the kernel against the `libc` workspace.
- Do not use C++ exceptions.
- Do not use RTTI.
- Do not assume heap allocation exists during early boot.
- Avoid global constructors until runtime support is intentionally implemented.
- Keep early boot code deterministic and easy to inspect.
- Prefer fixed-size buffers and explicit bounds in early subsystems.
- Keep assembly small and isolated behind C or C++ interfaces.

## Logging

Logging is a core subsystem, not ad hoc printing.

- Use the shared logging API for diagnostics, boot messages, warnings, errors, and panic output.
- Initial sinks are framebuffer console and serial output.
- Early logging must work without heap allocation.
- Panic logging must avoid recursion and allocation.
- Future logging phases should add a ring buffer, replay, per-CPU buffers, deferred flushing, and debug retrieval without changing callers.

## Boot And Memory

- The kernel entry adapter must collect boot protocol data, validate required fields, construct Tori-owned boot structures, and then call generic kernel initialization.
- Generic kernel code should use `BootInfo`, not Limine or UEFI structures.
- Milestone 1 parses and validates the boot memory map only.
- Physical page allocation, heap allocation, and virtual memory management are later milestones.

## Build And Verification Expectations

Once implementation starts, changes should keep these checks in mind:

- Configure and build with CMake using Clang and LLD.
- Boot under QEMU with OVMF through Limine.
- Use `scripts/run-qemu.sh -noreboot` to build the ISO and boot it under QEMU.
- The `run-qemu` script will also route serial output from COM1 to stdout.
- Always run the `scripts/run-qemu.sh` script with timeout to ensure non blocking.
- Capture and read serial logs from `stdio` for deterministic test output and debugging.
- Confirm framebuffer output appears when a framebuffer is provided.
- Confirm serial-only diagnostics still work when framebuffer initialization fails.

## Repository Discipline

- Keep files small enough that subsystem ownership stays obvious.
- Do not add broad abstractions unless they protect a real boundary: boot source, CPU architecture, platform, or kernel subsystem.
- Do not let temporary milestone code leak protocol-specific types into generic interfaces.
- Do not put generic utilities in `shared` unless they are part of an intentional kernel/userspace ABI.
- Prefer clear milestone progress over implementing many partially connected subsystems at once.

## Configuration

- Kernel build-time constants live in `kernel/config.h` as preprocessor defines.
- All `CONFIG_*` macros must have a comment explaining what they control and the minimum allowed value.
- Default values target single-CPU QEMU/OVMF. Bump `CONFIG_MAX_CPUS` when SMP support arrives.
- Do not duplicate config values across source files; always reference `kernel/config.h`.

## Git Discipline

- Keep commits atomic: each commit must be a single logical change that builds successfully.
- Do not mix unrelated changes in the same commit (e.g., a bug fix with a refactor, or two independent features).
- Write commit messages that explain the motivation (the "why"), not just a description of the diff.
- Run a build before committing to verify the change compiles and links.
- Squash fixup commits into the logical change they belong to before asking for review or merging.
