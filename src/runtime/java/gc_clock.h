// Time the host JVM has spent in garbage collection, for stall records: a
// stall whose time went to GC is Venture's (the hosted Java framework's heap).
#pragma once
#include <cstdint>

namespace venture::java {
// Starts counting through a JVMTI environment of its own (GC start/finish
// events, a clock read each). Call once on a thread attached to the JVM.
void start_gc_clock();
// Nanoseconds spent in GC so far, including one in progress; -1 when unknown.
std::int64_t gc_ns();
}  // namespace venture::java
