// Linux system calls issued by guest code with `svc #0` (number in X8,
// arguments in X0..X5, result or -errno in X0). The libc HLE implements the
// same operations as functions; this dispatcher maps raw syscalls onto them.
#pragma once

namespace venture {
class GuestThread;

// Executes the syscall described by the thread's registers and writes X0.
void linux_syscall(GuestThread& thread);
// ARM EABI raw syscall entry. Layout-dependent services are added separately.
void linux_syscall32(GuestThread& thread);

}  // namespace venture
