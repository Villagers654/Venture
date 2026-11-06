#include "hle/openxr/passthrough_report.h"
#include "common/paths.h"

namespace venture::hle::openxr {
void save_passthrough_report(Json report) {
  try {
    report["updated"] = paths::timestamp();
    paths::atomic_write(paths::data_dir() / "passthrough.json", report.dump(2));
  } catch (...) {}
}
} // namespace venture::hle::openxr
