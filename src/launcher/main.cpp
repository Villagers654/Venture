#include "launcher/app.h"
#include "common/meta/native_host.h"
#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i)
    args.push_back(venture::paths::utf8(venture::fs::path(argv[i])));
  LocalFree(argv);
  // Package checks call the installed binary headlessly. A GUI-subsystem
  // program has no console: write to redirected stdout, else the parent's.
  if (args.size() == 2 && args[1] == "--version") {
    auto out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
      out = GetStdHandle(STD_OUTPUT_HANDLE);
    const std::string text = std::string("Venture ") + VENTURE_VERSION + "\n";
    DWORD written = 0;
    if (out && out != INVALID_HANDLE_VALUE) WriteFile(out, text.data(), DWORD(text.size()), &written, nullptr);
    return 0;
  }
  // Chrome launches the native messaging host (this same executable) with
  // no window; the args shape itself says so, before any GUI setup runs.
  if (int host = venture::meta::handle_native_messaging_launch(args); host >= 0)
    return host;
  return venture::launcher::run(venture::launcher::parse_options(args));
}
#else
#include "launcher/auth_browser.h"
#include <cstdio>
#include <cstring>
int main(int argc, char **argv) {
  // Package managers and smoke tests check the installed binary headlessly.
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
    std::printf("Venture %s\n", VENTURE_VERSION);
    return 0;
  }
  if (int helper = venture::launcher::auth_browser_helper(argc, argv); helper >= 0)
    return helper;
  if (int host = venture::meta::handle_native_messaging_launch({argv, argv + argc}); host >= 0)
    return host;
  return venture::launcher::run(venture::launcher::parse_options({argv, argv + argc}));
}
#endif
