#include "common/openxr_layers.h"
#include "common/json.h"
#include "common/paths.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace venture::openxr_layers {
namespace {
std::uint16_t le16(const unsigned char *p) { return std::uint16_t(p[0] | (p[1] << 8)); }
[[maybe_unused]] std::uint32_t le32(const unsigned char *p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
const char *machine_name(std::uint16_t machine, bool pe) {
  if (pe) return machine == 0x8664 ? "x64" : machine == 0xAA64 ? "ARM64" : machine == 0x14C ? "x86" : "another CPU";
  return machine == 62 ? "x86-64" : machine == 183 ? "AArch64" : machine == 3 ? "x86" : "another CPU";
}
std::string text_of(const Json &node, const char *key) {
  const auto it = node.find(key);
  return it != node.end() && it->is_string() ? it->get<std::string>() : std::string();
}
}  // namespace

std::string library_problem(const std::filesystem::path &library) {
  std::error_code ec;
  if (!std::filesystem::exists(library, ec)) return "the library " + paths::utf8(library) + " is missing";
  std::ifstream in(paths::io_path(library), std::ios::binary);
  std::array<unsigned char, 4096> head{};
  if (!in) return "the library " + paths::utf8(library) + " cannot be read";
  in.read(reinterpret_cast<char *>(head.data()), head.size());
  const auto size = static_cast<std::size_t>(in.gcount());
  const std::string named = "the library " + paths::utf8(library);
#ifdef _WIN32
  if (size < 0x40 || head[0] != 'M' || head[1] != 'Z') return named + " is not a DLL";
  const auto pe = le32(&head[0x3C]);
  if (std::size_t(pe) + 6 > size || std::memcmp(&head[pe], "PE\0\0", 4) != 0) return named + " is not a DLL";
  const auto machine = le16(&head[pe + 4]);
#if defined(_M_ARM64) || defined(__aarch64__)
  constexpr std::uint16_t wanted = 0xAA64;
#elif defined(_M_X64) || defined(__x86_64__)
  constexpr std::uint16_t wanted = 0x8664;
#else
  constexpr std::uint16_t wanted = 0x14C;
#endif
  if (machine != wanted)
    return named + " is a " + machine_name(machine, true) + " DLL, not one for this " + machine_name(wanted, true) +
           " process";
#else
  if (size < 20 || std::memcmp(head.data(), "\x7f" "ELF", 4) != 0) return named + " is not a shared library";
  const auto machine = le16(&head[18]);
#if defined(__aarch64__)
  constexpr std::uint16_t wanted = 183;
#elif defined(__x86_64__)
  constexpr std::uint16_t wanted = 62;
#else
  constexpr std::uint16_t wanted = 3;
#endif
  if (machine != wanted)
    return named + " is an " + machine_name(machine, false) + " library, not one for this " +
           machine_name(wanted, false) + " process";
#endif
  return {};
}

Manifest parse(const std::filesystem::path &path, std::string_view text) {
  Manifest out;
  out.manifest = path;
  // jsoncpp, the loader's parser, accepts comments.
  const auto json = Json::parse(text.begin(), text.end(), nullptr, false, true);
  if (json.is_discarded() || !json.is_object()) {
    out.problem = "the manifest is not valid JSON";
    return out;
  }
  const auto layer = json.find("api_layer");
  if (layer == json.end() || !layer->is_object()) {
    out.problem = "the manifest has no \"api_layer\"";
    return out;
  }
  out.name = text_of(*layer, "name");
  out.library_path = text_of(*layer, "library_path");
  out.disable_environment = text_of(*layer, "disable_environment");
  out.enable_environment = text_of(*layer, "enable_environment");
  if (const auto functions = layer->find("functions"); functions != layer->end() && functions->is_object())
    for (const auto &[from, to] : functions->items())
      if (to.is_string()) out.functions[from] = to.get<std::string>();
  for (const char *required : {"name", "api_version", "library_path", "implementation_version"})
    if (const auto field = layer->find(required); field == layer->end() || !field->is_string()) {
      out.problem = std::string("the manifest lacks \"") + required + "\"";
      return out;
    }
  if (const auto version = json.find("file_format_version"); version == json.end() || !version->is_string())
    out.problem = "the manifest lacks \"file_format_version\"";
  else if (out.disable_environment.empty()) out.problem = "the manifest lacks \"disable_environment\"";
  // A path with a separator is the file itself (relative to the manifest); a
  // bare name is found through the library search path.
  if (out.library_path.find_first_of("/\\") != std::string::npos) {
    auto library = paths::from_utf8(out.library_path);
    out.library = library.is_absolute() ? library : (path.parent_path() / library).lexically_normal();
  } else {
    out.library = paths::from_utf8(out.library_path);
  }
  return out;
}

Manifest read(const std::filesystem::path &path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    Manifest out;
    out.manifest = path;
    out.problem = "the manifest is missing";
    return out;
  }
  std::ifstream in(paths::io_path(path), std::ios::binary);
  std::ostringstream text;
  if (in) text << in.rdbuf();
  if (!in) {
    Manifest out;
    out.manifest = path;
    out.problem = "the manifest cannot be read";
    return out;
  }
  auto out = parse(path, text.str());
  if (out.problem.empty() && out.library.has_parent_path()) out.problem = library_problem(out.library);
  return out;
}

bool enabled(const Manifest &layer) {
  if (!layer.disable_environment.empty() && environment_set(layer.disable_environment.c_str())) return false;
  return layer.enable_environment.empty() || environment_set(layer.enable_environment.c_str());
}
}  // namespace venture::openxr_layers
