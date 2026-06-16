#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::terminal {

void init();
void put_char(char c);
bool read_line(char* buf, size_t max, size_t* out_len);

} // namespace tori::terminal
