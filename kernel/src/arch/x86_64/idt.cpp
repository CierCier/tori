#include <tori/kernel/idt.hpp>

#include <config.h>
#include <tori/kernel/gdt.hpp>
#include <tori/kernel/lapic.hpp>
#include <tori/kernel/log.hpp>
#include <tori/kernel/sched/sched.hpp>
#include <tori/kernel/task.hpp>
#include <tori/kernel/time.hpp>
#include <tori/kernel/timer.hpp>
#include <tori/kernel/vmm.hpp>
#include <tori/kernel/process/thread.hpp>
#include <tori/kernel/process/process.hpp>
#include <tori/kernel/arch/ps2.hpp>

#include "io.hpp"

namespace {

using namespace tori::arch::x86_64;

uint64_t current_user_pml4() {
    auto* task = tori::sched::current_task();
    if (!task || !task->thread) return 0;

    auto* thread = static_cast<tori::proc::Thread*>(task->thread);
    return thread->process ? thread->process->pml4_phys : 0;
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

    if (tori::memory::vmm::handle_cow_fault(
            cr2, frame->error_code, current_user_pml4())) {
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

  // IRQ 1: PS/2 keyboard
  if (vec == 33) {
    tori::arch::x86_64::ps2::handle_irq();
    // Send EOI to both PIC (master) and LAPIC
    outb(0x20, 0x20);
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
void unmask_keyboard_irq() {
  // Unmask IRQ1 on master PIC (clear bit 1)
  uint8_t mask = inb(0x21);
  mask &= ~0x02;
  outb(0x21, mask);
  io_delay();
  TORI_LOG_INFO("pic", "IRQ1 (keyboard) unmasked on PIC");
}

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
