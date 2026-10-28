#pragma once
namespace venture::platform {
// The host operating system's SQLite (winsqlite3.dll from System32 on Windows,
// libsqlite3.so.0 elsewhere), loaded on first use and kept for process
// lifetime. Returns nullptr when the library or the symbol is unavailable.
void* sqlite_symbol(const char* name);
}  // namespace venture::platform
