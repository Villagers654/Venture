#include "cpu/cpu.h"
#include "cpu/outlined_atomics.h"
#include "cpu/invalidation_gate.h"
#include "cpu/block_list.h"
#include <utility>
#include "cpu/a32_backend.h"
#include "common/environment.h"
#include "common/log.h"
#include "core/process.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/thread.h"
#include "core/wait.h"
#include "core/profile.h"
#include "core/pc_trace.h"
#include "platform/memory.h"
#include "platform/process_info.h"
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/backend/x64/a64_jitstate.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/profiling.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace venture {
namespace {
using Dynarmic::HaltReason;
using Dynarmic::A64::VAddr;
using Dynarmic::A64::Vector;
class DynarmicCpu;
class Backend final : public CpuBackend {
 public:
  Backend() {
    // Reservation generations close the exclusive ABA (a store-exclusive after
    // another CPU's successful one fails although the value was restored);
    // VENTURE_JIT_EXCLUSIVE_GENERATIONS=0 restores plain value compare.
    if (const auto* g = environment::getenv("VENTURE_JIT_EXCLUSIVE_GENERATIONS"); !g || std::string_view(g) != "0")
      monitor.EnableGenerations();
  }
  // The audited ARMv8.0 baseline, crypto/CRC and ARMv8.1 LSE atomics (Quest 2 and
  // 3 implement LSE and their kernels report HWCAP_ATOMICS). JSCVT/RDM/LRCPC are
  // commented out in this pin's A64 decoder.
  const CpuBackendInfo information{"Venture JIT A64", 0x1fb, 0};
  std::mutex mutex;
  // One slot per live guest thread. Unity titles keep more than 64 native threads
  // alive (OVRPlugin telemetry starts more on fresh data); Android allows hundreds.
  // The patched monitor scans only slots used by guest CPUs.
  std::array<DynarmicCpu*, kGuestCpuSlots> cpus{};
  Dynarmic::ExclusiveMonitor monitor{cpus.size()};
  // Live-CPU peak logging only with the dispatch census (VENTURE_JIT_DISPATCH_CENSUS=1).
  const bool census = [] { const auto* e = environment::getenv("VENTURE_JIT_DISPATCH_CENSUS"); return e && std::string_view(e) == "1"; }();
  std::size_t peak = 0;
  const CpuBackendInfo& info() const override { return information; }
  std::unique_ptr<Cpu> create_cpu() override;
  void invalidate(GuestAddr address, std::uint64_t size, InvalidationCause cause) override;
  void refresh_dispatch(GuestAddr address, std::uint64_t size, InvalidationCause cause = InvalidationCause::other);
};
class DynarmicCpu final : public Cpu, private Dynarmic::A64::UserCallbacks {
 public:
  DynarmicCpu(Backend& backend, std::size_t slot) : backend_(backend), slot_(slot) {
    if (profile::enabled) Dynarmic::SetCodeRegistrationCallback(profile::register_translation);
    if (profile::compile_timing_enabled || JitBlockList::enabled()) Dynarmic::SetCompileTimingCallback(JitBlockList::compilation);
    Dynarmic::A64::UserConfig config{};
    config.callbacks = this;
    const auto* outlined = environment::getenv("VENTURE_PROFILE_OUTLINED_ATOMICS");
    outlined_enabled_ = profile::enabled && outlined && std::string_view(outlined) == "1";
    if (outlined_enabled_) {
      const auto counter = [this](VAddr address, unsigned kind) -> std::uint64_t* {
        if (address & 3) return nullptr;
        auto helper = outlined_atomics::recognize([&](std::size_t index) -> std::optional<std::uint32_t> {
          std::uint32_t word;
          if (address > UINT64_MAX - index * 4 ||
              !platform::read_memory(address + index * 4, &word, sizeof(word))) return {};
          return word;
        });
        return helper ? &outlined_counts_[helper->index() * 2 + kind] : nullptr;
      };
      config.profile_entry_counter = [counter](VAddr address) { return counter(address, 0); };
      config.profile_direct_call_counter = [counter](VAddr, VAddr target) { return counter(target, 1); };
    }
    config.processor_id = slot;
    config.global_monitor = &backend.monitor;
    config.fastmem_pointer = 0;
    // The fastmem fault trampoline identifies the guest access by host RIP,
    // including x86 non-canonical-address faults whose reported address is 0.
    // Callbacks below implement EL0 TBI and report precise guest faults.
    config.fastmem_address_space_bits = 64;
    config.silently_mirror_fastmem = false;
    config.enable_register_cache = true;
    if (const auto* chain = environment::getenv("VENTURE_JIT_FRAME_CHAIN")) {
      config.enable_block_frame_chain = std::string_view(chain) == "1";
      if (config.enable_block_frame_chain) config.enable_register_cache = false;
    }
    config.fastmem_exclusive_access = true;
    // Exclusive loads as plain MOVs; VENTURE_JIT_LOCKED_EXCLUSIVE_LOADS=1 restores LOCK XADD for comparisons.
    if (const auto* locked = environment::getenv("VENTURE_JIT_LOCKED_EXCLUSIVE_LOADS"); !locked || std::string_view(locked) != "1")
      config.plain_exclusive_loads = true;
    // Plain store-exclusives of the loaded value succeed without the locked store;
    // VENTURE_JIT_EXCLUSIVE_ELISION=0 keeps it.
    if (const auto* elision = environment::getenv("VENTURE_JIT_EXCLUSIVE_ELISION"); !elision || std::string_view(elision) != "0")
      config.elide_unchanged_exclusive_stores = true;
    config.check_halt_on_memory_access = true;
    // Diagnostics: VENTURE_JIT_DISABLED_HOST_FEATURES=<mask> hides host ISA extensions
    // (dynarmic HostFeature bits, e.g. 0xfe0 for every AVX-512 subset) from the emitter.
    if (const auto* mask = environment::getenv("VENTURE_JIT_DISABLED_HOST_FEATURES"))
      config.disabled_host_features = static_cast<std::uint32_t>(std::strtoul(mask, nullptr, 0));
    // Registers overwritten later in a block reach JitState only on an access's
    // abort exit, with the same values precise stores would hold (Live, default;
    // exp-jit-02 North Star -0.75%, Batman -0.6% main). off restores precise
    // stores and all also extends live ranges (tests).
    {
      using Cold = Dynarmic::A64::UserConfig::ColdAbortState;
      config.cold_memory_abort_state = Cold::Live;
      if (const auto* cold = environment::getenv("VENTURE_JIT_COLD_ABORT_STATE")) {
        const std::string_view mode(cold);
        config.cold_memory_abort_state = mode == "off" ? Cold::Off : mode == "all" ? Cold::All : Cold::Live;
      }
    }
    // Exact low-lane lowering (default); 0 restores lane moves for comparisons.
    if (const auto* lane = environment::getenv("VENTURE_JIT_LOW_LANE_ALIAS");
        lane && std::string_view(lane) == "0")
      config.optimizations &= ~Dynarmic::OptimizationFlag::LowLaneAliasing;
    // Hot code layout (defaults; each 0 restores the old layout for comparisons):
    // immediate address offsets folded into fastmem operands, deferred handlers
    // and unlinked exits in a separate code-cache region, block links reduced to
    // the patchable jump, and block entries packed without padding
    // (VENTURE_JIT_BLOCK_ALIGN=<power of two> pads them again).
    if (const auto* fold = environment::getenv("VENTURE_JIT_ADDRESS_FOLD"); fold && std::string_view(fold) == "0")
      config.optimizations &= ~Dynarmic::OptimizationFlag::AddressFolding;
    config.cold_code_region = true;
    if (const auto* region = environment::getenv("VENTURE_JIT_COLD_REGION"); region && std::string_view(region) == "0")
      config.cold_code_region = false;
    config.compact_block_links = true;
    if (const auto* links = environment::getenv("VENTURE_JIT_COMPACT_LINKS"); links && std::string_view(links) == "0")
      config.compact_block_links = false;
    config.block_alignment = 1;
    if (const auto* align = environment::getenv("VENTURE_JIT_BLOCK_ALIGN")) {
      const std::string_view value{align};
      if (value == "1" || value == "2" || value == "4" || value == "8" || value == "16" || value == "32" || value == "64")
        config.block_alignment = static_cast<std::uint32_t>(std::stoul(std::string(value)));
    }
    // Exact inline FRECPE (default); 1 restores the per-element reference call for comparisons.
    if (const auto* recip = environment::getenv("VENTURE_JIT_RECIP_ESTIMATE_CALL"); recip && std::string_view(recip) == "1")
      config.optimizations &= ~Dynarmic::OptimizationFlag::InlineRecipEstimate;
    // Value-compared exclusive stores without the global lock; the backend's
    // reservation generations make them fail after another CPU's exclusive store.
    config.unsafe_optimizations = true;
    config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    // Skip per-operation NaN fixups on the default-NaN-off path. Only NaN sign
    // and payload bits can differ from ARM.
    config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    // Measurement only (upper bound for an exact inline FRECPE/FRSQRTE): host
    // approximations instead of per-element host calls.
    if (const auto* e = environment::getenv("VENTURE_JIT_REDUCED_ERROR_FP"); e && std::string_view(e) == "1")
      config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
    config.tpidr_el0 = &tp_;
    config.tpidrro_el0 = &tpro_;
    // Keep load-time translations when the busiest thread exceeds 128 MiB.
    // This is a bounded reservation; the section commits pages only on emission.
    config.code_cache_size = 256 * 1024 * 1024;
    config.cntfrq_el0 = 1'000'000'000;
    config.ctr_el0 = 0x8444c004;  // 64-byte instruction/data lines.
    config.dczid_el0 = 0x10;     // DZP: the initial profile forbids DC ZVA.
    config.hook_data_cache_operations = true;
    config.hook_isb = true;
    config.wall_clock_cntpct = true;
    config.enable_cycle_counting = false;
    // Forward links inside a 64 KiB window skip the halt poll; backward and
    // window-crossing links still poll, so every loop observes halts.
    // Batman main 5.79 -> 5.45 ms, North Star 7.05 -> 6.88 ms. "0" disables.
    config.enable_bounded_halt_polling = true;
    if (const auto* bounded = environment::getenv("VENTURE_JIT_BOUNDED_HALT"))
      config.enable_bounded_halt_polling = std::string_view(bounded) == "1";
    // One JIT per guest thread: 2^16 entries on zero-filled OS pages (upstream's
    // filled 2^20-entry arrays committed about 1 GiB in Unity titles and missed
    // L2/TLB; Batman main thread -3.1%). VENTURE_JIT_DISPATCH_BITS overrides.
    config.fast_dispatch_table_bits = 16;
    config.fast_dispatch_table_pages = true;
    if (const auto* bits = environment::getenv("VENTURE_JIT_DISPATCH_BITS"))
      config.fast_dispatch_table_bits = static_cast<std::uint8_t>(std::clamp(std::atoi(bits), 10, 24));
    // Candidate: per-site dispatch lookups for indirect branches (default off).
    if (const auto* inline_dispatch = environment::getenv("VENTURE_JIT_INLINE_DISPATCH"))
      config.inline_fast_dispatch = std::string_view(inline_dispatch) == "1";
    // Candidate: returns pop the RSB at their own site, each with its own host
    // indirect jump (default off). Zen 4 Linux gains (2000x2040: North Star
    // 6.53 -> 6.29 ms, Batman 5.19 -> 5.04 ms); Lion Cove Windows h2h scenes
    // lose (North Star 286.9 -> 281 fps, Batman 342.6 -> 325.8, Cooking
    // 555.6 -> 544.4), and so does a compact per-site form. "1" pops inline;
    // "compact" (candidate) has each return call one shared lookup and jump on
    // its result.
    config.inline_rsb_pop = false;
    if (const auto* inline_pop = environment::getenv("VENTURE_JIT_INLINE_RSB_POP")) {
      config.inline_rsb_pop = std::string_view(inline_pop) == "1";
      config.compact_rsb_pop = std::string_view(inline_pop) == "compact";
    }
    // Supervisor calls (HLE thunks) link to the next instruction's block instead
    // of pushing and popping the RSB. AC Nexus h2h scene 286.5 -> 291.4 fps. "0"
    // pushes and pops.
    config.svc_return_link = true;
    if (const auto* svc_link = environment::getenv("VENTURE_JIT_SVC_RETURN_LINK"))
      config.svc_return_link = std::string_view(svc_link) == "1";
    // Candidate: guest calls and returns through the host's CALL/RET, so the
    // CPU's return predictor predicts guest returns (default off). Never while
    // a hardware shadow stack is enforced: the stack drops entries without RET.
    if (const auto* host_returns = environment::getenv("VENTURE_JIT_HOST_RETURNS"))
      config.host_return_stack = std::string_view(host_returns) == "1" && !platform::user_shadow_stack_active();
    // Indirect branches (virtual calls, PLT and icall pointers) jump through a
    // per-site slot holding their last target. "0" uses the shared lookup.
    config.dispatch_site_cache = true;
    if (const auto* site_cache = environment::getenv("VENTURE_JIT_SITE_CACHE"))
      config.dispatch_site_cache = std::string_view(site_cache) == "1";
    // Conditional exits store NZCV only where the successor's code may read it
    // before overwriting it. "0" stores it on every exit.
    config.nzcv_exit_liveness = true;
    if (const auto* liveness = environment::getenv("VENTURE_JIT_NZCV_LIVENESS"))
      config.nzcv_exit_liveness = std::string_view(liveness) == "1";
    // Calls to compiler-rt outlined atomic helpers run their LSE instruction in
    // the calling block. "0" keeps the call and return.
    config.inline_outlined_atomics = true;
    if (const auto* inline_atomics = environment::getenv("VENTURE_JIT_INLINE_ATOMICS"))
      config.inline_outlined_atomics = std::string_view(inline_atomics) == "1";
    if (const auto* census = environment::getenv("VENTURE_JIT_DISPATCH_CENSUS");
        census && std::string_view(census) == "1") {
      census_.emplace();
      config.fast_dispatch_census = census_->counts.data();
    }
    config.svc_fp_transfer = kSvcHleCallFastFp;
    config.svc_fp_guest = kSvcHleCallFastGuestFp;
    config.svc_fast = kSvcHleCallFast;
    // Diagnostic comparison uses the same binary and JIT configuration.
    const auto* thunk_metadata = environment::getenv("VENTURE_HLE_THUNK_METADATA");
    if (!thunk_metadata || std::string_view(thunk_metadata) != "0")
      config.svc_lookup = [](VAddr entry, std::uint32_t svc) { return hle::resolve_thunk(entry, svc); };
    if (pc_trace::selection().count) {
      pc_observer_slot_ = pc_trace::claim();
      config.diagnostic_entry.selected = [](VAddr pc) { return pc_trace::selection().contains(pc); };
      config.diagnostic_entry.user = this;
      config.diagnostic_entry.record = [](void* user, VAddr pc, const void* raw) noexcept {
        const auto& state=*static_cast<const Dynarmic::Backend::X64::A64JitState*>(raw);
        const auto slot=static_cast<DynarmicCpu*>(user)->pc_observer_slot_;
        pc_trace::record(slot,pc,state.sp,state.GetPstate(),state.reg,
                         platform::profile_thread_id(),api_result_trace::now());
      };
      config.nzcv_exit_liveness = false;
      config.inline_outlined_atomics = false;
    }
    jit_bits_ = config.fast_dispatch_table_bits;
    jit_ = std::make_unique<Dynarmic::A64::Jit>(config);
    cache_begin_ = reinterpret_cast<std::uintptr_t>(jit_->CodeCacheBegin());
    cache_end_ = cache_begin_ + jit_->CodeCacheSize();
  }
  ~DynarmicCpu() override {
    if (census_) log_census();
    JitBlockList::retire(std::move(block_list_));
    std::lock_guard lock(backend_.mutex);
    backend_.monitor.ClearProcessor(slot_);
    backend_.cpus[slot_] = nullptr;
  }
  CpuExit run() override {
    std::optional<JitBlockList::Scope> recording;
    if (hints_) recording.emplace(&block_list_);
    if (external_halt_.exchange(false)) { jit_->ClearHalt(HaltReason::UserDefined2); return CpuExit::HaltRequested; }
    for (;;) {
      invalidations_->enter([&](GuestAddr p, std::uint64_t n) { jit_->InvalidateCacheRange(p, n); });
      exit_.reset();
      HaltReason reason;
      try { reason = jit_->Run(); }
      catch (...) { acknowledge_stop(); throw; }
      acknowledge_stop();
      for (const auto& [p, n] : cache_operations_) backend_.invalidate(p, n, InvalidationCause::ic_operation);
      cache_operations_.clear();
      if (census_) log_census(10'000'000'000);
      if (exit_) return *exit_;
      if (Dynarmic::Has(reason, HaltReason::UserDefined2)) { external_halt_.store(false); return CpuExit::HaltRequested; }
      if (Dynarmic::Has(reason, HaltReason::MemoryAbort)) return CpuExit::MemoryFault;
      // Invalidation is serviced by the owning host thread, never concurrently with JIT state.
    }
  }
  void request_halt() override { external_halt_.store(true); jit_->HaltExecution(HaltReason::UserDefined2); }
  JitBlockList detach_jit_hints() noexcept override { return std::exchange(block_list_, JitBlockList{}); }
  void prepare_call(GuestAddr entry) override {
    if (!hints_ || block_list_.prepared() || jit_->IsExecuting() || external_halt_.load()) return;
    // Hold the gate only while compiling: the hint snapshot may wait on files,
    // and an invalidating thread must not wait on that.
    bool entered = false;
    try {
      block_list_.prepare(entry, false, [&](std::uint64_t descriptor) {
        if (!entered) { invalidations_->enter([&](GuestAddr p, std::uint64_t n) { jit_->InvalidateCacheRange(p, n); }); entered = true; }
        return !external_halt_.load() && jit_->Precompile(descriptor);
      });
    }
    catch (...) { if (entered) acknowledge_stop(); throw; }
    if (entered) acknowledge_stop();
  }
  std::pair<std::shared_ptr<InvalidationGate>, std::uint64_t> queue_invalidation(GuestAddr p, std::uint64_t n) {
    return {invalidations_, invalidations_->queue(p, n, [&] { jit_->HaltExecution(HaltReason::UserDefined3); })};
  }
  std::uint32_t svc_immediate() const override { return svc_; }
  CpuFault fault() const override { return fault_; }
  std::uint64_t x(int i) const override { return jit_->GetRegister(i); }
  void set_x(int i, std::uint64_t v) override { jit_->SetRegister(i, v); }
  std::uint64_t sp() const override { return jit_->GetSP(); }
  void set_sp(std::uint64_t v) override { jit_->SetSP(v); }
  std::uint64_t pc() const override { return jit_->GetPC(); }
  void set_pc(std::uint64_t v) override { jit_->SetPC(v); }
  std::uint32_t pstate() const override { return jit_->GetPstate(); }
  void set_pstate(std::uint32_t v) override { jit_->SetPstate(v); }
  std::uint32_t fpcr() const override { return jit_->GetFpcr(); }
  void set_fpcr(std::uint32_t v) override { jit_->SetFpcr(v); }
  std::uint32_t fpsr() const override { return jit_->GetFpsr(); }
  void set_fpsr(std::uint32_t v) override { jit_->SetFpsr(v); }
  Vec128 v(int i) const override { auto v = jit_->GetVector(i); return {v[0], v[1]}; }
  void set_v(int i, Vec128 v) override { jit_->SetVector(i, {v.lo, v.hi}); }
  std::uint64_t tpidr() const override { return tp_; }
  void set_tpidr(std::uint64_t v) override { tp_ = v; }
  std::uint64_t tpidrro() const override { return tpro_; }
  void set_tpidrro(std::uint64_t v) override { tpro_ = v; }
  void clear_exclusive() override { jit_->ClearExclusiveState(); backend_.monitor.ClearProcessor(slot_); }
  GuestIsa isa() const override { return GuestIsa::a64; }
  // libdl's ISA-neutral caller lookup retains one virtual call on A64.
  std::uint64_t lr() const override { return jit_->GetRegister(30); }
  void set_lr(std::uint64_t value) override { jit_->SetRegister(30, value); }
  std::uint64_t hle_call_id() const override { return jit_->GetRegister(16); }
  std::uint64_t syscall_number() const override { return jit_->GetRegister(8); }
  std::uint64_t frame_pointer() const override { return jit_->GetRegister(29); }
  std::uint32_t trap_instruction_size() const override { return 4; }
  CpuProfileSnapshot profile_snapshot(std::uintptr_t ip) const noexcept override {
    return {jit_->GetPC(), ip >= cache_begin_ && ip < cache_end_, outlined_enabled_, outlined_counts_};
  }
 private:
  Backend& backend_;
  std::size_t slot_;
  std::size_t pc_observer_slot_ = pc_trace::thread_capacity;
  bool outlined_enabled_ = false;
  unsigned jit_bits_ = 0;
  std::array<std::uint64_t, 16> outlined_counts_{};
  std::uint64_t tp_{}, tpro_{};
  std::unique_ptr<Dynarmic::A64::Jit> jit_;
  std::uintptr_t cache_begin_ = 0, cache_end_ = 0;
  std::optional<CpuExit> exit_;
  CpuFault fault_{};
  std::uint32_t svc_{};
  std::shared_ptr<InvalidationGate> invalidations_ = std::make_shared<InvalidationGate>();
  std::vector<std::pair<GuestAddr, std::uint64_t>> cache_operations_;
  std::atomic<bool> external_halt_{false};
  JitBlockList block_list_;
  const bool hints_ = JitBlockList::enabled(); // Read once per CPU, not per run.
  void acknowledge_stop() { invalidations_->leave(); }
  // VENTURE_JIT_DISPATCH_CENSUS=1: cumulative per-thread dispatch and callback counts.
  struct Census {
    // [0..5] see UserConfig::fast_dispatch_census; [6] untagged, [7] tagged data callbacks.
    std::array<std::uint64_t, 8> counts{};
    std::int64_t logged_ns = 0;
  };
  std::optional<Census> census_;
  void log_census(std::int64_t interval_ns = 0) {
    const auto now = monotonic_ns();
    if (now - census_->logged_ns < interval_ns) return;
    census_->logged_ns = now;
    const auto& c = census_->counts;
    const auto* thread = GuestThread::current();
    log::info("jit-census", "tid {} slot {} at {} ns: rsb pops {} misses {}; fast hints {}; table misses 2^{} {} 2^18 {} 2^16 {}; "
              "data callbacks untagged {} tagged {}", thread ? thread->tid() : 0, slot_, now, c[0], c[1], c[2], jit_bits_, c[3], c[4], c[5], c[6], c[7]);
  }
  void count_callback(VAddr address) {
    if (census_) [[unlikely]] ++census_->counts[(address >> 56) ? 7 : 6];
  }
  static VAddr data_address(VAddr address) {
    // Linux enables TBI0 for EL0 data accesses. Ignore only the top byte;
    // bits 55:48 remain significant and must not alias a valid host address.
    return address & 0x00ff'ffff'ffff'ffffull;
  }
  void memory_fault(VAddr address, bool write) {
    if (!exit_) { exit_ = CpuExit::MemoryFault; fault_ = {address, write, false}; }
    jit_->HaltExecution(HaltReason::MemoryAbort);
  }
  template<class T> T read(VAddr address) {
    count_callback(address);
    T value{};
    if (!platform::read_memory(data_address(address), &value, sizeof(value))) memory_fault(address, false);
    return value;
  }
  template<class T> void write(VAddr address, T value) {
    count_callback(address);
    if (!platform::write_memory(data_address(address), &value, sizeof(value))) memory_fault(address, true);
  }
  template<class T> bool exclusive(VAddr address, T value, T expected) {
    // Fault callbacks must not take the exclusive monitor: Dynarmic may hold it here.
    const auto mapped = data_address(address);
    if ((address & (sizeof(T) - 1)) || !platform::memory_accessible(mapped, sizeof(T), true)) { memory_fault(address, true); return false; }
    return platform::compare_exchange(mapped, &value, &expected, sizeof(T));
  }
  // LSE atomics the JIT cannot do inline (faulting, unaligned or non-fastmem
  // addresses). Add, Swap and CAS are one locked host instruction; the others
  // retry a CAS. A fault is contained and reported as a precise write fault.
  std::uint64_t MemoryAtomic(VAddr address, std::size_t bytes, Dynarmic::A64::AtomicOp op,
                             std::uint64_t value, std::uint64_t desired) override {
    using Op = Dynarmic::A64::AtomicOp;
    using platform::AtomicRmw;
    const unsigned shift = 64 - 8 * static_cast<unsigned>(bytes);
    const auto mask = UINT64_MAX >> shift;
    const auto sign = [&](std::uint64_t v) { return static_cast<std::int64_t>(v << shift) >> shift; };
    std::uint64_t old = 0;
    const auto mapped = data_address(address);
    bool ok = !(address & (bytes - 1));
    if (ok && (op == Op::Add || op == Op::Swap || op == Op::CompareAndSwap)) {
      const auto kind = op == Op::Add ? AtomicRmw::add : op == Op::Swap ? AtomicRmw::exchange : AtomicRmw::compare_exchange;
      ok = platform::atomic_rmw(mapped, static_cast<unsigned>(bytes), kind, value, desired, old);
    } else if (ok) {
      ok = platform::read_memory(mapped, &old, bytes);
      for (std::uint64_t seen = 0; ok; old = seen) {
        std::uint64_t next = 0;
        switch (op) {
          case Op::Clear: next = old & ~value; break;
          case Op::Eor: next = old ^ value; break;
          case Op::Set: next = old | value; break;
          case Op::SMax: next = sign(old) > sign(value) ? old : value; break;
          case Op::SMin: next = sign(old) < sign(value) ? old : value; break;
          case Op::UMax: next = (old & mask) > (value & mask) ? old : value; break;
          default: next = (old & mask) < (value & mask) ? old : value; break;
        }
        ok = platform::atomic_rmw(mapped, static_cast<unsigned>(bytes), AtomicRmw::compare_exchange, old, next & mask, seen);
        if (ok && seen == old) break;
      }
    }
    if (!ok) { memory_fault(address, true); return 0; }
    return old;
  }
  Vector MemoryAtomicCompareAndSwap128(VAddr address, Vector expected, Vector desired) override {
    // CASP of doublewords. A mismatching pair is stored back so the result is one
    // snapshot (x86 CMPXCHG16B always writes too). A CAS that fails while memory
    // still holds the pair it compared against cannot store: a write fault.
    Vector seen{}, again{};
    const auto mapped = data_address(address);
    while (!(address & 15) && platform::read_memory(mapped, &seen, 16)) {
      const Vector store = seen == expected ? desired : seen;
      if (platform::compare_exchange(mapped, &store, &seen, 16)) return seen;
      if (!platform::read_memory(mapped, &again, 16) || again == seen) break;
    }
    memory_fault(address, true);
    return expected;
  }
  std::optional<std::uint32_t> MemoryReadCode(VAddr address) override {
    std::uint32_t insn{};
    // Translation reads every instruction once. Use a plain guarded load, not a
    // ReadProcessMemory call per instruction; a racing unmap still faults precisely.
    if (!AddressSpace::get().is_guest_executable(address) || !platform::guarded_load32(address, insn)) return std::nullopt;
    // CNTVCT has the same clock domain here as CNTPCT. This pin only decodes CNTPCT.
    if ((insn & ~31u) == 0xd53be040u) return (insn & 31u) | 0xd53be020u;
    // Arm's backwards-compatible HINT space includes PAC/AUT, BTI and XPACLRI.
    if ((insn & 0xfffff01fu) == 0xd503201fu) return 0xd503201fu;
    // Expose HLT through the same debugger stop as BRK (the backend only decodes BRK).
    if ((insn & 0xffe0001fu) == 0xd4400000u) return 0xd4200000u | (insn & 0x001fffe0u);
    // Privileged cache operations and forbidden ZVA become a precise undefined instruction.
    if ((insn & ~31u) == 0xd50b7420u || (insn & ~31u) == 0xd5087620u ||
        (insn & ~31u) == 0xd5087640u || (insn & ~31u) == 0xd5087a40u ||
        (insn & ~31u) == 0xd5087e40u || insn == 0xd508751fu || insn == 0xd508711fu) return 0;
    return insn;
  }
  std::uint64_t MemoryRead(VAddr p, std::size_t size) override {
    switch (size) {
      case 1: return read<std::uint8_t>(p);
      case 2: return read<std::uint16_t>(p);
      case 4: return read<std::uint32_t>(p);
      case 8: return read<std::uint64_t>(p);
      default: memory_fault(p, false); return 0;
    }
  }
  Vector MemoryRead128(VAddr p) override { return read<Vector>(p); }
  void MemoryWrite(VAddr p, std::uint64_t v, std::size_t size) override {
    switch (size) {
      case 1: return write(p, static_cast<std::uint8_t>(v));
      case 2: return write(p, static_cast<std::uint16_t>(v));
      case 4: return write(p, static_cast<std::uint32_t>(v));
      case 8: return write(p, v);
      default: memory_fault(p, true);
    }
  }
  void MemoryWrite128(VAddr p, Vector v) override { write(p, v); }
  bool MemoryWriteExclusive8(VAddr p, std::uint8_t v, std::uint8_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive16(VAddr p, std::uint16_t v, std::uint16_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive32(VAddr p, std::uint32_t v, std::uint32_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive64(VAddr p, std::uint64_t v, std::uint64_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive128(VAddr p, Vector v, Vector e) override { return exclusive(p, v, e); }
  void CallSVC(std::uint32_t immediate) override { svc_ = immediate; exit_ = CpuExit::Svc; jit_->HaltExecution(); }
  void CallSVCFast(std::uint32_t immediate) override {
    svc_ = immediate;
    if (hle::GuestFpScope fp(immediate == kSvcHleCallFastGuestFp); dispatch_hle_in_jit()) return;
    exit_ = has_hle_exception() ? CpuExit::HleException : CpuExit::Svc;
    jit_->HaltExecution();
  }
  void CallSVCFastKnown(std::uint32_t immediate, const void* metadata, void* state) override {
    svc_ = immediate;
    auto& registers = *static_cast<Dynarmic::Backend::X64::A64JitState*>(state);
    if (hle::GuestFpScope fp(immediate == kSvcHleCallFastGuestFp);
        dispatch_hle_in_jit(*static_cast<const hle::FunctionInfo*>(metadata), hle::Registers(registers.reg.data())))
      return;
    exit_ = has_hle_exception() ? CpuExit::HleException : CpuExit::Svc;
    jit_->HaltExecution();
  }
  void ExceptionRaised(VAddr address, Dynarmic::A64::Exception exception) override {
    using E = Dynarmic::A64::Exception;
    jit_->SetPC(address);
    if (exception == E::NoExecuteFault) { exit_ = CpuExit::MemoryFault; fault_ = {address, false, true}; }
    else exit_ = exception == E::Breakpoint ? CpuExit::Breakpoint : CpuExit::UndefinedInstruction;
    jit_->HaltExecution();
  }
  void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation, VAddr address) override {
    // Broadcast after leaving Run; simultaneous guest IC operations must not await each other's callbacks.
    cache_operations_.emplace_back(align_down(address, 64), 64);
    jit_->HaltExecution(HaltReason::UserDefined3);
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1ull << 60; }
  std::uint64_t GetCNTPCT() override { return static_cast<std::uint64_t>(monotonic_ns()); }
};
std::unique_ptr<Cpu> Backend::create_cpu() {
  std::lock_guard lock(mutex);
  auto it = std::find(cpus.begin(), cpus.end(), nullptr);
  if (it == cpus.end()) throw std::runtime_error("Dynarmic exclusive monitor slots exhausted (" + std::to_string(cpus.size()) + " threads)");
  auto cpu = std::make_unique<DynarmicCpu>(*this, static_cast<std::size_t>(it - cpus.begin()));
  *it = cpu.get();
  if (census) {
    const auto live = cpus.size() - std::count(cpus.begin(), cpus.end(), nullptr);
    if (live > peak) { peak = live; log::info("cpu", "A64 live CPUs peak {}/{}", peak, cpus.size()); }
  }
  return cpu;
}
void Backend::invalidate(GuestAddr p, std::uint64_t n, InvalidationCause cause) {
  if (!n) return;
  refresh_dispatch(p,n,cause);
}
void Backend::refresh_dispatch(GuestAddr p, std::uint64_t n, InvalidationCause cause) {
  if (!n) return;
  const auto begin = profile::invalidation_tracing ? monotonic_ns() : 0;
  std::vector<std::pair<std::shared_ptr<InvalidationGate>, std::uint64_t>> tickets;
  {
    std::lock_guard lock(mutex);
    for (auto* cpu : cpus) if (cpu) tickets.push_back(cpu->queue_invalidation(p, n));
  }
  for (const auto& [gate, epoch] : tickets) gate->wait(epoch);
  if (profile::invalidation_tracing) {
    const auto running = std::count_if(tickets.begin(), tickets.end(), [](const auto& t) { return t.second != 0; });
    profile::record_invalidation(cause, n, static_cast<unsigned>(tickets.size()), static_cast<unsigned>(running),
                                 begin, static_cast<std::uint64_t>(monotonic_ns() - begin));
  }
}
}
CpuBackend& cpu_backend() {
  static auto* backend = process_config().isa() == GuestIsa::a32 ? &a32_cpu_backend() : new Backend;
  return *backend;
}
}
