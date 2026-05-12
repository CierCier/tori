#pragma once

#include <stdint.h>

namespace tori::sched {

// Called by the timer ISR to request preemption on the current CPU.
void flag_preempt();

// Extern "C" functions called from the ISR assembly stub.
// sched_needs_preempt() returns true if preemption was requested.
// sched_do_preempt() clears the flag and invokes schedule().
extern "C" bool sched_needs_preempt();
extern "C" void sched_do_preempt();

} // namespace tori::sched
