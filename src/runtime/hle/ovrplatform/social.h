#pragma once
#include "hle/ovrplatform/session.h"

namespace venture::ovrplatform {
void register_social_endpoints(HttpTransport&);
void register_social_handlers(Session&);
// The diagnostic passes retry_reads=false and never calls SendInvites.
Result social_request(Provider&, std::string_view name, const RequestArgs&, std::stop_token,
                      bool retry_reads = true);
}  // namespace venture::ovrplatform
