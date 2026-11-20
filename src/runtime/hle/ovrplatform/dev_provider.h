#pragma once
#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
// Local identity for development builds and apps from other stores.
//
// Selected only by an explicit per-app opt-in (--dev-identity). It never
// contacts Meta, never reads the account vault, and is never reached for an
// app the user did not opt in. Everything it answers is local: a stable
// developer user, a dummy token, empty purchases and canned Graph replies.
// Entitlement is reported only because the user marked this app as a dev
// build; apps that are not opted in keep the real Meta path unchanged.
std::shared_ptr<Provider> dev_provider(AppIdentity);
// The canned Graph replies, exposed for tests.
HttpResponse dev_graph_response(const HttpRequest&);
// True if the package looks like a dev build (a "dev" dot-segment, e.g.
// com.dev.studio.game or dev.studio.game). Only a suggestion for the launcher
// to offer; it never enables dev identity by itself.
bool package_suggests_dev(std::string_view package);
}  // namespace venture::ovrplatform
