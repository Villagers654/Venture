#pragma once
#include <functional>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "core/guest.h"

namespace venture::test::audio_core {
// HLE entry points and callbacks execute through the real core.
using Callback = std::function<std::uint64_t(std::span<const std::uint64_t>)>;
GuestAddr add_callback(Callback fn);
std::int64_t call(GuestAddr thunk, std::initializer_list<std::uint64_t> args);
GuestAddr symbol(std::string_view library, std::string_view name);
bool stop_null_output();
}  // namespace venture::test::audio_core
