// Connect the JIT's diagnostics to Venture's logger (MIT).
#pragma once
#include <type_traits>
#include <fmt/format.h>
#include "common/log.h"

template <typename T>
struct fmt::formatter<T, std::enable_if_t<std::is_enum_v<T>, char>>
    : formatter<std::underlying_type_t<T>> {
    template <typename FormatContext>
    auto format(const T& value, FormatContext& ctx) const -> decltype(ctx.out()) {
        return fmt::formatter<std::underlying_type_t<T>>::format(
            static_cast<std::underlying_type_t<T>>(value), ctx);
    }
};
#define LOG_ERROR(category, ...) ::venture::log::write(::venture::log::Level::error, "dynarmic", ::fmt::format(__VA_ARGS__))
#define LOG_WARNING(category, ...) ::venture::log::write(::venture::log::Level::warn, "dynarmic", ::fmt::format(__VA_ARGS__))
#define LOG_INFO(category, ...) ::venture::log::write(::venture::log::Level::info, "dynarmic", ::fmt::format(__VA_ARGS__))
#define LOG_DEBUG(category, ...) ::venture::log::write(::venture::log::Level::debug, "dynarmic", ::fmt::format(__VA_ARGS__))
#define LOG_CRITICAL(category, ...) LOG_ERROR(category, __VA_ARGS__)
