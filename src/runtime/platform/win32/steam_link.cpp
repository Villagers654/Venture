#include "xr/steam_link.h"
#include "xr/steam_link_protocol.h"
#include "common/log.h"
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace venture::xr {
namespace {
struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
  ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
// The registered runtime and every endpoint image must be local, without
// reparse-point substitution. A familiar process name alone proves nothing.
bool local_path(const std::filesystem::path& path) {
  const auto name = path.wstring();
  if (name.size() < 3 || name[1] != L':' || name[2] != L'\\' || GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED)
    return false;
  auto part = path.root_path();
  for (const auto& component : path.relative_path()) {
    if (component == L"..") return false;
    part /= component;
    const auto attributes = GetFileAttributesW(part.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
  }
  return true;
}
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
  return local_path(a) && local_path(b) && _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}
std::vector<std::filesystem::path> runtime_servers() {
  wchar_t local[32768]{};
  const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, std::size(local));
  if (!length || length >= std::size(local)) return {};
  const auto registry = std::filesystem::path(local) / L"openvr/openvrpaths.vrpath";
  if (!local_path(registry)) return {};
  std::ifstream input(registry, std::ios::binary);
  std::string raw(16385, '\0');
  input.read(raw.data(), raw.size());
  if (!input.eof() || input.gcount() <= 0 || input.gcount() > 16384) return {};
  raw.resize(std::size_t(input.gcount()));
  const auto json = nlohmann::json::parse(raw);
  if (json.value("jsonid", "") != "vrpathreg" || json.value("version", 0) != 1 ||
      !json.contains("runtime") || !json["runtime"].is_array() || json["runtime"].size() > 8) return {};
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : json["runtime"]) {
    if (!entry.is_string()) return {};
    const auto text = entry.get<std::string>();
    auto runtime = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    runtime = runtime.lexically_normal().make_preferred();
    for (const auto* executable : {L"vrserver.exe", L"vrcompositor.exe"}) {
      const auto path = runtime / L"bin/win64" / executable;
      if (local_path(path)) paths.push_back(path);
    }
  }
  return paths;
}
bool matches_process(HANDLE process, const std::vector<std::filesystem::path>& paths) {
  wchar_t image[32768]{};
  DWORD length = std::size(image);
  if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT || !QueryFullProcessImageNameW(process, 0, image, &length)) return false;
  for (const auto& path : paths) if (same_path(path, image)) return true;
  return false;
}
} // namespace
struct SteamLinkSource::Impl {
  // Odd intent epochs request a lease; even epochs withdraw it. Epoch matching
  // prevents a late read from publishing after opt-out or a new session.
  std::atomic<std::uint64_t> intent{0}, granted_epoch{0}, lease{0};
  std::atomic<bool> stopping{false};
  Handle changed{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  std::thread worker;
  std::uint64_t serial = 0;
  Impl() { if (changed) worker = std::thread([this] { run(); }); }
  ~Impl() {
    stopping.store(true); SetEvent(changed.value);
    if (worker.joinable()) worker.join();
  }
  bool current(std::uint64_t epoch) const { return !stopping.load() && intent.load() == epoch; }
  void revoke() {
    const auto previous = lease.exchange(0, std::memory_order_acq_rel);
    granted_epoch.store(0);
    if (previous && (intent.load() & 1) && !stopping.load())
      log::warn("xr-passthrough", "Steam Link source permission lost; ordinary VR until reconnection");
  }
  void request(bool active) {
    auto previous = intent.load();
    while (bool(previous & 1) != active) {
      if (intent.compare_exchange_weak(previous, previous + 1)) { SetEvent(changed.value); break; }
    }
  }
  std::uint64_t permission() const {
    const auto epoch = intent.load();
    if (!(epoch & 1) || granted_epoch.load() != epoch) return 0;
    const auto value = lease.load(std::memory_order_acquire);
    return intent.load() == epoch ? value : 0;
  }
  // Overlapped I/O retains its buffers until cancellation has completed.
  // Shutdown/opt-out wakes within 100 ms; no pipe wait occurs on an XR thread.
  bool transfer(HANDLE pipe, HANDLE owner, bool writing, void* bytes, DWORD size, std::uint64_t epoch,
                std::uint64_t deadline = 0) {
    auto* data = static_cast<std::uint8_t*>(bytes);
    const auto alive = [&] { return current(epoch) && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT; };
    for (DWORD used = 0; used < size;) {
      Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!event) return false;
      OVERLAPPED operation{}; operation.hEvent = event.value;
      DWORD count = 0;
      const bool immediate = writing ? WriteFile(pipe, data + used, size - used, &count, &operation)
                                     : ReadFile(pipe, data + used, size - used, &count, &operation);
      if (!immediate) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        while (alive() && (!deadline || GetTickCount64() < deadline) &&
               WaitForSingleObject(event.value, 100) == WAIT_TIMEOUT) {}
        if (!alive() || (deadline && GetTickCount64() >= deadline)) {
          revoke(); CancelIoEx(pipe, &operation);
          GetOverlappedResult(pipe, &operation, &count, TRUE);
          return false;
        }
        if (!GetOverlappedResult(pipe, &operation, &count, TRUE)) return false;
      }
      if (!count || !alive()) return false;
      used += count;
      // A healthy idle lease has no expiry. An incomplete record has a bounded
      // assembly time once its first bytes arrive, including after admission.
      if (used < size && !deadline) deadline = GetTickCount64() + 2000;
    }
    return true;
  }
  bool observe(DWORD pid, HANDLE process, const std::vector<std::filesystem::path>& paths,
               std::uint64_t epoch, bool budget) {
    const auto name = L"\\\\.\\pipe\\SFNP.StreamBridge.V1." + std::to_wstring(pid);
    Handle pipe(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    ULONG server = 0;
    if (!pipe || !GetNamedPipeServerProcessId(pipe.value, &server) || server != pid ||
        !matches_process(process, paths)) return false;
    steam_link::Nonce nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), nonce.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 ||
        !steam_link::nonzero(nonce)) return false;
    auto request = steam_link::request(GetCurrentProcessId(), nonce, budget);
    const auto deadline = GetTickCount64() + 2000;
    if (!transfer(pipe.value, process, true, request.data(), request.size(), epoch, deadline)) return false;
    bool observed = false;
    std::optional<steam_link::Permission> previous;
    while (current(epoch)) {
      steam_link::Record record{};
      if (!transfer(pipe.value, process, false, record.data(), record.size(), epoch, observed ? 0 : deadline)) break;
      auto permission = steam_link::reply(record, GetCurrentProcessId(), pid, nonce, budget);
      if (!permission || !matches_process(process, paths)) break;
      observed = true;
      if (!previous || *permission != *previous) {
        revoke();
        if (permission->rgba && current(epoch)) {
          granted_epoch.store(epoch); lease.store(++serial, std::memory_order_release);
          log::info("xr-passthrough", "Steam Link RGBA source accepted (SBN1 v{})", budget ? 2 : 1);
        }
        previous = permission;
      }
    }
    revoke();
    return observed; // EOF after a valid v2 reply must not start a fallback observer.
  }
  void run() noexcept {
    std::uint64_t reported_epoch = 0;
    while (!stopping.load()) {
      const auto epoch = intent.load();
      if (epoch & 1) {
        try {
          if (reported_epoch != epoch) {
            reported_epoch = epoch;
            log::info("xr-passthrough", "Waiting for a verified Steam Link companion; ordinary VR until accepted");
          }
          const auto paths = runtime_servers();
          Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
          PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
          unsigned candidates = 0;
          if (snapshot && Process32FirstW(snapshot.value, &entry)) do {
            if (_wcsicmp(entry.szExeFile, L"vrserver.exe") && _wcsicmp(entry.szExeFile, L"vrcompositor.exe")) continue;
            if (++candidates > 8 || !current(epoch)) break;
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
            if (!process || !matches_process(process.value, paths)) continue;
            const bool observed = observe(entry.th32ProcessID, process.value, paths, epoch, true);
            if (!observed && current(epoch)) observe(entry.th32ProcessID, process.value, paths, epoch, false);
          } while (Process32NextW(snapshot.value, &entry));
        } catch (...) { revoke(); }
      }
      revoke();
      WaitForSingleObject(changed.value, 1000);
    }
    revoke();
  }
};
SteamLinkSource::SteamLinkSource() : impl_(std::make_unique<Impl>()) {}
SteamLinkSource::~SteamLinkSource() = default;
void SteamLinkSource::request(bool active) { impl_->request(active); }
std::uint64_t SteamLinkSource::permission() const { return impl_->permission(); }
} // namespace venture::xr
