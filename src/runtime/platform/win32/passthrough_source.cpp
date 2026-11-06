// The external passthrough source's Windows plumbing: overlapped named pipes
// served by a process of the same user (which program serves one is not
// checked), process ID, randomness and time.
#include "xr/passthrough_source.h"
#include "platform/win32/passthrough_pipe.h"
#include <windows.h>
#include <bcrypt.h>
#include <utility>
#include <vector>

namespace venture::xr::passthrough_source_platform {
namespace {
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
std::vector<std::uint8_t> token_user(HANDLE process) {
  HANDLE raw = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return {};
  Handle token(raw);
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  if (!size || size > 4096) return {};
  std::vector<std::uint8_t> data(size);
  if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) return {};
  return data;
}
// A pipe name is machine-wide; only a listener running as this user is spoken to.
bool same_user(HANDLE pipe) {
  ULONG server = 0;
  if (!GetNamedPipeServerProcessId(pipe, &server) || !server) return false;
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server));
  if (!process) return false;
  const auto theirs = token_user(process.value), ours = token_user(GetCurrentProcess());
  if (theirs.empty() || ours.empty()) return false;
  return EqualSid(reinterpret_cast<const TOKEN_USER*>(theirs.data())->User.Sid,
                  reinterpret_cast<const TOKEN_USER*>(ours.data())->User.Sid);
}
struct Pipe final : Channel {
  Handle pipe;
  explicit Pipe(HANDLE handle) : pipe(handle) {}
  // Overlapped I/O keeps its buffers until cancellation has completed; no wait
  // here runs on an XR thread except the bounded probe.
  bool transfer(bool writing, void* bytes, std::size_t size, const std::function<bool()>& alive,
                std::uint64_t deadline) override {
    auto* data = static_cast<std::uint8_t*>(bytes);
    for (std::size_t used = 0; used < size;) {
      Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!event) return false;
      OVERLAPPED operation{};
      operation.hEvent = event.value;
      DWORD count = 0;
      const auto rest = DWORD(size - used);
      const bool immediate = writing ? WriteFile(pipe.value, data + used, rest, &count, &operation)
                                     : ReadFile(pipe.value, data + used, rest, &count, &operation);
      if (!immediate) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        const auto expired = [&] { return deadline && GetTickCount64() >= deadline; };
        while (alive() && !expired() && WaitForSingleObject(event.value, 100) == WAIT_TIMEOUT) {}
        if (!alive() || expired()) {
          CancelIoEx(pipe.value, &operation);
          GetOverlappedResult(pipe.value, &operation, &count, TRUE);
          return false;
        }
        if (!GetOverlappedResult(pipe.value, &operation, &count, TRUE)) return false;
      }
      if (!count || !alive()) return false;
      used += count;
      if (used < size && !deadline) deadline = GetTickCount64() + 2000;
    }
    return true;
  }
};
} // namespace
std::unique_ptr<Channel> open_pipe(const wchar_t* name, std::uint32_t& server) {
  Handle pipe(CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  ULONG id = 0;
  if (!pipe || !GetNamedPipeServerProcessId(pipe.value, &id) || !same_user(pipe.value)) return nullptr;
  server = id;
  const auto handle = std::exchange(pipe.value, INVALID_HANDLE_VALUE);
  return std::make_unique<Pipe>(handle);
}
std::uint32_t process_id() { return GetCurrentProcessId(); }
bool random(void* data, std::size_t size) {
  return BCryptGenRandom(nullptr, static_cast<PUCHAR>(data), ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}
std::uint64_t now_ms() { return GetTickCount64(); }
} // namespace venture::xr::passthrough_source_platform
