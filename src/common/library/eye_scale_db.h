// The eye scale database: how much larger than the eye size it is told each
// Quest title renders (Unity's eyeTextureResolutionScale, Unreal's
// vr.PixelDensity). The runtime compensates it (src/runtime/xr/eye_scale.h).
//
// Records come from, in order: this user's own (eye-scale.json in the data
// folder, which the runtime writes as sessions observe a game), then the
// bundled table (third_party/venture-eye-scale/titles.json, a copy of the
// separate venture-eye-scale repository, from Venture's runs and debug reports).
#pragma once
#include "common/json.h"
#include "common/paths.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace venture::library::eye_scale {
struct Extent {
  std::uint32_t width = 0, height = 0;
  bool operator==(const Extent&) const = default;
};

enum class State {
  native,     // renders at (or within 10% of) the size it is told: nothing to compensate
  learned,    // multiplier observed, compensated from the next launch
  confirmed,  // compensated and observed rendering at the target
  fixed,      // keeps its size whatever it is told: not compensated
};
std::string_view state_name(State);
std::optional<State> parse_state(std::string_view);

struct Record {
  double multiplier = 1;  // the game's eye over the size it was told (geometric mean of the sides)
  State state = State::native;
  double applied = 1;         // compensation in force when observed
  Extent told{}, rendered{};  // last observation (zero: none)
  bool own = false;           // this user's record (else the bundled table's)
};

// Sides within this fraction of each other count as equal (rounding, padding,
// a game's quantized scale steps); a multiplier up to 1 + kTolerance is native.
inline constexpr double kTolerance = 0.10;
inline constexpr double kMinMultiplier = 0.25, kMaxMultiplier = 4;

// One session's observation folded into the record from its start: `rendered`
// is the largest projection eye this session, `told` the eye size the game was
// given under `applied` compensation, `target` the size it should render at.
Record learn(const std::optional<Record>& previous, double applied, Extent told, Extent target, Extent rendered);
// The compensation a record calls for (1: none).
double compensation_for(const Record&);

// A record from its JSON (nullopt: not a valid record).
std::optional<Record> read_record(const Json&, bool own);
Json record_json(const Record&);

// The bundled table's entry for a package.
std::optional<Record> bundled(std::string_view package);
// The user's records file in a data folder.
fs::path user_file(const fs::path& data);
// The user's record of a package, if any (unreadable files count as none).
std::optional<Record> user_record(const fs::path& data, std::string_view package);
// The record in force for a package: the user's, else the bundled one.
std::optional<Record> record(const fs::path& data, std::string_view package);
} // namespace venture::library::eye_scale
