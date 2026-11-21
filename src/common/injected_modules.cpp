// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/injected_modules.h"
#include "common/environment.h"
#include "common/log.h"
#include "common/paths.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <mutex>
#include <optional>

namespace venture::injected_modules {
namespace {
std::string normal(std::string_view path, bool windows) {
  std::string out(path);
  if (windows) {
    std::ranges::replace(out, '\\', '/');
    for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}
std::string trimmed(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return std::string(text);
}
std::string file_name(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  return std::string(slash == path.npos ? path : path.substr(slash + 1));
}
// The message of a log line ("<seconds>.<ms> <thread> <level> [<tag>] <message>").
std::string_view message_after(std::string_view line, std::string_view prefix) {
  const auto at = line.find(prefix);
  if (at == line.npos) return {};
  auto rest = line.substr(at + prefix.size());
  while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.remove_suffix(1);
  return rest;
}
template <class Fn> void each_line(std::string_view text, Fn &&fn) {
  while (!text.empty()) {
    const auto end = text.find('\n');
    fn(text.substr(0, end));
    text = end == text.npos ? std::string_view{} : text.substr(end + 1);
  }
}
}  // namespace

bool inside(std::string_view module, std::span<const std::string> roots, bool windows) {
  const auto path = normal(module, windows);
  for (const auto &root : roots) {
    auto dir = normal(root, windows);
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    if (dir.empty()) continue;
    if (path.size() > dir.size() + 1 && path.starts_with(dir) && path[dir.size()] == '/') return true;
  }
  return false;
}

std::vector<std::string> starter_folders(std::span<const std::string> ancestors,
                                         std::span<const std::string> excluded, bool windows) {
  std::vector<std::string> out;
  auto same = [&](const std::string &a, const std::string &b) {
    auto x = normal(a, windows), y = normal(b, windows);
    while (!x.empty() && x.back() == '/') x.pop_back();
    while (!y.empty() && y.back() == '/') y.pop_back();
    return x == y;
  };
  for (const auto &image : ancestors) {
    const auto slash = image.find_last_of(windows ? "/\\" : "/");
    if (slash == std::string::npos || slash == 0) continue;
    std::string folder = image.substr(0, slash);
    // A program in an excluded folder (explorer.exe, a shell) or in Venture's own install.
    if (inside(folder, excluded, windows) ||
        std::ranges::any_of(excluded, [&](const std::string &root) { return same(folder, root); }))
      continue;
    if (std::ranges::none_of(out, [&](const std::string &f) { return same(f, folder); })) out.push_back(std::move(folder));
  }
  return out;
}

std::string program_name(const Module &module) {
  std::string name = trimmed(module.product);
  if (name.empty()) name = trimmed(module.description);
  if (name.empty()) name = file_name(module.path);
  // The summary separates programs with ", ".
  std::erase(name, ',');
  if (name.size() > 60) name = trimmed(name.substr(0, 60));
  return name;
}

std::string summary(std::span<const Module> modules) {
  std::vector<std::string> names;
  for (const auto &module : modules)
    if (auto name = program_name(module); !module.from_starter && std::ranges::find(names, name) == names.end()) names.push_back(std::move(name));
  std::string out;
  for (const auto &name : names) out += (out.empty() ? "" : ", ") + name;
  return out.empty() ? "none" : out;
}

std::string detail(const Module &module) {
  std::string out = module.path;
  std::string facts;
  auto add = [&](std::string_view label, const std::string &value) {
    if (auto text = trimmed(value); !text.empty()) facts += std::format("{}{} \"{}\"", facts.empty() ? "" : ", ", label, text);
  };
  add("product", module.product);
  add("company", module.company);
  if (module.product.empty()) add("description", module.description);
  add("version", module.version);
  if (module.from_starter) facts += std::string(facts.empty() ? "" : ", ") + std::string(kFromStarter);
  return facts.empty() ? out : out + " (" + facts + ")";
}

std::string last_programs(std::string_view log) {
  std::string_view last;
  each_line(log, [&](std::string_view line) {
    if (line.find("[modules] ") == line.npos) return;
    if (auto text = message_after(line, kSummary); !text.empty()) last = text;
  });
  return last == "none" ? std::string{} : std::string(last);
}

std::vector<std::string> details(std::string_view log) {
  std::vector<std::string> out;
  each_line(log, [&](std::string_view line) {
    if (line.find("[modules] ") == line.npos) return;
    if (auto text = message_after(line, kDetail); !text.empty() && std::ranges::find(out, text) == out.end())
      out.emplace_back(text);
  });
  return out;
}

std::string device_loss_advice(std::string_view programs) {
  if (programs.find(", ") == programs.npos)
    return std::format("A capture or overlay program ({}) was loaded into the game. Close it fully, then try again.",
                       programs);
  return std::format("Capture or overlay programs ({}) were loaded into the game. Close them fully, then try again.",
                     programs);
}

namespace {
std::mutex trust_mutex;
std::vector<std::string> trusted;
}  // namespace
void trust(const std::filesystem::path &folder) {
  std::lock_guard lock(trust_mutex);
  trusted.push_back(paths::utf8(folder.lexically_normal()));
}
std::vector<std::string> trusted_folders() {
  std::lock_guard lock(trust_mutex);
  return trusted;
}

namespace {
#ifdef _WIN32
constexpr bool kWindows = true;
constexpr char kSeparator = ';';
#else
constexpr bool kWindows = false;
constexpr char kSeparator = ':';
#endif
std::vector<std::string> split(std::string_view rest) {
  std::vector<std::string> out;
  while (!rest.empty()) {
    const auto end = std::min(rest.find(kSeparator), rest.size());
    if (end) out.emplace_back(rest.substr(0, end));
    rest.remove_prefix(std::min(end + 1, rest.size()));
  }
  return out;
}
}  // namespace

void record_starters() {
  try {
    auto excluded = system_folders();
    excluded.push_back(paths::utf8(paths::executable().parent_path()));
    // A launcher started by another one keeps the folders it was given.
    const char *given = environment::getenv(kStartersVariable);
    auto folders = split(given ? given : "");
    for (auto &folder : starter_folders(ancestor_images(), excluded, kWindows))
      if (folder.find(kSeparator) == std::string::npos && std::ranges::find(folders, folder) == folders.end())
        folders.push_back(std::move(folder));
    if (folders.empty()) return;
    std::string value;
    for (const auto &folder : folders) value += (value.empty() ? "" : std::string(1, kSeparator)) + folder;
    set_process_variable(kStartersVariable, value);
    environment::refresh();
    log::info("launcher", "Started by programs in: {}", value);
  } catch (const std::exception &e) {
    log::warn("launcher", "Could not read the programs that started Venture: {}", e.what());
  }
}

void log_loaded(bool always) {
  static std::mutex mutex;
  // The modules of the last check, one path per line; none before the first.
  static std::optional<std::string> last_modules;
  static std::vector<std::string> logged;
  std::vector<Module> modules;
  try {
    modules = scan();
  } catch (const std::exception &e) {
    log::warn("modules", "Could not list the modules loaded in this process: {}", e.what());
    return;
  }
  const char *given = environment::getenv(kStartersVariable);
  const auto started_by = split(given ? given : "");
  std::string key;
  for (auto &module : modules) {
    module.from_starter = inside(module.path, started_by, kWindows);
    key += module.path + "\n";
  }
  const auto text = summary(modules);
  std::lock_guard lock(mutex);
  if (!always && key == last_modules) return;
  last_modules = key;
  for (const auto &module : modules)
    if (std::ranges::find(logged, module.path) == logged.end()) {
      logged.push_back(module.path);
      log::info("modules", "{}{}", kDetail, detail(module));
    }
  if (text == "none") log::info("modules", "{}none", kSummary);
  else log::warn("modules", "{}{}", kSummary, text);
}
}  // namespace venture::injected_modules
