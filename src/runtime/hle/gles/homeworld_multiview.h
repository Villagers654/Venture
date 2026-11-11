#pragma once
#include "gfx/dispatch.h"
namespace venture::gfx::homeworld_multiview {
bool enabled();
hle::Handler handler(std::string_view name);
}
