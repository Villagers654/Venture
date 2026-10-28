#pragma once

#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
// Diagnostics never include response values, credentials or account/user IDs.
std::string response_fields(const HttpResponse&);
void log_auth_response(std::string_view path, std::string_view app_id,
                       const HttpResponse&);
}  // namespace venture::ovrplatform
