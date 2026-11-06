#include "launcher/app.h"
#include "launcher/sign_in_link.h"
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
  // A browser handing back Meta's sign-in address (sign_in_link.h).
  if (args.size() == 4 && args[1] == "--sign-in-callback")
    return venture::launcher::sign_in_link::deliver(venture::paths::from_utf8(args[2]), args[3]);
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
  // A browser handing back Meta's sign-in address (sign_in_link.h).
  if (argc == 4 && std::strcmp(argv[1], "--sign-in-callback") == 0)
    return venture::launcher::sign_in_link::deliver(argv[2], argv[3]);
  if (int helper = venture::launcher::auth_browser_helper(argc, argv); helper >= 0)
    return helper;
  return venture::launcher::run(venture::launcher::parse_options({argv, argv + argc}));
}
#endif
