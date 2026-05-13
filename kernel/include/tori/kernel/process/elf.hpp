#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::vfs {
struct Vnode;
}

namespace tori::proc {

struct ElfLoadResult {
    uint64_t entry;
    uint64_t stack_top;
};

constexpr uint64_t user_stack_base = 0x00007FFFF0000000ULL;
constexpr uint64_t user_stack_pages = 16;
constexpr uint64_t user_stack_size = user_stack_pages * 4096;
constexpr uint64_t user_stack_top = user_stack_base + user_stack_size;
constexpr uint64_t default_load_address = 0x400000;

int elf64_load(tori::vfs::Vnode* file, uint64_t pml4_phys, ElfLoadResult* out);

} // namespace tori::proc
