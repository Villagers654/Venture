#include "core/a32_abi.h"
#include "core/thread.h"
#include <algorithm>

namespace venture {
GuestCallResult GuestThread::call32(GuestAddr fn, const hle::a32::CallBuilder32& args) {
  if (current() != this || !cpu_ || cpu_->isa() != GuestIsa::a32)
    throw std::logic_error("call32 requires the current A32 thread");
  // Placement is decided by the typed builder. The CPU packet's word entry
  // owns stack bounds, ARM interworking, CpuState32 and nested-call unwinding.
  auto registers = args.registers(); auto stack = args.stack();
  std::array<std::uint64_t, 4> regs{};
  std::copy(registers.begin(), registers.end(), regs.begin());
  std::vector<std::uint64_t> spill(stack.begin(), stack.end());
  return call32(fn, GuestCallArgs{std::span(regs).first(registers.size()), {}, spill});
}
}
