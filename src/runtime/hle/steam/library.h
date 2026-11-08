#pragma once
#include "common/steam.h"
#include "core/hle.h"
namespace venture { class HleLibrary; }
namespace venture::steam {
struct Sdk;
// Inspection registers the same ABI without initializing the real Steam client.
void register_library(const Plan&, const fs::path& apk);
void add_exports(HleLibrary&, const Sdk&);
GuestAddr make_cpp_thunk(std::string_view name, hle::Handler handler);
} // namespace venture::steam
