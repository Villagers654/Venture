#pragma once
// The NuGet SDK expects the Windows SDK's EventToken.h, absent in mingw-w64.
// This is the public WinRT ABI: a single signed 64-bit registration value.
#include <stdint.h>
typedef struct EventRegistrationToken {
  int64_t value;
} EventRegistrationToken;
