#include "platform/java_host.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace venture::java::platform {
namespace {
std::runtime_error failed(const char* action) {
  return std::runtime_error(std::string(action) + " (Win32 " + std::to_string(GetLastError()) + ")");
}
std::wstring wide(const std::string& s) {
  int n =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (!n && !s.empty()) throw failed("UTF-8 conversion");
  std::wstring result(n, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
  return result;
}
std::wstring quote(const std::wstring& s) {
  std::wstring result = L"\"";
  unsigned slashes = 0;
  for (wchar_t c : s) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    result.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
    result += c;
    slashes = 0;
  }
  result.append(2 * slashes, L'\\');
  return result + L'"';
}
}  // namespace
std::vector<std::string> jvm_host_options() { return {}; }
void* loaded_agent_symbol(const std::filesystem::path& agent, const char* name) {
  auto module = GetModuleHandleW(agent.c_str());
  if (!module) throw failed("GetModuleHandle(JVM agent)");
  auto symbol = GetProcAddress(module, name);
  if (!symbol) throw failed("GetProcAddress(JVM agent)");
  return reinterpret_cast<void*>(symbol);
}
void* jvm_entry(const std::filesystem::path& home) {
  auto bin = std::filesystem::absolute(home / "bin");
  // HotSpot resolves agent libraries (including java.instrument) through its
  // startup PATH as well as the legacy Windows loader. AddDllDirectory alone
  // only affects LoadLibraryEx calls which opt into that search list. Match
  // java.exe's access to its JDK bin without changing the parent's environment
  // or the process-wide default DLL search policy. Keep both CRT and Win32
  // environment views in sync before JNI_CreateJavaVM reads them.
  std::wstring path = bin.native();
  if (const auto* prior = _wgetenv(L"PATH"); prior && *prior) {
    path += L';';
    path += prior;
  }
  if (_wputenv_s(L"PATH", path.c_str()) != 0)
    throw std::runtime_error("Cannot configure JDK library search path");
  auto directory = AddDllDirectory(bin.c_str());
  if (!directory) throw failed("AddDllDirectory(JDK bin)");
  auto module = LoadLibraryExW((bin / "server/jvm.dll").c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!module) {
    auto error = failed("LoadLibraryEx(jvm.dll)");
    RemoveDllDirectory(directory);
    throw error;
  }
  // Retain this scoped directory for HotSpot's delayed dependency loads. This
  // changes only this process's search list, never PATH or machine settings.
  auto entry = GetProcAddress(module, "JNI_CreateJavaVM");
  if (!entry) throw failed("GetProcAddress(JNI_CreateJavaVM)");
  return reinterpret_cast<void*>(entry);
}
void* executable_stub(std::span<const std::byte> bytes) {
  void* memory = VirtualAlloc(nullptr, bytes.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!memory) throw failed("VirtualAlloc(JNI stub)");
  std::memcpy(memory, bytes.data(), bytes.size());
  DWORD old;
  if (!VirtualProtect(memory, bytes.size(), PAGE_EXECUTE_READ, &old)) {
    auto error = failed("VirtualProtect(JNI stub)");
    VirtualFree(memory, 0, MEM_RELEASE);
    throw error;
  }
  if (!FlushInstructionCache(GetCurrentProcess(), memory, bytes.size()))
    throw failed("FlushInstructionCache(JNI stub)");
  return memory;
}
int run_process(const std::vector<std::string>& args, const std::vector<std::string>& unset_environment,
                const std::filesystem::path* output) {
  if (args.empty()) throw std::invalid_argument("empty process arguments");
  std::wstring command;
  for (const auto& arg : args) {
    if (!command.empty()) command += L' ';
    command += quote(wide(arg));
  }
  // A filtered copy of this process's (sorted) block; nullptr inherits it whole.
  std::vector<wchar_t> environment;
  if (!unset_environment.empty()) {
    std::vector<std::wstring> names;
    for (const auto& name : unset_environment) names.push_back(wide(name));
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) throw failed("GetEnvironmentStrings");
    for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1) {
      const std::wstring_view text(entry);
      // Names may start with '=' (per-drive directories), never contain one later.
      const auto name = text.substr(0, text.find(L'=', 1));
      const bool removed = std::any_of(names.begin(), names.end(), [&](const std::wstring& unset) {
        return CompareStringOrdinal(name.data(), static_cast<int>(name.size()), unset.data(),
                                    static_cast<int>(unset.size()), TRUE) == CSTR_EQUAL;
      });
      if (removed) continue;
      environment.insert(environment.end(), text.begin(), text.end());
      environment.push_back(L'\0');
    }
    FreeEnvironmentStringsW(block);
    if (environment.empty()) environment.push_back(L'\0');
    environment.push_back(L'\0');
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  // The output file is the only handle the child inherits.
  HANDLE file = INVALID_HANDLE_VALUE;
  std::vector<std::byte> attributes;
  if (output) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    file = CreateFileW(output->c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw failed("CreateFile(converter output)");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attributes.resize(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) ||
        !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &file,
                                   sizeof(file), nullptr, nullptr)) {
      auto error = failed("UpdateProcThreadAttribute(converter output)");
      CloseHandle(file);
      throw error;
    }
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = file;
  }
  PROCESS_INFORMATION process{};
  const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, output != nullptr,
                                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                                      environment.empty() ? nullptr : environment.data(), nullptr,
                                      &startup.StartupInfo, &process);
  auto error = created ? std::runtime_error("") : failed("CreateProcess(converter)");
  if (output) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(file);
  }
  if (!created) throw error;
  CloseHandle(process.hThread);
  DWORD code = 0;
  if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0 ||
      !GetExitCodeProcess(process.hProcess, &code)) {
    auto error = failed("WaitForSingleObject(converter)");
    CloseHandle(process.hProcess);
    throw error;
  }
  CloseHandle(process.hProcess);
  return static_cast<int>(code);
}
}  // namespace venture::java::platform
