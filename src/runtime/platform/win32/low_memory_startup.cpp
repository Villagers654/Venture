#include "platform/low_memory.h"

// mingw-w64/crt/crtexe.c allocates argv and installs CRT exit handlers before
// _initterm_e. Use the PE entry point so even those allocations see the arena:
// https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-crt/crt/crtexe.c
// Windows has already mapped imports and TLS by entry; an occupied guard from
// that unavoidable loader work is preserved and selects checked A32 callbacks.
extern "C" int mainCRTStartup();
extern "C" int venture_low_memory_startup() {
  venture::platform::pre_reserve_low_memory();
  return mainCRTStartup();
}
