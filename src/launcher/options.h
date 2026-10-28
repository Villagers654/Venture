#pragma once
// Launcher command line. Header-only and free of UI dependencies so the
// accepted options are covered by the common test suite.
#include "common/json.h"
#include "common/paths.h"
#include <string>
#include <vector>
namespace venture::launcher {
struct Options {
  int smoke_seconds = 0;
  bool smoke_play = false, smoke_auth = false, smoke_details = false;
  bool offline = false;
  std::string launch_package;
  bool launch_requested = false, invalid_launch = false;
  bool automation = false;
  bool cli = false; // venture-cli: no window, its own log
  std::string smoke_view,
      smoke_select,                  // smoke_select: app id whose page to open
      smoke_install;                 // app id to install through the Install action
  fs::path smoke_report, screenshot; // screenshot: PNG of the last smoke frame
  fs::path smoke_import;             // a path scanned as if dropped on the window
  bool smoke_import_start = false;   // then imports what it found and exits when done
  bool interactive() const {
    return !automation && !cli && !launch_requested && smoke_seconds == 0 &&
           !smoke_play && !smoke_auth && !smoke_details && smoke_view.empty() &&
           smoke_select.empty() && smoke_install.empty() && smoke_report.empty() &&
           screenshot.empty() && smoke_import.empty();
  }
};
inline Options parse_options(const std::vector<std::string> &args) {
  Options out;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i].starts_with("--smoke-") || args[i] == "--screenshot" ||
        args[i] == "--headless") out.automation = true;
    if (args[i] == "--launch") {
      if (out.launch_requested) out.invalid_launch = true;
      out.launch_requested = true;
      if (i + 1 < args.size() && paths::safe_package(args[i + 1])) out.launch_package = args[++i];
      else out.invalid_launch = true;
    }
    else if (args[i] == "--smoke-seconds" && i + 1 < args.size())
      out.smoke_seconds = int(number(Json(args[++i])));
    else if (args[i] == "--smoke-report" && i + 1 < args.size())
      out.smoke_report = paths::from_utf8(args[++i]);
    else if (args[i] == "--smoke-play")
      out.smoke_play = true;
    else if (args[i] == "--smoke-auth")
      out.smoke_auth = true;
    else if (args[i] == "--smoke-details")
      out.smoke_details = true;
    else if (args[i] == "--offline")
      out.offline = true;
    else if (args[i] == "--smoke-view" && i + 1 < args.size())
      out.smoke_view = args[++i];
    else if (args[i] == "--smoke-select" && i + 1 < args.size())
      out.smoke_select = args[++i];
    else if (args[i] == "--smoke-install" && i + 1 < args.size())
      out.smoke_install = args[++i];
    else if (args[i] == "--smoke-import" && i + 1 < args.size())
      out.smoke_import = paths::from_utf8(args[++i]);
    else if (args[i] == "--smoke-import-start")
      out.smoke_import_start = true;
    else if (args[i] == "--screenshot" && i + 1 < args.size())
      out.screenshot = paths::from_utf8(args[++i]);
  }
  return out;
}
} // namespace venture::launcher
