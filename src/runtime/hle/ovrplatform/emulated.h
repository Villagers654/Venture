#pragma once
#include <filesystem>

#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
class Session;
// Answers the Platform requests that have no host service (rooms, parties,
// matchmaking, challenges, the user data store, version 1 cloud storage,
// system dialogs, co-watching, NetSync, plain HTTP) the way the device answers
// them for a player on their own. `state` keeps what persists per account and
// app (user data store, cloud storage); empty keeps nothing on disk.
void register_emulated_handlers(Session& session, const AppIdentity& app, std::filesystem::path state);
}  // namespace venture::ovrplatform
