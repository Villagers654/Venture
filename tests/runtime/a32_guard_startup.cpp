#include <windows.h>

// The guard test simulates a foreign allocation that already holds the A32 wrap
// guard when the runtime starts. Take it from the PE entry, as the runtime's own
// arena reservation does, so CRT and heap allocations cannot claim 4 GiB first.
extern "C" bool venture_test_guard_reserved = false;
extern "C" int mainCRTStartup();
extern "C" int venture_guard_test_startup() {
  venture_test_guard_reserved =
      VirtualAlloc(reinterpret_cast<void *>(0x100000000ull), 65536, MEM_RESERVE, PAGE_NOACCESS) != nullptr;
  return mainCRTStartup();
}
