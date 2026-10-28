// Fault-contained x86-64 atomics shared by the Win32 and Linux platform layers.
// Include after defining VENTURE_A0..VENTURE_A3 as the first four integer argument
// registers of the host ABI (AT&T names, e.g. "%rcx").
//
// Every routine is a frameless leaf whose only access to the target object is
// one instruction. x86 faults are precise, so a fault leaves memory and the
// caller's `old` untouched; the platform fault handler moves RIP from anywhere
// in [venture_guarded_begin, venture_guarded_fault) to venture_guarded_fault, which returns
// false through the return address still on top of the stack.
//
//   bool venture_guarded_add<N>(void* p, uint64_t value, uint64_t* old);     lock xadd
//   bool venture_guarded_xchg<N>(void* p, uint64_t value, uint64_t* old);    xchg (implicitly locked)
//   bool venture_guarded_cas<N>(void* p, uint64_t expected, uint64_t desired, uint64_t* old);
//   bool venture_guarded_load32(const void* p, uint32_t* out);
//   bool venture_guarded_read(void* dst, const void* src, size_t n);       one load for n = 1, 2, 4, 8
// LOCK CMPXCHG leaves memory unchanged when the comparison fails (Intel SDM
// Vol. 2A CMPXCHG); RAX then holds the current value, so `old` is correct in
// both cases. r11 is volatile in both the Microsoft x64 and System V ABIs.
#define VENTURE_GUARDED_OP(name, body, widen, out) \
  ".globl " name "\n" name ":\n" body "\n" widen "\n  movq %rax, (" out ")\n  movl $1, %eax\n  ret\n"
#define VENTURE_GUARDED_RMW(name, insn, reg, widen) \
  VENTURE_GUARDED_OP(name, "  movq " VENTURE_A1 ", %rax\n  " insn " " reg ", (" VENTURE_A0 ")", widen, VENTURE_A2)
#define VENTURE_GUARDED_CAS(name, insn, reg, widen) \
  VENTURE_GUARDED_OP(name, "  movq " VENTURE_A2 ", %r11\n  movq " VENTURE_A1 ", %rax\n  lock " insn " " reg ", (" VENTURE_A0 ")", widen, VENTURE_A3)

asm(".text\n.p2align 4\n.globl venture_guarded_begin\nventure_guarded_begin:\n"
    VENTURE_GUARDED_RMW("venture_guarded_add1", "lock xaddb", "%al", "  movzbl %al, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_add2", "lock xaddw", "%ax", "  movzwl %ax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_add4", "lock xaddl", "%eax", "  movl %eax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_add8", "lock xaddq", "%rax", "")
    VENTURE_GUARDED_RMW("venture_guarded_xchg1", "xchgb", "%al", "  movzbl %al, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_xchg2", "xchgw", "%ax", "  movzwl %ax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_xchg4", "xchgl", "%eax", "  movl %eax, %eax")
    VENTURE_GUARDED_RMW("venture_guarded_xchg8", "xchgq", "%rax", "")
    VENTURE_GUARDED_CAS("venture_guarded_cas1", "cmpxchgb", "%r11b", "  movzbl %al, %eax")
    VENTURE_GUARDED_CAS("venture_guarded_cas2", "cmpxchgw", "%r11w", "  movzwl %ax, %eax")
    VENTURE_GUARDED_CAS("venture_guarded_cas4", "cmpxchgl", "%r11d", "  movl %eax, %eax")
    VENTURE_GUARDED_CAS("venture_guarded_cas8", "cmpxchgq", "%r11", "")
    ".globl venture_guarded_load32\nventure_guarded_load32:\n  movl (" VENTURE_A0 "), %eax\n  movl %eax, (" VENTURE_A1 ")\n"
    "  movl $1, %eax\n  ret\n"
    // venture_guarded_read(dst, src, n): eight-byte words, then bytes; never reads
    // outside [src, src + n). Uses only argument registers and rax. A 1, 2, 4
    // or 8-byte read is one load of that width, so a naturally aligned guest
    // access through a JIT callback stays single-copy atomic (ARMv8 B2.2.1).
    ".globl venture_guarded_read\nventure_guarded_read:\n"
    "  cmpq $4, " VENTURE_A2 "\n  je 5f\n  cmpq $2, " VENTURE_A2 "\n  je 6f\n"
    "  cmpq $8, " VENTURE_A2 "\n  jb 2f\n"
    "1:\n  movq (" VENTURE_A1 "), %rax\n  movq %rax, (" VENTURE_A0 ")\n  addq $8, " VENTURE_A1 "\n  addq $8, " VENTURE_A0 "\n"
    "  subq $8, " VENTURE_A2 "\n  cmpq $8, " VENTURE_A2 "\n  jae 1b\n"
    "2:\n  testq " VENTURE_A2 ", " VENTURE_A2 "\n  jz 4f\n"
    "3:\n  movb (" VENTURE_A1 "), %al\n  movb %al, (" VENTURE_A0 ")\n  incq " VENTURE_A1 "\n  incq " VENTURE_A0 "\n"
    "  decq " VENTURE_A2 "\n  jnz 3b\n"
    "4:\n  movl $1, %eax\n  ret\n"
    "5:\n  movl (" VENTURE_A1 "), %eax\n  movl %eax, (" VENTURE_A0 ")\n  jmp 4b\n"
    "6:\n  movw (" VENTURE_A1 "), %ax\n  movw %ax, (" VENTURE_A0 ")\n  jmp 4b\n"
    ".globl venture_guarded_fault\nventure_guarded_fault:\n  xorl %eax, %eax\n  ret\n");
#undef VENTURE_GUARDED_CAS
#undef VENTURE_GUARDED_RMW
#undef VENTURE_GUARDED_OP

extern "C" {
extern const char venture_guarded_begin[], venture_guarded_fault[];
bool venture_guarded_add1(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_add2(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_add4(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_add8(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg1(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg2(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg4(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_xchg8(void*, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas1(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas2(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas4(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_cas8(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
bool venture_guarded_load32(const void*, std::uint32_t*);
bool venture_guarded_read(void*, const void*, std::size_t);
}

namespace venture::platform {
namespace {
bool guarded_ip(std::uintptr_t ip) {
  return ip >= reinterpret_cast<std::uintptr_t>(venture_guarded_begin) && ip < reinterpret_cast<std::uintptr_t>(venture_guarded_fault);
}
bool guarded_access_ready();  // the platform fault handler is installed
}
bool atomic_rmw(GuestAddr address, unsigned size, AtomicRmw op, std::uint64_t value,
                std::uint64_t desired, std::uint64_t& old) {
  if ((size != 1 && size != 2 && size != 4 && size != 8) || (address & (size - 1)) || !guarded_access_ready())
    return false;
  using Rmw = bool (*)(void*, std::uint64_t, std::uint64_t*);
  using Cas = bool (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t*);
  static constexpr Rmw add[]{venture_guarded_add1, venture_guarded_add2, venture_guarded_add4, venture_guarded_add8};
  static constexpr Rmw exchange[]{venture_guarded_xchg1, venture_guarded_xchg2, venture_guarded_xchg4, venture_guarded_xchg8};
  static constexpr Cas cas[]{venture_guarded_cas1, venture_guarded_cas2, venture_guarded_cas4, venture_guarded_cas8};
  const auto index = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
  const auto mask = size == 8 ? ~0ull : (1ull << (size * 8)) - 1;
  auto* target = g2h(address);
  switch (op) {
    case AtomicRmw::add: return add[index](target, value & mask, &old);
    case AtomicRmw::exchange: return exchange[index](target, value & mask, &old);
    case AtomicRmw::compare_exchange: return cas[index](target, value & mask, desired & mask, &old);
  }
  return false;
}
bool guarded_load32(GuestAddr address, std::uint32_t& value) {
  return !(address & 3) && guarded_access_ready() && venture_guarded_load32(g2h(address), &value);
}
bool guarded_store(GuestAddr address, const void* data, std::size_t size) {
  return address + size >= address && guarded_access_ready() && venture_guarded_read(g2h(address), data, size);
}
namespace {
// A copy that reports a fault on any source byte as false, like the process
// memory system calls, without entering the kernel when it succeeds.
[[maybe_unused]] bool guarded_read(GuestAddr address, void* out, std::size_t size) {
  return address + size >= address && guarded_access_ready() && venture_guarded_read(out, g2h(address), size);
}
}
}  // namespace venture::platform
