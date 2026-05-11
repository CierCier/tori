#pragma once

namespace tori::arch::x86_64::serial {

void init();
void write_char(char c);
void write_string(const char* text);

} // namespace tori::arch::x86_64::serial
