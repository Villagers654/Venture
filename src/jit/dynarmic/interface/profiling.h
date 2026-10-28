// Venture opt-in diagnostics; no translated-code instrumentation.
#pragma once
#include <string_view>
#include <array>
#include <cstdint>
namespace Dynarmic {
using CodeRegistrationCallback = void (*)(const void*, const void*, std::string_view);
void SetCodeRegistrationCallback(CodeRegistrationCallback callback);
// Exclusive wall-clock phases: nested emission bookkeeping is charged once.
enum class CompilePhase : std::uint8_t {
    Prepare, Decode, Polyfill, CallbackConfig, Naming, GetSet,
    DeadCodeGetSet, ConstantMemory, ConstantPropagation, DeadCodeConstant,
    IdentityRemoval, Verification, Emit, Registration, LinkPatch, CacheInsert,
    Finalize, Count
};
inline constexpr std::size_t compile_phase_count = std::size_t(CompilePhase::Count);
inline constexpr std::array<std::string_view, compile_phase_count> compile_phase_names = {
    "prepare", "decode_ir", "polyfill", "callback_config", "naming", "get_set",
    "dead_code_get_set", "constant_memory", "constant_propagation", "dead_code_constant",
    "identity_removal", "verification", "x64_emit", "code_registration", "link_patch",
    "cache_insert", "finalize"
};
struct CompileTiming {
    std::uint64_t guest_pc = 0, begin_ns = 0, end_ns = 0, location_descriptor = 0;
    std::array<std::uint64_t, compile_phase_count> phase_ns{};
    bool a32 = false, completed = false;
};
using CompileTimingCallback = void (*)(const CompileTiming&) noexcept;
void SetCompileTimingCallback(CompileTimingCallback callback);
// Cold compilation only. Disabled scopes do not read the clock or allocate.
class CompileTimingScope {
public:
    CompileTimingScope(std::uint64_t guest_pc, bool a32, std::uint64_t descriptor) noexcept;
    ~CompileTimingScope();
    CompileTimingScope(const CompileTimingScope&) = delete;
    CompileTimingScope& operator=(const CompileTimingScope&) = delete;
    void Complete() noexcept { timing.completed = true; }
    CompileTiming timing;
    CompileTimingScope* previous = nullptr;
    CompileTimingCallback callback = nullptr;
    CompilePhase phase = CompilePhase::Prepare;
    std::uint64_t last_ns = 0;
};
void SetCompilePhase(CompilePhase phase) noexcept;
}
