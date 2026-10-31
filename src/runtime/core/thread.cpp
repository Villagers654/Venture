#include "core/pc_trace.h"
#include "platform/runtime_child.h"
#include "core/process_exit.h"
#include "core/os_quit.h"
#include "core/av_telemetry.h"
#include "core/frame_cpu.h"
#include "core/thread_runtime.h"
#include "core/registry_lock.h"
#if defined(__SSE__) || defined(_M_X64)
#include <xmmintrin.h>
#endif
#include "core/profile.h"
#include "core/invalidation_stats.h"
#include "core/xr_call_trace.h"
#include "core/thread_api.h"
#include "core/address_space.h"
#include "core/memory_metadata.h"
#include "core/error.h"
#include "core/hle.h"
#include "core/engine_policy.h"
#include "core/hle_runtime.h"
#include "core/hle_unwind.h"
#include "core/signals.h"
#include "core/signal_frame.h"
#include "core/syscall.h"
#include "core/tls.h"
#include "core/process.h"
#include "core/process_image.h"
#include "core/guest_heap.h"
#include "core/wait.h"
#include "cpu/state.h"
#include "cpu/block_list.h"
#include "core/guest_call32.h"
#include "common/log.h"
#include "common/environment.h"
#include "common/first_frame_progress.h"
#include "platform/memory.h"
#include "platform/crash_report.h"
#include "platform/thread_priority.h"
#include "platform/timed_wait.h"
#include <bit>
#include <bitset>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace venture {
namespace {
thread_local GuestThread* current_thread = nullptr;
const bool wait_unwind_counts = environment::getenv("VENTURE_WAIT_UNWIND_COUNTS") != nullptr;
const bool registry_tracking = startup_diagnostics::enabled || startup_diagnostics::first_frame_timeout_seconds() > 0;
RegistryMutex registry_mutex{registry_tracking};
std::map<std::int32_t, GuestThread*> live_threads;
std::atomic<std::int32_t> next_tid{1000};
std::bitset<65536> mutex_owner_indices; // guarded by registry_mutex; zero is unowned
std::uint16_t acquire_mutex_owner_index() {
  for (unsigned i = 1; i < mutex_owner_indices.size(); ++i)
    if (!mutex_owner_indices[i]) { mutex_owner_indices.set(i); return static_cast<std::uint16_t>(i); }
  throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
}
std::atomic<int> process_exit_status{-1};
std::atomic<bool> terminal_process_exit{false};
void clear_tid(GuestThread& thread) {
  if (!thread.clear_child_tid) return;
  const std::uint32_t zero = 0;
  platform::write_memory(thread.clear_child_tid, &zero, sizeof(zero));
  futex_wake(thread.clear_child_tid, 0x7fffffff);
  thread.clear_child_tid = 0;
}
std::atomic<void (*)(GuestThread&)> thread_exit_handler{nullptr};
std::atomic<void (*)()> native_thread_cleanup{nullptr};
struct Resources {
  GuestAddr stack_allocation{};
  bool owns_pthread = false;
  GuestThread* previous{};
  GuestThreadOptions options;
  std::string name = "guest";
  bool finished = false;
};
std::map<GuestThread*, Resources> resources;
struct ManagedThread {
  explicit ManagedThread(int tid) : guest(tid) {}
  GuestThread guest;
  std::atomic<std::uint32_t> done{0};
  std::uint64_t result{};
  bool detached{}, joining{};
  std::exception_ptr failure;
};
// Never destroyed: guest threads still registered at exit() would otherwise
// release TLS blocks after another translation unit's statics are gone.
auto& managed_threads = *new std::map<GuestAddr, std::shared_ptr<ManagedThread>>;
std::condition_variable_any workers_finished;
std::size_t active_workers = 0;
bool shutting_down = false;
// A signal handler may longjmp into its interrupted code, bypassing rt_sigreturn.
// Reaching the enclosing host-call trampoline then unwinds any intervening HLE frames.
struct GuestCallReturned { int depth; };
}
GuestThread* GuestThread::current() { return current_thread; }
GuestThread& GuestThread::adopt_current() {
  if (current_thread) return *current_thread;
  // The heap's TLS owner must outlive adopted's destructor, which can run
  // guest TLS destructors that allocate or free memory during host-thread exit.
  if (process_config().isa() == GuestIsa::a32) guest_heap::prepare_thread();
  thread_local std::unique_ptr<GuestThread> adopted;
  if (!adopted) adopted = std::make_unique<GuestThread>(next_tid.fetch_add(1));
  adopted->attach_to_current_host_thread();
  return *adopted;
}
GuestThread::GuestThread(std::int32_t tid)
    : call_impl_(&GuestThread::call64), run_impl_(&GuestThread::run_for_isa<GuestIsa::a64>), tid_(tid) {
  if (process_config().isa() == GuestIsa::a32) {
    auto p = AddressSpace::get().allocate(4096, kProtRead | kProtWrite, "[errno]");
    if (!p) throw std::bad_alloc();
    errno_address_ = g2h<int>(p);
  }
  if (shutdown_trace::mutex_watch_enabled) mutex_watch_records = std::make_unique<shutdown_trace::MutexWatchRecords<4096>>();
  if (shutdown_trace::word_watch_enabled) word_watch_records = std::make_unique<shutdown_trace::WatchRecords<shutdown_trace::WordTransitionEvent, 1024>>();
  RegistryLock lock(registry_mutex); resources.emplace(this, Resources{});
}
GuestThread::~GuestThread() {
  process_image::thread_detached(*this);
  if (current_thread == this && cpu_) {
    try { finish_guest_thread(*this); }
    catch (const std::exception& e) { log::error("thread", "thread cleanup failed: {}", e.what()); }
    catch (...) { log::error("thread", "thread cleanup failed"); }
  }
  frame_cpu::detach(tid_);
  profile::detach(*this);
  RegistryLock lock(registry_mutex);
  if (mutex_owner_index_) mutex_owner_indices.reset(mutex_owner_index_);
  clear_tid(*this);
  if (auto it = live_threads.find(tid_); it != live_threads.end() && it->second == this) {
    platform::unbind_thread_priority(tid_);
    live_threads.erase(it);
  }
  signals::forget_thread(*this);
  tls::detach(*this);
  if (errno_address_ != &errno_) AddressSpace::get().free(h2g(errno_address_));
  auto it = resources.find(this);
  if (it != resources.end()) {
    if (current_thread == this) current_thread = it->second.previous;
    if (it->second.stack_allocation) AddressSpace::get().free(it->second.stack_allocation);
    if (it->second.owns_pthread) AddressSpace::get().free(pthread_self);
    resources.erase(it);
  }
}
void GuestThread::attach_to_current_host_thread() {
  attach_locked();
  process_image::thread_attached(*this);
}
void GuestThread::attach_locked() {
  platform::prepare_crash_stack();
  RegistryLock lock(registry_mutex);
  std::lock_guard cpu_lock(cpu_mutex_);
  if (cpu_) { if (current_thread != this) throw std::runtime_error("guest thread already attached"); return; }
  if (live_threads.contains(tid_)) throw std::runtime_error("duplicate guest tid");
  auto& r = resources.at(this);
  if (!stack_base) {
    if (!stack_size) stack_size = 1024 * 1024;
    r.stack_allocation = AddressSpace::get().allocate(stack_size + 4096, kProtNone, "[stack]");
    if (!r.stack_allocation) throw std::bad_alloc();
    stack_base = r.stack_allocation + 4096;
    if (AddressSpace::get().protect(stack_base, stack_size, kProtRead | kProtWrite)) throw std::bad_alloc();
    // As bionic names it: memory scanners skip the guard, not the stack.
    label_memory(r.stack_allocation, 4096, "[anon:thread stack guard]", 0);
  }
  if (!pthread_self) {
    pthread_self = AddressSpace::get().allocate(kPthreadKeyValuesOffset + kPthreadKeyCount * sizeof(PthreadKeyValue),
                                                kProtRead | kProtWrite, "[pthread handle]");
    if (!pthread_self) throw std::bad_alloc();
    r.owns_pthread = true;
    pthread_values = g2h<PthreadKeyValue>(pthread_self + kPthreadKeyValuesOffset);
  }
  cpu_ = cpu_backend().create_cpu();
  cpu_->bind_thread(*this);
  if (cpu_->isa() == GuestIsa::a32) {
    call_impl_ = &GuestThread::call32;
    run_impl_ = &GuestThread::run_for_isa<GuestIsa::a32>;
  }
  if (profile::enabled) run_impl_ = cpu_->isa() == GuestIsa::a32
      ? &GuestThread::run_for_isa<GuestIsa::a32, true> : &GuestThread::run_for_isa<GuestIsa::a64, true>;
  cpu_->set_sp(align_down(stack_base + stack_size, 16));
  tls::attach(*this);
  if (cpu_->isa() == GuestIsa::a32 && !mutex_owner_index_)
    mutex_owner_index_ = acquire_mutex_owner_index();
  platform::bind_thread_priority(tid_);
  r.previous = current_thread;
  current_thread = this;
  live_threads.emplace(tid_, this);
  frame_cpu::attach(tid_);
  profile::attach(*this);
  hle::census_guest_threads(live_threads.size());
}
GuestThread::ConditionWaitCleanup::~ConditionWaitCleanup() {
  if (wait_unwind_counts && occurrence)
    log::info("wait-unwind", "tid={} occurrence={} instructions={}", GuestThread::current() ? GuestThread::current()->tid() : -1, occurrence, instructions);
}
void GuestThread::count_wait_unwind_instruction() {
  for (auto* action = condition_wait_cleanup; action; action = action->previous) {
    if (!action->pending) continue;
    if (!action->instructions) action->occurrence = ++wait_unwind_occurrences;
    ++action->instructions; ++wait_unwind_instructions;
    break;
  }
}
void GuestThread::detach_from_current_host_thread() {
  if (wait_unwind_counts && cpu_ && cpu_->isa() == GuestIsa::a32)
    log::info("wait-unwind", "tid={} total_occurrences={} total_instructions={}", tid_, wait_unwind_occurrences, wait_unwind_instructions);
  guest_heap::DetachedThreadHeap retired_heap;
  JitBlockList retired_hints;
  if (current_thread == this) process_image::thread_detached(*this);
  {
    RegistryLock lock(registry_mutex);
    std::lock_guard cpu_lock(cpu_mutex_);
    if (current_thread != this) throw std::runtime_error("guest detach on wrong host thread");
    frame_cpu::detach(tid_);
    profile::detach(*this);
    live_threads.erase(tid_);
    platform::unbind_thread_priority(tid_);
    current_thread = resources.at(this).previous;
    if (cpu_) retired_hints = cpu_->detach_jit_hints();
    cpu_.reset();
    if (mutex_owner_index_) {
      mutex_owner_indices.reset(mutex_owner_index_);
      mutex_owner_index_ = 0;
    }
    if (process_config().isa() == GuestIsa::a32) retired_heap = guest_heap::detach_thread();
  }
  // Persistence runs on the background writer, never on this retiring thread.
  JitBlockList::retire(std::move(retired_hints));
  // Allocator collection can be slow. Both registry and CPU locks are released
  // before destroying the heap; completion still follows the full cleanup.
}
GuestCallResult GuestThread::call(GuestAddr fn, const GuestCallArgs& args) {
  return (this->*call_impl_)(fn, args);
}
// A callback into a guest running on a stack the app made itself (a coroutine or
// fiber stack, e.g. UE4 ctx::continuation in RE4) keeps using that stack, as on
// Android, when the words it pushes are writable guest memory.
static void app_stack_bounds(GuestAddr sp, std::uint64_t bytes, GuestAddr& base, std::uint64_t& size) {
  const auto need = bytes + 16;
  if (sp < need || !platform::memory_accessible(sp - need, need, true)) return;
  base = sp - need;
  size = need;
}
GuestCallResult GuestThread::call32(GuestAddr fn, const GuestCallArgs& args) {
  os_quit::Call executing(&os_call_depth);
  if (os_quit::requested()) throw ProcessExit(0);
  if (current_thread != this || !cpu_) throw std::runtime_error("guest call on wrong host thread");
  if (!fn) throw std::runtime_error("null guest call");
  if (!call_depth_ && JitBlockList::enabled()) cpu_->prepare_call(fn);
  CpuState32 saved(*cpu_);
  ++call_depth_;
  try {
    auto call_stack_base = stack_base;
    auto call_stack_size = stack_size;
    if (cpu_->sp() < stack_base || cpu_->sp() > stack_base + stack_size) {
      signals::AltStack alternate;
      signals::set_alt_stack(*this, nullptr, &alternate);
      if (!(alternate.flags & 2) && cpu_->sp() >= alternate.base &&
          cpu_->sp() <= alternate.base + alternate.size) {
        call_stack_base = alternate.base;
        call_stack_size = alternate.size;
      } else {
        app_stack_bounds(cpu_->sp(), 4 * args.integers.size() + 4 * args.stack.size(), call_stack_base, call_stack_size);
      }
    }
    prepare_guest_call32(*cpu_, fn, args, call_stack_base, call_stack_size, hle::call_return_trampoline(call_depth_));
    run_until_return(call_depth_);
    GuestCallResult result{cpu_->x(0), cpu_->x(1), {}, GuestIsa::a32};
    saved.restore(*cpu_); --call_depth_; return result;
  } catch (const GuestCallReturned&) { --call_depth_; throw; }
  catch (...) { signals::note_unwinding_guest_call(*this); saved.restore(*cpu_); --call_depth_; throw; }
}
GuestCallResult GuestThread::call64(GuestAddr fn, const GuestCallArgs& args) {
  os_quit::Call executing(&os_call_depth);
  if (os_quit::requested()) throw ProcessExit(0);
  if (current_thread != this || !cpu_) throw std::runtime_error("guest call on wrong host thread");
  if (!fn) throw std::runtime_error("null guest call");
  if (!call_depth_ && JitBlockList::enabled()) cpu_->prepare_call(fn);
  CpuState saved(*cpu_);
  ++call_depth_;
  try {
    auto int_stack = args.integers.size() > 8 ? args.integers.size() - 8 : 0;
    auto fp_stack = args.doubles.size() > 8 ? args.doubles.size() - 8 : 0;
    if (!args.stack.empty() && (int_stack || fp_stack))
      throw std::invalid_argument("explicit guest stack requires register-only argument spans");
    auto slots = args.stack.empty() ? int_stack + fp_stack : args.stack.size();
    auto call_stack_base = stack_base;
    auto call_stack_size = stack_size;
    if (cpu_->sp() < stack_base || cpu_->sp() > stack_base + stack_size)
      app_stack_bounds(cpu_->sp(), 8 * slots, call_stack_base, call_stack_size);
    if (slots > call_stack_size / 8) throw std::runtime_error("guest call stack overflow");
    auto sp = align_down(cpu_->sp() - 8 * slots, 16);
    if (sp < call_stack_base || sp > call_stack_base + call_stack_size) throw std::runtime_error("guest call stack overflow");
    cpu_->set_sp(sp);
    auto* overflow = g2h<std::uint64_t>(sp);
    for (auto word : args.stack) *overflow++ = word;
    for (std::size_t i = 0; i < args.integers.size(); ++i)
      if (i < 8) cpu_->set_x(static_cast<int>(i), args.integers[i]); else *overflow++ = args.integers[i];
    for (std::size_t i = 0; i < args.doubles.size(); ++i)
      if (i < 8) cpu_->set_v(static_cast<int>(i), {std::bit_cast<std::uint64_t>(args.doubles[i]), 0});
      else *overflow++ = std::bit_cast<std::uint64_t>(args.doubles[i]);
    cpu_->set_pc(fn); cpu_->set_lr(hle::call_return_trampoline(call_depth_));
    run_until_return(call_depth_);
    GuestCallResult result{cpu_->x(0), cpu_->x(1), cpu_->v(0)};
    saved.restore(*cpu_); --call_depth_; return result;
  } catch (const GuestCallReturned&) {
    // A guest longjmp has already restored an outer guest context. Preserve it while
    // unwinding intermediate HLE callbacks and host->guest calls.
    --call_depth_; throw;
  } catch (...) { signals::note_unwinding_guest_call(*this); saved.restore(*cpu_); --call_depth_; throw; }
}
void GuestThread::run_until_return(int depth) {
  // Guest FP status lives in the CPU. Keep the surrounding host environment
  // separate across the entire guest call, including its slow HLE dispatches.
  // Clearing inherited host flags once also avoids needless MXCSR changes on
  // each integer-only JIT exit/re-entry.
#if defined(__SSE__) || defined(_M_X64)
  const unsigned host_mxcsr = _mm_getcsr();
  _mm_setcsr(host_mxcsr & ~0x3fu);
  try { (this->*run_impl_)(depth); }
  catch (...) { _mm_setcsr(host_mxcsr); throw; }
  _mm_setcsr(host_mxcsr);
#else
  (this->*run_impl_)(depth);
#endif
}
template<GuestIsa Isa, bool Profile> void GuestThread::run_for_isa(int depth) {
  if constexpr (Profile) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) profile::mark("first_guest_native_code");
  }
  try {
    for (;;) {
      check_guest_exit(*this);
      signals::deliver_pending(*this);
      bool observe_wait_unwind = false;
      if constexpr (Isa == GuestIsa::a32) {
        // Exported EHABI unwinders reacquire through the cleanup personality.
        // Hidden unwinders and the opt-in census retain SP observation.
        for (auto* action = condition_wait_cleanup; action; action = action->previous) {
          if (!hle::legacy_wait_unwind_observation() && !action->observe_unwind) continue;
          if (action->pending && cpu_->sp() == action->caller_sp) {
            action->pending = false;
            action->acquire(action->argument);
          }
          observe_wait_unwind |= action->pending;
        }
      }
      const auto result = [&] {
        if (observe_wait_unwind && wait_unwind_counts) count_wait_unwind_instruction();
        if constexpr (Profile) { profile::Activity activity(profile::jit); return observe_wait_unwind ? cpu_->step() : cpu_->run(); }
        else return observe_wait_unwind ? cpu_->step() : cpu_->run();
      }();
      if (shutdown_trace::enabled) shutdown_trace::publish(*this);
      ++cpu_exits;
      if (startup_diagnostics::enabled) startup_diagnostics::sample(*this, *cpu_);
      switch (result) {
        case CpuExit::Svc:
          switch (cpu_->svc_immediate()) {
            case kSvcHleCallFast:
            case kSvcHleCallFastFp:
            case kSvcHleCallFastGuestFp:
            case kSvcHleCall: {
              const auto id = static_cast<hle::FunctionId>(cpu_->hle_call_id());
              if constexpr (Profile) { profile::Activity activity(id); hle::dispatch(*this, id); }
              else hle::dispatch(*this, id);
              break;
            }
            case kSvcLinuxSyscall: {
              bool sigreturn = cpu_->syscall_number() == (Isa == GuestIsa::a64 ? 139 : 173) ||
                  (Isa == GuestIsa::a32 && cpu_->syscall_number() == 119);
              if constexpr (Isa == GuestIsa::a64) linux_syscall(*this);
              else linux_syscall32(*this);
              if (sigreturn && depth < 0) return;
              break;
            }
            case kSvcEntryHook:
              if (!engine_policy::entry_hook(*this))
                signals::deliver_synchronous(*this, {signals::kSigSys, 1, cpu_->pc() - cpu_->trap_instruction_size()});
              break;
            case kSvcReturnToHost: {
              auto target = static_cast<int>(cpu_->hle_call_id());
              if (target <= 0 || target > call_depth_ || cpu_->pc() != hle::call_return_trampoline(target) + 8) target = call_depth_;
              if (target != depth) throw GuestCallReturned{target};
              return;
            }
            case signals::kSvcSigreturn:
              signals::return_from_signal(*this);
              if (depth < 0) return;
              break;
            default: signals::deliver_synchronous(*this, {signals::kSigSys, 1, cpu_->pc() - cpu_->trap_instruction_size()}); break;
          }
          break;
        case CpuExit::HleException:
          try { cpu_->rethrow_hle_exception(); }
          catch (const hle::MemoryFault& fault) {
            hle::deliver_memory_fault(*this, static_cast<hle::FunctionId>(cpu_->hle_call_id()), fault);
          }
          break;
        case CpuExit::HaltRequested: interrupted.store(false); break;
        case CpuExit::MemoryFault: {
          const auto fault = cpu_->fault();
          const auto access = fault.execute ? kProtExec : fault.write ? kProtWrite : kProtRead;
          if (!AddressSpace::get().grow_stack(fault.address, access)) {
            signals::Info info{signals::kSigSegv, 1, fault.address};
            info.syndrome = fault.write ? 1ull << 6 : 0;
            signals::deliver_synchronous(*this, info);
          }
          // Precise CPU faults preserve PC/registers. Successful VMA growth
          // retries that instruction without delivering a guest signal.
          break;
        }
        case CpuExit::UndefinedInstruction: signals::deliver_synchronous(*this, {signals::kSigIll, 1, cpu_->pc()}); break;
        case CpuExit::Breakpoint: signals::deliver_synchronous(*this, {signals::kSigTrap, 1, cpu_->pc()}); break;
      }
    }
  } catch (const GuestCallReturned& returned) { if (returned.depth != depth) throw; }
}
void GuestThread::interrupt() {
  interrupted.store(true); interrupt_epoch.fetch_add(1);
  { std::lock_guard lock(cpu_mutex_); if (cpu_) cpu_->request_halt(); }
  interrupt_waits(*this);
  signals::interrupt_stopped_threads();
}
void GuestThread::request_startup_sample() {
  { std::lock_guard lock(startup_state.mutex); startup_state.sample.pending = true; }
  std::unique_lock lock(cpu_mutex_, std::try_to_lock);
  if (lock.owns_lock() && cpu_) cpu_->request_halt();
}
RegistryMutex::Owner thread_registry_owner() { return registry_mutex.owner(); }
void GuestThread::request_shutdown_sample() {
  std::lock_guard lock(cpu_mutex_);
  if (cpu_) cpu_->request_halt();
}
void for_each_guest_thread(const std::function<void(GuestThread&)>& visit) {
  RegistryLock lock(registry_mutex);
  for (auto& [tid, thread] : live_threads) visit(*thread);
}
bool try_for_each_guest_thread(const std::function<void(GuestThread&)>& visit) {
  RegistryLock lock(registry_mutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  for (auto& [tid, thread] : live_threads) visit(*thread);
  return true;
}
std::vector<ManagedThreadSnapshot> managed_thread_snapshot() {
  RegistryLock lock(registry_mutex, std::try_to_lock);
  std::vector<ManagedThreadSnapshot> result;
  if (!lock.owns_lock()) return result;
  for (const auto& [handle, control] : managed_threads)
    result.push_back({control->guest.tid(), handle, h2g(&control->done), control->done.load(),
                      control->detached, control->joining, live_threads.contains(control->guest.tid())});
  return result;
}
std::shared_ptr<GuestThread> create_guest_thread(GuestAddr start, GuestAddr argument, const GuestThreadOptions& options) {
  if (!start || options.stack_size < 16384 || options.stack_size > (1ull << 40) ||
      options.guard_size > (1ull << 40) || (options.stack & 15)) return nullptr;
  auto control = std::make_shared<ManagedThread>(next_tid.fetch_add(1));
  auto& t = control->guest;
  t.stack_size = align_up(options.stack_size, 4096);
  t.stack_base = options.stack;
  control->detached = options.detached;
  { RegistryLock lock(registry_mutex); resources.at(&t).options = options; }
  if (!options.stack && options.guard_size != 4096) {
    auto guard = align_up(options.guard_size, 4096);
    auto allocation = AddressSpace::get().allocate(t.stack_size + guard, kProtNone, "[stack]");
    if (!allocation) return {};
    if (AddressSpace::get().protect(allocation + guard, t.stack_size, kProtRead | kProtWrite)) {
      AddressSpace::get().free(allocation); return {};
    }
    t.stack_base = allocation + guard;
    label_memory(allocation, guard, "[anon:thread stack guard]", 0);
    RegistryLock lock(registry_mutex); resources.at(&t).stack_allocation = allocation;
  }
  std::promise<bool> startup;
  auto ready = startup.get_future();
  RegistryLock creation_lock(registry_mutex);
  if (shutting_down) return {};
  ++active_workers;
  std::thread worker;
  try {
    worker = std::thread([control, start, argument, startup = std::move(startup)]() mutable {
    bool attached = false;
    try {
      control->guest.attach_to_current_host_thread();
      {
        RegistryLock lock(registry_mutex);
        managed_threads.emplace(control->guest.pthread_self, control);
        if (shutting_down) control->guest.exit_requested = true;
      }
      attached = true;
    } catch (...) { control->failure = std::current_exception(); }
    startup.set_value(attached);
    if (attached) {
      const auto [entry, entry_argument] = process_image::thread_entry(control->guest, start, argument);
      try { control->result = control->guest.call(entry, {entry_argument}); }
      catch (const ThreadExit& e) { control->result = e.value; }
      catch (const ProcessExit& e) {
        if (os_quit::requested()) log::info("thread", "OS quit stopped guest thread {}", control->guest.tid());
        else log::info("thread", "guest requested process exit {}", e.status);
      }
      catch (...) {
        // Android aborts the process on an uncaught native exception. A thread
        // that silently disappears instead leaves the game frozen (RE4's game thread).
        control->failure = std::current_exception();
        std::string name;
        { RegistryLock lock(registry_mutex); name = resources.at(&control->guest).name; }
        signals::log_guest_thread_failure(control->guest, name, control->failure);
        try { exit_guest_process(128 + signals::kSigAbrt); } catch (const ProcessExit&) {}
      }
    }
    if (attached) {
      try { finish_guest_thread(control->guest); }
      catch (...) { control->failure = std::current_exception(); }
    }
    clear_tid(control->guest);
    if (attached) control->guest.detach_from_current_host_thread();
    control->done.store(1, std::memory_order_release);
    futex_wake(h2g(&control->done), 0x7fffffff);
    RegistryLock lock(registry_mutex);
    live_threads.erase(control->guest.tid());
    if (control->detached) managed_threads.erase(control->guest.pthread_self);
    // Destroy detached resources before announcing that the host worker is drained.
    control.reset();
    --active_workers;
    workers_finished.notify_all();
    });
  } catch (...) { --active_workers; throw; }
  creation_lock.unlock();
  auto attached = ready.get();
  worker.detach();
  if (!attached && control->failure) {
    try { std::rethrow_exception(control->failure); }
    catch (const std::exception& e) { log::warn("thread", "guest thread could not start: {}", e.what()); }
    catch (...) { log::warn("thread", "guest thread could not start"); }
  }
  return attached ? std::shared_ptr<GuestThread>(control, &t) : nullptr;
}
int join_guest_thread(GuestAddr handle, std::uint64_t* result) {
  std::shared_ptr<ManagedThread> control;
  {
    RegistryLock lock(registry_mutex);
    auto it = managed_threads.find(handle); if (it == managed_threads.end()) return -err::kESRCH;
    control = it->second;
    if (&control->guest == GuestThread::current()) return -err::kEDEADLK;
    if (control->detached || control->joining) return -err::kEINVAL;
    control->joining = true;
  }
  auto* current = GuestThread::current();
  if (!current) { RegistryLock lock(registry_mutex); control->joining = false; return -err::kEINVAL; }
  struct Joining {
    std::shared_ptr<ManagedThread> control;
    ~Joining() { RegistryLock lock(registry_mutex); control->joining = false; }
  } joining{control};
  shutdown_trace::WaitScope wait_trace(*current, shutdown_trace::Wait::join, handle, 0, control->guest.tid());
  while (!control->done.load(std::memory_order_acquire)) {
    int r = futex_wait(*current, h2g(&control->done), 0, -1);
    if (r && r != -err::kEAGAIN && r != -err::kEINTR) {
      RegistryLock lock(registry_mutex); control->joining = false; return r;
    }
  }
  if (result) *result = control->result;
  { RegistryLock lock(registry_mutex); managed_threads.erase(handle); }
  if (control->failure) std::rethrow_exception(control->failure);
  return 0;
}
int detach_guest_thread(GuestAddr handle) {
  RegistryLock lock(registry_mutex);
  auto it = managed_threads.find(handle); if (it == managed_threads.end()) return -err::kESRCH;
  if (it->second->detached || it->second->joining) return -err::kEINVAL;
  it->second->detached = true;
  if (it->second->done.load(std::memory_order_acquire)) managed_threads.erase(it);
  return 0;
}
[[noreturn]] void exit_guest_thread(std::uint64_t result) { throw ThreadExit(result); }
void set_terminal_guest_process_exit(bool enabled) {
  if (enabled) log::prepare_terminal_handoff();
  terminal_process_exit.store(enabled);
}
bool terminal_guest_process_exit() { return terminal_process_exit.load(); }
namespace { std::atomic<int> failure_on_exit{0}; }
void record_guest_process_failure_on_exit(int status) {
  status &= 255;
  if (!status) return;
  int expected = 0;
  failure_on_exit.compare_exchange_strong(expected, status);
  // If another thread has already requested exit, it may be flushing now.
  // Upgrade that published outcome as well, so its final status cannot miss us.
  if (process_exit_status.load() >= 0) record_guest_process_failure(status);
}
void record_guest_process_failure(int status) {
  status &= 255;
  if (!status) return;
  auto previous = process_exit_status.load();
  // Guest signals/lifecycle grace are emulated; the host OS cannot classify
  // their failure for us. Upgrade the existing status used by OS process exit.
  // An observed failure upgrades pending/success, while retaining the first
  // nonzero outcome. Ordinary guest exits still use the first-request rule.
  while (previous <= 0 && !process_exit_status.compare_exchange_weak(previous, status)) {}
}
void interrupt_guest_process_after_grace() {
  // Host-enforced interruption is a failed quit even if Android later exits 0.
  record_guest_process_failure(124);
  for_each_guest_thread([](GuestThread& thread) { thread.exit_requested = true; thread.interrupt(); });
}
[[noreturn]] void end_guest_process_after_grace() {
  record_guest_process_failure(124);
  exit_guest_process(guest_process_exit_status());
}
namespace { void (*exit_capture_flush)() = nullptr; }
void set_guest_exit_capture_flush(void (*flush)()) { exit_capture_flush = flush; }
void flush_guest_process_files(int status) {
  if (auto failure = failure_on_exit.load()) {
    record_guest_process_failure(failure);
    status = guest_process_exit_status();
  }
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::profiler_flush, status);
  hle::write_census();
  // A terminal exit ends the process without atexit: write the profile and
  // invalidation reports here, so a fatal exit keeps its diagnostics.
  api_result_trace::flush();
  pc_trace::flush();
  xr_call_trace::flush();
  profile::stop();
  if (profile::invalidation_tracing) profile::report_invalidation_at_exit();
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::audio_capture_flush, status);
  if (exit_capture_flush) exit_capture_flush();
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::av_flush, status);
  av::finish();
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::log_flush, status);
  if (terminal_process_exit.load()) log::terminal_handoff(status);
  else log::flush();
}
[[noreturn]] void exit_guest_process(int status) {
  platform::notify_runtime_exit_wait(platform::RuntimeExitWait::arbitration, status);
  std::lock_guard decision(worker_exit_mutex);
  if (auto failure = failure_on_exit.load()) record_guest_process_failure(failure);
  status &= 255;
  // exit_group is terminal. Later teardown requests must not replace the
  // original status (in particular, killProcess(0) after a fatal guest signal).
  int expected = -1;
  if (!process_exit_status.compare_exchange_strong(expected, status)) status = expected;
  if (terminal_process_exit.load()) {
    flush_guest_process_files(status);
    // Diagnostics may flush while another thread records a fatal outcome.
    platform::terminate_runtime_process(process_exit_status.load());
  }
  for_each_guest_thread([](GuestThread& t) { if (&t != GuestThread::current()) { t.exit_requested = true; t.interrupt(); } });
  throw ProcessExit(process_exit_status.load());
}
void os_quit::stop_guest_execution() {
  if (!enabled) throw std::logic_error("OS quit disabled by VENTURE_MENU_QUIT=0");
  stopping.store(true, std::memory_order_release);
  for_each_guest_thread([](GuestThread& thread) { thread.interrupt(); });
}
void check_guest_exit(GuestThread& thread) {
  platform::apply_pending_thread_priority();
  if (auto status = process_exit_status.load(); status >= 0) throw ProcessExit(status);
  if (os_quit::requested()) throw ProcessExit(0);
  if (thread.exit_requested.load()) throw ThreadExit(0);
}
int guest_process_exit_status() { return process_exit_status.load(); }
int recorded_guest_process_failure() {
  const auto status = process_exit_status.load();
  return status > 0 ? status : failure_on_exit.load();
}
void shutdown_guest_threads() {
  RegistryLock lock(registry_mutex);
  shutting_down = true;
  for (auto& [handle, control] : managed_threads) {
    control->guest.exit_requested = true;
    control->guest.interrupt();
  }
  workers_finished.wait(lock, [] { return active_workers == 0; });
  managed_threads.clear();
}
void set_guest_thread_exit_handler(void (*handler)(GuestThread&)) { thread_exit_handler.store(handler); }
void set_guest_thread_native_cleanup(void (*handler)()) { native_thread_cleanup.store(handler); }
void finish_guest_thread(GuestThread& thread) {
  if (os_quit::requested()) return;
  void (*handler)(GuestThread&) = nullptr;
  {
    RegistryLock lock(registry_mutex);
    auto& r = resources.at(&thread);
    if (r.finished) return;
    r.finished = true;
    handler = r.options.on_exit ? r.options.on_exit : thread_exit_handler.load();
  }
  // _exit/abort deliberately bypass language-level teardown.
  struct NativeCleanup { ~NativeCleanup() { if (auto fn=native_thread_cleanup.load()) fn(); } } native_cleanup;
  if (handler && process_exit_status.load() < 0 && !thread.exit_requested.load()) handler(thread);
}
namespace threads {
int create(const Options& o, GuestAddr entry, GuestAddr arg, GuestAddr* handle) {
  if (!handle || !entry) return -err::kEINVAL;
  try {
    auto thread = create_guest_thread(entry, arg, {o.stack, o.stack_size, o.guard_size, o.detached, o.on_exit});
    if (!thread) return -err::kEAGAIN;
    *handle = thread->pthread_self;
    return 0;
  } catch (const std::bad_alloc&) { return -err::kENOMEM; }
  catch (const std::system_error&) { return -err::kEAGAIN; }
}
int join(GuestThread& caller, GuestAddr handle, GuestAddr* result) {
  if (&caller != GuestThread::current()) return -err::kEINVAL;
  return join_guest_thread(handle, result);
}
int detach(GuestAddr handle) { return detach_guest_thread(handle); }
int describe(GuestAddr handle, Options* options, std::int32_t* tid, std::string* name) {
  RegistryLock lock(registry_mutex);
  for (const auto& [thread, r] : resources) {
    if (!handle || thread->pthread_self != handle) continue;
    if (options) {
      const auto& o = r.options;
      *options = {thread->stack_base, thread->stack_size, o.guard_size, o.detached, o.on_exit};
      if (auto it = managed_threads.find(handle); it != managed_threads.end()) options->detached = it->second->detached;
    }
    if (tid) *tid = thread->tid();
    if (name) *name = r.name;
    return 0;
  }
  return -err::kESRCH;
}
int set_name(GuestAddr handle, const char* name) {
  if (!name) return -err::kEFAULT;
  if (std::char_traits<char>::length(name) > 15) return -err::kERANGE;
  RegistryLock lock(registry_mutex);
  for (auto& [thread, r] : resources) if (handle && thread->pthread_self == handle) {
    r.name = name; frame_cpu::name(thread->tid(), name); profile::set_thread_name(thread->tid(), name); return 0;
  }
  return -err::kESRCH;
}
std::vector<std::pair<std::int32_t, std::string>> list() {
  RegistryLock lock(registry_mutex);
  std::vector<std::pair<std::int32_t, std::string>> out;
  for (auto [tid, thread] : live_threads) out.emplace_back(tid, resources.at(thread).name);
  return out;
}
std::size_t count() {
  RegistryLock lock(registry_mutex);
  return live_threads.size();
}
[[noreturn]] void exit(GuestThread&, GuestAddr result) { exit_guest_thread(result); }
[[noreturn]] void exit_process(GuestThread&, int status) { exit_guest_process(status); }
}

}
