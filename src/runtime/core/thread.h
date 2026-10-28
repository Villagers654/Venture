// Guest threads. Every guest thread runs on its own host thread and owns a Cpu.
#pragma once

#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <span>

#include "core/guest.h"
#include "core/shutdown_trace.h"
#include "core/mutex_watch.h"
#include "cpu/cpu.h"
#include "core/startup_diagnostics.h"

namespace venture {

namespace hle::a32 { class CallBuilder32; }
namespace platform { class TimedWait; }

// Arguments for a host->guest call, allocated per AAPCS64.
struct GuestCallArgs {
  std::span<const std::uint64_t> integers;  // X0..X7, overflow to the stack
  std::span<const double> doubles;          // D0..D7
  // Optional explicitly ordered AAPCS64 spill slots. When nonempty, integers
  // and doubles contain at most eight register values each; copy these slots
  // in order to the 16-byte-aligned outgoing SP. This permits mixed GP/FP
  // overflow (JNI), whose ordering cannot be recovered from separate lists.
  // doubles carry raw D-register bits; fixed float args use low 32 bits (S).
  std::span<const std::uint64_t> stack{};
};

struct GuestCallResult {
  std::uint64_t x0 = 0;
  std::uint64_t x1 = 0;
  Vec128 v0{};
  GuestIsa isa = GuestIsa::a64;
  std::int32_t i32() const { return static_cast<std::int32_t>(x0); }
  std::uint64_t u64() const { return isa == GuestIsa::a32 ? (x0 & UINT32_MAX) | (x1 << 32) : x0; }
  std::int64_t i64() const { return static_cast<std::int64_t>(u64()); }
  float f32() const { return std::bit_cast<float>(static_cast<std::uint32_t>(isa == GuestIsa::a32 ? x0 : v0.lo)); }
  double f64() const { return std::bit_cast<double>(isa == GuestIsa::a32 ? u64() : v0.lo); }
};

class GuestThread {
 public:
  // Host condition waits remain on the stack while a guest handler executes.
  // Only a handler escaping that wait needs its mutex reacquired by cleanup.
  struct ConditionWaitCleanup {
    ConditionWaitCleanup* previous;
    void (*acquire)(void*);
    void* argument;
    GuestAddr caller_sp;
    bool pending = true;
    std::uint64_t instructions = 0;
    std::uint64_t occurrence = 0;
    bool observe_unwind = false;
    ~ConditionWaitCleanup();
  };
  ConditionWaitCleanup* condition_wait_cleanup = nullptr;
  std::uint64_t wait_unwind_occurrences = 0, wait_unwind_instructions = 0;
  void count_wait_unwind_instruction();
  // The guest thread bound to the calling host thread, or nullptr.
  static GuestThread* current();
  // The calling host thread's guest thread, creating one (guest stack, TLS
  // block, Cpu, tid) if the host thread has none yet. Used when host-created
  // threads enter guest code: JVM threads calling native methods, audio
  // callbacks, XR/graphics worker threads. Released when the host thread exits.
  static GuestThread& adopt_current();

  Cpu& cpu() { return *cpu_; }
  bool has_cpu() const { return cpu_ != nullptr; }
  std::int32_t tid() const { return tid_; }
  // Compact bionic ARM mutexes encode a live owner in 16 bits. This index
  // is independent of the monotonically increasing Linux-visible tid.
  std::uint16_t mutex_owner_index() const { return mutex_owner_index_; }

  // Calls guest function `fn` on this thread and returns when it returns.
  // Re-entrant: may be used from inside an HLE handler (callbacks such as
  // qsort comparators, pthread start routines, JNI native methods, Vulkan
  // allocation callbacks). Saves and restores the interrupted guest state.
  GuestCallResult call(GuestAddr fn, const GuestCallArgs& args);
  GuestCallResult call32(GuestAddr fn, const hle::a32::CallBuilder32& args);
  std::uint64_t call(GuestAddr fn, std::initializer_list<std::uint64_t> integers) {
    return call(fn, GuestCallArgs{std::span(integers.begin(), integers.size()), {}}).x0;
  }

  // Thread-local errno seen by the guest via __errno().
  int* errno_location() { return errno_address_; }
  void set_errno(int value) { *errno_address_ = value; }

  // --- runtime-internal ---------------------------------------------------
  GuestAddr stack_base = 0;      // lowest address of the guest stack
  std::uint64_t stack_size = 0;
  GuestAddr thread_pointer = 0;  // TPIDR_EL0 value (bionic TCB slot array)
  GuestAddr pthread_self = 0;    // core-owned opaque pthread_t handle (not bionic internals)
  struct PthreadKeyValue { std::uint64_t sequence = 0; GuestAddr value = 0; };
  // Owner-thread key data in the guest page after the handle page, where A64
  // guest code reaches it through TLS slot 1 (hle/libc/threads.cpp). Sequence
  // matching makes a deleted/reused key start empty on every guest thread.
  static constexpr std::uint64_t kPthreadKeyCount = 256, kPthreadKeyValuesOffset = 4096;
  PthreadKeyValue* pthread_values = nullptr;
  GuestAddr jni_env = 0;         // guest JNIEnv* when attached to the JVM

  // Interrupts a blocking HLE wait or running guest code so that pending
  // signals / suspension / exit can be processed. Thread-safe.
  void interrupt();
  std::atomic<std::uint64_t> pending_signals{0};
  std::uint64_t blocked_signals = 0;
  // Run-loop boundaries, including completed in-JIT HLE calls (owner only).
  // The signal model distinguishes an immediate retry from guest progress.
  std::uint64_t cpu_exits = 0;
  // Interrupt generations prevent a wake between a wait's check and enqueue from being lost.
  std::atomic<std::uint64_t> interrupt_epoch{0};
  std::atomic<bool> interrupted{false};
  std::atomic<std::uint64_t> signal_mask_snapshot{0};
  std::atomic<bool> exit_requested{false};
  GuestAddr clear_child_tid = 0; // Linux set_tid_address: clear and futex-wake at exit.
  // Survives the destructor body, including adopted-thread pthread key cleanup.
  std::unique_ptr<platform::TimedWait> timed_wait;
  startup_diagnostics::ThreadState startup_state;
  void request_startup_sample();
  shutdown_trace::State shutdown_trace;
  std::unique_ptr<shutdown_trace::MutexWatchRecords<4096>> mutex_watch_records;
  std::unique_ptr<shutdown_trace::WatchRecords<shutdown_trace::WordTransitionEvent, 1024>> word_watch_records;
  std::atomic<unsigned> os_call_depth{0};
  void request_shutdown_sample();

  explicit GuestThread(std::int32_t tid);
  ~GuestThread();
  GuestThread(const GuestThread&) = delete;
  GuestThread& operator=(const GuestThread&) = delete;

  // Binds this object to the calling host thread and creates its Cpu.
  void attach_to_current_host_thread();
 private:
  void attach_locked();
 public:
  // Retires the CPU/monitor slot at thread exit; join may retain stack/TLS metadata.
  void detach_from_current_host_thread();

  // Runs guest code until the outermost call returns (or the thread exits).
  // Dispatches HLE calls, Linux syscalls, faults and signals.
  void run_until_return(int depth);

 private:
  GuestCallResult call64(GuestAddr fn, const GuestCallArgs& args);
  GuestCallResult call32(GuestAddr fn, const GuestCallArgs& args);
  template<GuestIsa Isa, bool Profile = false> void run_for_isa(int depth);
  // Selected once on attach: no ISA condition in either call implementation.
  GuestCallResult (GuestThread::*call_impl_)(GuestAddr, const GuestCallArgs&) = nullptr;
  void (GuestThread::*run_impl_)(int) = nullptr;
  std::unique_ptr<Cpu> cpu_;
  std::mutex cpu_mutex_;
  std::int32_t tid_;
  std::uint16_t mutex_owner_index_ = 0;
  int errno_ = 0;
  int* errno_address_ = &errno_;
  int call_depth_ = 0;
};

}  // namespace venture
