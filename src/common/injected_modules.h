// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Libraries other programs put into the game's process: capture and overlay
// tools (Medal's MedalHook_x64.dll) inject DLLs directly, past the Vulkan layer
// filter, and an AMD user's H3VR2 and Pavlov runs lost the GPU device until
// Medal was closed. The runtime logs them; the launcher names them in its
// failure text and the debug report.
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::injected_modules {
struct Module {
  std::string path;  // UTF-8, as the system reports it
  // From the file's version resource (Windows); empty when it has none.
  std::string product, company, description, version;
  // From the install folder of the program that started Venture (Steam's
  // overlay, for a game started from a Steam library shortcut): listed, but not
  // named as a cause, since that program is in use.
  bool from_starter = false;
};

// The runtime log's lines: one summary per check, one detail line per module.
inline constexpr std::string_view kSummary = "Injected modules: ";
inline constexpr std::string_view kDetail = "Injected module: ";
// Ends a detail line of a module from the program that started Venture.
inline constexpr std::string_view kFromStarter = "from the program that started Venture";
// The environment variable the launcher passes those programs' folders in.
inline constexpr const char *kStartersVariable = "VENTURE_STARTED_BY";

// Whether `module` lies inside one of `roots` (directories). Windows paths
// compare without case and with either separator.
bool inside(std::string_view module, std::span<const std::string> roots, bool windows);
// The folders of the programs that started Venture: the folder of each of
// `ancestors` (executable paths, parent first) that is neither one of
// `excluded` (the system's and Venture's own folders) nor inside one.
std::vector<std::string> starter_folders(std::span<const std::string> ancestors,
                                         std::span<const std::string> excluded, bool windows);
// The program a user knows the module by: its product name, else its file
// description, else its file name.
std::string program_name(const Module &module);
// The distinct program names of `modules` in order, joined with ", " ("none"
// for none), leaving out modules from the program that started Venture: the
// summary line's text.
std::string summary(std::span<const Module> modules);
// One module's detail line text: its path, product, company and version, and
// kFromStarter for a module from the program that started Venture.
std::string detail(const Module &module);

// The programs of the last summary line in a runtime log; empty when there is
// none or it says "none".
std::string last_programs(std::string_view log);
// The distinct detail line texts in a runtime log, in order.
std::vector<std::string> details(std::string_view log);
// What the user should do when the game lost its GPU device with `programs`
// (a summary) loaded into it.
std::string device_loss_advice(std::string_view programs);

// Platform parts. The modules loaded in this process that come from none of
// the trusted places: Windows' system folders (with the GPU driver's
// DriverStore), Venture's install, the Vulkan drivers the loader is given, the
// OpenXR runtime the game runs on, the registered OpenXR API layers and the
// folders passed to trust(). Linux: the libraries LD_PRELOAD names.
std::vector<Module> scan();
// A folder Venture loads libraries from itself (the JDK it runs the game's Java on).
void trust(const std::filesystem::path &folder);
// The folders passed to trust(), UTF-8.
std::vector<std::string> trusted_folders();
// The launcher, at its start: records the folders of the programs that started
// it (its parent and their ancestors) in kStartersVariable, which the runtime
// it starts inherits.
void record_starters();
// Platform part: the executables of this process's ancestors, parent first.
std::vector<std::string> ancestor_images();
// Platform part: folders whose programs never count as starters (the system's).
std::vector<std::string> system_folders();
// Platform part: sets a variable in this process's environment (UTF-8).
void set_process_variable(const char *name, const std::string &value);
// Logs the summary and the detail line of each module not logged before, when
// `always` or the summary changed since the last check.
void log_loaded(bool always = false);
}  // namespace venture::injected_modules
