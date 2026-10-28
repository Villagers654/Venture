#include "core/syscall_runtime.h"
#include "core/thread_runtime.h"
#include "core/address_space.h"
#include "core/error.h"
#include "core/signal_frame.h"
#include "core/signals.h"
#include "core/wait.h"
#include "platform/memory.h"
#include "common/log.h"
#include <atomic>
#include <chrono>

namespace venture {
namespace {
std::atomic<LinuxSyscallHandler> extension{};
struct Timespec { std::int64_t sec, nsec; };
}
void set_linux_syscall_handler(LinuxSyscallHandler handler) { extension.store(handler); }
void linux_syscall32(GuestThread& thread) {
  if (auto fn = extension.load(); fn && fn(thread)) return;
  auto& c = thread.cpu();
  // Linux arch/arm/tools/syscall.tbl and uapi/asm/unistd.h. Do not interpret
  // ARM numbers or ILP32 structs through the A64 syscall table.
  switch (c.syscall_number()) {
    case 1: exit_guest_thread(c.x(0));
    case 248: exit_guest_process(static_cast<int>(c.x(0)));
    case 20: c.set_x(0, 1); return;
    case 224: c.set_x(0, thread.tid()); return;
    case 119: signals::return_from_signal(thread, false); return;
    case 173: signals::return_from_signal(thread); return;
    case 0xf0005: c.set_tpidrro(c.x(0)); c.set_x(0, 0); return; // __ARM_NR_set_tls
    case 0xf0002: { // __ARM_NR_cacheflush(start, end, flags)
      if (c.x(2) || c.x(1) < c.x(0)) { c.set_x(0, -err::kEINVAL); return; }
      cpu_backend().invalidate(c.x(0), c.x(1) - c.x(0), InvalidationCause::cache_flush); c.set_x(0, 0); return;
    }
    default:
      log::warn("syscall", "unimplemented ARM EABI syscall {} at {:#x}", c.syscall_number(), c.pc());
      c.set_x(0, -err::kENOSYS);
  }
}
void linux_syscall(GuestThread& thread) {
  if (auto fn = extension.load(); fn && fn(thread)) return;
  auto& c = thread.cpu();
  const auto number = c.syscall_number();
  std::int64_t result = -err::kENOSYS;
  switch (number) {
    case 93: exit_guest_thread(c.x(0));
    case 94: exit_guest_process(static_cast<int>(c.x(0)));
    case 96: thread.clear_child_tid = c.x(0); result = thread.tid(); break;
    case 98: {
      auto operation = static_cast<unsigned>(c.x(1)) & ~0x180u;
      if (operation == 1 || operation == 10) result = futex_wake(c.x(0), static_cast<int>(c.x(2)), operation == 10 ? static_cast<std::uint32_t>(c.x(5)) : ~0u);
      else if (operation == 0 || operation == 9) {
        std::int64_t deadline = -1;
        if (c.x(3)) {
          Timespec ts;
          if (!platform::read_memory(c.x(3), &ts, sizeof(ts))) { result = -err::kEFAULT; break; }
          if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1'000'000'000 || ts.sec > 9'000'000'000) { result = -err::kEINVAL; break; }
          deadline = ts.sec * 1'000'000'000 + ts.nsec;
          if (operation == 0) deadline += monotonic_ns();
          else if (c.x(1) & 0x100) {
            auto realtime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            deadline += monotonic_ns() - realtime;
          }
        }
        do { result = futex_wait(thread, c.x(0), static_cast<std::uint32_t>(c.x(2)), deadline, operation == 9 ? static_cast<std::uint32_t>(c.x(5)) : ~0u); }
        while (result == -err::kEINTR && signals::restart_last_handler());
      }
      break;
    }
    case 101: {
      Timespec ts;
      if (!platform::read_memory(c.x(0), &ts, sizeof(ts))) { result = -err::kEFAULT; break; }
      if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1'000'000'000 || ts.sec > 9'000'000'000) { result = -err::kEINVAL; break; }
      auto deadline = monotonic_ns() + ts.sec * 1'000'000'000 + ts.nsec;
      result = sleep_until(thread, deadline);
      if (result == -err::kEINTR && c.x(1)) {
        auto left = std::max<std::int64_t>(0, deadline - monotonic_ns());
        ts = {left / 1'000'000'000, left % 1'000'000'000};
        if (!platform::write_memory(c.x(1), &ts, sizeof(ts))) result = -err::kEFAULT;
      }
      break;
    }
    case 113: {
      std::int64_t ns;
      if (c.x(0) == 0) ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      else if (c.x(0) == 1 || c.x(0) == 4 || c.x(0) == 6 || c.x(0) == 7) ns = monotonic_ns();
      else { result = -err::kEINVAL; break; }
      Timespec ts{ns / 1'000'000'000, ns % 1'000'000'000};
      result = platform::write_memory(c.x(1), &ts, sizeof(ts)) ? 0 : -err::kEFAULT; break;
    }
    case 129: result = signals::send_to_process({static_cast<int>(c.x(1)), 0, 0, 1, 0}); break;
    case 130: result = signals::send_to_thread(static_cast<std::int32_t>(c.x(0)), {static_cast<int>(c.x(1)), -6, 0, 1, 0}); break;
    case 131: result = signals::send_to_thread(static_cast<std::int32_t>(c.x(1)), {static_cast<int>(c.x(2)), -6, 0, 1, 0}); break;
    case 132: {
      signals::Stack input{}, previous{};
      signals::AltStack replacement{}, old;
      if (c.x(0)) {
        if (!platform::read_memory(c.x(0), &input, sizeof(input))) { result = -err::kEFAULT; break; }
        replacement = {input.sp, input.size, input.flags};
      }
      result = signals::set_alt_stack(thread, c.x(0) ? &replacement : nullptr, &old);
      previous = {old.base, old.flags, 0, old.size};
      if (!result && c.x(1) && !platform::write_memory(c.x(1), &previous, sizeof(previous))) result = -err::kEFAULT;
      break;
    }
    case 134: {
      // Linux arm64 __kernel_sigaction: handler, flags, restorer, 64-bit kernel sigset.
      struct KernelAction { std::uint64_t handler, flags, restorer, mask; } input{}, output{};
      if (c.x(3) != 8) { result = -err::kEINVAL; break; }
      signals::Action action, old;
      if (c.x(1)) {
        if (!platform::read_memory(c.x(1), &input, sizeof(input))) { result = -err::kEFAULT; break; }
        action = {input.handler, input.mask, static_cast<std::uint32_t>(input.flags)};
      }
      result = signals::set_action(static_cast<int>(c.x(0)), c.x(1) ? &action : nullptr, &old);
      output = {old.handler, old.flags, 0, old.mask};
      if (!result && c.x(2) && !platform::write_memory(c.x(2), &output, sizeof(output))) result = -err::kEFAULT;
      break;
    }
    case 135: {
      if (c.x(3) != 8) { result = -err::kEINVAL; break; }
      std::uint64_t input{}, old{};
      if (c.x(1) && !platform::read_memory(c.x(1), &input, 8)) { result = -err::kEFAULT; break; }
      result = signals::set_mask(thread, static_cast<int>(c.x(0)), c.x(1) ? &input : nullptr, &old);
      if (!result && c.x(2) && !platform::write_memory(c.x(2), &old, 8)) result = -err::kEFAULT;
      break;
    }
    case 139: signals::return_from_signal(thread); return;
    case 172: result = 1; break;
    case 178: result = thread.tid(); break;
    case 215: result = AddressSpace::get().unmap(c.x(0), c.x(1)); break;
    case 222: result = AddressSpace::get().map(c.x(0), c.x(1), static_cast<std::uint32_t>(c.x(2)), static_cast<std::uint32_t>(c.x(3)), nullptr, c.x(5), "[syscall mmap]"); break;
    case 226: result = AddressSpace::get().protect(c.x(0), c.x(1), static_cast<std::uint32_t>(c.x(2))); break;
    case 233: result = AddressSpace::get().advise(c.x(0), c.x(1), static_cast<int>(c.x(2))); break;
    default: break;
  }
  if (result == -err::kENOSYS) log::warn("syscall", "unimplemented Linux syscall {} operation {:#x} at {:#x}", number, c.x(1), c.pc() - c.trap_instruction_size());
  c.set_x(0, static_cast<std::uint64_t>(result));
}
}
