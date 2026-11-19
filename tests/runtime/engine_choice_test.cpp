// The guest code engine follows what the host CPU and kernel offer.
#include "cpu/engine_choice.h"

#include <doctest/doctest.h>

using namespace venture;

TEST_CASE("x64 hosts translate every guest; AArch64 hosts run A64 directly where the kernel allows") {
  CHECK(choose_engine(GuestIsa::a64, {.arch = HostArch::x64}).engine == CpuEngine::dynarmic_x64);
  CHECK(choose_engine(GuestIsa::a32, {.arch = HostArch::x64}).engine == CpuEngine::dynarmic_x64);

  const HostCpuFacts arm64{.arch = HostArch::arm64, .page_size = 4096, .guest_syscall_trap = true};
  CHECK(choose_engine(GuestIsa::a64, arm64).engine == CpuEngine::native_a64);
  // A32 never runs directly in an AArch64 process.
  CHECK(choose_engine(GuestIsa::a32, arm64).engine == CpuEngine::dynarmic_arm64);

  auto large_pages = arm64;
  large_pages.page_size = 16384;
  const auto paged = choose_engine(GuestIsa::a64, large_pages);
  CHECK(paged.engine == CpuEngine::dynarmic_arm64);
  CHECK(paged.reason.find("16384") != std::string::npos);

  auto no_trap = arm64;
  no_trap.guest_syscall_trap = false;
  CHECK(choose_engine(GuestIsa::a64, no_trap).engine == CpuEngine::dynarmic_arm64);

  CHECK(choose_engine(GuestIsa::a64, {.arch = HostArch::other}).engine == CpuEngine::none);
  CHECK(std::string(engine_name(CpuEngine::native_a64)) == "native-a64");
  // This build's own architecture picks the engine it carries.
  if constexpr (build_arch() == HostArch::x64)
    CHECK(choose_engine(GuestIsa::a64, {.arch = build_arch()}).engine == CpuEngine::dynarmic_x64);
}
