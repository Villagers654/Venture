#include "common/environment.h"
#include "cpu/a32_backend.h"
#include "cpu/invalidation_gate.h"
#include "cpu/block_list.h"
#include <utility>
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/wait.h"
#include "core/profile.h"
#include "common/log.h"
#include "platform/low_memory.h"
#include "platform/memory.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/backend/x64/a32_jitstate.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/profiling.h>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace venture {
namespace {
using Dynarmic::HaltReason;
using Dynarmic::A32::VAddr;
using Dynarmic::A32::CoprocReg;
class A32Cpu;
class A32Backend final : public CpuBackend {
 public:
  explicit A32Backend(A32BackendConfig config)
      : cpus(config.monitor_slots), monitor(config.monitor_slots), query(config.memory_query),
        use_global_monitor(config.use_global_monitor), enable_fastmem(config.enable_fastmem) {}
  // Linux arch/arm/include/uapi/asm/hwcap.h: ARMv7, Thumb2, VFPv3 D32,
  // NEON, TLS, and integer division. No ARMv8 crypto/FP16 promises.
  // Linux arm64 COMPAT_HWCAP: HALF, THUMB, FASTMULT, VFP, EDSP, NEON,
  // VFPv3, TLS, VFPv4, IDIVA, IDIVT, VFPD32 and LPAE.
  const CpuBackendInfo information{"Venture JIT A32", 0x001fb0d6, 0};
  std::mutex mutex;
  std::vector<A32Cpu*> cpus;
  Dynarmic::ExclusiveMonitor monitor;
  A32MemoryQuery query;
  const bool use_global_monitor;
  const bool enable_fastmem;
  std::size_t peak = 0;
  const CpuBackendInfo& info() const override { return information; }
  std::unique_ptr<Cpu> create_cpu() override;
  void invalidate(GuestAddr address, std::uint64_t size, InvalidationCause cause) override;
};
class A32Cpu final : public Cpu, private Dynarmic::A32::UserCallbacks {
 public:
  A32Cpu(A32Backend& backend, std::size_t slot) : backend_(backend), slot_(slot) {
    if (profile::enabled) Dynarmic::SetCodeRegistrationCallback(profile::register_translation);
    if (profile::compile_timing_enabled || JitBlockList::enabled()) Dynarmic::SetCompileTimingCallback(JitBlockList::compilation);
    Dynarmic::A32::UserConfig config{};
    // Diagnostics: VENTURE_JIT_DISABLED_HOST_FEATURES=<mask> hides host ISA extensions
    // (dynarmic HostFeature bits, e.g. 0xfe0 for every AVX-512 subset) from the emitter.
    if (const auto* mask = environment::getenv("VENTURE_JIT_DISABLED_HOST_FEATURES"))
      config.disabled_host_features = static_cast<std::uint32_t>(std::strtoul(mask, nullptr, 0));
    config.callbacks = this;
    config.processor_id = slot;
    config.global_monitor = &backend.monitor;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    // Wide host loads near UINT32_MAX must not reach a host-owned allocation
    // above 4 GiB. Dynarmic's callbacks check the complete guest range instead.
    if (backend.enable_fastmem && platform::reserve_low_memory_guard()) {
      config.fastmem_pointer = 0;
      config.fastmem_exclusive_access = true;
      // Exclusive loads as plain MOVs; VENTURE_JIT_LOCKED_EXCLUSIVE_LOADS=1 restores LOCK XADD.
      if (const auto* locked = environment::getenv("VENTURE_JIT_LOCKED_EXCLUSIVE_LOADS"); !locked || std::string_view(locked) != "1")
        config.plain_exclusive_loads = true;
      // Plain STREX of the loaded value succeeds without the locked store;
      // VENTURE_JIT_EXCLUSIVE_ELISION=0 keeps it.
      if (const auto* elision = environment::getenv("VENTURE_JIT_EXCLUSIVE_ELISION"); !elision || std::string_view(elision) != "0")
        config.elide_unchanged_exclusive_stores = true;
    }
    config.recompile_on_fastmem_failure = true;
    config.recompile_on_exclusive_fastmem_failure = true;
    config.check_halt_on_memory_access = true;
    if (const auto* lane = environment::getenv("VENTURE_JIT_LOW_LANE_ALIAS");
        !lane || std::string_view(lane) != "1")
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
    // Match main's value-based exclusive stores (no cross-CPU ABA tracking).
    if (!backend.use_global_monitor) {
      config.unsafe_optimizations = true;
      config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    }
    // As on A64: skip per-operation NaN fixups on the
    // default-NaN-off path; only NaN sign and payload bits can differ from ARM.
    // VENTURE_JIT_A32_ACCURATE_NAN=1 restores them for comparisons.
    if (const auto* nan = environment::getenv("VENTURE_JIT_A32_ACCURATE_NAN"); !nan || std::string_view(nan) != "1") {
      config.unsafe_optimizations = true;
      config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    }
    config.always_little_endian = true;
    config.define_unpredictable_behaviour = true;
    config.enable_cycle_counting = false;
    // Default omission; 1 restores the diagnostic chain. Keep the old A32
    // experiment spelling so already-pinned measurement arms remain usable.
    const auto* chain = environment::getenv("VENTURE_JIT_FRAME_CHAIN");
    if (!chain) chain = environment::getenv("VENTURE_JIT_A32_FRAME_CHAIN");
    if (chain) {
      config.enable_block_frame_chain = std::strcmp(chain, "0") != 0;
      log::info("jit-a32-frame", "per-block frame chain {}",
                config.enable_block_frame_chain ? "enabled" : "disabled");
    }
    config.svc_fp_transfer = kSvcHleCallFastFp;
    config.svc_fast = kSvcHleCallFast;
    // Diagnostic comparison uses the same binary and JIT configuration.
    const auto* thunk_metadata = environment::getenv("VENTURE_HLE_THUNK_METADATA");
    if (!thunk_metadata || std::string_view(thunk_metadata) != "0")
      config.svc_lookup = [](VAddr entry, std::uint32_t svc) { return hle::resolve_thunk(entry, svc); };
    config.wall_clock_cntpct = true;
    // As on A64, retain load-time translations beyond 128 MiB. This bounded
    // reservation commits pages only as code is emitted, including on cold threads.
    config.code_cache_size = 256 * 1024 * 1024;
    for (unsigned i = 0; i < 16; ++i) config.coprocessors[i] = std::make_shared<Coprocessor>(*this, i);
    jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
    jit_->SetCpsr(0x10); // User mode, ARM, little endian.
    cache_begin_ = reinterpret_cast<std::uintptr_t>(jit_->CodeCacheBegin());
    cache_end_ = cache_begin_ + jit_->CodeCacheSize();
  }
  ~A32Cpu() override {
    JitBlockList::retire(std::move(block_list_));
    std::lock_guard lock(backend_.mutex);
    backend_.monitor.ClearProcessor(slot_);
    backend_.cpus[slot_] = nullptr;
  }
  CpuExit run() override { return execute(false); }
  CpuExit step() override { return execute(true); }
  CpuExit execute(bool single_step) {
    std::optional<JitBlockList::Scope> recording;
    if (hints_) recording.emplace(&block_list_);
    if (external_halt_.exchange(false)) { jit_->ClearHalt(HaltReason::UserDefined2); return CpuExit::HaltRequested; }
    for (;;) {
      invalidations_->enter([&](GuestAddr p, std::uint64_t n) { jit_->InvalidateCacheRange(static_cast<VAddr>(p), n); });
      exit_.reset();
      coprocessor_fault_.reset();
      HaltReason reason;
      try { reason = single_step ? jit_->Step() : jit_->Run(); }
      catch (...) { acknowledge_stop(); throw; }
      if (single_step) jit_->ClearHalt(HaltReason::Step);
      acknowledge_stop();
      if (coprocessor_fault_) {
        jit_->Regs() = coprocessor_fault_->regs;
        jit_->SetCpsr(coprocessor_fault_->cpsr);
      }
      if (exit_) return *exit_;
      if (Dynarmic::Has(reason, HaltReason::UserDefined2)) { external_halt_.store(false); return CpuExit::HaltRequested; }
      if (Dynarmic::Has(reason, HaltReason::MemoryAbort)) return CpuExit::MemoryFault;
      if (single_step) return CpuExit::HaltRequested;
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
      block_list_.prepare(entry, true, [&](std::uint64_t descriptor) {
        if (!entered) { invalidations_->enter([&](GuestAddr p, std::uint64_t n) { jit_->InvalidateCacheRange(static_cast<VAddr>(p), n); }); entered = true; }
        return !external_halt_.load() && jit_->Precompile(descriptor);
      });
    }
    catch (...) { if (entered) acknowledge_stop(); throw; }
    if (entered) acknowledge_stop();
  }
  std::pair<std::shared_ptr<InvalidationGate>, std::uint64_t> queue_invalidation(GuestAddr p, std::uint64_t n) {
    return {invalidations_, invalidations_->queue(p, n, [&] { jit_->HaltExecution(HaltReason::UserDefined3); })};
  }
  GuestIsa isa() const override { return GuestIsa::a32; }
  std::uint64_t lr() const override { return jit_->Regs()[14]; }
  void set_lr(std::uint64_t value) override { jit_->Regs()[14] = narrow(value); }
  std::uint64_t hle_call_id() const override { return jit_->Regs()[12]; }
  std::uint64_t syscall_number() const override { return jit_->Regs()[7]; }
  std::uint64_t frame_pointer() const override { return jit_->Regs()[(jit_->Cpsr() & 0x20) ? 7 : 11]; }
  std::uint32_t trap_instruction_size() const override { return svc_width_; }
  std::uint32_t svc_immediate() const override { return svc_; }
  CpuFault fault() const override { return fault_; }
  std::uint64_t x(int i) const override { return jit_->Regs().at(i); }
  void set_x(int i, std::uint64_t value) override { jit_->Regs().at(i) = static_cast<std::uint32_t>(value); }
  std::uint64_t sp() const override { return jit_->Regs()[13]; }
  void set_sp(std::uint64_t value) override { jit_->Regs()[13] = narrow(value); }
  std::uint64_t pc() const override { return jit_->Regs()[15]; }
  void set_pc(std::uint64_t value) override {
    const auto p = narrow(value);
    jit_->SetCpsr((jit_->Cpsr() & ~0x20u) | ((p & 1) << 5));
    jit_->Regs()[15] = p & ((p & 1) ? ~1u : ~3u);
  }
  std::uint32_t pstate() const override { return jit_->Cpsr(); }
  void set_pstate(std::uint32_t value) override {
    if (value & 0x200) throw std::invalid_argument("A32 big-endian state is unsupported");
    jit_->SetCpsr(value);
  }
  // A32 exposes FPSCR as the control word; there is no separate FPSR.
  std::uint32_t fpcr() const override { return jit_->Fpscr(); }
  void set_fpcr(std::uint32_t value) override { jit_->SetFpscr(value); }
  std::uint32_t fpsr() const override { return jit_->Fpscr(); }
  void set_fpsr(std::uint32_t value) override { jit_->SetFpscr(value); }
  // Shared v(i).lo denotes D<i>; A32 has 32 D registers, not 32 Q registers.
  Vec128 v(int i) const override {
    if (i < 0 || i >= 32) throw std::out_of_range("A32 D register");
    return {jit_->ExtRegs()[2 * i] | (std::uint64_t{jit_->ExtRegs()[2 * i + 1]} << 32), 0};
  }
  void set_v(int i, Vec128 value) override {
    if (i < 0 || i >= 32) throw std::out_of_range("A32 D register");
    jit_->ExtRegs()[2 * i] = static_cast<std::uint32_t>(value.lo);
    jit_->ExtRegs()[2 * i + 1] = static_cast<std::uint32_t>(value.lo >> 32);
  }
  std::uint64_t tpidr() const override { return tprw_; }
  void set_tpidr(std::uint64_t value) override { tprw_ = narrow(value); }
  std::uint64_t tpidrro() const override { return tpro_; }
  void set_tpidrro(std::uint64_t value) override { tpro_ = narrow(value); }
  void clear_exclusive() override { jit_->ClearExclusiveState(); backend_.monitor.ClearProcessor(slot_); }
  CpuProfileSnapshot profile_snapshot(std::uintptr_t ip) const noexcept override {
    return {jit_->Regs()[15], ip >= cache_begin_ && ip < cache_end_};
  }
 private:
  static std::uint32_t narrow(std::uint64_t value) {
    if (value > UINT32_MAX) throw std::overflow_error("A32 address exceeds 4 GiB");
    return static_cast<std::uint32_t>(value);
  }
  A32Backend& backend_;
  std::size_t slot_;
  std::uint32_t tprw_{}, tpro_{};
  std::unique_ptr<Dynarmic::A32::Jit> jit_;
  std::uintptr_t cache_begin_ = 0, cache_end_ = 0;
  std::optional<CpuExit> exit_;
  CpuFault fault_{};
  std::uint32_t svc_{};
  std::uint32_t svc_width_ = 4;
  struct CoprocessorFault { std::array<std::uint32_t, 16> regs; std::uint32_t cpsr; };
  std::optional<CoprocessorFault> coprocessor_fault_;
  struct FaultSite { A32Cpu* cpu; VAddr pc; std::uint32_t mode; };
  std::map<std::uint64_t, FaultSite> fault_sites_;
  VAddr compiling_pc_{};
  std::uint32_t compiling_mode_{};
  std::shared_ptr<InvalidationGate> invalidations_ = std::make_shared<InvalidationGate>();
  std::atomic<bool> external_halt_{false};
  JitBlockList block_list_;
  const bool hints_ = JitBlockList::enabled(); // Read once per CPU, not per run.
  void acknowledge_stop() { invalidations_->leave(); }
  bool accessible(VAddr p, std::size_t n, std::uint32_t prot) const {
    return n <= (1ull << 32) - p && backend_.query(p, n, prot);
  }
  void memory_fault(VAddr p, bool write) {
    if (!exit_) { exit_ = CpuExit::MemoryFault; fault_ = {p, write, false}; }
    jit_->HaltExecution(HaltReason::MemoryAbort);
  }
  template<class T> T read(VAddr p) {
    T value{};
    if (!accessible(p, sizeof(T), kProtRead) || !platform::read_memory(p, &value, sizeof(T)))
      memory_fault(p, false);
    return value;
  }
  template<class T> void write(VAddr p, T value) {
    if (!accessible(p, sizeof(T), kProtWrite) || !platform::guarded_store(p, &value, sizeof(T)))
      memory_fault(p, true);
  }
  template<class T> bool exclusive(VAddr p, T value, T expected) {
    // Dynarmic may hold the monitor here. Use its existing fault-contained
    // host atomic, without an arena mutation lock or recursive monitor lock.
    if ((p & (sizeof(T) - 1)) || !accessible(p, sizeof(T), kProtRead | kProtWrite)) {
      memory_fault(p, true); return false;
    }
    std::uint64_t seen = 0;
    if (!platform::atomic_rmw(p, sizeof(T), platform::AtomicRmw::compare_exchange, expected, value, seen)) {
      memory_fault(p, true); return false;
    }
    return seen == expected;
  }
  std::optional<std::uint32_t> MemoryReadCode(VAddr p) override {
    if (!accessible(p, 4, kProtRead | kProtExec)) return std::nullopt;
    std::uint32_t word;
    // Thumb instructions may start on a halfword rather than a word boundary.
    if (!read_code_word(p, word)) return std::nullopt;
    return word;
  }
  static bool read_code_word(VAddr p, std::uint32_t& word) {
    return (p & 3) ? platform::read_memory(p, &word, sizeof(word))
                   : platform::guarded_load32(p, word);
  }
  bool IsReadOnlyMemory(VAddr p) override {
    return accessible(p, 1, kProtRead | kProtExec) && !accessible(p, 1, kProtWrite);
  }
  // Coprocessor callbacks in this pin have no immediate halt check. Isolate
  // each non-VFP coprocessor instruction, preserving ARM conditions and Thumb
  // IT translation, so an unsupported operation cannot execute a later store.
  bool is_coprocessor(bool thumb, VAddr p) {
    if (!accessible(p, 4, kProtRead | kProtExec)) return false;
    std::uint32_t word;
    if (!read_code_word(p, word)) return false;
    if (thumb) word = (word << 16) | (word >> 16);
    return (word & 0x0c000000) == 0x0c000000 && (word & 0x0f000000) != 0x0f000000 &&
        ((word >> 8) & 14) != 10 && (!thumb || (word & 0xec000000) == 0xec000000);
  }
  bool PreCodeReadHook(bool thumb, VAddr p, Dynarmic::A32::IREmitter& ir) override {
    const auto start = Dynarmic::A32::LocationDescriptor{ir.block.Location()}.PC();
    if (p != start && is_coprocessor(thumb, p)) {
      ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::LinkBlock{ir.current_location}});
      return false;
    }
    return true;
  }
  bool PostCodeTranslationHook(bool thumb, VAddr p, Dynarmic::A32::IREmitter& ir) override {
    if (!is_coprocessor(thumb, p)) return true;
    // A pre-translation terminal cannot stop conditional instructions: their
    // decoder may overwrite it. End the block after condition selection. Keep
    // the fault location out of line (the pin's emitted SetCpsr drops IT bits).
    compiling_pc_ = p;
    compiling_mode_ = ir.current_location.CPSR().Value();
    auto next = ir.current_location.AdvancePC(4);
    if (thumb) next = next.AdvanceIT();
    ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::LinkBlock{next}});
    return false;
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
  void MemoryWrite(VAddr p, std::uint64_t v, std::size_t size) override {
    switch (size) {
      case 1: return write(p, static_cast<std::uint8_t>(v));
      case 2: return write(p, static_cast<std::uint16_t>(v));
      case 4: return write(p, static_cast<std::uint32_t>(v));
      case 8: return write(p, v);
      default: memory_fault(p, true);
    }
  }
  bool MemoryWriteExclusive8(VAddr p, std::uint8_t v, std::uint8_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive16(VAddr p, std::uint16_t v, std::uint16_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive32(VAddr p, std::uint32_t v, std::uint32_t e) override { return exclusive(p, v, e); }
  bool MemoryWriteExclusive64(VAddr p, std::uint64_t v, std::uint64_t e) override { return exclusive(p, v, e); }
  void CallSVC(std::uint32_t immediate) override {
    svc_width_ = (jit_->Cpsr() & 0x20) ? 2 : 4;
    svc_ = immediate & 0x00ffffff; exit_ = CpuExit::Svc; jit_->HaltExecution();
  }
  void CallSVCFast(std::uint32_t immediate) override {
    svc_width_ = (jit_->Cpsr() & 0x20) ? 2 : 4;
    svc_ = immediate & 0x00ffffff;
    if (dispatch_hle_in_jit()) return;
    exit_ = has_hle_exception() ? CpuExit::HleException : CpuExit::Svc;
    jit_->HaltExecution();
  }
  void CallSVCFastKnown(std::uint32_t immediate, const void* metadata, void* state) override {
    svc_ = immediate & 0x00ffffff;
    auto& registers = *static_cast<Dynarmic::Backend::X64::A32JitState*>(state);
    if (dispatch_hle_in_jit(*static_cast<const hle::FunctionInfo*>(metadata),
                           hle::Registers(registers.Reg.data()))) return;
    exit_ = has_hle_exception() ? CpuExit::HleException : CpuExit::Svc;
    jit_->HaltExecution();
  }
  void ExceptionRaised(VAddr p, Dynarmic::A32::Exception e) override {
    using E = Dynarmic::A32::Exception;
    jit_->Regs()[15] = p;
    if (e == E::NoExecuteFault) { exit_ = CpuExit::MemoryFault; fault_ = {p, false, true}; }
    else exit_ = e == E::Breakpoint ? CpuExit::Breakpoint : CpuExit::UndefinedInstruction;
    jit_->HaltExecution();
  }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1ull << 60; }
  class Coprocessor final : public Dynarmic::A32::Coprocessor {
   public:
    Coprocessor(A32Cpu& cpu, unsigned number) : cpu_(cpu), number_(number) {}
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return unsupported(); }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc, CoprocReg n, CoprocReg m, unsigned opc2) override {
      if (number_ == 15 && !two && opc == 0) {
        if (n == CoprocReg::C13 && m == CoprocReg::C0 && opc2 == 2) return &cpu_.tprw_;
        // ARMv7 CP15 encodings for ISB, DSB and DMB, not cache maintenance.
        if (n == CoprocReg::C7 && ((m == CoprocReg::C5 && opc2 == 4) ||
            (m == CoprocReg::C10 && (opc2 == 4 || opc2 == 5)))) return Callback{fence, &cpu_};
      }
      return unsupported();
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return unsupported(); }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc, CoprocReg n, CoprocReg m, unsigned opc2) override {
      if (number_ == 15 && !two && opc == 0) {
        if (n == CoprocReg::C13 && m == CoprocReg::C0) {
          if (opc2 == 2) return &cpu_.tprw_;
          if (opc2 == 3) return &cpu_.tpro_;
        }
        if (n == CoprocReg::C14 && m == CoprocReg::C0 && opc2 == 0) return Callback{frequency, &cpu_};
      }
      return unsupported();
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool two, unsigned opc, CoprocReg m) override {
      if (number_ == 15 && !two && opc == 1 && m == CoprocReg::C14) return Callback{counter, &cpu_};
      return unsupported();
    }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return unsupported(); }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return unsupported(); }
   private:
    A32Cpu& cpu_;
    unsigned number_;
    Callback unsupported() {
      auto key = cpu_.compiling_pc_ | (std::uint64_t{cpu_.compiling_mode_} << 32);
      auto [it, inserted] = cpu_.fault_sites_.try_emplace(key, FaultSite{&cpu_, cpu_.compiling_pc_, cpu_.compiling_mode_});
      return {undefined, &it->second};
    }
    static std::uint64_t undefined(void* p, std::uint32_t, std::uint32_t) {
      auto& site = *static_cast<FaultSite*>(p);
      auto& c = *site.cpu;
      constexpr auto mask = Dynarmic::A32::LocationDescriptor::CPSR_MODE_MASK;
      c.coprocessor_fault_ = CoprocessorFault{c.jit_->Regs(), (c.jit_->Cpsr() & ~mask) | site.mode};
      c.coprocessor_fault_->regs[15] = site.pc;
      c.exit_ = CpuExit::UndefinedInstruction;
      c.jit_->HaltExecution();
      return 0;
    }
    static std::uint64_t fence(void*, std::uint32_t, std::uint32_t) { std::atomic_thread_fence(std::memory_order_seq_cst); return 0; }
    static std::uint64_t frequency(void*, std::uint32_t, std::uint32_t) { return 1'000'000'000; }
    static std::uint64_t counter(void*, std::uint32_t, std::uint32_t) { return monotonic_ns(); }
  };
};
std::unique_ptr<Cpu> A32Backend::create_cpu() {
  std::lock_guard lock(mutex);
  auto it = std::find(cpus.begin(), cpus.end(), nullptr);
  if (it == cpus.end()) {
    log::warn("cpu", "A32 exclusive monitor slots exhausted ({})", cpus.size());
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again), "A32 exclusive monitor slots exhausted");
  }
  auto cpu = std::make_unique<A32Cpu>(*this, static_cast<std::size_t>(it - cpus.begin()));
  *it = cpu.get();
  const auto live = cpus.size() - std::count(cpus.begin(), cpus.end(), nullptr);
  if (live > peak) { peak = live; log::info("cpu", "A32 live CPUs peak {}/{}", peak, cpus.size()); }
  return cpu;
}
void A32Backend::invalidate(GuestAddr p, std::uint64_t n, InvalidationCause cause) {
  if (!n || p >= (1ull << 32)) return;
  n = std::min<std::uint64_t>(n, (1ull << 32) - p);
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
std::atomic<A32MemoryQuery> memory_query{nullptr};
}
void set_a32_memory_query(A32MemoryQuery query) {
  if (!query) throw std::invalid_argument("null A32 memory query");
  A32MemoryQuery empty = nullptr;
  if (!memory_query.compare_exchange_strong(empty, query) && empty != query)
    throw std::logic_error("A32 memory query is immutable once installed");
}
std::unique_ptr<CpuBackend> make_a32_backend(A32BackendConfig config) {
  if (!config.memory_query) throw std::invalid_argument("A32 requires the low arena's lock-free memory query");
  if (!config.monitor_slots || config.monitor_slots > 65535) throw std::invalid_argument("A32 monitor slots must be in [1,65535]");
  return std::make_unique<A32Backend>(config);
}
CpuBackend& a32_cpu_backend() {
  static auto* backend = [] {
    A32BackendConfig config{kGuestCpuSlots, memory_query.load()};
    log::info("cpu", "A32 memory uses {}", platform::reserve_low_memory_guard()
        ? "identity fastmem with a reserved wrap guard" : "checked callbacks (wrap guard unavailable)");
    if (const auto* value = venture::environment::getenv("VENTURE_A32_MONITOR_SLOTS")) {
      const auto* end = value + std::strlen(value);
      auto result = std::from_chars(value, end, config.monitor_slots);
      if (result.ec != std::errc{} || result.ptr != end) throw std::invalid_argument("invalid VENTURE_A32_MONITOR_SLOTS");
    }
    return make_a32_backend(config).release();
  }();
  return *backend;
}
} // namespace venture
