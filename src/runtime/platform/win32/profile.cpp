#include "platform/profile.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace venture::platform {
namespace {
std::shared_ptr<void> retain(HANDLE h) {
  return h && h != INVALID_HANDLE_VALUE ? std::shared_ptr<void>(h, [](void* p) { CloseHandle(p); }) : nullptr;
}
std::string utf8(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (!n) return {};
  std::string out(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  out.pop_back(); return out;
}
ProfileThread describe(DWORD id, HANDLE h) {
  ProfileThread t{id, {}, retain(h)};
  FILETIME created{}, exited{}, kernel{}, user{};
  if (h && GetThreadTimes(h, &created, &exited, &kernel, &user))
    t.creation_ticks = (std::uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  using Description = HRESULT (WINAPI*)(HANDLE, PWSTR*);
  static auto get_description = reinterpret_cast<Description>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  PWSTR name = nullptr;
  if (h && get_description && SUCCEEDED(get_description(h, &name)) && name) {
    t.name = utf8(name); LocalFree(name);
  }
  return t;
}
std::vector<ProfileSymbol> exports(HMODULE module) {
  const auto* base = reinterpret_cast<const std::byte*>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  std::vector<ProfileSymbol> out;
  if (!directory.VirtualAddress || !directory.Size) return out;
  const auto* e = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
  const auto* names = reinterpret_cast<const DWORD*>(base + e->AddressOfNames);
  const auto* ordinals = reinterpret_cast<const WORD*>(base + e->AddressOfNameOrdinals);
  const auto* functions = reinterpret_cast<const DWORD*>(base + e->AddressOfFunctions);
  const auto image_size = nt->OptionalHeader.SizeOfImage;
  const auto& exception_directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  const bool valid_exception_table = exception_directory.VirtualAddress && exception_directory.VirtualAddress <= image_size &&
      exception_directory.Size <= image_size - exception_directory.VirtualAddress;
  const auto* unwind_table = reinterpret_cast<const RUNTIME_FUNCTION*>(base + exception_directory.VirtualAddress);
  const auto unwind_count = valid_exception_table ? exception_directory.Size / sizeof(RUNTIME_FUNCTION) : 0;
  const auto executable = [&](DWORD rva) {
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
      if ((sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= sections[i].VirtualAddress &&
          rva - sections[i].VirtualAddress < std::max(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData)) return true;
    return false;
  };
  for (DWORD i = 0; i < e->NumberOfNames; ++i) {
    const auto rva = functions[ordinals[i]];
    // Forwarded exports point into the export directory, not executable code.
    if (!rva || rva >= image_size || (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size)) continue;
    out.push_back({reinterpret_cast<std::uintptr_t>(base + rva), 0, reinterpret_cast<const char*>(base + names[i])});
    // Public CRT exports can be tail-jump stubs far from their implementation.
    // Index a bounded direct jump target under that same exported name. No
    // private helper is inferred from its preceding public export.
    auto target = rva;
    for (unsigned hop = 0; hop < 4 && target < image_size && executable(target); ++hop) {
      std::int64_t next = target;
      if (image_size - target >= 5 && std::to_integer<unsigned char>(base[target]) == 0xe9) {
        std::int32_t relative = 0;
        std::memcpy(&relative, base + target + 1, sizeof(relative));
        next += 5ll + relative;
      } else if (image_size - target >= 2 && std::to_integer<unsigned char>(base[target]) == 0xeb) {
        next += 2 + static_cast<std::int8_t>(std::to_integer<unsigned char>(base[target + 1]));
      } else break;
      if (next < 0 || next >= image_size || next == target || !executable(static_cast<DWORD>(next))) break;
      target = static_cast<DWORD>(next);
      out.push_back({reinterpret_cast<std::uintptr_t>(base + target), 0, reinterpret_cast<const char*>(base + names[i])});
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
  for (std::size_t i = 0; i < out.size(); ++i) {
    auto next = i + 1;
    while (next < out.size() && out[next].begin == out[i].begin) ++next;
    DWORD64 image_base = 0;
    const auto* unwind = RtlLookupFunctionEntry(out[i].begin, &image_base, nullptr);
    // Unwind extents keep neighbouring private functions out of an export's
    // attribution. Leaf exports (notably syscall stubs) have no unwind record;
    // their nearest-export fallback stays explicitly bounded.
    out[i].end = unwind ? image_base + unwind->EndAddress :
        (next < out.size() ? std::min(out[next].begin, out[i].begin + 4096) : out[i].begin + 32);
    if (!unwind && unwind_count) {
      const auto rva = out[i].begin - reinterpret_cast<std::uintptr_t>(base);
      const auto* boundary = std::upper_bound(unwind_table, unwind_table + unwind_count, rva,
          [](auto address, const auto& entry) { return address < entry.BeginAddress; });
      if (boundary != unwind_table + unwind_count)
        out[i].end = std::min(out[i].end, reinterpret_cast<std::uintptr_t>(base) + boundary->BeginAddress);
    }
    out[i].end = std::min(out[i].end, reinterpret_cast<std::uintptr_t>(base) + image_size);
  }
  return out;
}
}
bool profile_supported() { return true; }
int profile_sample_signal() noexcept { return 0; }
std::uint32_t profile_thread_id() noexcept { return GetCurrentThreadId(); }
ProfilePowerThrottling profile_power_throttling() noexcept {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  const bool ok = GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state)) != FALSE;
  return {ok, state.ControlMask, state.StateMask, ok ? 0u : GetLastError()};
}
ProfileThread profile_current_thread() {
  auto t = describe(GetCurrentThreadId(), OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId()));
  if (!t.handle) throw std::runtime_error("profiler cannot open guest host thread");
  return t;
}
std::vector<ProfileThread> profile_threads() {
  std::vector<ProfileThread> out;
  auto snapshot = retain(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
  if (!snapshot) return out;
  THREADENTRY32 e{}; e.dwSize = sizeof(e);
  if (Thread32First(snapshot.get(), &e)) do {
    if (e.th32OwnerProcessID != GetCurrentProcessId() || e.th32ThreadID == GetCurrentThreadId()) continue;
    auto h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, e.th32ThreadID);
    if (h) out.push_back(describe(e.th32ThreadID, h));
  } while (Thread32Next(snapshot.get(), &e));
  return out;
}
bool profile_cpu_time(const ProfileThread& t, std::uint64_t& ns) noexcept {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(t.handle.get(), &created, &exited, &kernel, &user)) return false;
  const auto ticks = [](FILETIME f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  ns = (ticks(kernel) + ticks(user)) * 100;
  return true;
}
std::int64_t profile_cpu_cycles(const ProfileThread& t) noexcept {
  ULONG64 cycles = 0;
  return QueryThreadCycleTime(t.handle.get(), &cycles) ? static_cast<std::int64_t>(cycles) : -1;
}
bool profile_sample(const ProfileThread& t, const Cpu* cpu, const std::atomic<std::uint32_t>* activity, ProfileSample& out) noexcept {
  auto h = t.handle.get();
  if (SuspendThread(h) == DWORD(-1)) return false;
  // Microsoft GetThreadContext: only a suspended target has a valid context.
  // Resume even on context failure; nothing that can throw belongs in here.
  CONTEXT context{}; context.ContextFlags = out.inspect_state ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
  bool valid = GetThreadContext(h, &context) != FALSE;
  if (valid) {
    if (activity) out.activity = activity->load(std::memory_order_relaxed);
    out.ip = context.Rip;
    out.cpu_cycles = profile_cpu_cycles(t);
    if (cpu) {
      const auto snapshot = cpu->profile_snapshot(out.ip);
      out.guest_pc = snapshot.pc; out.translated = snapshot.translated;
        out.outlined_atomics = snapshot.outlined_atomics; out.outlined_counts = snapshot.outlined_counts;
      if (out.inspect_state) {
        out.spilled_register_count = cpu->isa() == GuestIsa::a64 ? 31 : 16;
        for (unsigned i = 0; i < out.spilled_register_count; ++i)
          out.spilled_registers[i] = cpu->x(i);
        if (cpu->isa() == GuestIsa::a64) {
          for (unsigned i = 0; i < out.register_memory.size(); ++i) {
            auto& probe = out.register_memory[i];
            probe.address = out.spilled_registers[19 + i];
            SIZE_T copied = 0;
            probe.readable = probe.address && ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(probe.address), probe.words.data(),
                sizeof(probe.words), &copied) && copied == sizeof(probe.words);
            if (!probe.readable) probe.words = {};
          }
          out.host_rbp = context.Rbp;
          for (std::uint64_t fp = out.spilled_registers[29]; out.frame_count < out.frame_returns.size() && fp && !(fp & 7);) {
            std::array<std::uint64_t, 2> record{};
            SIZE_T copied = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(fp), record.data(),
                                   sizeof(record), &copied) || copied != sizeof(record)) break;
            out.frame_returns[out.frame_count++] = record[1];
            if (record[0] <= fp || record[0] - fp > (1u << 20)) break;
            if (out.frame_count <= 2) {
              const auto words = std::min<std::uint64_t>((record[0] - fp - 16) / 8, 64);
              const auto take = std::min<std::uint64_t>(words, out.stack_words.size() - out.stack_word_count);
              if (take && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(fp + 16),
                                            out.stack_words.data() + out.stack_word_count, take * 8, &copied) &&
                  copied == take * 8) out.stack_word_count += static_cast<unsigned>(take);
            }
            fp = record[0];
          }
        }
      }
    }
  }
  if (ResumeThread(h) == DWORD(-1)) {
    // Continuing with a suspended guest could hang teardown indefinitely.
    RaiseFailFastException(nullptr, nullptr, 0);
  }
  profile_cpu_time(t, out.cpu_ns);
  return valid;
}
std::vector<ProfileModule> profile_modules() {
  std::vector<ProfileModule> out;
  auto snapshot = retain(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0));
  if (!snapshot) return out;
  std::array<wchar_t, MAX_PATH> system{}; GetSystemDirectoryW(system.data(), system.size());
  MODULEENTRY32W e{}; e.dwSize = sizeof(e);
  if (Module32FirstW(snapshot.get(), &e)) do {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(e.modBaseAddr), &pinned)) continue;
    auto path = utf8(e.szExePath);
    auto prefix = utf8(system.data());
    auto lower = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; };
    out.push_back({reinterpret_cast<std::uintptr_t>(e.modBaseAddr), reinterpret_cast<std::uintptr_t>(e.modBaseAddr) + e.modBaseSize,
                   lower(utf8(e.szModule)), path, lower(path).starts_with(lower(prefix) + "\\"),
                   GetProcAddress(e.hModule, "vk_icdGetInstanceProcAddr") != nullptr, exports(e.hModule)});
    FreeLibrary(pinned);
  } while (Module32NextW(snapshot.get(), &e));
  return out;
}
std::vector<ProfileSymbol> profile_wait_symbols() {
  std::vector<ProfileSymbol> out;
  // ntdll syscall stubs are bounded by their PE exception/export neighbours;
  // 32 bytes covers their syscall/return sequences on Windows x64.
  for (const char* name : {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution", "NtSignalAndWaitForSingleObject",
                           "NtRemoveIoCompletion", "NtRemoveIoCompletionEx", "NtWaitForAlertByThreadId", "NtReadFile", "NtWaitForWorkViaWorkerFactory",
                           "RtlSleepConditionVariableSRW", "RtlSleepConditionVariableCS", "RtlWaitOnAddress"}) {
    auto p = reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
    if (!p) continue;
    DWORD64 base = 0;
    const auto* function = RtlLookupFunctionEntry(p, &base, nullptr);
    // Unwind metadata bounds normal functions; leaf syscall stubs have none.
    out.push_back({function ? base + function->BeginAddress : p,
                   function ? base + function->EndAddress : p + 32, name});
  }
  return out;
}
std::uintptr_t profile_image_base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }
std::filesystem::path profile_executable() {
  std::array<wchar_t, 32768> path{}; GetModuleFileNameW(nullptr, path.data(), path.size()); return path.data();
}
void profile_delay(std::int64_t nanoseconds) {
  // Windows' ordinary sleep can quantize a 1 kHz request to ~64 Hz. A high
  // resolution waitable timer does not change the machine-wide timer period.
  thread_local auto timer = retain(CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS));
  if (!timer) { std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds)); return; }
  LARGE_INTEGER due{}; due.QuadPart = -std::max<std::int64_t>(1, nanoseconds / 100);
  if (SetWaitableTimer(timer.get(), &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer.get(), INFINITE);
}
} // namespace venture::platform
