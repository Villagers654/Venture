#include "platform/low_memory.h"

// POSIX counterpart of win32/low_memory_startup.cpp for executables whose
// main() is not Venture's (test drivers): reserve the A32 arena before other
// C++ constructors allocate. The state it touches is constant-initialized.
__attribute__((constructor(101))) static void venture_low_memory_startup() {
  venture::platform::pre_reserve_low_memory();
}
