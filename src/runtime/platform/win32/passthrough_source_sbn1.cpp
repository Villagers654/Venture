// TEMPORARY COMPATIBILITY PATH: see xr/passthrough_source_sbn1.h. The SBN1
// pipe carries its server's process ID in its name; the pipe namespace is
// listed (which opens no instance) and each same-user server is tried.
#include "xr/passthrough_source_sbn1.h"
#include "platform/win32/passthrough_pipe.h"
#include <windows.h>
#include <string>
#include <string_view>

namespace venture::xr::passthrough_source_sbn1::platform {
std::unique_ptr<passthrough_source_platform::Channel> connect(std::uint32_t& server) {
  constexpr std::wstring_view prefix = L"SFNP.StreamBridge.V1.";
  WIN32_FIND_DATAW entry{};
  const HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\*", &entry);
  if (find == INVALID_HANDLE_VALUE) return nullptr;
  std::unique_ptr<passthrough_source_platform::Channel> channel;
  unsigned tried = 0;
  do {
    const std::wstring_view name(entry.cFileName);
    if (!name.starts_with(prefix) || name.size() == prefix.size() || name.size() - prefix.size() > 10) continue;
    std::uint64_t id = 0;
    bool digits = true;
    for (const auto c : name.substr(prefix.size())) {
      if (c < L'0' || c > L'9') { digits = false; break; }
      id = id * 10 + std::uint64_t(c - L'0');
    }
    if (!digits || !id || id > 0xffffffffu || ++tried > 8) continue;
    const auto path = L"\\\\.\\pipe\\" + std::wstring(name);
    std::uint32_t answered = 0;
    channel = passthrough_source_platform::open_pipe(path.c_str(), answered);
    // The name states the server; another process holding it is not that server.
    if (channel && answered != id) channel.reset();
    if (channel) server = answered;
  } while (!channel && FindNextFileW(find, &entry));
  FindClose(find);
  return channel;
}
} // namespace venture::xr::passthrough_source_sbn1::platform
