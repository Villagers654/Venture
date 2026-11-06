#pragma once
#include "xr/passthrough_source.h"
#include <cstdint>
#include <memory>

namespace venture::xr::passthrough_source_platform {
// An overlapped connection to the named pipe `name` when its server runs as
// this user, with that server's process ID; nullptr otherwise.
std::unique_ptr<Channel> open_pipe(const wchar_t* name, std::uint32_t& server);
} // namespace venture::xr::passthrough_source_platform
