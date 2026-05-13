#pragma once

#include <stdint.h>
#include <stddef.h>

namespace tori::demangle {

const char* demangle(const char* mangled, char* buf, size_t bufsz);

} // namespace tori::demangle
