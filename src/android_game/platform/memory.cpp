// SPDX-License-Identifier: GPL-3.0-or-later
// platform::read_memory for the Platform loader inside a game: the game's
// arguments are ordinary addresses of its own process, read without faulting
// (process_vm_readv), as the PC runtime's POSIX fallback reads guest memory.
#include <sys/uio.h>
#include <unistd.h>

#include "platform/memory.h"

namespace venture::platform {
bool read_memory(GuestAddr address, void* out, std::size_t size) {
  if (address + size < address) return false;
  iovec local{out, size}, remote{g2h(address), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}
}  // namespace venture::platform
