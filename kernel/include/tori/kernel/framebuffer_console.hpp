#pragma once

#include <tori/kernel/boot_info.hpp>

namespace tori::log::framebuffer_console {

void init(const boot::Framebuffer& framebuffer);
void write_char(char c, unsigned color);
void write_string(const char* text, unsigned color);
bool is_available();

} // namespace tori::log::framebuffer_console
