#pragma once
#include "common/android/adb.h"
#include "common/android/manifest.h"
#include "common/meta/store.h"
#include <memory>

// Importing a game installed elsewhere: an app on a Quest reached over adb, or
// an APK with its data folder (loose or in a ZIP archive) on this computer.
// An import installs the source's files as they are: its APK (a modified
// build included), expansion files and other data files, needing no account.
// When a signed-in account owns Meta's build of the same version, that build
// only supplies the files the source lacks.
namespace venture::import {
struct SourceFile {
  std::string name;  // its basename in the install
  std::uint64_t size = 0;
  std::string location;  // where the source reads it from (path, member or device path)
};
class Source {
public:
  virtual ~Source() = default;
  // Saved with a queued job so an interrupted import resumes from the same
  // place: "path:<file or folder>" or "adb:<serial>".
  virtual std::string spec() const = 0;
  virtual std::string describe() const = 0;  // for the user
  // Streams `file` from `offset` (a multiple of android::adb::kPullBlock) to
  // its end; throws when it is gone, changed size, or the read failed.
  void read(const SourceFile &file, const std::function<void(std::span<const std::uint8_t>)> &sink,
            std::stop_token stop, std::uint64_t offset = 0) const {
    read_from(file, offset, sink, stop);
  }
  // The file's SHA-256 as the source computes it on its side, for sources
  // reached over a link (a headset); nullopt when it cannot.
  virtual std::optional<std::string> sha256(const SourceFile &, std::stop_token) const { return std::nullopt; }
  android::Manifest manifest;
  SourceFile apk;
  std::vector<SourceFile> data;
  // The app's external storage on a headset (saves), named by their path
  // under /sdcard/Android/data/<package>/.
  std::vector<SourceFile> saves;
  std::vector<std::string> skipped;  // found but not importable (nested folders, split APKs)
  std::uint64_t bytes() const;  // the APK and data
  std::uint64_t save_bytes() const;

protected:
  virtual void read_from(const SourceFile &file, std::uint64_t offset,
                         const std::function<void(std::span<const std::uint8_t>)> &sink,
                         std::stop_token stop) const = 0;
};
// Every app found at a picked path: an APK, a folder holding APKs (each with
// its data in a <package>/ folder, Android/obb/<package>/, or beside it), or
// a ZIP archive laid out the same way.
std::vector<std::unique_ptr<Source>> open_local(const fs::path &picked, std::stop_token stop = {});
std::unique_ptr<Source> open_device(const android::adb::Client &client, const android::adb::App &app);
// Reopens a saved spec for one package; throws when it is gone.
std::unique_ptr<Source> reopen(std::string_view spec, std::string_view package, const fs::path &adb,
                               std::stop_token stop = {});

// What an import of `source` does with each file.
struct Step {
  meta::File file;        // as recorded in the install
  const SourceFile *from;  // null: downloaded from Meta
};
struct Plan {
  meta::Plan plan;          // what plan.json records
  std::vector<Step> steps;  // the files to write, in order
};
// Into Meta's build `catalog` of the source's version: the source's APK
// replaces the catalog's, catalog files the source has (same name and size)
// come from it, and the rest required is downloaded. Source files the catalog
// does not list are kept as optional local assets (empty id).
Plan plan(const meta::Plan &catalog, const Source &source);
// The source alone: its APK, expansion files ("obb") and other data files
// (required assets), all copied. `app_id` is Meta's id for the app when
// known; the plan is local (no binary id) either way.
Plan local_plan(const Source &source, std::string app_id = {});
// The Meta build of `package` at `version_code` among `builds` (Store::builds).
std::string binary_for(const Json &builds, std::int64_t version_code);
}  // namespace venture::import
