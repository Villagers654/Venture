#pragma once
#include "common/json.h"
#include <cstdint>
#include <span>
#include <string_view>
#include <atomic>
#include <memory>

namespace venture::profile {
// async: a guest pipeline created by a background worker (gfx/pipeline_trace.cpp).
enum class PipelineSource { guest, angle, decode, async };
struct PipelineFeedback { std::uint64_t duration_ns; std::uint32_t flags; };
// Exact wall time, independent of the optional sampling profiler. Feedback's
// cache-hit bit describes the supplied VkPipelineCache, not the driver disk cache.
void record_pipeline(PipelineSource source, bool compute, std::uint32_t count,
                     std::int64_t start_ns, std::uint64_t wall_ns,
                     std::span<const PipelineFeedback> feedback) noexcept;
Json pipeline_report();
// VENTURE_COMPILE_TRACE=<file> (developer diagnostics, off by default): one
// JSON line per pipeline creation, shader compile or program link, on the
// monotonic clock of the mock runtime's frame log, so long frames can be
// attributed to GPU-side compilation.
bool compile_trace_enabled() noexcept;
// `extra`: further JSON members, appended verbatim ("\"name\":value,...").
void trace_compile(std::string_view kind, std::int64_t start_ns, std::uint64_t wall_ns,
                   std::uint32_t count = 1, std::uint32_t hits = 0, std::string_view extra = {}) noexcept;
void report_pipelines(std::string_view phase) noexcept;
void start_pipeline_telemetry();
// Sampling-only activity for native ANGLE/decoder workers, whose threads have
// no GuestThread. Resolve before suspending the target; stores never lock.
std::shared_ptr<std::atomic<std::uint32_t>> pipeline_thread_activity(std::uint32_t tid);
std::uint32_t pipeline_exchange_activity(std::uint32_t value) noexcept;
}
