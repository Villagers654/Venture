#pragma once
#include "core/syscall.h"
namespace venture {
// Optional libc OS-service dispatcher. Return false to use the core fallback.
using LinuxSyscallHandler = bool (*)(GuestThread&);
void set_linux_syscall_handler(LinuxSyscallHandler handler);
}
