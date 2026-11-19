#include "cpu/guest_entry.h"

#include <stdexcept>
#include <string>

namespace venture {
namespace {
class TrapEntry final : public GuestEntry {
 public:
  const char* name() const noexcept override { return "trap"; }
  StubCode stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) const override {
    return guest_stub(isa, kind, argument, svc);
  }
};
const TrapEntry trap;
GuestEntrySlot process_slot;
}  // namespace

const GuestEntry& trap_entry() noexcept { return trap; }

const GuestEntry& GuestEntrySlot::entry() const noexcept {
  const auto* entry = installed_.load(std::memory_order_acquire);
  return entry ? *entry : trap;
}
void GuestEntrySlot::install(const GuestEntry& entry) {
  if (&entry == &this->entry()) return;
  if (used_.load(std::memory_order_acquire))
    throw std::logic_error(std::string("guest stubs already use the ") + this->entry().name() + " entry");
  installed_.store(&entry == &trap ? nullptr : &entry, std::memory_order_release);
}
StubCode GuestEntrySlot::stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) {
  if (!used_.load(std::memory_order_relaxed)) used_.store(true, std::memory_order_release);
  const auto* entry = installed_.load(std::memory_order_acquire);
  // Dynarmic engines (every x64 host) keep the trap entry: no indirect call.
  return entry ? entry->stub(isa, kind, argument, svc) : guest_stub(isa, kind, argument, svc);
}

const GuestEntry& guest_entry() noexcept { return process_slot.entry(); }
void set_guest_entry(const GuestEntry& entry) { process_slot.install(entry); }
StubCode guest_entry_stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) {
  return process_slot.stub(isa, kind, argument, svc);
}
}  // namespace venture
