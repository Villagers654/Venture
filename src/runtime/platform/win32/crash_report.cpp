#include "core/pc_trace.h"
#include "core/api_result_trace.h"
#include "platform/crash_report.h"
#include "platform/memory.h"
#include "common/log.h"
#include "common/paths.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <filesystem>
#include <string>

namespace venture::platform {
namespace {
decltype(&MiniDumpWriteDump) write_dump;
HANDLE requested, completed;
EXCEPTION_RECORD record;
CONTEXT context;
EXCEPTION_POINTERS exception{&record, &context};
DWORD fault_tid;
volatile LONG reporting;
std::wstring dump_path, dump_pattern, dump_directory;
std::string dump_line;

void location(void* address, char* output, std::size_t size) {
  MEMORY_BASIC_INFORMATION memory{};
  wchar_t path[1024]{};
  char name[3072] = "unknown";
  std::uintptr_t base = 0;
  if (VirtualQuery(address, &memory, sizeof(memory)) && memory.Type == MEM_IMAGE) {
    base = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    if (GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), path, 1024)) {
      auto* leaf = wcsrchr(path, L'\\');
      WideCharToMultiByte(CP_UTF8, 0, leaf ? leaf + 1 : path, -1, name, sizeof(name), nullptr, nullptr);
    }
  }
  std::snprintf(output, size, "%s+0x%llx", name,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address) - base));
}

// No filesystem containers or allocation in the crash path. Keep the newest
// three runtime dumps in this log directory, including dumps from earlier runs.
void prune_dumps() {
  struct Kept { FILETIME time{}; wchar_t name[MAX_PATH]{}; } newest[3];
  unsigned count = 0;
  WIN32_FIND_DATAW data{};
  auto search = FindFirstFileW(dump_pattern.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    ++count;
    Kept candidate{};
    candidate.time = data.ftLastWriteTime;
    wcscpy(candidate.name, data.cFileName);
    for (auto& kept : newest) {
      const auto order = CompareFileTime(&candidate.time, &kept.time);
      if (order > 0 || (order == 0 && wcscmp(candidate.name, kept.name) > 0))
        std::swap(candidate, kept);
    }
  } while (FindNextFileW(search, &data));
  FindClose(search);
  if (count <= 3) return;
  search = FindFirstFileW(dump_pattern.c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const auto order = CompareFileTime(&data.ftLastWriteTime, &newest[2].time);
    if (order < 0 || (order == 0 && wcscmp(data.cFileName, newest[2].name) < 0)) {
      wchar_t path[32768];
      if (dump_directory.size() + wcslen(data.cFileName) + 1 < std::size(path)) {
        wcscpy(path, dump_directory.c_str());
        wcscat(path, data.cFileName);
        DeleteFileW(path);
      }
    }
  } while (FindNextFileW(search, &data));
  FindClose(search);
}

DWORD WINAPI dump_worker(void*) {
  WaitForSingleObject(requested, INFINITE);
  venture::api_result_trace::crash_flush();
  venture::pc_trace::flush();
  char where[3200], line[3500];
  location(record.ExceptionAddress, where, sizeof(where));
  auto n = std::snprintf(line, sizeof(line), " E [runtime] Host exception 0x%08lx at %s (thread %lu)\n",
                         record.ExceptionCode, where, fault_tid);
  venture::log::crash_write(line, n);
  // Walk the original exception context, not the helper's own stack. Leaf
  // return addresses are read through the OS to stop cleanly at bad stacks.
  auto walk = context;
  for (unsigned i = 0; i < 16 && walk.Rip; ++i) {
    location(reinterpret_cast<void*>(walk.Rip), where, sizeof(where));
    n = std::snprintf(line, sizeof(line), " E [host-stack]   #%u %s\n", i, where);
    venture::log::crash_write(line, n);
    DWORD64 base = 0;
    auto* entry = RtlLookupFunctionEntry(walk.Rip, &base, nullptr);
    const auto old_rsp = walk.Rsp;
    if (entry) {
      PVOID handler_data = nullptr;
      DWORD64 frame = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, walk.Rip, entry, &walk, &handler_data, &frame, nullptr);
    } else {
      SIZE_T read = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(walk.Rsp), &walk.Rip,
                             sizeof(walk.Rip), &read) || read != sizeof(walk.Rip)) break;
      walk.Rsp += sizeof(DWORD64);
    }
    if (walk.Rsp <= old_rsp) break;
  }
  if (write_dump && !dump_path.empty()) {
    auto file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION info{fault_tid, &exception, FALSE};
      const auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo |
                           MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
      const bool written = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, flags,
                                      &info, nullptr, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
      if (written) {
        venture::log::crash_write(dump_line.data(), dump_line.size());
        prune_dumps();
      } else {
        DeleteFileW(dump_path.c_str());
      }
    }
  }
  SetEvent(completed);
  return 0;
}

LONG WINAPI host_exception(EXCEPTION_POINTERS* fault) {
  if (InterlockedCompareExchange(&reporting, 1, 0))
    return EXCEPTION_CONTINUE_SEARCH;
  record = *fault->ExceptionRecord;
  context = *fault->ContextRecord;
  fault_tid = GetCurrentThreadId();
  // Drain the logging buffer immediately, even if dump generation later fails.
  venture::log::crash_write("", 0);
  SetEvent(requested);
  WaitForSingleObject(completed, 30000);
  // Suppress the system error dialog without changing the original exit status.
  return EXCEPTION_EXECUTE_HANDLER;
}
}

void prepare_crash_stack() {
  ULONG bytes = 65536;
  SetThreadStackGuarantee(&bytes);
}
void install_crash_report(std::string_view log_path, std::string_view package) {
  static bool installed = false;
  if (installed) return;
  prepare_crash_stack();
  // Force initialization of the vectored guarded-read handler before the filter.
  std::uint32_t value;
  read_memory(0, &value, sizeof(value));
  if (!log_path.empty()) {
    auto directory = std::filesystem::absolute(venture::paths::from_utf8(log_path)).parent_path();
    std::string safe_package(package.empty() ? "unknown" : package);
    for (auto& c : safe_package)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-')) c = '_';
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char stamp[80];
    std::snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u%02u-%03u-%lu",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                  now.wMilliseconds, GetCurrentProcessId());
    auto path = directory / venture::paths::from_utf8("runtime-" + safe_package + "-" + stamp + ".dmp");
    dump_path = path.wstring();
    dump_directory = directory.wstring() + L"\\";
    dump_pattern = dump_directory + L"runtime-*.dmp";
    dump_line = " I [runtime] Crash dump: " + venture::paths::utf8(path) + "\n";
  }
  auto dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (dbghelp) write_dump = reinterpret_cast<decltype(write_dump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
  requested = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!requested || !completed) throw std::runtime_error("Cannot create crash report events");
  auto worker = CreateThread(nullptr, 0, dump_worker, nullptr, 0, nullptr);
  if (!worker) throw std::runtime_error("Cannot create crash report thread");
  CloseHandle(worker);
  SetUnhandledExceptionFilter(host_exception);
  installed = true;
}
}
