#include <tori/kernel/terminal.hpp>
#include <tori/kernel/framebuffer_console.hpp>
#include <tori/kernel/arch/ps2.hpp>
#include <tori/kernel/sched/sched.hpp>

namespace {

using namespace tori::arch::x86_64;

static constexpr size_t line_buf_size = 256;
static char g_line[line_buf_size];
static size_t g_pos = 0;
static bool g_line_ready = false;

static constexpr unsigned term_color = 0x00ffffff;

} // namespace

namespace tori::terminal {

void init() {
    g_pos = 0;
    g_line_ready = false;
    g_line[0] = '\0';
}

void put_char(char c) {
    if (g_line_ready) return;

    if (c == '\b') {
        if (g_pos > 0) {
            --g_pos;
            // Erase on screen: move cursor back, write space, move back
            log::framebuffer_console::write_char('\b', term_color);
        }
        return;
    }

    if (c == '\n') {
        g_line[g_pos] = '\n';
        log::framebuffer_console::write_char('\n', term_color);
        g_line_ready = true;
        return;
    }

    if (c < 0x20 || c > 0x7E) return; // non-printable

    if (g_pos < line_buf_size - 1) {
        g_line[g_pos++] = c;
        log::framebuffer_console::write_char(c, term_color);
    }
}

bool read_line(char* buf, size_t max, size_t* out_len) {
    // Drain PS/2 ring buffer into terminal
    while (ps2::has_char()) {
        char c = ps2::read_char();
        put_char(c);

        if (g_line_ready) {
            // Copy completed line
            size_t len = g_pos < max ? g_pos : max - 1;
            for (size_t i = 0; i < len; ++i) {
                buf[i] = g_line[i];
            }
            buf[len] = '\0';
            *out_len = len;

            g_pos = 0;
            g_line_ready = false;
            return true;
        }
    }
    return false;
}

} // namespace tori::terminal
