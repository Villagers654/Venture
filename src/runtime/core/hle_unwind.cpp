// Guest unwind metadata for Venture's leaf HLE calls and ARM signal restorers.
// Encodings follow DWARF/EHABI; no host C++ frames are exposed to the guest.
#include "core/hle_unwind.h"
#include "core/address_space.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/hle_runtime.h"
#include "core/guest_storage.h"
#include "core/a32_abi.h"
#include "common/environment.h"
#include "common/log.h"
#include "platform/memory.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace venture::hle {
namespace {
struct Entry { GuestAddr address; std::uint32_t size; GuestStub kind; std::uint32_t argument; };
struct Phdr64 { std::uint32_t type, flags; std::uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Phdr32 { std::uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };
class Image final : public Library {
 public:
  GuestAddr exidx{}; std::int32_t entries{}; std::vector<Entry> ranges;
  Image() { soname_ = path_ = "[venture hle]"; }
  GuestAddr find_symbol(std::string_view, std::string_view) const override { return 0; }
  GuestIsa isa() const override { return process_config().isa(); }
};
struct State {
  std::mutex mutex;
  std::vector<Entry> ranges;
  std::map<GuestAddr, std::uint32_t> leaves;
  // Unwinders can cache FDE addresses: published snapshots live until exit.
  std::vector<std::shared_ptr<Image>> snapshots;
  std::array<std::size_t, 2> published{};
  std::array<std::shared_ptr<Image>, 2> latest{};
};
State& state() { static auto* value = new State; return *value; }
const bool legacy_wait_steps = [] {
  const auto* value = environment::getenv("VENTURE_WAIT_UNWIND_COUNTS");
  return value && std::string_view(value) == "step";
}();
void condition_personality32(CallContext& context) {
  // EHABI's context is opaque. Use the unwinder which invoked this personality,
  // rather than assuming GCC's or LLVM's private register-set layout.
  auto& thread = context.thread();
  const auto state = std::uint32_t(context.x(0)) & ~8u; // _US_FORCE_UNWIND
  const auto vrs = context.x(2);
  auto* owner = Linker::get().library_at(context.cpu().lr() & ~GuestAddr{1});
  const auto get = owner ? owner->find_symbol("_Unwind_VRS_Get") : 0;
  const auto set = owner ? owner->find_symbol("_Unwind_VRS_Set") : 0;
  if (!get || !set || state > 2) {
    log::error("unwind", "condition cleanup requires the caller's EHABI VRS APIs; state={}", state);
    context.return_x(9); return; // _URC_FAILURE
  }
  GuestStorage storage;
  const auto scratch = storage.resize(16);
  if (thread.call(get, {vrs, 0, 13, 0, scratch}) != 0) { context.return_x(9); return; }
  std::uint32_t sp;
  if (!platform::read_memory(scratch, &sp, 4)) throw MemoryFault{scratch};
  std::array<std::uint32_t, 2> saved;
  if (!platform::read_memory(sp, saved.data(), sizeof(saved))) { context.return_x(9); return; }
  if (state == 1) {
    for (auto* action = thread.condition_wait_cleanup; action; action = action->previous) {
      if (!action->pending || action->caller_sp != std::uint64_t(sp) + 8) continue;
      action->pending = false;
      if (!action->occurrence) action->occurrence = ++thread.wait_unwind_occurrences;
      action->acquire(action->argument);
      break;
    }
  }
  // The thunk's only frame is push {r4,lr}. Restore r4, advance virtual SP,
  // and finish through the saved LR (which retains its Thumb interworking bit).
  const std::array<std::uint32_t, 3> values{saved[0], sp + 8, saved[1]};
  const std::array<unsigned, 3> registers{4, 13, 15};
  for (unsigned i = 0; i < values.size(); ++i) {
    if (!platform::guarded_store(scratch, &values[i], 4)) throw MemoryFault{scratch, true};
    if (thread.call(set, {vrs, 0, registers[i], 0, scratch}) != 0) { context.return_x(9); return; }
  }
  context.return_x(8); // _URC_CONTINUE_UNWIND
}
void condition_personality(CallContext& context) {
  if (context.x(1) & 2) for (auto* action = context.thread().condition_wait_cleanup; action; action = action->previous)
    if (action->pending) { action->pending = false; action->acquire(action->argument); break; }
  context.return_x(8); // _URC_CONTINUE_UNWIND
}
GuestAddr condition_personality_address() {
  if (process_config().isa() == GuestIsa::a32) {
    static const auto address = [] {
      constexpr std::string_view name = "<condition_wait_personality32>";
      a32::register_adapter("libc.so", name, condition_personality32);
      return make_thunk("libc.so", name, condition_personality32);
    }();
    return legacy_wait_steps ? 0 : address;
  }
  static const auto address = make_thunk("libc.so", "<condition_wait_personality>", condition_personality);
  return address;
}
template<class T> void put(GuestAddr address, T value) { std::memcpy(g2h(address), &value, sizeof(value)); }
std::uint32_t prel31(GuestAddr target, GuestAddr field) {
  auto distance = static_cast<std::int64_t>(target - field);
  if (distance < -(1ll << 30) || distance >= (1ll << 30)) throw std::runtime_error("HLE EHABI metadata outside PREL31 range");
  return std::uint32_t(distance) & 0x7fffffff;
}
bool use_condition_personality() {
  if (process_config().isa() != GuestIsa::a32) return true;
  auto* thread = GuestThread::current();
  auto* owner = thread ? Linker::get().library_at(thread->cpu().lr() & ~GuestAddr{1}) : nullptr;
  const bool available = !legacy_wait_steps && owner && owner->find_symbol("_Unwind_VRS_Get") && owner->find_symbol("_Unwind_VRS_Set");
  // Hidden, statically linked unwinders cannot expose their opaque context.
  // Retain the checked fallback for them, selecting compact metadata for the
  // same caller. Exported EHABI implementations use full-speed cleanup.
  if (thread) for (auto* action = thread->condition_wait_cleanup; action; action = action->previous)
    if (action->pending) action->observe_unwind = !available;
  return available;
}
std::shared_ptr<Image> snapshot(bool cleanup_personality) {
  const auto personality = cleanup_personality ? condition_personality_address() : 0;
  const unsigned view = cleanup_personality ? 1 : 0;
  auto& s = state(); std::lock_guard lock(s.mutex);
  if (s.ranges.empty()) return {};
  if (s.published[view] == s.ranges.size() && s.latest[view]) return s.latest[view];
  auto image = std::make_shared<Image>(); image->ranges = s.ranges;
  std::sort(image->ranges.begin(), image->ranges.end(), [](auto& a, auto& b) { return a.address < b.address; });
  image->map_start = image->ranges.front().address;
  auto& last = image->ranges.back(); image->map_size = last.address + last.size - image->map_start;
  const auto n = image->ranges.size();
  auto storage = AddressSpace::get().allocate(4096 + n * 96, kProtRead | kProtWrite, "[hle unwind metadata]");
  if (!storage) throw std::bad_alloc();
  const bool a64 = process_config().isa() == GuestIsa::a64;
  // libunwind rejects dlpi_addr above the PC. Every thunk page reserves a
  // header prefix, so the lowest code page is a valid, readable object base.
  const auto base = align_down(image->map_start, 4096);
  image->load_bias = base; image->phdr = storage + 64; image->phnum = 3;
  // Publish a readable ELF header, as for an ordinary bionic/vDSO image.
  // dl_iterate_phdr consumers may inspect the object at dlpi_addr.
  const std::uint8_t ident[]{0x7f,'E','L','F',std::uint8_t(a64 ? 2 : 1),1,1};
  std::memcpy(g2h(storage), ident, sizeof(ident));
  put(storage + 16, std::uint16_t(3)); put(storage + 18, std::uint16_t(a64 ? 183 : 40));
  put(storage + 20, std::uint32_t(1));
  if (a64) {
    put(storage + 32, std::uint64_t(image->phdr-base)); put(storage + 52, std::uint16_t(64));
    put(storage + 54, std::uint16_t(sizeof(Phdr64))); put(storage + 56, std::uint16_t(3));
    const auto cie = storage + 256;
    // CIE v1: zR, absolute pointers, CFA=SP, return address=same LR.
    const std::uint8_t body[]{1,'z','R',0,1,0x78,30,1,0,0x0c,31,0,0x08,30,0,0};
    put(cie, std::uint32_t(20)); put(cie + 4, std::uint32_t(0));
    std::memcpy(g2h(cie + 8), body, sizeof(body));
    const auto cleanup_cie = cie + 24;
    // zPR: absolute guest personality, CFA=SP, same-value LR. Only condition
    // waits have a cleanup action; the ordinary leaf metadata stays unchanged.
    const std::uint8_t prefix[]{1,'z','P','R',0,1,0x78,30,10,0};
    const std::uint8_t suffix[]{0,0x0c,31,0,0x08,30};
    put(cleanup_cie, std::uint32_t(28)); put(cleanup_cie + 4, std::uint32_t(0));
    std::memcpy(g2h(cleanup_cie + 8), prefix, sizeof(prefix));
    put(cleanup_cie + 18, std::uint64_t(personality));
    std::memcpy(g2h(cleanup_cie + 26), suffix, sizeof(suffix));
    for (std::size_t i = 0; i < n; ++i) {
      const auto fde = cie + 56 + i * 48; const auto& r = image->ranges[i];
      const auto record = r.kind == GuestStub::hle_call && r.argument ? cleanup_cie : cie;
      put(fde, std::uint32_t(44)); put(fde + 4, std::uint32_t(fde + 4 - record));
      put(fde + 8, std::uint64_t(r.address)); put(fde + 16, std::uint64_t(r.size)); put(fde + 24, std::uint32_t(0));
      if (r.kind == GuestStub::hle_call && r.argument) {
        // The STP at +20 saves x29/LR on a 16-byte frame; the LDP at +28 restores both.
        const std::uint8_t cfi[]{0x58,0x0e,16,0x9d,2,0x9e,1,0x48,0x0e,0,0x08,29,0x08,30};
        std::memcpy(g2h(fde + 25), cfi, sizeof(cfi));
      }
      // A host-call boundary has no further guest caller. End the walk here.
      if (r.kind == GuestStub::return_to_host || r.kind == GuestStub::call_return) {
        put(fde + 25, std::uint8_t(0x07)); put(fde + 26, std::uint8_t(30));
      }
    }
    const auto final_header = cie + 56 + n * 48 + 4;
    // Version 1, absolute EH-frame pointer, udata4 count, udata8 table.
    // Absolute table entries also cover code/metadata more than 2 GiB apart.
    put(final_header, std::uint32_t(0x04030001));
    put(final_header + 4, std::uint64_t(cie)); put(final_header + 12, std::uint32_t(n));
    for (std::size_t i = 0; i < n; ++i) {
      put(final_header + 16 + i * 16, std::uint64_t(image->ranges[i].address));
      put(final_header + 24 + i * 16, std::uint64_t(cie + 56 + i * 48));
    }
    put(image->phdr, Phdr64{1,5,0,image->map_start-base,0,0,image->map_size,4096});
    put(image->phdr + sizeof(Phdr64), Phdr64{1,4,storage-base,storage-base,0,4096+n*96,4096+n*96,4096});
    put(image->phdr + 2*sizeof(Phdr64), Phdr64{0x6474e550,4,final_header-base,final_header-base,0,16+n*16,16+n*16,8});
  } else {
    put(storage + 28, std::uint32_t(image->phdr-base)); put(storage + 40, std::uint16_t(52));
    put(storage + 42, std::uint16_t(sizeof(Phdr32))); put(storage + 44, std::uint16_t(3));
    std::map<GuestAddr, const Entry*> starts;
    for (auto& r : image->ranges) { starts[r.address] = &r; starts.try_emplace(r.address + r.size, nullptr); }
    image->exidx = storage + 256; image->entries = std::int32_t(starts.size());
    auto extra = image->exidx + starts.size() * 8;
    std::size_t i = 0;
    for (auto [address, r] : starts) {
      const auto row = image->exidx + i++ * 8; put(row, prel31(address, row));
      if (!r) put(row + 4, std::uint32_t(1));
      else if (r->kind == GuestStub::sigreturn) {
        // Skip to saved r0, then restore r0-r15 from the kernel ucontext.
        put(extra, std::uint32_t((r->argument & 1) ? 0x810127b1 : 0x810107b1));
        // A leaf HLE call has no stack frame: its caller's return PC is LR.
        // Restore r0-r14 then finish through LR, instead of introducing a
        // duplicate-SP frame into EHABI phase two. Normal sigreturn still
        // restores the unmodified kernel context, including its real PC.
        put(extra + 4, std::uint32_t((r->argument & 2) ? 0x0f87ffb0 : 0x0f8fffb0));
        put(extra + 8, std::uint32_t(0)); // empty personality-1 descriptor list
        put(row + 4, prel31(extra, row + 4)); extra += 12;
      } else if (r->kind == GuestStub::hle_call && r->argument) {
        if (personality) {
          put(extra, prel31(personality, extra)); // generic EHABI personality
          put(extra + 4, std::uint32_t(0));
          put(row + 4, prel31(extra, row + 4)); extra += 8;
        } else put(row + 4, std::uint32_t(0x80a8b0b0)); // stepping census only
      } else put(row + 4, std::uint32_t(r->kind == GuestStub::hle_call ? 0x80b0b0b0 : 1));
    }
    put(image->phdr, Phdr32{1,0,std::uint32_t(image->map_start-base),0,0,std::uint32_t(image->map_size),5,4096});
    put(image->phdr + sizeof(Phdr32), Phdr32{1,std::uint32_t(storage-base),std::uint32_t(storage-base),0,std::uint32_t(4096+n*96),std::uint32_t(4096+n*96),4,4096});
    put(image->phdr + 2*sizeof(Phdr32), Phdr32{0x70000001,std::uint32_t(image->exidx-base),std::uint32_t(image->exidx-base),0,std::uint32_t(starts.size()*8),std::uint32_t(starts.size()*8),4,4});
  }
  std::memcpy(g2h(base), g2h(storage), 64);
  s.published[view] = s.ranges.size(); s.latest[view] = image; s.snapshots.push_back(image); return image;
}
}
bool legacy_wait_unwind_observation() { return legacy_wait_steps; }
void record_unwind_stub(GuestAddr address, std::uint32_t size, GuestStub kind, std::uint32_t argument) {
  if (kind != GuestStub::hle_call && kind != GuestStub::return_to_host && kind != GuestStub::call_return &&
      !(kind == GuestStub::sigreturn && process_config().isa() == GuestIsa::a32)) return;
  auto& s = state(); std::lock_guard lock(s.mutex); s.ranges.push_back({address,size,kind,argument});
  if (kind == GuestStub::hle_call && !argument) s.leaves.emplace(address, size);
}
bool is_unwind_leaf(GuestAddr pc) {
  auto& s = state(); std::lock_guard lock(s.mutex);
  auto it = s.leaves.upper_bound(pc & ~GuestAddr{1});
  if (it == s.leaves.begin()) return false;
  --it; return (pc & ~GuestAddr{1}) - it->first < it->second;
}
void for_each_unwind_image(const std::function<void(const Library&)>& visit) {
  if (auto image = snapshot(use_condition_personality())) visit(*image);
}
GuestAddr unwind_exidx(GuestAddr pc, std::int32_t* count) {
  if (process_config().isa() != GuestIsa::a32) return 0;
  auto image = snapshot(use_condition_personality()); if (!image) return 0;
  auto it = std::upper_bound(image->ranges.begin(), image->ranges.end(), pc, [](auto p, auto& r) { return p < r.address; });
  if (it == image->ranges.begin()) return 0;
  auto& r = *std::prev(it); if (pc - r.address >= r.size) return 0;
  if (count) *count = image->entries; return image->exidx;
}
}
