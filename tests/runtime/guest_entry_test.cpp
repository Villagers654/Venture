// Guest stubs come from the process's guest entry: the SVC trap entry of the
// Dynarmic engines unless an engine installs its own before the first stub.
#include "cpu/guest_entry.h"

#include <doctest/doctest.h>

#include <stdexcept>
#include <string_view>

using namespace venture;

namespace {
constexpr GuestStub kKinds[] = {GuestStub::hle_call, GuestStub::return_to_host, GuestStub::call_return,
                                GuestStub::sigreturn, GuestStub::tlsdesc, GuestStub::tlsdesc_weak};

// What hle::resolve_thunk() and hle::function_at() rely on.
void check_hle_call_contract(const GuestEntry& entry) {
  constexpr std::uint32_t id = 0x00345678;
  const auto a64 = entry.stub(GuestIsa::a64, GuestStub::hle_call, id, kSvcHleCallFast);
  REQUIRE(a64.count >= 2);
  CHECK(a64.count <= 8);
  CHECK((a64.words[0] & 0xffe0001fu) == 0xd2800010u);  // MOVZ X16, #lo
  CHECK((a64.words[1] & 0xffe0001fu) == 0xf2a00010u);  // MOVK X16, #hi, LSL #16
  CHECK(((a64.words[0] >> 5) & 0xffffu) == (id & 0xffffu));
  CHECK(((a64.words[1] >> 5) & 0xffffu) == (id >> 16));
  const auto a32 = entry.stub(GuestIsa::a32, GuestStub::hle_call, id, kSvcHleCallFast);
  REQUIRE(a32.count >= 2);
  CHECK(a32.count <= 7);
  const auto immediate = [](std::uint32_t word) { return ((word >> 4) & 0xf000u) | (word & 0xfffu); };
  CHECK((a32.words[0] & 0xfff0f000u) == 0xe300c000u);  // MOVW R12, #lo
  CHECK((a32.words[1] & 0xfff0f000u) == 0xe340c000u);  // MOVT R12, #hi
  CHECK(immediate(a32.words[0]) == (id & 0xffffu));
  CHECK(immediate(a32.words[1]) == (id >> 16));
}

class MarkedEntry final : public GuestEntry {
 public:
  const char* name() const noexcept override { return "marked"; }
  StubCode stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) const override {
    auto code = guest_stub(isa, kind, argument, svc);
    if (kind == GuestStub::hle_call && isa == GuestIsa::a64) code.words[2] = 0xd503221fu;  // a marker in place of a NOP
    return code;
  }
};
}  // namespace

TEST_CASE("the trap entry writes exactly the existing SVC stubs") {
  const auto& trap = trap_entry();
  CHECK(std::string_view(trap.name()) == "trap");
  for (const auto kind : kKinds) {
    for (const std::uint32_t svc : {kSvcHleCall, kSvcHleCallFast, kSvcHleCallFastFp, kSvcHleCallFastGuestFp}) {
      const auto expected = guest_stub(GuestIsa::a64, kind, 7, svc);
      const auto actual = trap.stub(GuestIsa::a64, kind, 7, svc);
      CHECK(actual.count == expected.count);
      CHECK(actual.words == expected.words);
    }
    if (kind == GuestStub::tlsdesc || kind == GuestStub::tlsdesc_weak) continue;
    const auto expected = guest_stub(GuestIsa::a32, kind, 3);
    const auto actual = trap.stub(GuestIsa::a32, kind, 3, kSvcHleCall);
    CHECK(actual.count == expected.count);
    CHECK(actual.words == expected.words);
  }
  check_hle_call_contract(trap);
}

TEST_CASE("an engine's entry is installed before the first stub and is fixed after it") {
  static const MarkedEntry marked;
  check_hle_call_contract(marked);
  GuestEntrySlot slot;
  CHECK(&slot.entry() == &trap_entry());
  slot.install(marked);
  CHECK(&slot.entry() == &marked);
  slot.install(trap_entry());  // still unused: an engine may change its mind
  CHECK(&slot.entry() == &trap_entry());
  slot.install(marked);

  const auto code = slot.stub(GuestIsa::a64, GuestStub::hle_call, 9, kSvcHleCallFast);
  CHECK(code.words[2] == 0xd503221fu);
  CHECK(slot.stub(GuestIsa::a64, GuestStub::return_to_host, 0, kSvcHleCall).words ==
        guest_stub(GuestIsa::a64, GuestStub::return_to_host).words);
  // Stubs in guest memory now use the marked entry; another one would mix them.
  CHECK_THROWS_AS(slot.install(trap_entry()), std::logic_error);
  CHECK_NOTHROW(slot.install(marked));
  CHECK(&slot.entry() == &marked);

  GuestEntrySlot used_trap;
  (void)used_trap.stub(GuestIsa::a32, GuestStub::hle_call, 1, kSvcHleCall);
  CHECK_THROWS_AS(used_trap.install(marked), std::logic_error);
}

TEST_CASE("the process writes trap stubs: no engine on this host installs another entry") {
  CHECK(&guest_entry() == &trap_entry());
  const auto code = guest_entry_stub(GuestIsa::a64, GuestStub::hle_call, 5, kSvcHleCallFastFp);
  CHECK(code.words == guest_stub(GuestIsa::a64, GuestStub::hle_call, 5, kSvcHleCallFastFp).words);
  CHECK_NOTHROW(set_guest_entry(trap_entry()));
}
