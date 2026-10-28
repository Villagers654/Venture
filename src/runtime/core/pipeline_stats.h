#pragma once
#include "common/json.h"
#include <cstdint>
#include <span>
#include <atomic>
#include <memory>

namespace venture::profile {
enum class PipelineSource { guest, angle, decode };
struct PipelineFeedback { std::uint64_t duration_ns; std::uint32_t flags; };
// Exact wall time, independent of the optional sampling profiler. Feedback's
// cache-hit bit describes the supplied VkPipelineCache, not the driver disk cache.
void record_pipeline(PipelineSource source, bool compute, std::uint32_t count,
                     std::int64_t start_ns, std::uint64_t wall_ns,
                     std::span<const PipelineFeedback> feedback) noexcept;
Json pipeline_report();
void report_pipelines(std::string_view phase) noexcept;
void start_pipeline_telemetry();
// Sampling-only activity for native ANGLE/decoder workers, whose threads have
// no GuestThread. Resolve before suspending the target; stores never lock.
std::shared_ptr<std::atomic<std::uint32_t>> pipeline_thread_activity(std::uint32_t tid);
std::uint32_t pipeline_exchange_activity(std::uint32_t value) noexcept;
}
