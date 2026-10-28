#include "platform/memory.h"
#include "platform/process_info.h"
#include "core/address_space.h"
#include "core/error.h"
#include <windows.h>
#include <psapi.h>
#include <io.h>
#include <cstring>
#include <algorithm>
#include <vector>

namespace venture::platform {
#include "platform/native_memory.inc"
namespace {
int native_memory_filter(EXCEPTION_POINTERS* exception, NativeMemoryCall& call) {
  const auto* record = exception->ExceptionRecord;
  if ((record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_IN_PAGE_ERROR) ||
      record->NumberParameters < 2 || record->ExceptionInformation[0] > 1)
    return EXCEPTION_CONTINUE_SEARCH;
  if (!native_fault_matches(call, record->ExceptionInformation[1], record->ExceptionInformation[0] == 1))
    return EXCEPTION_CONTINUE_SEARCH;
  call.fault = {record->ExceptionInformation[1], record->ExceptionInformation[0] == 1,
      record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR};
  return EXCEPTION_EXECUTE_HANDLER;
}
}
bool native_memory(NativeMemoryOp op, GuestAddr destination, GuestAddr source, std::size_t size,
                   int value, std::uint64_t& result, NativeMemoryFault& fault, bool source_guest) {
  NativeMemoryCall call{op, destination, source, size, value};
  call.source_guest = source_guest;
  if (!native_ranges_valid(call)) { fault = call.fault; return false; }
  __try {
    invoke_native_memory(call);
  } __except(native_memory_filter(GetExceptionInformation(), call)) {
    fault = call.fault; return false;
  }
  result = call.result; return true;
}
namespace {
// Keep the CRT invocation behind a potentially throwing C++ call. Clang's GNU
// Windows driver ignores -fasync-exceptions, and its SEH scope otherwise ends
// before a nounwind CRT builtin. This leaf owns no C++ cleanup state.
template<NativeMemoryOp Op>
int invoke_native_fixed(GuestAddr destination, GuestAddr source, std::size_t size, int value) {
  if constexpr (Op == NativeMemoryOp::copy) std::memcpy(g2h<void>(destination), g2h<const void>(source), size);
  if constexpr (Op == NativeMemoryOp::move) std::memmove(g2h<void>(destination), g2h<const void>(source), size);
  if constexpr (Op == NativeMemoryOp::fill) std::memset(g2h<void>(destination), value, size);
  if constexpr (Op == NativeMemoryOp::compare) return std::memcmp(g2h<const void>(destination), g2h<const void>(source), size);
  return 0;
}
template<NativeMemoryOp Op>
bool native_fixed(GuestAddr destination, GuestAddr source, std::size_t size, int value,
                  int* result, NativeMemoryFault& fault) {
  NativeMemoryCall call{Op, destination, source, size, value};
  if (!native_ranges_valid(call)) { fault = call.fault; return false; }
  int comparison = 0;
  __try {
    if (size) comparison = invoke_native_fixed<Op>(destination, source, size, value);
  } __except(native_memory_filter(GetExceptionInformation(), call)) {
    fault = call.fault; return false;
  }
  if constexpr (Op == NativeMemoryOp::compare) *result = comparison;
  return true;
}
}
bool native_copy(GuestAddr d, GuestAddr s, std::size_t n, NativeMemoryFault& f) {
  return native_fixed<NativeMemoryOp::copy>(d, s, n, 0, nullptr, f);
}
bool native_move(GuestAddr d, GuestAddr s, std::size_t n, NativeMemoryFault& f) {
  return native_fixed<NativeMemoryOp::move>(d, s, n, 0, nullptr, f);
}
bool native_fill(GuestAddr d, int v, std::size_t n, NativeMemoryFault& f) {
  return native_fixed<NativeMemoryOp::fill>(d, 0, n, v, nullptr, f);
}
bool native_compare(GuestAddr a, GuestAddr b, std::size_t n, int& result, NativeMemoryFault& f) {
  return native_fixed<NativeMemoryOp::compare>(a, b, n, 0, &result, f);
}
std::optional<std::uint64_t> committed_bytes(GuestAddr address, std::uint64_t size) {
  if (address + size < address) return std::nullopt;
  const auto end = address + size;
  std::uint64_t bytes = 0;
  MEMORY_BASIC_INFORMATION region{};
  while (address < end) {
    if (!VirtualQuery(g2h(address), &region, sizeof(region))) return std::nullopt;
    const auto next = std::min(end, h2g(region.BaseAddress) + region.RegionSize);
    if (next <= address) return std::nullopt;
    // Private allocations and mapped views (the dual-mapped JIT cache is a section).
    if (region.State == MEM_COMMIT && region.Type != MEM_IMAGE) bytes += next - address;
    address = next;
  }
  return bytes;
}
bool process_debugger_attached() { return IsDebuggerPresent(); }
bool user_shadow_stack_active() {
  PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY policy{};
  return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessUserShadowStackPolicy, &policy, sizeof(policy)) &&
         policy.EnableUserShadowStack;
}
std::string mutex_owner_thread(std::recursive_mutex&) { return "unknown"; }
bool process_resident_memory(ProcessResidentMemory& out) {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return false;
  out = {counters.WorkingSetSize / 1024, counters.PeakWorkingSetSize / 1024};
  return true;
}
bool process_memory(ProcessMemory& out) {
  // PSAPI_WORKING_SET_INFORMATION/PSAPI_WORKING_SET_BLOCK (psapi.h).
  // ShareCount is capped at 7 by Windows; proportional shared residency is
  // consequently an upper bound when a page is shared by more processes.
  std::vector<ULONG_PTR> buffer(8192);
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    auto* ws = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
    if (K32QueryWorkingSet(GetCurrentProcess(), ws,
                          static_cast<DWORD>(buffer.size() * sizeof(ULONG_PTR)))) {
      SYSTEM_INFO system{};
      GetSystemInfo(&system);
      std::uint64_t private_bytes = 0;
      double shared_bytes = 0;
      for (ULONG_PTR i = 0; i < ws->NumberOfEntries; ++i) {
        const auto& page = ws->WorkingSetInfo[i];
        if (!page.Shared || page.ShareCount <= 1) private_bytes += system.dwPageSize;
        else shared_bytes += double(system.dwPageSize) / page.ShareCount;
      }
      out = {private_bytes / 1024, static_cast<std::uint64_t>(shared_bytes / 1024)};
      return true;
    }
    if (GetLastError() != ERROR_BAD_LENGTH) return false;
    const auto capacity = std::max(buffer.size() * 2, std::size_t(ws->NumberOfEntries) + 4096);
    if (capacity > MAXDWORD / sizeof(ULONG_PTR)) return false;
    buffer.resize(capacity);
  }
  return false;
}
namespace {
__attribute__((target("cx16"))) bool cas128(GuestAddr p, const void* value, const void* expected) {
  unsigned __int128 v, e;
  std::memcpy(&v, value, 16); std::memcpy(&e, expected, 16);
  return __sync_bool_compare_and_swap(g2h<unsigned __int128>(p), e, v);
}
DWORD protection(std::uint32_t prot) {
  if (prot & kProtWrite) return PAGE_READWRITE;
  if (prot & (kProtRead | kProtExec)) return PAGE_READONLY;
  return PAGE_NOACCESS;
}
}
std::uint64_t reservation_granularity() { return 65536; }
GuestAddr reserve(GuestAddr address, std::uint64_t size, bool exact) {
  auto p = VirtualAlloc(g2h(address), size, MEM_RESERVE, PAGE_NOACCESS);
  if (!p && !exact) p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
  return h2g(p);
}
void release(GuestAddr address, std::uint64_t) { VirtualFree(g2h(address), 0, MEM_RELEASE); }
bool commit(GuestAddr address, std::uint64_t size, std::uint32_t prot) {
  return VirtualAlloc(g2h(address), size, MEM_COMMIT, protection(prot)) != nullptr;
}
bool decommit(GuestAddr address, std::uint64_t size) {
  return VirtualFree(g2h(address), size, MEM_DECOMMIT) != 0;
}
bool protect(GuestAddr address, std::uint64_t size, std::uint32_t prot) {
  DWORD previous;
  return VirtualProtect(g2h(address), size, protection(prot), &previous) != 0;
}
int map_shared_file(int fd, std::uint64_t offset, std::uint64_t size, std::uint32_t prot, FileMapping& out) {
  const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
  if (handle == INVALID_HANDLE_VALUE) return -err::kEBADF;
  LARGE_INTEGER length;
  if (!GetFileSizeEx(handle, &length)) return -err::kEIO;
  if (offset >= static_cast<std::uint64_t>(length.QuadPart)) return -err::kEOPNOTSUPP;
  const auto available = static_cast<std::uint64_t>(length.QuadPart) - offset;
  // Linux permits the zero-filled remainder of the final file page. Windows
  // MapViewOfFile takes a byte length bounded by EOF and rounds the view itself.
  // Entire pages beyond EOF need SIGBUS support and are still rejected.
  if (size > align_up(available, kGuestPageSize)) return -err::kEOPNOTSUPP;
  const auto backed_size = std::min(size, available);
  auto mapping = CreateFileMappingW(handle, nullptr, (prot & kProtWrite) ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) return GetLastError() == ERROR_ACCESS_DENIED ? -err::kEACCES : -err::kENOMEM;
  const auto aligned = align_down(offset, reservation_granularity());
  const auto delta = offset - aligned;
  auto* view = MapViewOfFile(mapping, (prot & kProtWrite) ? FILE_MAP_WRITE : FILE_MAP_READ,
                             static_cast<DWORD>(aligned >> 32), static_cast<DWORD>(aligned), backed_size + delta);
  auto error = GetLastError();
  CloseHandle(mapping);
  if (!view) return error == ERROR_ACCESS_DENIED ? -err::kEACCES : -err::kENOMEM;
  out = {h2g(view), h2g(view) + delta, backed_size + delta};
  if (!protect(out.address, align_up(size, kGuestPageSize), prot)) {
    UnmapViewOfFile(view); out = {}; return -err::kEACCES;
  }
  return 0;
}
void unmap_shared_file(const FileMapping& mapping) { UnmapViewOfFile(g2h(mapping.base)); }
int sync_shared_file(GuestAddr address, std::uint64_t size, bool) {
  return FlushViewOfFile(g2h(address), size) ? 0 : -err::kEIO;
}
bool write_memory(GuestAddr address, const void* data, std::size_t size) {
  SIZE_T copied = 0;
  // WriteProcessMemory can override read-only protections. Check first.
  if (!memory_accessible(address, size, true)) return false;
  return WriteProcessMemory(GetCurrentProcess(), g2h(address), data, size, &copied) && copied == size;
}
bool memory_accessible(GuestAddr address, std::size_t size, bool write) {
  if (address + size < address) return false;
  MEMORY_BASIC_INFORMATION region{};
  for (auto pos = address; pos < address + size;) {
    if (!VirtualQuery(g2h(pos), &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        (write && !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)))) return false;
    pos = h2g(region.BaseAddress) + region.RegionSize;
  }
  return true;
}
bool compare_exchange(GuestAddr address, const void* value, void* expected, std::size_t size) {
  MEMORY_BASIC_INFORMATION region{};
  if ((address & (size - 1)) || !VirtualQuery(g2h(address), &region, sizeof(region)) ||
      region.State != MEM_COMMIT || region.Protect != PAGE_READWRITE ||
      address + size > h2g(region.BaseAddress) + region.RegionSize) return false;
  // The JIT's fast path uses the same locked x86 compare/exchange semantics.
  switch (size) {
    case 1: return __atomic_compare_exchange_n(g2h<std::uint8_t>(address), static_cast<std::uint8_t*>(expected), *static_cast<const std::uint8_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 2: return __atomic_compare_exchange_n(g2h<std::uint16_t>(address), static_cast<std::uint16_t*>(expected), *static_cast<const std::uint16_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 4: return __atomic_compare_exchange_n(g2h<std::uint32_t>(address), static_cast<std::uint32_t*>(expected), *static_cast<const std::uint32_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 8: return __atomic_compare_exchange_n(g2h<std::uint64_t>(address), static_cast<std::uint64_t*>(expected), *static_cast<const std::uint64_t*>(value), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    case 16: {
      return cas128(address, value, expected);
    }
    default: return false;
  }
}
}

// Microsoft x64 calling convention: rcx, rdx, r8, r9.
#define VENTURE_A0 "%rcx"
#define VENTURE_A1 "%rdx"
#define VENTURE_A2 "%r8"
#define VENTURE_A3 "%r9"
#include "platform/guarded_atomics.inc"

namespace venture::platform {
namespace {
// Vectored handlers run before frame-based (and Dynarmic's function-table)
// handlers, so an access violation in a guarded routine never reaches them.
// Guard-page and other exceptions are left to the rest of the chain.
LONG CALLBACK guarded_fault(EXCEPTION_POINTERS* exception) {
  const auto code = exception->ExceptionRecord->ExceptionCode;
  if ((code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
       code != EXCEPTION_GUARD_PAGE) ||
      !guarded_ip(exception->ContextRecord->Rip))
    return EXCEPTION_CONTINUE_SEARCH;
  if (code == EXCEPTION_GUARD_PAGE) {
    // The system disarmed the guard to report it; a guarded routine only
    // fails its access, so re-arm it for the page's owner (e.g. stack growth).
    auto* page = reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]);
    MEMORY_BASIC_INFORMATION region{};
    DWORD old = 0;
    if (VirtualQuery(page, &region, sizeof(region)))
      VirtualProtect(page, 1, region.Protect | PAGE_GUARD, &old);
  }
  exception->ContextRecord->Rip = reinterpret_cast<DWORD64>(venture_guarded_fault);
  return EXCEPTION_CONTINUE_EXECUTION;
}
bool guarded_access_ready() {
  static const bool ready = AddVectoredExceptionHandler(1, guarded_fault) != nullptr;
  return ready;
}
}
bool guarded_access_ip(std::uintptr_t ip) { return guarded_ip(ip); }
// Same result as ReadProcessMemory (false when any byte is unreadable,
// including guard pages), without a system call per read.
bool read_memory(GuestAddr address, void* out, std::size_t size) { return guarded_read(address, out, size); }
}
