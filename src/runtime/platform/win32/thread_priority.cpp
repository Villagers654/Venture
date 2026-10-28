#include "platform/thread_priority.h"
#include "platform/profile.h"
#include <intrin.h>
#include "core/error.h"
#include <windows.h>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <stdexcept>

namespace venture::platform {
void lower_background_thread_priority() {
  SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
}
namespace {
struct Entry { HANDLE handle; bool scoped_audio = false; std::atomic<bool> audio{false}; std::atomic<int> nice{0}; int fifo = 0; Entry* previous = nullptr; std::unique_ptr<AudioThreadPriority> priority; };
struct Threads { std::mutex mutex; std::map<std::int32_t, std::unique_ptr<Entry>> handles; };
thread_local Entry* current_entry = nullptr;
thread_local unsigned mmcss_depth = 0, realtime_depth = 0;
struct Avrt {
  HMODULE library = LoadLibraryW(L"avrt.dll");
  using Set = HANDLE (WINAPI*)(LPCWSTR, LPDWORD);
  using Revert = BOOL (WINAPI*)(HANDLE);
  Set set = library ? reinterpret_cast<Set>(GetProcAddress(library, "AvSetMmThreadCharacteristicsW")) : nullptr;
  Revert revert = library ? reinterpret_cast<Revert>(GetProcAddress(library, "AvRevertMmThreadCharacteristics")) : nullptr;
};
Avrt& avrt() { static Avrt value; return value; }
Threads& threads() { static auto* value = new Threads; return *value; }
}
AudioThreadPriority::AudioThreadPriority() {
  if (current_audio_priority_class() != AudioPriorityClass::normal) return;
  previous_priority_ = GetThreadPriority(GetCurrentThread());
  auto& api = avrt();
  DWORD index = 0;
  if (api.set && api.revert) task_ = api.set(L"Pro Audio", &index);
  if (task_) ++mmcss_depth;
  else if ((changed_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))) ++realtime_depth;
}
AudioThreadPriority::~AudioThreadPriority() {
  if (task_) { avrt().revert(task_); --mmcss_depth; }
  else if (changed_) { SetThreadPriority(GetCurrentThread(), previous_priority_); --realtime_depth; }
}
AudioPriorityClass current_audio_priority_class() {
  return mmcss_depth ? AudioPriorityClass::mmcss : realtime_depth ? AudioPriorityClass::realtime : AudioPriorityClass::normal;
}
std::int64_t thread_cpu_time_ns() {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
  const auto ticks = ((std::uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime)
                   + ((std::uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);
  return std::int64_t(ticks * 100);
}
std::int64_t thread_cpu_cycles(const ProfileThread& thread) {
  ULONG64 cycles = 0;
  return thread.handle && QueryThreadCycleTime(thread.handle.get(), &cycles) ? std::int64_t(cycles) : -1;
}
std::int64_t thread_cpu_cycles() {
  ULONG64 cycles = 0;
  return QueryThreadCycleTime(GetCurrentThread(), &cycles) ? std::int64_t(cycles) : -1;
}
const char* thread_cycles_method() { return "query_thread_cycles_tsc_estimate"; }
double thread_cycles_per_ns() {
  static const double rate = [] {
    LARGE_INTEGER frequency{}, begin{}, end{};
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&begin)) return 0.0;
    _mm_lfence(); const auto first = __rdtsc(); _mm_lfence();
    Sleep(25);
    _mm_lfence(); const auto last = __rdtsc(); _mm_lfence();
    if (!QueryPerformanceCounter(&end) || end.QuadPart <= begin.QuadPart || last <= first) return 0.0;
    return double(last - first) * double(frequency.QuadPart) / double(end.QuadPart - begin.QuadPart) / 1e9;
  }();
  return rate;
}
void apply_pending_thread_priority() {
  if (!current_entry) return;
  if (current_entry->audio.load(std::memory_order_acquire)) {
    if (!current_entry->priority) current_entry->priority = std::make_unique<AudioThreadPriority>();
  } else if (current_entry->priority) {
    current_entry->priority.reset();
    if (current_entry->scoped_audio) return;
    const auto nice = current_entry->nice.load(std::memory_order_relaxed);
    const int priority = nice <= -10 ? THREAD_PRIORITY_HIGHEST : nice < 0 ? THREAD_PRIORITY_ABOVE_NORMAL
        : nice == 0 ? THREAD_PRIORITY_NORMAL : nice < 10 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
    SetThreadPriority(GetCurrentThread(), priority);
  }
}
int set_audio_thread_priority(std::int32_t tid, int fifo_priority) {
  if (fifo_priority != 0 && fifo_priority != 2 && fifo_priority != 3) return -err::kEINVAL;
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.handles.find(tid);
  if (it == state.handles.end()) return -err::kESRCH;
  it->second->fifo = fifo_priority;
  auto nice = it->second->nice.load(std::memory_order_relaxed);
  it->second->audio = fifo_priority || nice == -16 || nice == -19;
  if (it->second.get() == current_entry) apply_pending_thread_priority();
  return 0;
}

int get_audio_thread_scheduler(std::int32_t tid, int* policy, int* priority) {
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.handles.find(tid);
  if (it == state.handles.end()) return -err::kESRCH;
  *policy = it->second->fifo ? 1 : 0;
  *priority = it->second->fifo;
  return 0;
}
void bind_thread_priority(std::int32_t tid) {
  HANDLE handle{};
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                       THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, 0))
    throw std::runtime_error("Cannot retain native thread for guest scheduling");
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto entry = std::make_unique<Entry>();
  entry->handle = handle;
  entry->previous = current_entry;
  if (current_entry) current_entry->priority.reset();
  entry->scoped_audio = current_audio_priority_class() != AudioPriorityClass::normal;
  auto [it, inserted] = state.handles.emplace(tid, std::move(entry));
  if (!inserted) { CloseHandle(handle); throw std::runtime_error("Duplicate scheduling tid"); }
  current_entry = it->second.get();
}
void unbind_thread_priority(std::int32_t tid) {
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  if (auto it = state.handles.find(tid); it != state.handles.end()) {
    current_entry->priority.reset();
    current_entry = it->second->previous;
    CloseHandle(it->second->handle); state.handles.erase(it);
    apply_pending_thread_priority();
  }
}
int get_thread_priority(std::int32_t tid, int* nice) {
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.handles.find(tid);
  if (it == state.handles.end()) return -err::kESRCH;
  *nice = it->second->nice.load(std::memory_order_relaxed);
  return 0;
}
int set_thread_priority(std::int32_t tid, int nice) {
  if (nice < -20 || nice > 19) return -err::kEINVAL;
  auto& state = threads();
  std::lock_guard lock(state.mutex);
  auto it = state.handles.find(tid);
  if (it == state.handles.end() || WaitForSingleObject(it->second->handle, 0) == WAIT_OBJECT_0) return -err::kESRCH;
  if (nice == -16 || nice == -19 || it->second->fifo) {
    it->second->nice = nice;
    it->second->audio = true;
    // Remote requests enter MMCSS on the target's next guest entry.
    if (it->second.get() == current_entry) apply_pending_thread_priority();
    return 0;
  }
  // Map Android's nice bands to ordinary Win32 scheduling priorities. Avoid
  // time-critical priority: Windows has no one-to-one Linux nice equivalent.
  const int priority = nice <= -10 ? THREAD_PRIORITY_HIGHEST : nice < 0 ? THREAD_PRIORITY_ABOVE_NORMAL
      : nice == 0 ? THREAD_PRIORITY_NORMAL : nice < 10 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_LOWEST;
  if (it->second->scoped_audio || SetThreadPriority(it->second->handle, priority)) {
    it->second->nice = nice;
    it->second->audio = false;
    if (it->second.get() == current_entry) apply_pending_thread_priority();
    return 0;
  }
  return GetLastError() == ERROR_ACCESS_DENIED ? -err::kEPERM : -err::kEINVAL;
}
}
