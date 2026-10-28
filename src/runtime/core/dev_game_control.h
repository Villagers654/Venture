#pragma once
#include "common/json.h"
#include "core/guest.h"
#include <memory>
#include <atomic>
#include <string_view>

namespace venture { class GuestThread; }
namespace venture::dev_control {
// Compiled into developer configurations only, and requires both DEV_RUN=1
// and a command file. The ordinary dispatch path tests one cached flag; a
// sealed batch clears it when finished so measurement has the disabled path.
extern std::atomic<bool> enabled;
void boundary(GuestThread&, std::string_view function, bool completed);
Json parse_commands(std::string_view);
class Il2Cpp {
 public:
  explicit Il2Cpp(GuestThread&, std::string_view library = "libil2cpp.so");
  ~Il2Cpp();
  Json execute(const Json&);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace venture::dev_control
