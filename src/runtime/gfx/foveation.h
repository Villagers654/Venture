#pragma once
// Foveated rendering: games' eye passes shade fewer fragments away from where
// the user looks (eye-tracked) or away from the lens centre (fixed).
//
// The shape follows Meta's foveation (XR_FB_foveation_configuration, Quest's
// fixed and eye-tracked foveation): full resolution in the middle, half and
// then quarter resolution (2x2 fragments) towards the edges, with Low, Medium
// and High levels that shrink the full-resolution region. Venture defines the
// regions by angle from the centre (eccentricity), so they cover the same
// part of the view at any eye resolution and field of view. Eye-tracked
// regions are smaller: they follow the gaze, so only the gaze error and
// latency (about 50 ms on Quest Pro) need margin.
//
// This file holds the pattern only; gfx/foveation_vulkan.h applies it to the
// game's render passes as a shading-rate attachment (VK_KHR_fragment_shading_rate,
// desktop GPUs) or a fragment density map (VK_EXT_fragment_density_map, the
// Steam Frame's Adreno).
#include <cstdint>
#include <span>
#include <string_view>

namespace venture::gfx::foveation {
// VENTURE_FOVEATION = auto | fixed | off
//   auto (the launcher's "Foveated rendering" Automatic, the default) is
//   eye-tracked when the headset reports eye tracking, else off. fixed centres
//   the pattern on the lens; off renders everything at full resolution.
enum class Mode { automatic, fixed, off };
Mode parse_mode(std::string_view value);
const char* mode_name(Mode mode);

// XR_FB_foveation_configuration's levels.
enum class Level : std::uint8_t { none, low, medium, high };
// The level a game asked for (XR_FB_foveation profiles, VrApi's foveation
// level) or, when it asked for none, Venture's default: Medium, Meta's
// recommended balance. Foveation is Venture's own setting, so a game's "none"
// (often a Quest 3 tuning for its faster GPU) does not turn it off.
Level effective_level(Level requested);

// Fragment sizes the pattern uses. Every GPU with shading-rate attachments
// supports these (VK_KHR_fragment_shading_rate's required rates), and a
// density map expresses them as densities 1 and 0.5.
enum class Rate : std::uint8_t { full, half_x, half_y, quarter };

// One eye's projection, as XrFovf: angles in radians from the view axis,
// left and down negative.
struct Fov {
  float left = -0.8f, right = 0.8f, up = 0.8f, down = -0.8f;
};
// Where the pattern is centred, as a direction in the eye's view space (x
// right, y up, z forward is -1). Fixed foveation looks straight ahead.
struct Centre {
  float x = 0, y = 0, z = -1;
};

struct Pattern {
  Level level = Level::medium;
  bool eye_tracked = false;
  // XR_FB_foveation_configuration's verticalOffset: the fixed pattern's
  // centre moves up by this many degrees ("High Top" games keep the upper
  // view sharper). Ignored when eye-tracked.
  float vertical_offset_degrees = 0;
};
// Angles (degrees from the centre) where the half- and quarter-rate regions
// begin; 180 means never.
struct Regions {
  float half = 180, quarter = 180;
};
Regions regions(const Pattern& pattern);

// The rate at a point of the eye image: u and v in 0..1 from the image's top
// left, v down.
Rate rate_at(const Pattern& pattern, const Fov& fov, const Centre& centre, float u, float v);

// VK_KHR_fragment_shading_rate's attachment encoding of a rate:
// (log2(width) << 2) | log2(height).
std::uint8_t shading_rate_value(Rate rate);
// VK_EXT_fragment_density_map's R8G8_UNORM density of a rate (x, y).
void density_value(Rate rate, std::uint8_t& x, std::uint8_t& y);

// Fills a width x height map whose texels each cover texel_width x
// texel_height pixels of a framebuffer_width x framebuffer_height eye image.
// Shading-rate maps take one byte per texel; density maps two (R8G8).
void fill_shading_rates(std::span<std::uint8_t> out, std::uint32_t width, std::uint32_t height,
                        std::uint32_t texel_width, std::uint32_t texel_height, std::uint32_t framebuffer_width,
                        std::uint32_t framebuffer_height, const Pattern& pattern, const Fov& fov,
                        const Centre& centre);
void fill_densities(std::span<std::uint8_t> out, std::uint32_t width, std::uint32_t height, const Pattern& pattern,
                    const Fov& fov, const Centre& centre);

// The share of the eye image's fragments a map shades (1 at full rate), for
// the log and tests.
float shaded_fraction(std::span<const std::uint8_t> shading_rates);
}  // namespace venture::gfx::foveation
