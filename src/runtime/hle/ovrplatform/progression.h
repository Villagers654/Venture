#pragma once
#include "hle/ovrplatform/session.h"

namespace venture::ovrplatform {
struct ProgressionPolicy {
  bool leaderboard_writes = true;
  bool achievement_writes = true;
  // This metadata edge awaits its bounded live read (plan section 3).
  bool leaderboard_metadata_reads = false;
  // GET /me/achievements: loopback coverage and one Batman owning-account
  // read verified HTTP 200 and parsed progress on 2026-10-09.
  bool achievement_progress_reads = true;
  bool retry_reads = true;  // Diagnostics opt out to enforce one wire read.
};
void register_progression_endpoints(HttpTransport&);
void register_progression_handlers(Session&, ProgressionPolicy = {});
} // namespace venture::ovrplatform
