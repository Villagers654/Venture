// Venture: the arm64 backend's host CPU features, the counterpart of
// backend/x64/host_feature.h. Emitters choose optional ARMv8.x instructions
// only through these bits, read once from the OS (HWCAP on Linux and Android),
// never from a device or vendor name. UserConfig::disabled_host_features hides
// detected bits (diagnostics and tests).
#pragma once

#include <cstdint>

namespace Dynarmic::Backend::Arm64 {

enum class HostFeature : std::uint32_t {
    AES = 1u << 0,      // AESE, AESD, AESMC, AESIMC
    PMULL = 1u << 1,    // PMULL/PMULL2 on 64-bit lanes
    SHA256 = 1u << 2,   // SHA256H, SHA256H2, SHA256SU0, SHA256SU1
    CRC32 = 1u << 3,    // CRC32{B,H,W,X}, CRC32C{B,H,W,X} (optional before ARMv8.1)
    LSE = 1u << 4,      // CAS, CASP, LDADD, LDCLR, LDEOR, LDSET, SWP, LD{S,U}{MAX,MIN}
    LSE2 = 1u << 5,     // single-copy atomic unaligned accesses within 16 bytes
    LRCPC = 1u << 6,    // LDAPR
    FlagM = 1u << 7,    // CFINV, RMIF, SETF8, SETF16
    FP16 = 1u << 8,     // half-precision data processing
};

constexpr HostFeature operator|(HostFeature a, HostFeature b) {
    return static_cast<HostFeature>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

// The detected features minus the disabled bits.
std::uint32_t HostFeatures(std::uint32_t disabled_host_features);

constexpr bool HasHostFeature(std::uint32_t features, HostFeature feature) {
    return (features & static_cast<std::uint32_t>(feature)) == static_cast<std::uint32_t>(feature);
}

}  // namespace Dynarmic::Backend::Arm64
