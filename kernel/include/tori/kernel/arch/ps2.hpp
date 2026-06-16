#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::arch::x86_64::ps2 {

bool init();
void handle_irq();
char read_char();
bool has_char();

} // namespace tori::arch::x86_64::ps2
