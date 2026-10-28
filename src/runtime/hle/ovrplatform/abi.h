// Platform ABI facts: Meta OVR_Types.h / OVR_Platform.h and the versioned
// com.meta.xr.sdk.platform CAPI.cs declarations (see README.md).
#pragma once
#include <cstddef>
#include <cstdint>

namespace venture::ovrplatform {
using Request = std::uint64_t;
using Id = std::uint64_t;
using Handle = std::uint64_t;

// Pinned Meta OVR_VoipMuteState.h and OVR_SystemVoipStatus.h: C enums are int32.
enum class VoipMuteState : std::int32_t { unknown, muted, unmuted };
enum class SystemVoipStatus : std::int32_t { unknown, unavailable, suppressed, active };
enum class InitResult : std::int32_t {
  success = 0,
  uninitialized = -1,
  version_mismatch = -6,
  unknown = -7,
  invalid_credentials = -8,
  not_entitled = -9,
};
// These are opaque in every surveyed SDK. Their private Horizon layouts are
// NOT part of the guest ABI: callers must use the registered accessors.
struct HandleCell {
  std::uint64_t serial;
};
struct KeyValuePair64 {
  std::uint64_t key;
  std::int32_t type;
  std::uint32_t pad0;
  std::uint64_t string_value;
  std::int32_t int_value;
  std::uint32_t pad1;
  double double_value;
};
struct MatchmakingCriterion64 {
  std::uint64_t key;
  std::int32_t importance;
  std::uint32_t pad0;
  std::uint64_t parameters;
  std::uint32_t count, pad1;
};
struct MatchmakingQuery64 {
  std::uint64_t data;
  std::uint32_t data_count, pad0;
  std::uint64_t criteria;
  std::uint32_t criteria_count, pad1;
};
struct OculusInitParams64 {
  std::int32_t type;
  std::uint32_t pad;
  std::uint64_t email, password, app_id, uri_prefix;
};
struct NetSyncVec3 {
  float x, y, z;
};
static_assert(sizeof(Request) == 8 && sizeof(Id) == 8 &&
              sizeof(InitResult) == 4);
static_assert(sizeof(HandleCell) == 8);
static_assert(sizeof(KeyValuePair64) == 40 &&
              offsetof(KeyValuePair64, double_value) == 32);
static_assert(offsetof(KeyValuePair64, string_value) == 16 &&
              offsetof(KeyValuePair64, int_value) == 24);
static_assert(sizeof(MatchmakingCriterion64) == 32 &&
              offsetof(MatchmakingCriterion64, parameters) == 16);
static_assert(sizeof(MatchmakingQuery64) == 32 &&
              offsetof(MatchmakingQuery64, criteria) == 16);
static_assert(sizeof(OculusInitParams64) == 40 &&
              offsetof(OculusInitParams64, app_id) == 24);
static_assert(sizeof(NetSyncVec3) == 12);
}  // namespace venture::ovrplatform
