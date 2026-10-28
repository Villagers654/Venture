// Integration hooks implemented by libc Part A.
#pragma once
namespace venture {
class GuestThread;
// Part B calls this on the exiting guest thread, before pthread key destructors
// and before detaching its CPU/TLS. Guest calls may register more destructors.
void run_guest_thread_destructors(GuestThread& thread);
// Normal exit() calls this before tearing down libraries. _exit()/abort() do not.
void run_guest_exit_handlers(GuestThread& thread);
}  // namespace venture
