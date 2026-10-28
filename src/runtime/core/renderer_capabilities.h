#pragma once
namespace venture::renderer_capabilities {
// VrApi composition is full-rate. There is no variable-rate backend yet;
// advertise this consistently and prepare optional engine defaults accordingly.
inline constexpr bool vrapi_variable_rate_shading = false;
}
