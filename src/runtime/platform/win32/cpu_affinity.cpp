#include "platform/cpu_affinity.h"
#include <windows.h>
#include <stdexcept>

namespace venture::platform {
int current_cpu() {
  PROCESSOR_NUMBER processor{};
  GetCurrentProcessorNumberEx(&processor);
  return processor.Group * 64 + processor.Number;
}
std::vector<HostCpu> host_cpus() {
  DWORD size = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
  std::vector<std::byte> buffer(size);
  auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &size)) throw std::runtime_error("cannot query CPU topology");
  std::vector<HostCpu> out;
  for (DWORD offset = 0; offset < size;) {
    auto* item = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
    const auto& core = item->Processor;
    for (unsigned g = 0; g < core.GroupCount; ++g)
      for (unsigned b = 0; b < 64; ++b) if (core.GroupMask[g].Mask & (std::uint64_t{1} << b))
        out.push_back({core.GroupMask[g].Group * 64u + b, core.EfficiencyClass});
    offset += item->Size;
  }
  return out;
}
bool pin_current_thread(std::uint32_t index) {
  GROUP_AFFINITY affinity{};
  affinity.Group = static_cast<WORD>(index / 64);
  affinity.Mask = std::uint64_t{1} << (index % 64);
  return SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != 0;
}
} // namespace venture::platform
