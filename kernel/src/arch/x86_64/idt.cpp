#include <tori/kernel/idt.hpp>

#include <config.h>
#include <tori/kernel/address.hpp>
#include <tori/kernel/gdt.hpp>
#include <tori/kernel/lapic.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/pmm.hpp>
#include <tori/kernel/sched/sched.hpp>
#include <tori/kernel/task.hpp>
#include <tori/kernel/time.hpp>
#include <tori/kernel/timer.hpp>
#include <tori/kernel/vmm.hpp>
#include <tori/kernel/process/thread.hpp>
#include <tori/kernel/process/process.hpp>

#include "io.hpp"

namespace {

using namespace tori::arch::x86_64;

// Page table walk helpers for COW handling.
struct PageTable {
    uint64_t entries[512];
};

PageTable* get_pt(uint64_t phys) {
    return static_cast<PageTable*>(tori::memory::address::physical_to_virtual(phys));
}

constexpr uint64_t PTE_PRESENT = 1ULL << 0;
constexpr uint64_t PTE_WRITABLE = 1ULL << 1;
constexpr uint64_t PTE_USER = 1ULL << 2;
constexpr uint64_t PTE_COW = 1ULL << 9;
constexpr uint64_t PTE_ADDR_MASK = 0x000ffffffffff000ULL;

// Handle a copy-on-write page fault.
// Returns true if the fault was resolved (COW handled), false if it should fall through to panic.
bool handle_cow_fault(uint64_t cr2, uint64_t error_code) {
    // COW fault: user-mode write to a present non-writable page with COW_PENDING set.
    // error_code bits: P(0)=1, W(1)=1, U(2)=1  →  0x7
    if ((error_code & 0x7) != 0x7) return false;

    auto* task = tori::sched::current_task();
    if (!task || !task->thread) return false;

    auto* thread = static_cast<tori::proc::Thread*>(task->thread);
    if (!thread->process) return false;

    uint64_t pml4_phys = thread->process->pml4_phys;
    if (!pml4_phys) return false;

    // Walk page table to find the leaf PTE.
    const uint64_t pml4_idx = (cr2 >> 39) & 0x1FF;
    const uint64_t pdpt_idx = (cr2 >> 30) & 0x1FF;
    const uint64_t pd_idx   = (cr2 >> 21) & 0x1FF;
    const uint64_t pt_idx   = (cr2 >> 12) & 0x1FF;

    PageTable* pml4 = get_pt(pml4_phys);
    if (!(pml4->entries[pml4_idx] & PTE_PRESENT)) return false;

    uint64_t pdpt_phys = pml4->entries[pml4_idx] & PTE_ADDR_MASK;
    PageTable* pdpt = get_pt(pdpt_phys);
    if (!(pdpt->entries[pdpt_idx] & PTE_PRESENT)) return false;

    uint64_t pd_phys = pdpt->entries[pdpt_idx] & PTE_ADDR_MASK;
    PageTable* pd = get_pt(pd_phys);
    if (!(pd->entries[pd_idx] & PTE_PRESENT)) return false;

    uint64_t pt_phys = pd->entries[pd_idx] & PTE_ADDR_MASK;
    PageTable* pt = get_pt(pt_phys);

    uint64_t pte = pt->entries[pt_idx];
    if (!(pte & PTE_PRESENT)) return false;
    if (!(pte & PTE_COW)) return false;

    uint64_t old_page_phys = pte & PTE_ADDR_MASK;
    if (tori::memory::pmm::page_ref_count(old_page_phys) <= 1) {
        pt->entries[pt_idx] = (pte | PTE_WRITABLE) & ~PTE_COW;
        asm volatile("invlpg (%0)" : : "r"(cr2) : "memory");
        return true;
    }

    // Allocate a new physical page.
    uint64_t new_page_phys = tori::memory::pmm::alloc_page();
    if (new_page_phys == tori::memory::pmm::invalid_physical_address) {
        return false;
    }

    // Copy content from the old page.
    auto* old_src = static_cast<volatile uint8_t*>(
        tori::memory::address::physical_to_virtual(old_page_phys));
    auto* new_dst = static_cast<volatile uint8_t*>(
        tori::memory::address::physical_to_virtual(new_page_phys));
    for (size_t i = 0; i < 4096; ++i) {
        new_dst[i] = old_src[i];
    }

    // Update the PTE: replace physical address, set writable, clear COW.
    // Preserve all other original flags (User, NX, WT, CD, PAT, Global, etc.).
    uint64_t preserved = pte & ~(PTE_ADDR_MASK | PTE_WRITABLE | PTE_COW);
    pt->entries[pt_idx] = (new_page_phys & PTE_ADDR_MASK) | preserved | PTE_WRITABLE;
    tori::memory::pmm::free_page(old_page_phys);

    asm volatile("invlpg (%0)" : : "r"(cr2) : "memory");

    return true;
}

// IDT entry (16 bytes)
struct [[gnu::packed]] IDTEntry {
  uint16_t offset_low;
  uint16_t selector;
  uint8_t ist;
  uint8_t type_attr;
  uint16_t offset_mid;
  uint32_t offset_high;
  uint32_t zero;
};

// IDTR for lidt
struct [[gnu::packed]] IDTR {
  uint16_t limit;
  uint64_t base;
};

// ISR stub table populated by interrupts_asm.S
extern "C" void *isr_stub_table[256];

// IDT entries
alignas(16) IDTEntry idt[256] = {};

// Exception names
const char *exception_names[32] = {
    "divide-by-zero",
    "debug",
    "non-maskable-interrupt",
    "breakpoint",
    "overflow",
    "bound-range",
    "invalid-opcode",
    "device-not-available",
    "double-fault",
    "coprocessor-segment-overrun",
    "invalid-tss",
    "segment-not-present",
    "stack-segment-fault",
    "general-protection-fault",
    "page-fault",
    "reserved-15",
    "x87-fpu-error",
    "alignment-check",
    "machine-check",
    "simd-fp-exception",
    "virtualization-exception",
    "control-protection",
    "reserved-22",
    "reserved-23",
    "reserved-24",
    "reserved-25",
    "reserved-26",
    "reserved-27",
    "reserved-28",
    "reserved-29",
    "security-exception",
    "reserved-31",
};

const char *exception_name(uint8_t vec) {
  if (vec < 32) {
    return exception_names[vec];
  }
  return nullptr;
}

void dump_frame(const InterruptFrame *frame) {
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "vector", frame->vector);
  if (frame->vector < 32) {
    TORI_LOG_TEXT_VALUE(tori::log::Level::Panic, "isr", "exception",
                        exception_names[frame->vector]);
  }
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "error_code",
                 frame->error_code);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rip", frame->rip);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "cs", frame->cs);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rflags", frame->rflags);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rax", frame->rax);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rbx", frame->rbx);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rcx", frame->rcx);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rdx", frame->rdx);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rsi", frame->rsi);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rdi", frame->rdi);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "rbp", frame->rbp);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r8", frame->r8);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r9", frame->r9);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r10", frame->r10);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r11", frame->r11);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r12", frame->r12);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r13", frame->r13);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r14", frame->r14);
  TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "r15", frame->r15);
}

void handle_exception(InterruptFrame *frame) {
  if (frame->vector == 14) {
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    // Try to resolve as a COW page fault before panicking.
    if (handle_cow_fault(cr2, frame->error_code)) {
      return;
    }

    dump_frame(frame);

    // Print stack trace from the faulting context
    uint64_t rsp_at_fault = (uint64_t)(frame + 1);
    tori::log::print_stack_trace_from(frame->rip, rsp_at_fault, frame->rbp);

    // Page fault: dump CR2
    TORI_LOG_VALUE(tori::log::Level::Panic, "isr", "cr2", cr2);

    // Bits in error code: P=present(1), W=write(2), U=user(4), RSVD=8, ID=16
    TORI_LOG_TEXT_VALUE(tori::log::Level::Panic, "isr", "page_fault_type",
                        (frame->error_code & 1) ? "protection-violation"
                                                : "non-present");
    TORI_LOG_TEXT_VALUE(tori::log::Level::Panic, "isr", "access",
                        (frame->error_code & 2) ? "write" : "read");
  } else {
    dump_frame(frame);

    // Print stack trace from the faulting context
    uint64_t rsp_at_fault = (uint64_t)(frame + 1);
    tori::log::print_stack_trace_from(frame->rip, rsp_at_fault, frame->rbp);
  }

  TORI_PANIC("isr", "unhandled exception");
}

} // namespace

namespace tori::arch::x86_64 {

extern "C" void handle_interrupt(InterruptFrame *frame) {
  const uint8_t vec = static_cast<uint8_t>(frame->vector);

  // IRQ 0: Timer (previously PIT, now LAPIC)
  if (vec == 32) {
    tori::time::tick(lapic::id());
    tori::sched::flag_preempt();
    tori::timer::tick();
    lapic::eoi();
    return;
  }

  // Spurious IRQ or unknown: ignore
  if (vec >= 32) {
    lapic::eoi();
    return;
  }

  // CPU exception (vectors 0-31)
  handle_exception(frame);
}

void init_idt() {
  for (int i = 0; i < 256; ++i) {
    uint64_t addr = reinterpret_cast<uint64_t>(isr_stub_table[i]);

    idt[i].offset_low = addr & 0xFFFF;
    idt[i].selector = gdt_selector(GDT_KERNEL_CODE);
    idt[i].ist = 0;
    idt[i].type_attr = 0x8E; // present, ring 0, interrupt gate
    idt[i].offset_mid = (addr >> 16) & 0xFFFF;
    idt[i].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[i].zero = 0;
  }

  // Set IST1 for double fault (vector 8)
  idt[8].ist = 1;

  IDTR idtr = {};
  idtr.limit = sizeof(idt) - 1;
  idtr.base = reinterpret_cast<uint64_t>(idt);

  asm volatile("lidt %0" : : "m"(idtr) : "memory");

  TORI_LOG_INFO("idt", "IDT initialized with 256 entries");
}

static void io_delay() { inb(0x80); }

// Disable the local APIC so the legacy PIC delivers interrupts directly.
// OVMF may enable the APIC, causing the CPU to ignore the PIC's INT line.
void init_pic() {
  // Remap PIC so IRQs don't conflict with CPU exceptions (0-31)
  outb(0x20, 0x11);
  io_delay(); // ICW1: init
  outb(0xA0, 0x11);
  io_delay(); // ICW1: init

  outb(0x21, 0x20);
  io_delay(); // ICW2: master offset = 32
  outb(0xA1, 0x28);
  io_delay(); // ICW2: slave offset = 40

  outb(0x21, 0x04);
  io_delay(); // ICW3: slave at IRQ2
  outb(0xA1, 0x02);
  io_delay(); // ICW3: cascade ID

  outb(0x21, 0x01);
  io_delay(); // ICW4: 8086 mode
  outb(0xA1, 0x01);
  io_delay(); // ICW4: 8086 mode

  // Mask all legacy PIC interrupts
  outb(0x21, 0xFF);
  io_delay();
  outb(0xA1, 0xFF);
  io_delay();

  TORI_LOG_INFO("pic", "Legacy PIC initialized and fully masked");
}

void init_pit() {
  // PIT channel 0, rate generator mode, binary
  // Divisor for ~1000 Hz: 1193182 / 1000 = 1193
  constexpr uint16_t divisor = 1193;

  outb(0x43, 0x36);           // CW: channel 0, lobyte/hibyte, rate gen, binary
  outb(0x40, divisor & 0xFF); // Low byte
  outb(0x40, divisor >> 8);   // High byte

  TORI_LOG_INFO("pit", "PIT initialized at ~1000 Hz");
}

uint64_t timer_tick_count() { return tori::time::uptime_ms(); }

} // namespace tori::arch::x86_64
