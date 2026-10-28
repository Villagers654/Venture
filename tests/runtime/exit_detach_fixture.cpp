#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_DETACH) Sleep(INFINITE);
  return TRUE;
}
