// A controlled SBN1 server (the temporary compatibility transport): serves
// \\.\pipe\SFNP.StreamBridge.V1.<pid> as this user, one thread per connection
// with the next instance always listening, answering in one test mode.
#pragma once
#include "xr/passthrough_source_sbn1.h"
#include <windows.h>
#include <cstring>
#include <string>
#include <thread>

bool read_all(HANDLE pipe, void* bytes, DWORD length);
bool write_all(HANDLE pipe, const void* bytes, DWORD length);

namespace sbn1_fixture {
namespace wire = venture::xr::passthrough_source_sbn1;
inline void serve(HANDLE pipe, const std::string& mode, unsigned connection, HANDLE connected, HANDLE resume) {
  wire::Record request{};
  if (!read_all(pipe, request.data(), DWORD(request.size())) || std::memcmp(request.data(), "SBN1", 4)) {
    DisconnectNamedPipe(pipe); CloseHandle(pipe); return;
  }
  SetEvent(connected);
  const auto version = unsigned(wire::read(request, 6, 2));
  // A version 1 server closes on a version 2 request.
  if (mode == "sbn1-v1" && version != 1) { DisconnectNamedPipe(pipe); CloseHandle(pipe); return; }
  if (mode == "sbn1-reconnect" && connection > 1) WaitForSingleObject(resume, 5000);
  const bool v2 = version == 2;
  const auto send = [&](bool rgba, std::uint64_t generation) {
    wire::Record reply{};
    std::memcpy(reply.data(), "SBN1", 4);
    wire::write(reply, 4, 2, 2); wire::write(reply, 6, 2, version);
    wire::write(reply, 8, 2, reply.size()); wire::write(reply, 12, 4, 1);
    wire::write(reply, 10, 2, rgba ? (v2 ? 5 : 1) : 0);
    wire::write(reply, 16, 4, wire::read(request, 16, 4) + (mode == "sbn1-wrong-pid" ? 1 : 0));
    wire::write(reply, 20, 4, GetCurrentProcessId() + (mode == "sbn1-wrong-server" ? 1 : 0));
    std::memcpy(reply.data() + 24, request.data() + 24, 16);
    if (mode == "sbn1-wrong-nonce") reply[27] ^= 1;
    if (rgba) { reply[40] = 5; wire::write(reply, 56, 8, generation); }
    if (mode == "sbn1-malformed") reply[100] = 1;
    if (mode == "sbn1-partial" || mode == "sbn1-partial-hang") {
      write_all(pipe, reply.data(), 17);
      if (mode == "sbn1-partial-hang") return;
      Sleep(30);
      write_all(pipe, reply.data() + 17, DWORD(reply.size() - 17));
    } else {
      write_all(pipe, reply.data(), DWORD(reply.size()));
    }
  };
  if (mode == "sbn1-delayed") Sleep(200);
  if (mode != "sbn1-silent") send(mode != "sbn1-denied", 17);
  if (mode == "sbn1-toggle") { Sleep(300); send(false, 0); Sleep(300); send(true, 18); }
  if (mode == "sbn1-reconnect" && connection == 1) Sleep(400);
  else { unsigned char stop{}; DWORD count{}; ReadFile(pipe, &stop, 1, &count, nullptr); }
  DisconnectNamedPipe(pipe); CloseHandle(pipe);
}
inline int run(const std::string& mode, HANDLE ready, HANDLE connected, HANDLE resume) {
  // "sbn1-misnamed" names the pipe after another process.
  const auto pid = GetCurrentProcessId() + (mode == "sbn1-misnamed" ? 1 : 0);
  const auto name = L"\\\\.\\pipe\\SFNP.StreamBridge.V1." + std::to_wstring(pid);
  for (unsigned connections = 0;;) {
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 4;
    SetEvent(ready);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(pipe); continue; }
    std::thread([=] { serve(pipe, mode, connections + 1, connected, resume); }).detach();
    ++connections;
  }
}
} // namespace sbn1_fixture
