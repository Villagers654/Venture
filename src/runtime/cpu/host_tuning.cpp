#include "cpu/host_tuning.h"
#include "common/environment.h"
#include "common/log.h"
#include <string_view>
#if defined(__x86_64__) || defined(_M_X64)
#include <cpuid.h>
#endif

namespace venture::host_tuning {
namespace {
struct HostCpu {
  bool amd = false;
  unsigned family = 0;  // CPUID display family (base plus extended)
  bool avx512f = false;
};
HostCpu identify() noexcept {
  HostCpu cpu;
#if defined(__x86_64__) || defined(_M_X64)
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(0, &a, &b, &c, &d)) return cpu;
  const unsigned max_leaf = a;
  // "AuthenticAMD" is spread over EBX, EDX and ECX.
  cpu.amd = b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return cpu;
  const unsigned base_family = (a >> 8) & 0xf;
  cpu.family = base_family == 0xf ? base_family + ((a >> 20) & 0xff) : base_family;
  if (max_leaf >= 7 && __get_cpuid_count(7, 0, &a, &b, &c, &d)) cpu.avx512f = (b >> 16) & 1;
#endif
  return cpu;
}
// Per-site return pops: Zen 4 Linux, 2000x2040, North Star 6.53 -> 6.29 ms and
// Batman 5.19 -> 5.04 ms. Lion Cove Windows lost (North Star 286.9 -> 281 fps,
// Batman 342.6 -> 325.8, Cooking 555.6 -> 544.4), so Intel and older AMD keep
// the shared lookup. Family 19h is Zen 3 and Zen 4; only Zen 4 has AVX-512.
// Family 1Ah and later (Zen 5 onward) inherit Zen 4's choice.
bool per_site_measured_faster(const HostCpu& cpu) noexcept {
  if (!cpu.amd) return false;
  return cpu.family >= 0x1a || (cpu.family == 0x19 && cpu.avx512f);
}
ReturnPop choose() noexcept {
  if (const auto* value = environment::getenv("VENTURE_JIT_INLINE_RSB_POP")) {
    const std::string_view text{value};
    return text == "1" ? ReturnPop::per_site : text == "compact" ? ReturnPop::compact : ReturnPop::shared;
  }
  const auto cpu = identify();
  const auto choice = per_site_measured_faster(cpu) ? ReturnPop::per_site : ReturnPop::shared;
  try {
    log::info("jit", "Return lookups {} for this CPU ({} family {:#x}); VENTURE_JIT_INLINE_RSB_POP overrides",
              choice == ReturnPop::per_site ? "per site" : "shared", cpu.amd ? "AMD" : "non-AMD", cpu.family);
  } catch (...) {}
  return choice;
}
}  // namespace
ReturnPop return_pop() noexcept {
  static const ReturnPop choice = choose();
  return choice;
}
}  // namespace venture::host_tuning
