#include "core/engine_defaults.h"
#include "core/process.h"
#include "common/log.h"
#include "common/paths.h"
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <stdexcept>
#include <cctype>

namespace venture::engine_policy {
namespace {
constexpr std::string_view begin = "-- Venture renderer defaults begin";
constexpr std::string_view end = "-- Venture renderer defaults end";
constexpr std::uintmax_t maximum_size = 1024 * 1024;

bool has_library(std::span<const std::filesystem::path> libraries, std::string_view name) {
  for (const auto& library : libraries)
    if (library.filename() == name) return true;
  return false;
}

bool has_explicit_shading_setting(std::string_view text) {
  constexpr std::string_view key = "r_variable_rate_shading";
  while (!text.empty()) {
    const auto newline = text.find('\n');
    auto line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
    if (line.size() < key.size()) continue;
    bool matches = true;
    for (std::size_t i = 0; i < key.size(); ++i)
      matches &= std::tolower(static_cast<unsigned char>(line[i])) == key[i];
    if (matches && (line.size() == key.size() || line[key.size()] == '=' ||
        std::isspace(static_cast<unsigned char>(line[key.size()])))) return true;
  }
  return false;
}

std::string without_owned_block(std::string text) {
  // Include the separator we wrote, so restoring capability restores the
  // original file byte-for-byte unless the user added subsequent commands.
  for (unsigned removed = 0; ; ++removed) {
    auto position = text.find(begin);
    if (position == std::string::npos) return text;
    if (removed == 32 || (position && text[position - 1] != '\n'))
      throw std::runtime_error("Invalid renderer configuration ownership marker");
    auto after_begin = position + begin.size();
    const auto newline = text.compare(after_begin, 2, "\r\n") == 0 ? "\r\n" : "\n";
    if (text.compare(after_begin, std::char_traits<char>::length(newline), newline) != 0)
      throw std::runtime_error("Invalid renderer configuration ownership marker");
    auto finish = text.find(std::string(newline) + std::string(end) + newline, after_begin);
    if (finish == std::string::npos)
      throw std::runtime_error("Incomplete renderer configuration ownership block");
    finish += std::char_traits<char>::length(newline) + end.size() + std::char_traits<char>::length(newline);
    auto first = position;
    if (first && text[first - 1] == '\n') {
      --first;
      if (first && text[first - 1] == '\r') --first;
    }
    // Later user commands must not join an originally unterminated last line.
    const bool separate = first && finish < text.size() && text[first - 1] != '\n' &&
                          text[finish] != '\n' && text[finish] != '\r';
    text.replace(first, finish - first, separate ? newline : "");
  }
}

void require_owned_directory(const std::filesystem::path& root,
                             const std::filesystem::path& directory) {
  const auto relative = std::filesystem::weakly_canonical(directory).lexically_relative(
      std::filesystem::weakly_canonical(root));
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
    throw std::runtime_error("Renderer configuration directory escapes app storage");
}
}

void prepare_renderer_defaults(const ProcessConfig& config,
    std::span<const std::filesystem::path> libraries, RendererCapabilities capabilities) noexcept try {
  // Public SDK library metadata, never package names, private layouts or code
  // patterns. VrApi reports the same backend capability to the guest.
  if (!has_library(libraries, "libCrySystem.so") || !has_library(libraries, "libvrapi.so")) return;
  if (config.data_dir.empty()) throw std::runtime_error("App storage is not configured");
  const auto directory = config.data_dir / "external-data" / "files";
  require_owned_directory(config.data_dir, directory);
  std::filesystem::create_directories(directory);
  const auto file = directory / "user.cfg";
  std::error_code error;
  const auto status = std::filesystem::symlink_status(file, error);
  if (error && error != std::errc::no_such_file_or_directory)
    throw std::filesystem::filesystem_error("Read renderer configuration", file, error);
  if (std::filesystem::is_symlink(status) ||
      (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)))
    throw std::runtime_error("Renderer configuration must be a regular app-local file");
  std::string original;
  if (std::filesystem::is_regular_file(status)) {
    if (std::filesystem::file_size(file) > maximum_size)
      throw std::runtime_error("Renderer configuration exceeds supported size");
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read renderer configuration");
    original.assign(std::istreambuf_iterator<char>(input), {});
    if (input.bad() || original.size() > maximum_size || original.find('\0') != std::string::npos)
      throw std::runtime_error("Renderer configuration is not a supported text file");
  }
  auto updated = without_owned_block(original);
  if (!capabilities.variable_rate_shading && !has_explicit_shading_setting(updated)) {
    const auto newline = updated.find("\r\n") == std::string::npos ? "\n" : "\r\n";
    // CryEngine's documented startup configuration supports -- comments and
    // last-setting-wins CVar assignments. Leave render cadence to the XR wait.
    // https://www.cryengine.com/docs/static/engines/cryengine-5/categories/23756816/pages/25535264
    updated += newline;
    updated += begin;
    updated += newline;
    updated += "r_variable_rate_shading = 0";
    updated += newline;
    updated += end;
    updated += newline;
  }
  if (updated == original) return;
  paths::atomic_write(file, updated);
  log::info("engine", "CryEngine VrApi renderer defaults updated: variable-rate shading {}",
            capabilities.variable_rate_shading ? "available" : "unavailable");
} catch (const std::exception& error) {
  // Optional renderer defaults must never prevent normal launch or overwrite
  // an unreadable/user-owned configuration. Leave the original file intact.
  log::warn("engine", "Renderer defaults skipped: {}", error.what());
} catch (...) {
  log::warn("engine", "Renderer defaults skipped: configuration unavailable");
}
}
