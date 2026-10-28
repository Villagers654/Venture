// Included only by platform memory implementations. No C++ owning objects may
// be introduced here: POSIX fault recovery returns across this leaf operation.
namespace {
struct NativeMemoryCall {
  NativeMemoryOp op;
  GuestAddr destination, source;
  std::size_t size;
  int value;
  std::uint64_t result{};
  NativeMemoryFault fault{};
  bool source_guest = true;
};
bool native_fault_matches(const NativeMemoryCall& call, GuestAddr address, bool write) {
  const auto within = [&](GuestAddr start) {
    // Some CRT read routines align their first probe down within the first
    // page. Such a fault still belongs to this declared guest source buffer.
    return call.size && ((address >= start && address - start < call.size) ||
        (!write && address < start && address >= align_down(start, kGuestPageSize)));
  };
  bool matches = false;
  switch (call.op) {
    case NativeMemoryOp::copy: case NativeMemoryOp::move:
      matches = write ? within(call.destination) : call.source_guest && within(call.source); break;
    case NativeMemoryOp::fill: matches = write && within(call.destination); break;
    case NativeMemoryOp::compare:
      matches = !write && (within(call.destination) || (call.source_guest && within(call.source))); break;
    case NativeMemoryOp::find: matches = !write && call.source_guest && within(call.source); break;
  }
  return matches;
}
bool native_ranges_valid(NativeMemoryCall& call) {
  // A noncanonical x86 operand can report fault address zero. Validate only
  // these unusual ranges before entering the CRT; ordinary user pointers take
  // no OS query, and valid wider Linux mappings remain supported.
  const auto check = [&](GuestAddr address, bool write) {
    if (!call.size) return true;
    constexpr GuestAddr usual_user_limit = 1ull << 47;
    if (call.size > UINT64_MAX - address ||
        ((address >= usual_user_limit || call.size > usual_user_limit - address) &&
         !memory_accessible(address, call.size, write))) {
      call.fault = {address, write}; return false;
    }
    return true;
  };
  if (call.op != NativeMemoryOp::find && !check(call.destination, call.op != NativeMemoryOp::compare)) return false;
  if (call.op != NativeMemoryOp::fill && call.source_guest && !check(call.source, false)) return false;
  return true;
}
void invoke_native_memory(NativeMemoryCall& call) {
  if (!call.size) {
    call.result = call.op == NativeMemoryOp::compare || call.op == NativeMemoryOp::find ? 0 : call.destination;
    return;
  }
  auto* d = g2h<void>(call.destination);
  const auto* s = g2h<const void>(call.source);
  switch (call.op) {
    case NativeMemoryOp::copy: call.result = h2g(std::memcpy(d, s, call.size)); break;
    case NativeMemoryOp::move: call.result = h2g(std::memmove(d, s, call.size)); break;
    case NativeMemoryOp::fill: call.result = h2g(std::memset(d, call.value, call.size)); break;
    case NativeMemoryOp::compare:
      call.result = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::memcmp(d, s, call.size))); break;
    case NativeMemoryOp::find: call.result = h2g(std::memchr(s, call.value, call.size)); break;
  }
}
}
