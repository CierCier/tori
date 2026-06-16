#include <tori/kernel/arch/ps2.hpp>
#include <tori/kernel/log.hpp>

#include "io.hpp"

namespace {

using namespace tori::arch::x86_64;

// ------------------------------------------------------------------
// 8042 PS/2 controller helpers
// ------------------------------------------------------------------

static void wait_write() {
    for (int i = 0; i < 100000; ++i) {
        if (!(inb(0x64) & 0x02)) return;
    }
}

static void wait_read() {
    for (int i = 0; i < 100000; ++i) {
        if (inb(0x64) & 0x01) return;
    }
}

static void write_cmd(uint8_t cmd) {
    wait_write();
    outb(0x64, cmd);
}

static void write_data(uint8_t data) {
    wait_write();
    outb(0x60, data);
}

static uint8_t read_data() {
    wait_read();
    return inb(0x60);
}

static void flush_data() {
    for (int i = 0; i < 100; ++i) {
        if (inb(0x64) & 0x01) inb(0x60);
        else break;
    }
}

// ------------------------------------------------------------------
// Scancode set 1 → ASCII  (US layout)
// ------------------------------------------------------------------

static constexpr char s1_normal[128] = {
    0,   27,   '1', '2', '3', '4', '5', '6',    // 00-07
    '7', '8', '9', '0', '-', '=', '\b', '\t',   // 08-0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',    // 10-17
    'o', 'p', '[', ']', '\n',   0,  'a', 's',    // 18-1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',     // 20-27
    '\'', '`',   0, '\\', 'z', 'x', 'c', 'v',    // 28-2F
    'b', 'n', 'm', ',', '.', '/',   0,  '*',      // 30-37
    0,   ' ',   0,   0,   0,   0,   0,   0,        // 38-3F
};

static constexpr char s1_shift[128] = {
    0,   27,   '!', '@', '#', '$', '%', '^',   // 00-07
    '&', '*', '(', ')', '_', '+', '\b', '\t',   // 08-0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',    // 10-17
    'O', 'P', '{', '}', '\n',   0,  'A', 'S',    // 18-1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',     // 20-27
    '"', '~',   0, '|', 'Z', 'X', 'C', 'V',     // 28-2F
    'B', 'N', 'M', '<', '>', '?',   0,  '*',      // 30-37
    0,   ' ',   0,   0,   0,   0,   0,   0,        // 38-3F
};

// ------------------------------------------------------------------
// Modifier state
// ------------------------------------------------------------------

static volatile bool g_shift_pressed = false;
static volatile bool g_extended = false;

// ------------------------------------------------------------------
// Character ring buffer (ISR → consumer)
// ------------------------------------------------------------------

static constexpr size_t buf_size = 256;
static volatile char g_buf[buf_size];
static volatile size_t g_head = 0;
static volatile size_t g_tail = 0;

static bool buf_push(char c) {
    size_t next = (g_head + 1) % buf_size;
    if (next == g_tail) return false;
    g_buf[g_head] = c;
    g_head = next;
    return true;
}

} // namespace

namespace tori::arch::x86_64::ps2 {

// ------------------------------------------------------------------
// Initialization
// ------------------------------------------------------------------

bool init() {
    // 1. Disable devices
    write_cmd(0xAD); // disable keyboard port
    write_cmd(0xA7); // disable mouse port (if any)

    // 2. Flush output buffer
    flush_data();

    // 3. Read config byte
    write_cmd(0x20);
    uint8_t config = read_data();

    // 4. Enable keyboard interrupt (bit 0), clock (bit 4),
    //    translation (bit 5), system flag (bit 2).
    config |= 0x01;  // enable IRQ for port 1
    config |= 0x04;  // set system flag
    config |= 0x10;  // enable clock for port 1
    config |= 0x20;  // translate scancode set 2 → set 1
    config &= ~0x02; // disable IRQ for port 2

    // 5. Write config
    write_cmd(0x60);
    write_data(config);

    // 6. Enable keyboard port
    write_cmd(0xAE);

    // 7. Controller self-test
    write_cmd(0xAA);
    if (read_data() != 0x55) {
        TORI_LOG_WARN("ps2", "controller self-test failed");
        return false;
    }

    // 8. Test keyboard port
    write_cmd(0xAB);
    if (read_data() != 0x00) {
        TORI_LOG_WARN("ps2", "keyboard port test failed");
        return false;
    }

    // 9. Reset keyboard
    write_data(0xFF);
    if (read_data() != 0xFA) { // ACK
        TORI_LOG_WARN("ps2", "keyboard reset NAK");
        return false;
    }
    if (read_data() != 0xAA) { // self-test passed
        TORI_LOG_WARN("ps2", "keyboard self-test failed");
        return false;
    }

    // 10. Enable scanning
    write_data(0xF4);
    if (read_data() != 0xFA) {
        TORI_LOG_WARN("ps2", "keyboard enable scanning NAK");
        return false;
    }

    TORI_LOG_INFO("ps2", "PS/2 keyboard initialized");
    return true;
}

// ------------------------------------------------------------------
// IRQ handler — called from handle_interrupt(vector 33)
// ------------------------------------------------------------------

void handle_irq() {
    uint8_t sc = inb(0x60);

    // Extended prefix byte
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }

    bool extended = g_extended;
    g_extended = false;

    bool make = !(sc & 0x80);
    uint8_t code = sc & 0x7F;

    // Update modifier state
    if (code == 0x2A || code == 0x36) {
        g_shift_pressed = make;
        return;
    }

    // Only process make codes (ignore break codes for non-modifiers)
    if (!make) return;

    // For now, skip extended keys (arrows, etc.)
    if (extended) return;

    // Skip unknown scancodes
    if (code >= 128) return;

    char ascii = g_shift_pressed ? s1_shift[code] : s1_normal[code];

    if (ascii == 0) return; // unmapped

    buf_push(ascii);
}

// ------------------------------------------------------------------
// Consumer interface
// ------------------------------------------------------------------

char read_char() {
    if (g_head == g_tail) return 0;
    char c = g_buf[g_tail];
    g_tail = (g_tail + 1) % buf_size;
    return c;
}

bool has_char() {
    return g_head != g_tail;
}

} // namespace tori::arch::x86_64::ps2
