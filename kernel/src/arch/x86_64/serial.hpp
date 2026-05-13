#pragma once

namespace tori::arch::x86_64::serial {

void init();
void write_char(char c);
void write_string(const char* text);

// Lock/unlock the serial port for atomic multi-call writes.
// Caller must hold the lock when using write_string_locked.
void lock();
void unlock();
void write_string_locked(const char* text);

} // namespace tori::arch::x86_64::serial
