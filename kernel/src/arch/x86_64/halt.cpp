#include "halt.hpp"

namespace tori::arch::x86_64 {

[[noreturn]] void halt_forever() {
    for (;;) {
        asm volatile("cli; hlt" : : : "memory");
    }
}

} // namespace tori::arch::x86_64
