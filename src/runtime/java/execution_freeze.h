#pragma once
#include <memory>
#include <optional>
#include <filesystem>
#include <string>

namespace venture::java {
// Uses HotSpot's JVMTI suspension, not host-thread suspension. The caller is a
// host-only monitor, and no guest/JVM work may be required by subsequent native
// cleanup. Retain until kernel termination; destruction resumes for fixtures or
// a rejected transition. Native guest calls must already have unwound.
class ExecutionFreeze {
 public:
  ExecutionFreeze();
  ~ExecutionFreeze();
  bool freeze(std::string* error);
  // Read the already resolved volatile field, including after suspension;
  // never invoke Java/class loading once execution is stopped.
  std::optional<int> recorded_failure() const;
  // `saving` (optional): set when the only runnable application threads are
  // inside JDK file natives (write, fsync), i.e. a save is completing.
  bool application_quiet(std::string* error, bool* saving = nullptr);
  bool save_work_quiet(const std::filesystem::path&, std::string* error);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::java
