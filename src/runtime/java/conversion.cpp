#include "java/conversion.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>

#include "common/log.h"
#include "common/paths.h"
#include "platform/java_host.h"
namespace venture::java {
bool convert_apk(const ConversionOptions& options, std::string* error) {
  return convert_apk(options, nullptr, error);
}
bool convert_apk(const ConversionOptions& options, ConvertedApk* output, std::string* error) {
  try {
    // Cache paths can exceed MAX_PATH; the Java converter owns those file
    // operations. Keep the subprocess result in the system temporary directory.
    if (!std::filesystem::is_regular_file(paths::from_utf8(options.java)) ||
        !std::filesystem::is_regular_file(paths::from_utf8(options.tools) / "ready"))
      throw std::runtime_error("incomplete Venture installation: bundled Java or DEX tools missing (" +
                               options.java + ", " + options.tools + ")");
    const auto temporary = std::filesystem::temp_directory_path();
    std::random_device random;
    std::filesystem::path directory;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      auto candidate = temporary / (".venture-java-" + std::to_string(random()) + "-" + std::to_string(random()));
      if (std::filesystem::create_directory(candidate)) {
        directory = std::move(candidate);
        break;
      }
    }
    if (directory.empty()) throw std::runtime_error("cannot create converter result directory");
    auto result_file = directory / "result.json";
    struct Cleanup {
      std::filesystem::path file, dir;
      ~Cleanup() {
        std::error_code ec;
        std::filesystem::remove(file, ec);
        std::filesystem::remove(dir / "request.json", ec);
        std::filesystem::remove(dir, ec);
      }
    } cleanup{result_file, directory};
    auto utf8 = result_file.u8string();
    std::vector<std::string> args{options.java, "-cp",     options.tools,   "com.venture.dex.ApkCache",
                                  options.apk,  "--package", options.package, "--cache",
                                  options.cache};
    args.push_back("--result-file");
    args.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    for (const auto& path : options.framework_jars) {
      args.push_back("--framework");
      args.push_back(path);
    }
    for (const auto& path : options.hierarchy_metadata) {
      args.push_back("--hierarchy");
      args.push_back(path);
    }
    // As for the converter's own child JVMs: launcher-wide Java option injection
    // must not change the verifier, its class path or the driver JVM's startup.
#ifdef _WIN32
    auto launcher = paths::from_utf8(options.java).parent_path().parent_path().parent_path() / "venture-java.exe";
    if (!std::filesystem::is_regular_file(launcher))
      launcher = paths::executable().parent_path() / "java/venture-java.exe";
    auto request = directory / "request.json";
    {
      std::ofstream stream(request, std::ios::binary);
      stream << nlohmann::json{{"command", args}}.dump();
      if (!stream) throw std::runtime_error("cannot write converter request");
    }
    args = {paths::utf8(launcher), paths::utf8(request)};
#endif
    int code = platform::run_process(args, {"JAVA_TOOL_OPTIONS", "_JAVA_OPTIONS", "JDK_JAVA_OPTIONS", "CLASSPATH"});
    std::string report;
    if (std::filesystem::exists(result_file)) {
      std::ifstream input(result_file);
      nlohmann::json result;
      input >> result;
      report = result.value("report", std::string());
      if (output) {
        output->app_jar = result.at("jar").get<std::string>();
        output->report = report;
        output->translation_complete = (code == 0 || code == 2) && result.value("translation_complete", false);
      }
      if (!code && result.at("ready").get<bool>()) return true;
    }
    // The debug report reads the failures from the report this names.
    throw std::runtime_error("APK conversion/verification failed (exit " + std::to_string(code) + "); " +
                             (report.empty() ? std::string("see package cache report.json") : "Details: " + report));
  } catch (const std::exception& e) {
    // Deferred optional linkage is not a failure; the caller reports it.
    if (!output || !output->translation_complete) log::error("java", "{}", e.what());
    if (error) *error = e.what();
    return false;
  }
}
}  // namespace venture::java
