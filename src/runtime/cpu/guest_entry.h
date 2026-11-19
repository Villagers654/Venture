#pragma once
// How guest code enters the host: the stubs Venture writes into guest memory
// for HLE calls, returns to the host, signal returns and TLS descriptors.
//
// Every Dynarmic engine uses the trap entry: a stub ends in an SVC that the
// engine reports as CpuExit::Svc (or dispatches in the JIT for the fast HLE
// SVCs). An engine that runs A64 guest code directly on an AArch64 host
// (CpuEngine::native_a64, docs/design/native-headsets.md) supplies its own
// entry, whose stubs branch to a host entry routine instead; it installs it
// with set_guest_entry() before the first stub is written.
//
// Contract for every entry: an hle_call stub starts with the two instructions
// that load the call id into X16 (A64) or R12 (A32), MOVZ/MOVK or MOVW/MOVT,
// is at most 8 words (32 bytes: the native entry's slot is 6 instructions and
// a 64-bit literal), and is the same for the same arguments, because
// hle::resolve_thunk() and hle::function_at() find HLE functions by them.
#include <atomic>
#include <cstdint>

#include "cpu/stubs.h"

namespace venture {
class GuestEntry {
 public:
  virtual ~GuestEntry() = default;
  virtual const char* name() const noexcept = 0;
  virtual StubCode stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc) const = 0;
};

// SVC stubs, the entry of every Dynarmic engine (guest_stub()).
const GuestEntry& trap_entry() noexcept;

// Which entry a process writes its stubs with: the trap entry until an engine
// installs another, fixed once the first stub is written, since stubs already
// in guest memory and translations of them would otherwise mix two entries.
class GuestEntrySlot {
 public:
  const GuestEntry& entry() const noexcept;
  // Throws std::logic_error once a stub was written with a different entry.
  void install(const GuestEntry& entry);
  // Takes no lock. The trap entry is called without the indirection.
  StubCode stub(GuestIsa isa, GuestStub kind, std::uint32_t argument, std::uint32_t svc);

 private:
  std::atomic<const GuestEntry*> installed_{nullptr};
  std::atomic<bool> used_{false};
};

// The process's slot.
const GuestEntry& guest_entry() noexcept;
void set_guest_entry(const GuestEntry& entry);
StubCode guest_entry_stub(GuestIsa isa, GuestStub kind, std::uint32_t argument = 0,
                          std::uint32_t svc = kSvcHleCall);
}  // namespace venture
