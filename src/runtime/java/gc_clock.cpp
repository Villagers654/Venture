#include "java/gc_clock.h"
#include "java/jvm.h"
#include "common/log.h"
#include <jni.h>
#include <jvmti.h>
#include <atomic>
#include <chrono>

namespace venture::java {
namespace {
std::atomic<bool> counting{false};
std::atomic<std::int64_t> total{0}, began{0};  // began: 0 outside a collection
std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Called by the VM at a safepoint: no JNI, no allocation, only atomics.
void JNICALL gc_start(jvmtiEnv*) { began.store(now_ns(), std::memory_order_relaxed); }
void JNICALL gc_finish(jvmtiEnv*) {
  if (const auto start = began.exchange(0, std::memory_order_relaxed))
    total.fetch_add(now_ns() - start, std::memory_order_relaxed);
}
}  // namespace

void start_gc_clock() {
  if (counting.load() || !host_vm()) return;
  jvmtiEnv* ti = nullptr;
  if (host_vm()->GetEnv(reinterpret_cast<void**>(&ti), JVMTI_VERSION_1_2) != JNI_OK || !ti) return;
  jvmtiCapabilities capabilities{};
  capabilities.can_generate_garbage_collection_events = 1;
  jvmtiEventCallbacks callbacks{};
  callbacks.GarbageCollectionStart = gc_start;
  callbacks.GarbageCollectionFinish = gc_finish;
  if (ti->AddCapabilities(&capabilities) != JVMTI_ERROR_NONE ||
      ti->SetEventCallbacks(&callbacks, sizeof(callbacks)) != JVMTI_ERROR_NONE ||
      ti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_START, nullptr) != JVMTI_ERROR_NONE ||
      ti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, nullptr) != JVMTI_ERROR_NONE) {
    log::info("stall", "Java GC time unavailable: JVMTI garbage collection events refused");
    ti->DisposeEnvironment();
    return;
  }
  // The environment lives as long as the VM: its callbacks must stay registered.
  counting.store(true);
}

std::int64_t gc_ns() {
  if (!counting.load(std::memory_order_relaxed)) return -1;
  const auto start = began.load(std::memory_order_relaxed);
  return total.load(std::memory_order_relaxed) + (start ? now_ns() - start : 0);
}
}  // namespace venture::java
