#include <iostream>
#include <nlohmann/json.hpp>

#include "java/conversion.h"
#include "common/paths.h"
#include "core/process_runtime.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
int main(int argc, char** argv) {
#ifdef _WIN32
  int count;
  auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
  std::vector<std::string> arguments;
  for (int i = 0; i < count; ++i) arguments.push_back(venture::paths::utf8(std::filesystem::path(wide[i])));
  LocalFree(wide);
  std::vector<char*> pointers;
  for (auto& argument : arguments) pointers.push_back(argument.data());
  argc = count;
  argv = pointers.data();
#endif
  if (argc == 3 && std::string_view(argv[1]) == "--configure") {
    try {
      venture::configure_game(venture::paths::from_utf8(argv[2]));
      auto files = venture::extract_native_libraries();
      return files.size() == 1 && venture::resolve_guest_path(venture::paths::utf8(files[0])) == files[0] ? 0 : 1;
    } catch (const std::exception& e) {
      std::cerr << e.what() << '\n';
      return 1;
    }
  }
  // java tools apk cache [framework hierarchy]
  if (argc != 5 && argc != 7) return 64;
  venture::java::ConversionOptions options;
  options.java = argv[1];
  options.tools = argv[2];
  options.apk = argv[3];
  options.cache = argv[4];
  options.package = "com.venture.fixture";
  if (argc == 7) {
    options.framework_jars = {argv[5]};
    options.hierarchy_metadata = {argv[6]};
  }
  venture::java::ConvertedApk converted;
  std::string error;
  if (!venture::java::convert_apk(options, &converted, &error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << nlohmann::json{{"app_jar", converted.app_jar}, {"report", converted.report}}.dump() << '\n';
}
