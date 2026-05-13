#pragma once

#include <stdint.h>

namespace tori::stacktrace {

int collect(uint64_t rip, uint64_t rsp, uint64_t rbp, uint64_t* out_frames, int max_frames);
int collect_current(uint64_t* out_frames, int max_frames);
const char* resolve(uint64_t addr, uint64_t& offset);

} // namespace tori::stacktrace
