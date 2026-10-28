#pragma once
#include <cstdlib>
namespace venture::environment {
// Immutable host settings captured on first use. Guest libc environment is separate.
// Returned pointers remain valid for the process lifetime, including after refresh.
const char* getenv(const char* name);
// For controlled initialization/test changes. Serialize host mutation with refresh;
// readers may continue concurrently. Previous snapshots are retained.
void refresh();
} // namespace venture::environment
