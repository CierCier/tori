#include <tori/kernel/process/elf.hpp>

#include <tori/kernel/vfs.hpp>
#include <tori/kernel/vmm.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/address.hpp>
#include <tori/kernel/log.hpp>
#include <tori/errno.h>

namespace {

struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PF_X = 1;
constexpr uint32_t PF_W = 2;
constexpr uint32_t PF_R = 4;

constexpr unsigned char elf_magic[4] = {0x7F, 'E', 'L', 'F'};

} // namespace

namespace tori::proc {

int elf64_load(tori::vfs::Vnode* file, uint64_t pml4_phys, ElfLoadResult* out) {
    if (!file || !out) return -E_INVALID;

    Elf64_Ehdr ehdr;
    size_t got = 0;
    int err = file->ops->read(file, 0, &ehdr, sizeof(ehdr), &got);
    if (err < 0 || got != sizeof(ehdr)) {
        TORI_LOG_WARN("elf", "failed to read ELF header");
        return -E_IO;
    }

    for (int i = 0; i < 4; ++i) {
        if (ehdr.e_ident[i] != elf_magic[i]) {
            TORI_LOG_WARN("elf", "invalid ELF magic");
            return -E_INVALID;
        }
    }
    if (ehdr.e_ident[4] != 2) {
        TORI_LOG_WARN("elf", "not a 64-bit ELF");
        return -E_INVALID;
    }
    if (ehdr.e_ident[5] != 1) {
        TORI_LOG_WARN("elf", "not little-endian");
        return -E_INVALID;
    }
    if (ehdr.e_version != 1) {
        TORI_LOG_WARN("elf", "invalid ELF version");
        return -E_INVALID;
    }
    if (ehdr.e_machine != 0x3E) {
        TORI_LOG_WARN("elf", "not x86_64");
        return -E_INVALID;
    }
    if (ehdr.e_type != 2 && ehdr.e_type != 3) {
        TORI_LOG_WARN("elf", "invalid ELF type (neither EXEC nor DYN)");
        return -E_INVALID;
    }

    bool is_pie = (ehdr.e_type == 3);
    uint64_t load_bias = is_pie ? default_load_address : 0;
    uint64_t entry = ehdr.e_entry + load_bias;

    if (ehdr.e_phentsize != sizeof(Elf64_Phdr)) {
        TORI_LOG_WARN("elf", "invalid program header size");
        return -E_INVALID;
    }

    uint64_t phdr_buf_phys = 0;
    Elf64_Phdr* phdrs = nullptr;
    uint64_t phdrs_size = static_cast<uint64_t>(ehdr.e_phnum) * sizeof(Elf64_Phdr);
    uint64_t phdrs_pages = (phdrs_size + 0xFFF) / 0x1000;
    if (phdrs_pages == 0) phdrs_pages = 1;

    phdr_buf_phys = tori::memory::pmm::alloc_pages(phdrs_pages);
    if (phdr_buf_phys == tori::memory::pmm::invalid_physical_address) {
        TORI_LOG_WARN("elf", "failed to allocate pages for program headers");
        return -E_NO_MEM;
    }
    phdrs = static_cast<Elf64_Phdr*>(tori::memory::address::physical_to_virtual(phdr_buf_phys));

    got = 0;
    err = file->ops->read(file, ehdr.e_phoff, phdrs, phdrs_size, &got);
    if (err < 0 || got != phdrs_size) {
        TORI_LOG_WARN("elf", "failed to read program headers");
        tori::memory::pmm::free_pages(phdr_buf_phys, phdrs_pages);
        return -E_IO;
    }

    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        auto& phdr = phdrs[i];
        if (phdr.p_type != PT_LOAD) continue;

        uint64_t vaddr = phdr.p_vaddr + load_bias;
        uint64_t memsz = phdr.p_memsz;
        uint64_t filesz = phdr.p_filesz;
        uint64_t file_offset = phdr.p_offset;
        uint64_t remaining_file = filesz;

        uint64_t page_start = vaddr & ~0xFFFULL;
        uint64_t page_end = (vaddr + memsz + 0xFFFULL) & ~0xFFFULL;
        uint64_t first_page_off = vaddr & 0xFFFULL;

        for (uint64_t page = page_start; page < page_end; page += 0x1000) {
            uint64_t phys = tori::memory::pmm::alloc_page();
            if (phys == tori::memory::pmm::invalid_physical_address) {
                TORI_LOG_WARN("elf", "failed to allocate page for segment");
                tori::memory::pmm::free_pages(phdr_buf_phys, phdrs_pages);
                return -E_NO_MEM;
            }

            auto* hhdm = static_cast<uint8_t*>(tori::memory::address::physical_to_virtual(phys));
            for (size_t z = 0; z < 0x1000; ++z) {
                hhdm[z] = 0;
            }

            auto flags = tori::memory::vmm::Flags::Present
                       | tori::memory::vmm::Flags::User;
            if (phdr.p_flags & PF_W) {
                flags = flags | tori::memory::vmm::Flags::Writable;
            }
            if (!(phdr.p_flags & PF_X)) {
                flags = flags | tori::memory::vmm::Flags::NoExecute;
            }

            tori::memory::vmm::map_page(page, phys, flags, pml4_phys);

            if (remaining_file > 0) {
                uint64_t page_off = (page == page_start) ? first_page_off : 0;
                uint64_t space = 0x1000 - page_off;
                uint64_t copy_size = remaining_file < space ? remaining_file : space;

                if (copy_size > 0) {
                    got = 0;
                    err = file->ops->read(file, file_offset,
                                          hhdm + page_off, copy_size, &got);
                    if (err < 0) {
                        TORI_LOG_WARN("elf", "failed to read segment data from file");
                        tori::memory::pmm::free_pages(phdr_buf_phys, phdrs_pages);
                        return -E_IO;
                    }
                    file_offset += copy_size;
                    remaining_file -= copy_size;
                }
            }
        }
    }

    uint64_t stack_top = 0;
    uint64_t last_stack_phys = 0;
    {
        uint64_t guard_page = user_stack_base - 0x1000;
        (void)guard_page;

        for (uint64_t page = user_stack_base; page < user_stack_top; page += 0x1000) {
            uint64_t phys = tori::memory::pmm::alloc_page();
            if (phys == tori::memory::pmm::invalid_physical_address) {
                TORI_LOG_WARN("elf", "failed to allocate page for user stack");
                tori::memory::pmm::free_pages(phdr_buf_phys, phdrs_pages);
                return -E_NO_MEM;
            }

            auto* hhdm = static_cast<uint8_t*>(tori::memory::address::physical_to_virtual(phys));
            for (size_t z = 0; z < 0x1000; ++z) {
                hhdm[z] = 0;
            }

            auto flags = tori::memory::vmm::Flags::Present
                       | tori::memory::vmm::Flags::User
                       | tori::memory::vmm::Flags::Writable
                       | tori::memory::vmm::Flags::NoExecute;
            tori::memory::vmm::map_page(page, phys, flags, pml4_phys);

            last_stack_phys = phys;
        }

        // Set up initial user stack per SysV x86-64 ABI:
        //   [rsp+0]  = argc
        //   [rsp+8]  = argv[0..argc], NULL-terminated
        //   [rsp+8+argc*8+8]  = envp[0..n], NULL-terminated
        // For init with no args and no env: all zeroes.
        // Write to the last (top) stack page via HHDM. The page is already
        // zeroed from the loop above, so only need to place RSP correctly.
        // Entries on stack (from end): padding (for 16-byte alignment),
        // NULL (envp terminator), NULL (argv terminator), 0 (argc).
        auto* stack_words = static_cast<uint64_t*>(
            tori::memory::address::physical_to_virtual(last_stack_phys));
        int idx = 0x1000 / sizeof(uint64_t);  // 512
        stack_words[--idx] = 0;  // envp terminator (NULL)
        stack_words[--idx] = 0;  // argv terminator (NULL)
        stack_words[--idx] = 0;  // argc = 0
        stack_top = (user_stack_top - 0x1000) + idx * sizeof(uint64_t);
    }

    tori::memory::pmm::free_pages(phdr_buf_phys, phdrs_pages);

    out->entry = entry;
    out->stack_top = stack_top;

    TORI_LOG_INFO("elf", "ELF64 loaded");
    TORI_LOG_VALUE(tori::log::Level::Info, "elf", "entry", entry);
    TORI_LOG_VALUE(tori::log::Level::Info, "elf", "load_bias", load_bias);

    return 0;
}

} // namespace tori::proc
