// A failed JIT assertion (mcl's ASSERT, ASSERT_MSG, UNREACHABLE) goes to
// Venture's log before the process ends (MIT). mcl's own handler prints only
// to stderr, which a launched runtime has no reader for, and then calls
// std::terminate: a user's log ended without any trace of the failure.
// Linked ahead of the mcl library, this definition replaces its handler.
#include <mcl/assert.hpp>
#include <exception>
#include <string>
#include "common/log.h"

namespace mcl::detail {
[[noreturn]] void assert_terminate_impl(const char* expr_str, fmt::string_view msg, fmt::format_args args) {
  std::string text = std::string("JIT assertion failed: ") + (expr_str ? expr_str : "?");
  try {
    const auto detail = fmt::vformat(msg, args);
    if (!detail.empty() && detail != "(none)") text += " (" + detail + ")";
  } catch (...) {
  }
  ::venture::log::set_fatal_reason(text);
  ::venture::log::write(::venture::log::Level::error, "jit", text);
  std::terminate();
}
}  // namespace mcl::detail
