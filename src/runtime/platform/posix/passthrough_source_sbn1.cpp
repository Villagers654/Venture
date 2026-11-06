// TEMPORARY COMPATIBILITY PATH: see xr/passthrough_source_sbn1.h. The SBN1
// companion exists for Windows only.
#include "xr/passthrough_source_sbn1.h"

namespace venture::xr::passthrough_source_sbn1::platform {
std::unique_ptr<passthrough_source_platform::Channel> connect(std::uint32_t&) { return nullptr; }
} // namespace venture::xr::passthrough_source_sbn1::platform
