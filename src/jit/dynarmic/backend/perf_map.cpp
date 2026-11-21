// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <atomic>
#include <chrono>
#include "dynarmic/interface/profiling.h"
namespace Dynarmic {
namespace {
std::atomic<CodeRegistrationCallback> code_registration{nullptr};
std::atomic<CompileTimingCallback> compile_timing_callback{nullptr};
thread_local CompileTimingScope* current_compile = nullptr;
thread_local std::atomic<std::uint64_t>* compile_marker = nullptr;
thread_local unsigned compile_depth = 0;
std::atomic<std::uint64_t> compiled_blocks{0}, compile_ns{0};
std::uint64_t CompileNow() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
void SetCompileTimingCallback(CompileTimingCallback cb) { compile_timing_callback.store(cb); }
void SetCompileMarker(std::atomic<std::uint64_t>* marker) noexcept { compile_marker = marker; }
CompileTotals GetCompileTotals() noexcept {
    return {compiled_blocks.load(std::memory_order_relaxed), compile_ns.load(std::memory_order_relaxed)};
}
CompileTimingScope::CompileTimingScope(std::uint64_t pc, bool a32, std::uint64_t descriptor) noexcept {
    // Only the owning thread writes its marker: a plain load and store suffice.
    if (auto* marker = compile_marker) {
        marker_before = marker->load(std::memory_order_relaxed);
        marker->store(pc | 1, std::memory_order_relaxed);
    }
    if (!compile_depth++) total_begin_ns = CompileNow();
    callback = compile_timing_callback.load(std::memory_order_relaxed);
    if (!callback) return;
    timing.guest_pc = pc; timing.a32 = a32; timing.location_descriptor = descriptor;
    timing.begin_ns = last_ns = CompileNow();
    previous = current_compile; current_compile = this;
}
void SetCompilePhase(CompilePhase phase) noexcept {
    if (!current_compile) return;
    const auto now = CompileNow();
    current_compile->timing.phase_ns[std::size_t(current_compile->phase)] += now - current_compile->last_ns;
    current_compile->last_ns = now; current_compile->phase = phase;
}
CompileTimingScope::~CompileTimingScope() {
    if (auto* marker = compile_marker) marker->store(marker_before, std::memory_order_relaxed);
    compiled_blocks.fetch_add(1, std::memory_order_relaxed);
    if (!--compile_depth) compile_ns.fetch_add(CompileNow() - total_begin_ns, std::memory_order_relaxed);
    if (!callback) return;
    SetCompilePhase(phase);
    timing.end_ns = last_ns;
    current_compile = previous;
    // Exclude a nested compilation from its parent's exclusive phase time.
    if (previous) {
        const auto nested = timing.end_ns - timing.begin_ns;
        previous->last_ns += nested;
    }
    callback(timing);
}
void SetCodeRegistrationCallback(CodeRegistrationCallback cb) { code_registration.store(cb); }
}

#include <cstddef>
#include <string>
#include <fmt/format.h>

#include "dynarmic/backend/perf_map.h"
#include "common/common_types.h"
#if defined(__linux__) && !defined(__ANDROID__)
#    include <cstdio>
#    include <cstdlib>
#    include <mutex>
#    include <sys/types.h>
#    include <unistd.h>

namespace Dynarmic::Backend {

namespace {
std::mutex mutex;
std::FILE* file = nullptr;

void OpenFile() {
    const char* perf_dir = std::getenv("PERF_BUILDID_DIR");
    if (!perf_dir) {
        file = nullptr;
        return;
    }

    const pid_t pid = getpid();
    const std::string filename = fmt::format("{:s}/perf-{:d}.map", perf_dir, pid);

    file = std::fopen(filename.c_str(), "w");
    if (!file) {
        return;
    }

    std::setvbuf(file, nullptr, _IONBF, 0);
}
}  // anonymous namespace

// PERF_BUILDID_DIR is read once: without it there is no perf map, and registrations skip the shared mutex.
static bool PerfMapFileWanted() noexcept {
    static const bool wanted = std::getenv("PERF_BUILDID_DIR") != nullptr;
    return wanted;
}

bool PerfMapActive() noexcept {
    return PerfMapFileWanted() || Dynarmic::code_registration.load(std::memory_order_relaxed);
}

namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {
    if (auto callback = Dynarmic::code_registration.load(std::memory_order_relaxed)) callback(start, end, friendly_name);
    if (start == end || !PerfMapFileWanted()) {
        // Nothing to register
        return;
    }

    std::lock_guard guard{mutex};

    if (!file) {
        OpenFile();
        if (!file) {
            return;
        }
    }

    const std::string line = fmt::format("{:016x} {:016x} {:s}\n", reinterpret_cast<u64>(start), reinterpret_cast<u64>(end) - reinterpret_cast<u64>(start), friendly_name);
    std::fwrite(line.data(), sizeof *line.data(), line.size(), file);
}
}  // namespace detail

void PerfMapClear() {
    // VENTURE_PROFILE_CACHE_CLEAR_V1: a full code-cache clear, reported on the owning thread.
    if (auto callback = Dynarmic::code_registration.load(std::memory_order_relaxed)) callback(nullptr, nullptr, "cache_clear");
    std::lock_guard guard{mutex};

    if (!file) {
        return;
    }

    std::fclose(file);
    file = nullptr;
    OpenFile();
}

}  // namespace Dynarmic::Backend

#else
namespace Dynarmic::Backend {
bool PerfMapActive() noexcept {
    return Dynarmic::code_registration.load(std::memory_order_relaxed);
}
namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view name) {
    if (auto callback = Dynarmic::code_registration.load(std::memory_order_relaxed)) callback(start, end, name);
}
}
void PerfMapClear() {
    if (auto callback = Dynarmic::code_registration.load(std::memory_order_relaxed)) callback(nullptr, nullptr, "cache_clear");
}
}
#endif
