// Venture: see host_feature.h.
#include "dynarmic/backend/arm64/host_feature.h"

#include <oaknut/feature_detection/feature_detection.hpp>

namespace Dynarmic::Backend::Arm64 {

static std::uint32_t DetectHostFeatures() {
    // The OS's own report (HWCAP on Linux and Android); an OS oaknut cannot
    // query reports nothing, so only the ARMv8.0 base set is used there.
    const oaknut::CpuFeatures cpu = oaknut::detect_features();
    std::uint32_t features = 0;
    const auto add = [&](oaknut::CpuFeature f, HostFeature bit) {
        if (cpu.has(f))
            features |= static_cast<std::uint32_t>(bit);
    };
    add(oaknut::CpuFeature::AES, HostFeature::AES);
    add(oaknut::CpuFeature::PMULL, HostFeature::PMULL);
    add(oaknut::CpuFeature::SHA256, HostFeature::SHA256);
    add(oaknut::CpuFeature::CRC32, HostFeature::CRC32);
    add(oaknut::CpuFeature::LSE, HostFeature::LSE);
    add(oaknut::CpuFeature::LSE2, HostFeature::LSE2);
    add(oaknut::CpuFeature::LRCPC, HostFeature::LRCPC);
    add(oaknut::CpuFeature::FlagM, HostFeature::FlagM);
    add(oaknut::CpuFeature::FP16, HostFeature::FP16);
    return features;
}

std::uint32_t HostFeatures(std::uint32_t disabled_host_features) {
    static const std::uint32_t detected = DetectHostFeatures();
    return detected & ~disabled_host_features;
}

}  // namespace Dynarmic::Backend::Arm64
