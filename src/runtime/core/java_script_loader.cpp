#include "core/java_script_loader.h"
#include "common/digest.h"
#include "common/log.h"
#include "common/paths.h"
#include "hosted_java_source.h"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <string>

namespace venture {
namespace {
using Json = nlohmann::json;
bool elf_file(const std::filesystem::path& path) {
  std::ifstream input(paths::io_path(path), std::ios::binary);
  std::array<char, 4> magic{};
  return input.read(magic.data(), magic.size()) && magic == std::array<char, 4>{'\177', 'E', 'L', 'F'};
}
std::string hash(std::string_view value) {
  return digest::sha256({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
}
std::size_t prepare_java_script_loaders(
    std::span<const std::filesystem::path> libraries,
    const std::function<std::filesystem::path(std::string_view)>& resolve_absolute) {
  std::size_t prepared = 0;
  for (const auto& config_path : libraries) {
    // A startup library may have any name and this Android configuration
    // suffix. Recognize its documented schema, never an application identity.
    const auto name = paths::utf8(config_path.filename());
    constexpr std::string_view suffix = ".config.so";
    if (!name.ends_with(suffix)) continue;
    const auto binary = config_path.parent_path() /
        paths::from_utf8(name.substr(0, name.size() - suffix.size()) + ".so");
    if (!elf_file(binary)) continue;
    auto config = Json::parse(paths::read(config_path, 1024 * 1024), nullptr, false);
    if (!config.is_object() || !config.contains("interaction")) continue;
    auto& interaction = config["interaction"];
    if (!interaction.is_object() || !interaction.contains("type") || !interaction["type"].is_string() ||
        interaction["type"] != "script" ||
        !interaction.contains("path") || !interaction["path"].is_string()) continue;
    if (interaction.contains("on_change") && interaction["on_change"] != "ignore") continue;
    if (config.contains("runtime") && !config["runtime"].is_string()) continue;
    const auto runtime = config.value("runtime", "default");
    if (runtime != "default" && runtime != "qjs" && runtime != "v8" &&
        runtime != "jit" && runtime != "interpreter") continue;
    const auto script_name = interaction["path"].get<std::string>();
    if (script_name.empty()) continue;
    std::filesystem::path script;
    if (script_name.starts_with('/')) script = resolve_absolute(script_name);
    else {
      const auto relative = paths::from_utf8(script_name);
      if (relative.has_root_path()) continue;
      script = config_path.parent_path() / relative;
      if (!paths::within(config_path.parent_path(), script)) continue;
    }
    if (script.empty() || !std::filesystem::is_regular_file(paths::io_path(script))) continue;
    const auto original = paths::read(script, 64 * 1024 * 1024);
    // Compiled bytecode is not JavaScript source. Leave unsupported formats to
    // their original loader rather than interpreting them as a text program.
    if (original.find('\0') != std::string::npos) continue;
    std::string source_literal;
    try { source_literal = Json(original).dump(-1, ' ', true); }
    catch (const Json::type_error&) { continue; } // non-UTF-8 bytecode/data
    // Script.evaluate preserves the original program's directive prologue,
    // global declarations and source name; concatenating a prelude would not.
    const std::string wrapper = std::string(hosted_java_source) +
        "\nScript.evaluate(" + Json(script_name).dump(-1, ' ', true) + "," + source_literal + ");\n";
    const auto wrapper_name = name + ".venture-java-" + hash(wrapper) + ".js";
    const auto wrapper_path = config_path.parent_path() / paths::from_utf8(wrapper_name);
    if (!std::filesystem::is_regular_file(paths::io_path(wrapper_path)) ||
        std::filesystem::file_size(paths::io_path(wrapper_path)) != wrapper.size() ||
        paths::read(wrapper_path, wrapper.size() + 1) != wrapper)
      paths::atomic_write(wrapper_path, wrapper);
    interaction["path"] = wrapper_name;
    // QuickJS is the documented interpreter backend; it needs no second
    // architecture-specific JavaScript JIT inside the guest process.
    config["runtime"] = "qjs";
    paths::atomic_write(config_path, config.dump());
    ++prepared;
  }
  if (prepared) log::info("java", "Prepared hosted Java support for {} embedded startup script(s)", prepared);
  return prepared;
}
}
