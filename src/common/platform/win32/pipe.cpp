#include "common/platform/pipe.h"
#include <stdexcept>
#include <windows.h>

namespace venture::platform {
std::array<std::uintptr_t, 4> make_pipes() {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE parent_read = nullptr, parent_write = nullptr, child_read = nullptr,
         child_write = nullptr;
  if (!CreatePipe(&parent_read, &child_write, &sa, 0) ||
      !CreatePipe(&child_read, &parent_write, &sa, 0) ||
      !SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0)) {
    for (auto handle : {parent_read, parent_write, child_read, child_write})
      if (handle)
        CloseHandle(handle);
    throw std::runtime_error("Could not create credential broker pipes.");
  }
  return {reinterpret_cast<std::uintptr_t>(parent_read),
          reinterpret_cast<std::uintptr_t>(parent_write),
          reinterpret_cast<std::uintptr_t>(child_read),
          reinterpret_cast<std::uintptr_t>(child_write)};
}
bool valid_pipe(std::uintptr_t h) {
  return h && GetFileType(reinterpret_cast<HANDLE>(h)) == FILE_TYPE_PIPE;
}
void close_pipe(std::uintptr_t h) {
  if (h)
    CloseHandle(reinterpret_cast<HANDLE>(h));
}
void read_pipe(std::uintptr_t h, void *out, std::size_t size) {
  auto *data = static_cast<unsigned char *>(out);
  while (size) {
    DWORD n = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(h), data, static_cast<DWORD>(size),
                  &n, nullptr) ||
        !n)
      throw std::runtime_error("Credential broker pipe closed.");
    data += n;
    size -= n;
  }
}
void write_pipe(std::uintptr_t h, const void *in, std::size_t size) {
  auto *data = static_cast<const unsigned char *>(in);
  while (size) {
    DWORD n = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(h), data, static_cast<DWORD>(size),
                   &n, nullptr) ||
        !n)
      throw std::runtime_error("Credential broker pipe closed.");
    data += n;
    size -= n;
  }
}
} // namespace venture::platform
