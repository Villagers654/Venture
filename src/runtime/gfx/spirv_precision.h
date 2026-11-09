#pragma once
// Guest GLES mediump reaches the host driver as SPIR-V RelaxedPrecision (ANGLE
// enablePrecisionQualifiers), and desktop drivers then run that math as fp16.
// Quest content leans on Adreno computing vector magnitudes with more range:
// UE4 materials take length() of a mediump offset in centimetres, whose squared
// sum overflows fp16 past 2.56 m (First Contact drew everything farther away
// untextured). Everything else keeps its relaxed precision; only the results of
// length, distance, normalize and dot, and a sqrt or inversesqrt of a dot, are
// computed at full precision.
#include <cstdint>
#include <span>
#include <vector>

namespace venture::gfx {
// Fills `out` and returns true when the module had relaxed magnitudes to
// promote; returns false (and leaves `out` alone) for anything else, including
// modules it cannot parse.
bool promote_relaxed_magnitudes(std::span<const std::uint32_t> words, std::vector<std::uint32_t>& out);
}
