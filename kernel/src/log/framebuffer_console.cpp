#include <tori/kernel/framebuffer_console.hpp>

#include <stdint.h>

namespace {

struct ConsoleState {
    uint32_t* pixels;
    uint64_t width;
    uint64_t height;
    uint64_t pitch_pixels;
    uint64_t cursor_x;
    uint64_t cursor_y;
    bool available;
};

ConsoleState state = {};

constexpr uint64_t glyph_width = 8;
constexpr uint64_t glyph_height = 8;
constexpr uint64_t columns_margin = 1;

uint32_t pack_color(unsigned color) {
    return static_cast<uint32_t>(color);
}

uint8_t glyph_pattern(char c, uint64_t row) {
    if (row >= 7) {
        return 0;
    }

    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }

    switch (c) {
    case 'A': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}; return p[row]; }
    case 'B': { constexpr uint8_t p[7] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}; return p[row]; }
    case 'C': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}; return p[row]; }
    case 'D': { constexpr uint8_t p[7] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}; return p[row]; }
    case 'E': { constexpr uint8_t p[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}; return p[row]; }
    case 'F': { constexpr uint8_t p[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}; return p[row]; }
    case 'G': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}; return p[row]; }
    case 'H': { constexpr uint8_t p[7] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}; return p[row]; }
    case 'I': { constexpr uint8_t p[7] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}; return p[row]; }
    case 'J': { constexpr uint8_t p[7] = {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c}; return p[row]; }
    case 'K': { constexpr uint8_t p[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}; return p[row]; }
    case 'L': { constexpr uint8_t p[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}; return p[row]; }
    case 'M': { constexpr uint8_t p[7] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}; return p[row]; }
    case 'N': { constexpr uint8_t p[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}; return p[row]; }
    case 'O': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}; return p[row]; }
    case 'P': { constexpr uint8_t p[7] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}; return p[row]; }
    case 'Q': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}; return p[row]; }
    case 'R': { constexpr uint8_t p[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}; return p[row]; }
    case 'S': { constexpr uint8_t p[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}; return p[row]; }
    case 'T': { constexpr uint8_t p[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return p[row]; }
    case 'U': { constexpr uint8_t p[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}; return p[row]; }
    case 'V': { constexpr uint8_t p[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}; return p[row]; }
    case 'W': { constexpr uint8_t p[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}; return p[row]; }
    case 'X': { constexpr uint8_t p[7] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}; return p[row]; }
    case 'Y': { constexpr uint8_t p[7] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}; return p[row]; }
    case 'Z': { constexpr uint8_t p[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}; return p[row]; }
    case '0': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}; return p[row]; }
    case '1': { constexpr uint8_t p[7] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}; return p[row]; }
    case '2': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}; return p[row]; }
    case '3': { constexpr uint8_t p[7] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}; return p[row]; }
    case '4': { constexpr uint8_t p[7] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}; return p[row]; }
    case '5': { constexpr uint8_t p[7] = {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}; return p[row]; }
    case '6': { constexpr uint8_t p[7] = {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}; return p[row]; }
    case '7': { constexpr uint8_t p[7] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}; return p[row]; }
    case '8': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}; return p[row]; }
    case '9': { constexpr uint8_t p[7] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}; return p[row]; }
    case '[': { constexpr uint8_t p[7] = {0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e}; return p[row]; }
    case ']': { constexpr uint8_t p[7] = {0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e}; return p[row]; }
    case '(': { constexpr uint8_t p[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}; return p[row]; }
    case ')': { constexpr uint8_t p[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}; return p[row]; }
    case ':': { constexpr uint8_t p[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}; return p[row]; }
    case '/': { constexpr uint8_t p[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}; return p[row]; }
    case '-': { constexpr uint8_t p[7] = {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}; return p[row]; }
    case '=': { constexpr uint8_t p[7] = {0x00, 0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00}; return p[row]; }
    case '.': { constexpr uint8_t p[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}; return p[row]; }
    case ',': { constexpr uint8_t p[7] = {0x00, 0x00, 0x00, 0x00, 0x0c, 0x04, 0x08}; return p[row]; }
    case ';': { constexpr uint8_t p[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x08}; return p[row]; }
    case '_': { constexpr uint8_t p[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f}; return p[row]; }
    case ' ': return 0;
    default: { constexpr uint8_t p[7] = {0x1f, 0x11, 0x02, 0x04, 0x04, 0x00, 0x04}; return p[row]; }
    }
}

uint8_t glyph_row(char c, uint64_t row) {
    return static_cast<uint8_t>(glyph_pattern(c, row) << 2);
}

void put_pixel(uint64_t x, uint64_t y, uint32_t color) {
    if (!state.available || x >= state.width || y >= state.height) {
        return;
    }

    state.pixels[y * state.pitch_pixels + x] = color;
}

void newline() {
    state.cursor_x = 0;
    ++state.cursor_y;

    const uint64_t max_rows = state.height / glyph_height;
    if (state.cursor_y >= max_rows) {
        state.cursor_y = 0;
    }
}

void draw_char(char c, uint32_t fg, uint32_t bg) {
    const uint64_t x0 = state.cursor_x * glyph_width;
    const uint64_t y0 = state.cursor_y * glyph_height;

    for (uint64_t y = 0; y < glyph_height; ++y) {
        const uint8_t bits = glyph_row(c, y);
        for (uint64_t x = 0; x < glyph_width; ++x) {
            const bool on = (bits & (0x80u >> x)) != 0;
            put_pixel(x0 + x, y0 + y, on ? fg : bg);
        }
    }
}

} // namespace

namespace tori::log::framebuffer_console {

void init(const boot::Framebuffer& framebuffer) {
    if (framebuffer.address == nullptr || framebuffer.bits_per_pixel != 32) {
        state.available = false;
        return;
    }

    state.pixels = static_cast<uint32_t*>(framebuffer.address);
    state.width = framebuffer.width;
    state.height = framebuffer.height;
    state.pitch_pixels = framebuffer.pitch / 4;
    state.cursor_x = 0;
    state.cursor_y = 0;
    state.available = true;
}

bool is_available() {
    return state.available;
}

void write_char(char c, unsigned color) {
    if (!state.available) {
        return;
    }

    if (c == '\n') {
        newline();
        return;
    }

    if (c == '\r') {
        state.cursor_x = 0;
        return;
    }

    draw_char(c, pack_color(color), 0x00000000);
    ++state.cursor_x;

    const uint64_t max_columns = state.width / glyph_width;
    if (state.cursor_x + columns_margin >= max_columns) {
        newline();
    }
}

void write_string(const char* text, unsigned color) {
    if (text == nullptr) {
        return;
    }

    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        write_char(*cursor, color);
    }
}

} // namespace tori::log::framebuffer_console
