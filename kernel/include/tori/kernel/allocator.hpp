#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tori::memory {

void* kalloc(size_t size, size_t alignment = alignof(uint64_t));
void kfree(void* pointer, size_t size);

} // namespace tori::memory
