// Venture: the emitters' side of dynarmic/interface/code_stats.h, shared by the
// x64 and arm64 backends. A disabled recorder costs one relaxed load per block.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::CodeStats {
enum class Chunk : std::uint8_t { Op = 0, Prologue = 1, Epilogue = 2, Deferred = 3 };
// One block. Each chunk holds whole host instructions; chunks are written when
// the recorder is destroyed, as one record under the stream's lock.
class Recorder {
public:
    Recorder(bool a64, std::uint64_t pc, std::uint32_t guest_instructions) noexcept;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    explicit operator bool() const noexcept { return active; }
    void Add(Chunk kind, IR::Opcode op, const void* begin, const void* end) {
        if (active) Record(kind, op, begin, end);
    }
    void Add(Chunk kind, const void* begin, const void* end) { Add(kind, IR::Opcode::Void, begin, end); }

private:
    void Record(Chunk kind, IR::Opcode op, const void* begin, const void* end);
    bool active;
    std::uint32_t chunks = 0;
    std::vector<std::uint8_t> buffer;
};
}  // namespace Dynarmic::Backend::CodeStats
