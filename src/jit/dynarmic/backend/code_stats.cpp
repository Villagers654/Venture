// Venture: see dynarmic/interface/code_stats.h.
//
// File: "VJCS1\n", the host architecture (u8: 0 x64, 1 arm64), then one record
// per block, little-endian:
//   u8 'B', u8 guest ISA (0 A32, 1 A64), u16 0, u32 guest instructions,
//   u64 guest PC, u32 chunk count, then per chunk:
//   u8 kind (Chunk), u8 0, u16 IR opcode, u32 byte count, the bytes.
#include "dynarmic/backend/code_stats.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "dynarmic/interface/code_stats.h"

namespace Dynarmic {
namespace {
std::atomic<std::FILE*> stream{nullptr};
std::mutex stream_lock;
template<typename T>
void Put(std::vector<std::uint8_t>& out, T value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}
}  // namespace

bool CodeStats::Enable(const std::string& path) {
    std::lock_guard lock(stream_lock);
    if (stream.load()) return true;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
#if defined(ARCHITECTURE_x86_64)
    const std::uint8_t arch = 0;
#else
    const std::uint8_t arch = 1;
#endif
    std::fwrite("VJCS1\n", 1, 6, file);
    std::fwrite(&arch, 1, 1, file);
    std::fflush(file);
    stream.store(file);
    return true;
}

namespace Backend::CodeStats {
Recorder::Recorder(bool a64, std::uint64_t pc, std::uint32_t guest_instructions) noexcept
        : active(stream.load(std::memory_order_relaxed) != nullptr) {
    if (!active) return;
    buffer.reserve(4096);
    Put<std::uint8_t>(buffer, 'B');
    Put<std::uint8_t>(buffer, a64 ? 1 : 0);
    Put<std::uint16_t>(buffer, 0);
    Put<std::uint32_t>(buffer, guest_instructions);
    Put<std::uint64_t>(buffer, pc);
    Put<std::uint32_t>(buffer, 0);  // chunk count, filled in on write
}

void Recorder::Record(Chunk kind, IR::Opcode op, const void* begin, const void* end) {
    if (end <= begin) return;
    const auto size = static_cast<std::uint32_t>(static_cast<const std::uint8_t*>(end) - static_cast<const std::uint8_t*>(begin));
    Put<std::uint8_t>(buffer, static_cast<std::uint8_t>(kind));
    Put<std::uint8_t>(buffer, 0);
    Put<std::uint16_t>(buffer, static_cast<std::uint16_t>(op));
    Put<std::uint32_t>(buffer, size);
    const auto at = buffer.size();
    buffer.resize(at + size);
    std::memcpy(buffer.data() + at, begin, size);
    ++chunks;
}

Recorder::~Recorder() {
    if (!active) return;
    std::memcpy(buffer.data() + 16, &chunks, sizeof(chunks));
    std::lock_guard lock(stream_lock);
    if (auto* file = stream.load()) {
        std::fwrite(buffer.data(), 1, buffer.size(), file);
        std::fflush(file);
    }
}
}  // namespace Backend::CodeStats
}  // namespace Dynarmic
