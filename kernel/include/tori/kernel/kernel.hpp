#pragma once

#include <tori/kernel/boot_info.hpp>

namespace tori {

[[noreturn]] void kernel_main(const boot::BootInfo& boot_info);

} // namespace tori
