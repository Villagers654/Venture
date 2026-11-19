#include "common/library/mods.h"

#include "common/android/archive.h"
#include "common/android/manifest.h"
#include "common/digest.h"
#include "common/file_ops.h"
#include "common/log.h"
#include "common/meta/store.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>

namespace venture::mods {
namespace {
std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return out;
}
std::string trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == text.npos) return {};
  return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}
std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> parts;
  for (std::size_t start = 0;;) {
    const auto end = text.find(separator, start);
    parts.emplace_back(text.substr(start, end == text.npos ? text.npos : end - start));
    if (end == text.npos) return parts;
    start = end + 1;
  }
}
// Mod ids name folders: letters, digits, '.', '_' and '-'.
bool safe_id(std::string_view id) {
  return !id.empty() && id.size() <= 100 && id.front() != '.' &&
         id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == id.npos &&
         id.find("..") == id.npos;
}
std::string string_field(const Json &j, std::string_view key) {
  const auto &v = field(j, key);
  return v.is_string() ? v.get<std::string>() : std::string();
}
std::vector<std::string> strings(const Json &j, std::string_view key) {
  std::vector<std::string> out;
  const auto &v = field(j, key);
  if (v.is_null()) return out;
  if (!v.is_array()) throw std::runtime_error("Its mod.json has an invalid \"" + std::string(key) + "\" list.");
  for (const auto &item : v) {
    if (!item.is_string() || item.get<std::string>().empty())
      throw std::runtime_error("Its mod.json has an invalid \"" + std::string(key) + "\" list.");
    out.push_back(item.get<std::string>());
  }
  return out;
}
const Json &objects(const Json &j, std::string_view key) {
  const auto &v = field(j, key);
  if (!v.is_null() && !v.is_array())
    throw std::runtime_error("Its mod.json has an invalid \"" + std::string(key) + "\" list.");
  return v;
}
// A package member's path in the extracted folder; empty when it is unsafe
// (absolute, "..", a name the host cannot hold).
fs::path member_path(std::string_view name) {
  std::string normalized(name);
  std::ranges::replace(normalized, '\\', '/');
  if (normalized.empty() || normalized.front() == '/') return {};
  fs::path out;
  for (const auto &part : split(normalized, '/')) {
    if (part.empty() || part == "." || part == "..") return {};
    const auto host = paths::host_name(part);
    if (!paths::safe_name(host)) return {};
    out /= paths::from_utf8(host);
  }
  return out;
}
std::string basename(std::string_view name) {
  std::string normalized(name);
  std::ranges::replace(normalized, '\\', '/');
  const auto slash = normalized.rfind('/');
  return slash == normalized.npos ? normalized : normalized.substr(slash + 1);
}

// --- Semantic versions -------------------------------------------------------
struct Version {
  std::uint64_t major = 0, minor = 0, patch = 0;
  std::vector<std::string> pre;
};
bool numeric(std::string_view s) { return digits(s, 1, 18); }
int compare_identifiers(const std::string &a, const std::string &b) {
  const bool na = numeric(a), nb = numeric(b);
  if (na && nb) {
    const auto x = std::stoull(a), y = std::stoull(b);
    return x < y ? -1 : x > y;
  }
  if (na != nb) return na ? -1 : 1;  // numeric identifiers sort first
  return a < b ? -1 : a > b;
}
int compare(const Version &a, const Version &b) {
  for (auto [x, y] : {std::pair{a.major, b.major}, std::pair{a.minor, b.minor}, std::pair{a.patch, b.patch}})
    if (x != y) return x < y ? -1 : 1;
  if (a.pre.empty() || b.pre.empty()) return a.pre.empty() == b.pre.empty() ? 0 : a.pre.empty() ? 1 : -1;
  for (std::size_t i = 0; i < std::min(a.pre.size(), b.pre.size()); ++i)
    if (int c = compare_identifiers(a.pre[i], b.pre[i])) return c;
  return a.pre.size() < b.pre.size() ? -1 : a.pre.size() > b.pre.size();
}
// A version that may leave out parts or write them as x, X or *: the parts it
// gives (0 to 3).
struct Partial {
  Version version;
  int parts = 0;
};
std::optional<Partial> parse_partial(std::string_view text) {
  std::string s = trim(text);
  while (!s.empty() && (s.front() == 'v' || s.front() == 'V' || s.front() == '=')) s.erase(0, 1);
  if (const auto plus = s.find('+'); plus != s.npos) s.resize(plus);  // build metadata
  Partial out;
  std::string core = s, pre;
  if (const auto dash = s.find('-'); dash != s.npos) {
    core = s.substr(0, dash);
    pre = s.substr(dash + 1);
  }
  if (core.empty() || core == "*" || core == "x" || core == "X") return pre.empty() ? std::optional(out) : std::nullopt;
  const auto parts = split(core, '.');
  if (parts.size() > 3) return std::nullopt;
  std::uint64_t *fields[] = {&out.version.major, &out.version.minor, &out.version.patch};
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (parts[i] == "x" || parts[i] == "X" || parts[i] == "*") break;
    if (!numeric(parts[i])) return std::nullopt;
    *fields[i] = std::stoull(parts[i]);
    out.parts = int(i) + 1;
  }
  if (!pre.empty()) {
    if (out.parts != 3) return std::nullopt;
    for (const auto &id : split(pre, '.')) {
      if (id.empty() || id.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") != id.npos)
        return std::nullopt;
      out.version.pre.push_back(id);
    }
  }
  return out;
}
std::optional<Version> parse_version(std::string_view text) {
  auto p = parse_partial(text);
  if (!p || p->parts != 3) return std::nullopt;
  return p->version;
}
// The lowest version above every version a partial covers: 1.2 -> 1.3.0-0.
Version above(const Partial &p) {
  Version v = p.version;
  v.pre = {"0"};
  if (p.parts == 1) v = {v.major + 1, 0, 0, {"0"}};
  else if (p.parts == 2) v = {v.major, v.minor + 1, 0, {"0"}};
  else v.pre.clear();
  return v;
}
struct Comparator {
  enum Op { lt, le, gt, ge, eq } op;
  Version version;
  bool test(const Version &v) const {
    const int c = compare(v, version);
    switch (op) {
      case lt: return c < 0;
      case le: return c <= 0;
      case gt: return c > 0;
      case ge: return c >= 0;
      case eq: return c == 0;
    }
    return false;
  }
};
// One space-separated term ("^1.2", ">=1.0.0") as primitive comparators.
std::optional<std::vector<Comparator>> desugar(std::string_view term) {
  std::string op;
  std::size_t i = 0;
  while (i < term.size() && std::string_view("<>=^~").find(term[i]) != std::string_view::npos) op += term[i++];
  if (op == "~>") op = "~";
  auto partial = parse_partial(term.substr(i));
  if (!partial) return std::nullopt;
  const auto &p = *partial;
  using C = Comparator;
  if (p.parts == 0) {
    if (op == "<" || op == ">") return std::vector<C>{{C::lt, {0, 0, 0, {"0"}}}};  // nothing
    return std::vector<C>{};
  }
  const Version floor = p.version;
  if (op.empty() || op == "=") {
    if (p.parts == 3) return std::vector<C>{{C::eq, floor}};
    return std::vector<C>{{C::ge, floor}, {C::lt, above(p)}};
  }
  if (op == "^") {
    Version top;
    if (floor.major || p.parts == 1) top = {floor.major + 1, 0, 0, {"0"}};
    else if (floor.minor || p.parts == 2) top = {0, floor.minor + 1, 0, {"0"}};
    else top = {0, 0, floor.patch + 1, {"0"}};
    return std::vector<C>{{C::ge, floor}, {C::lt, top}};
  }
  if (op == "~") {
    const Version top = p.parts == 1 ? Version{floor.major + 1, 0, 0, {"0"}} : Version{floor.major, floor.minor + 1, 0, {"0"}};
    return std::vector<C>{{C::ge, floor}, {C::lt, top}};
  }
  if (op == ">=") return std::vector<C>{{C::ge, floor}};
  if (op == "<") return std::vector<C>{{C::lt, p.parts == 3 ? floor : Version{floor.major, floor.minor, floor.patch, {"0"}}}};
  if (op == ">") return std::vector<C>{p.parts == 3 ? C{C::gt, floor} : C{C::ge, above(p)}};
  if (op == "<=") return std::vector<C>{p.parts == 3 ? C{C::le, floor} : C{C::lt, above(p)}};
  return std::nullopt;
}
std::optional<std::vector<Comparator>> parse_set(std::string_view text) {
  const auto set = trim(text);
  std::vector<Comparator> out;
  if (const auto hyphen = set.find(" - "); hyphen != set.npos) {
    auto low = parse_partial(set.substr(0, hyphen)), high = parse_partial(set.substr(hyphen + 3));
    if (!low || !high) return std::nullopt;
    out.push_back({Comparator::ge, low->version});
    if (high->parts == 3) out.push_back({Comparator::le, high->version});
    else if (high->parts) out.push_back({Comparator::lt, above(*high)});
    return out;
  }
  // Terms are separated by spaces; an operator may be written apart from its version.
  std::vector<std::string> terms;
  std::string pending;
  for (const auto &word : split(set, ' ')) {
    if (word.empty()) continue;
    if (word.find_first_not_of("<>=^~") == word.npos) {
      pending += word;
      continue;
    }
    terms.push_back(pending + word);
    pending.clear();
  }
  if (!pending.empty()) return std::nullopt;
  for (const auto &term : terms) {
    auto parts = desugar(term);
    if (!parts) return std::nullopt;
    out.insert(out.end(), parts->begin(), parts->end());
  }
  return out;
}

// --- The mods folder ---------------------------------------------------------
fs::path installed_root(const fs::path &mods) { return mods / "installed"; }
fs::path state_file(const fs::path &mods) { return installed_root(mods) / "state.json"; }
Json read_state(const fs::path &mods) {
  std::error_code ec;
  const auto file = state_file(mods);
  if (!fs::is_regular_file(file, ec) || is_link(file)) return Json::object();
  auto j = Json::parse(paths::read(file), nullptr, false);
  return j.is_object() ? j : Json::object();
}
void write_state(const fs::path &mods, const Json &state) {
  paths::atomic_write(state_file(mods), state.dump(2));
}
Json &state_entry(Json &state, std::string_view id) {
  auto &all = state["mods"];
  if (!all.is_object()) all = Json::object();
  auto &entry = all[std::string(id)];
  if (!entry.is_object()) entry = Json::object();
  return entry;
}
// Store changes and syncs take turns.
struct StoreLock {
  explicit StoreLock(const fs::path &mods) : lock((fs::create_directories(installed_root(mods)), installed_root(mods) / ".lock")) {}
  FileLock lock;
};
std::vector<Installed> list_locked(const fs::path &mods) {
  std::vector<Installed> out;
  const auto root = installed_root(mods);
  std::error_code ec;
  if (!fs::is_directory(root, ec) || is_link(root)) return out;
  const auto state = read_state(mods);
  for (const auto &entry : fs::directory_iterator(root, ec)) {
    const auto name = paths::utf8(entry.path().filename());
    if (!safe_id(name) || is_link(entry.path()) || !entry.is_directory(ec)) continue;
    try {
      Installed mod;
      mod.manifest = parse_manifest(paths::read(entry.path() / "mod.json"));
      if (mod.manifest.id != name) continue;
      mod.directory = entry.path();
      const auto &s = field(field(state, "mods"), name);
      mod.enabled = !field(s, "enabled").is_boolean() || field(s, "enabled").get<bool>();
      mod.dependency = field(s, "dependency").is_boolean() && field(s, "dependency").get<bool>();
      out.push_back(std::move(mod));
    } catch (const std::exception &e) {
      log::warn("mods", "Skipped the installed mod {}: {}", name, e.what());
    }
  }
  std::ranges::sort(out, [](const Installed &a, const Installed &b) {
    return lower(a.manifest.name) < lower(b.manifest.name) ||
           (lower(a.manifest.name) == lower(b.manifest.name) && a.manifest.id < b.manifest.id);
  });
  return out;
}
const Installed *find(const std::vector<Installed> &all, std::string_view id) {
  auto it = std::ranges::find(all, id, [](const Installed &m) { return std::string_view(m.manifest.id); });
  return it == all.end() ? nullptr : &*it;
}
std::string random_suffix() { return random_account_key().substr(0, 12); }
std::string extension_of(const fs::path &file) {
  auto ext = lower(paths::utf8(file.extension()));
  if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
  return ext;
}
std::string normalized_extension(std::string_view ext) {
  auto out = lower(trim(ext));
  while (!out.empty() && out.front() == '.') out.erase(0, 1);
  return out;
}
fs::path items_root(const fs::path &mods) { return mods / "items"; }
// A guest folder (copyExtensions) or file (fileCopies) path ending in '/' takes
// the file's own name.
fs::path destination_file(std::string_view package, std::string_view destination, std::string_view name) {
  std::string guest(destination);
  if (guest.ends_with('/')) guest += name;
  return storage_path(package, guest);
}
}  // namespace

// --- Manifests ---------------------------------------------------------------
const char *loader_name(Loader loader) { return loader == Loader::scotland2 ? "Scotland2" : "QuestLoader"; }

Manifest parse_manifest(std::string_view text) {
  auto j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) throw std::runtime_error("Its mod.json is not a valid manifest.");
  Manifest m;
  m.schema = string_field(j, "_QPVersion");
  m.id = string_field(j, "id");
  if (!safe_id(m.id)) throw std::runtime_error("Its mod.json has no valid mod id.");
  m.name = string_field(j, "name");
  if (m.name.empty()) m.name = m.id;
  m.author = string_field(j, "author");
  m.porter = string_field(j, "porter");
  m.version = string_field(j, "version");
  if (!valid_version(m.version)) throw std::runtime_error("Its mod.json has no valid version.");
  m.description = string_field(j, "description");
  m.cover = string_field(j, "coverImage");
  m.package = string_field(j, "packageId");
  if (!m.package.empty() && !paths::safe_package(m.package))
    throw std::runtime_error("Its mod.json names an invalid game package.");
  m.package_version = string_field(j, "packageVersion");
  m.library = field(j, "isLibrary").is_boolean() && field(j, "isLibrary").get<bool>();
  if (const auto loader = lower(string_field(j, "modloader")); loader == "scotland2")
    m.loader = Loader::scotland2;
  else if (loader.empty() || loader == "questloader")
    m.loader = Loader::questloader;
  else
    throw std::runtime_error("It is for the mod loader " + string_field(j, "modloader") + ", which Venture does not know.");
  m.mod_files = strings(j, "modFiles");
  m.late_mod_files = strings(j, "lateModFiles");
  m.library_files = strings(j, "libraryFiles");
  for (const auto &c : objects(j, "fileCopies")) {
    FileCopy copy{string_field(c, "name"), string_field(c, "destination")};
    if (copy.name.empty() || copy.destination.empty()) throw std::runtime_error("Its mod.json has an invalid file copy.");
    m.copies.push_back(std::move(copy));
  }
  for (const auto &e : objects(j, "copyExtensions")) {
    CopyExtension ext{normalized_extension(string_field(e, "extension")), string_field(e, "destination")};
    if (ext.extension.empty() || ext.destination.empty())
      throw std::runtime_error("Its mod.json has an invalid file type it takes.");
    m.extensions.push_back(std::move(ext));
  }
  for (const auto &d : objects(j, "dependencies")) {
    Dependency dep;
    dep.id = string_field(d, "id");
    if (!safe_id(dep.id)) throw std::runtime_error("Its mod.json has a dependency without a valid id.");
    if (const auto range = trim(string_field(d, "version")); !range.empty()) dep.range = range;
    dep.download = string_field(d, "downloadIfMissing");
    if (field(d, "required").is_boolean()) dep.required = field(d, "required").get<bool>();
    m.dependencies.push_back(std::move(dep));
  }
  return m;
}

bool valid_version(std::string_view version) { return parse_version(version).has_value(); }
bool satisfies(std::string_view version, std::string_view range) {
  const auto v = parse_version(version);
  if (!v) return false;
  for (const auto &set : split(range, '|')) {
    if (trim(set).empty() && range.find("||") != range.npos) continue;  // the gap inside "||"
    const auto comparators = parse_set(set);
    if (comparators && std::ranges::all_of(*comparators, [&](const Comparator &c) { return c.test(*v); })) return true;
  }
  return false;
}

fs::path storage_path(std::string_view package, std::string_view guest) {
  std::vector<std::string> parts;
  for (auto &part : split(guest, '/')) {
    if (part.empty() || part == ".") continue;
    if (part == "..") return {};
    parts.push_back(std::move(part));
  }
  static const std::vector<std::vector<std::string>> roots{
      {"sdcard"}, {"storage", "emulated", "0"}, {"storage", "self", "primary"}, {"mnt", "sdcard"}};
  std::size_t skip = 0;
  for (const auto &root : roots)
    if (parts.size() > root.size() && std::equal(root.begin(), root.end(), parts.begin())) skip = root.size();
  if (!skip || !guest.starts_with('/')) return {};
  parts.erase(parts.begin(), parts.begin() + std::ptrdiff_t(skip));
  fs::path out = "external";
  if (parts.size() >= 3 && parts[0] == "Android" && parts[2] == package) {
    if (parts[1] == "obb") return {};  // expansion files are the install's
    if (parts[1] == "data") {
      out = "external-data";
      parts.erase(parts.begin(), parts.begin() + 3);
      if (parts.empty()) return {};
    }
  }
  for (const auto &part : parts) {
    const auto host = paths::host_name(part);
    if (!paths::safe_name(host)) return {};
    out /= paths::from_utf8(host);
  }
  return out;
}

// --- Installing --------------------------------------------------------------
Manifest read_package(const fs::path &qmod) {
  android::Archive archive(qmod);
  const auto *manifest = archive.find("mod.json");
  if (!manifest) throw std::runtime_error(paths::utf8(qmod.filename()) + " is not a mod package: it has no mod.json.");
  const auto bytes = archive.read(*manifest, 4 << 20);
  return parse_manifest({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

Installed install(const fs::path &mods, std::string_view package, const fs::path &qmod, bool dependency) {
  android::Archive archive(qmod);
  const auto *manifest_entry = archive.find("mod.json");
  if (!manifest_entry)
    throw std::runtime_error(paths::utf8(qmod.filename()) + " is not a mod package: it has no mod.json.");
  const auto bytes = archive.read(*manifest_entry, 4 << 20);
  const auto m = parse_manifest({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
  if (!m.package.empty() && m.package != package)
    throw std::runtime_error(m.name + " is a mod for " + m.package + ", not for " + std::string(package) + ".");
  auto holds = [&](std::string_view name) {
    std::string normalized(name);
    std::ranges::replace(normalized, '\\', '/');
    return archive.find(normalized) != nullptr;
  };
  std::vector<std::string> listed = m.mod_files;
  listed.insert(listed.end(), m.late_mod_files.begin(), m.late_mod_files.end());
  listed.insert(listed.end(), m.library_files.begin(), m.library_files.end());
  for (const auto &c : m.copies) listed.push_back(c.name);
  for (const auto &name : listed)
    if (!holds(name) || member_path(name).empty())
      throw std::runtime_error(m.name + "'s package lists " + name + ", which it does not hold.");
  for (const auto &c : m.copies)
    if (destination_file(package, c.destination, basename(c.name)).empty())
      throw std::runtime_error(m.name + " copies a file to " + c.destination + ", outside the game's storage.");
  for (const auto &e : m.extensions)
    if (destination_file(package, e.destination + "/", "file").empty())
      throw std::runtime_error(m.name + " puts ." + e.extension + " files in " + e.destination +
                               ", outside the game's storage.");
  std::uint64_t total = 0;
  for (const auto &e : archive.entries()) {
    if (member_path(e.name).empty())
      throw std::runtime_error(m.name + "'s package holds a file with an unsafe name: " + e.name);
    total += e.size;
  }
  const auto root = installed_root(mods);
  fs::create_directories(root);
  if (is_link(root) || is_link(mods)) throw std::runtime_error("The mods folder is a link.");
  if (fs::space(root).available < total + (64ull << 20))
    throw std::runtime_error("Not enough free space to install " + m.name + ".");
  // Extracted beside the installed mods, then swapped in whole.
  const auto staging = root / paths::from_utf8("." + m.id + "-" + random_suffix());
  struct Cleanup {
    fs::path root, path;
    ~Cleanup() {
      std::error_code ec;
      if (fs::exists(path, ec)) try { remove_tree(root, path); } catch (...) {}
    }
  } cleanup{root, staging};
  for (const auto &e : archive.entries()) {
    const auto target = staging / member_path(e.name);
    fs::create_directories(target.parent_path());
    std::ofstream out(target, std::ios::binary);
    if (!out) throw std::runtime_error("Could not write " + e.name + ".");
    archive.extract(e, [&](std::span<const std::uint8_t> chunk) {
      out.write(reinterpret_cast<const char *>(chunk.data()), std::streamsize(chunk.size()));
    });
    if (!out.flush()) throw std::runtime_error("Could not write " + e.name + ".");
  }
  StoreLock lock(mods);
  const auto target = root / paths::from_utf8(m.id);
  auto state = read_state(mods);
  auto &entry = state_entry(state, m.id);
  std::error_code ec;
  const bool replacing = fs::exists(target, ec);
  if (replacing) remove_tree(root, target);
  fs::rename(staging, target);
  // The player's own choice of a mod outlives a dependency's reinstall.
  const bool was_dependency = !replacing || (field(entry, "dependency").is_boolean() && entry["dependency"].get<bool>());
  entry["dependency"] = dependency && was_dependency;
  entry["enabled"] = true;
  write_state(mods, state);
  log::info("mods", "Installed {} {} ({}) for {}", m.id, m.version, loader_name(m.loader), package);
  return {m, target, true, dependency && was_dependency};
}

std::vector<Installed> list(const fs::path &mods) {
  std::error_code ec;
  if (!fs::is_directory(installed_root(mods), ec)) return {};
  StoreLock lock(mods);
  return list_locked(mods);
}

void set_enabled(const fs::path &mods, std::string_view id, bool enabled) {
  StoreLock lock(mods);
  const auto all = list_locked(mods);
  if (!find(all, id)) throw std::runtime_error("That mod is not installed.");
  auto state = read_state(mods);
  std::set<std::string> visited;
  std::function<void(std::string_view)> apply = [&](std::string_view next) {
    if (!visited.emplace(next).second) return;
    const auto *mod = find(all, next);
    if (!mod) return;
    state_entry(state, next)["enabled"] = enabled;
    if (enabled) {
      for (const auto &dep : mod->manifest.dependencies) apply(dep.id);
    } else {
      for (const auto &other : all)
        if (std::ranges::any_of(other.manifest.dependencies,
                                [&](const Dependency &d) { return d.id == next && d.required; }))
          apply(other.manifest.id);
    }
  };
  apply(id);
  write_state(mods, state);
}

void remove(const fs::path &mods, std::string_view id) {
  StoreLock lock(mods);
  auto all = list_locked(mods);
  const auto *mod = find(all, id);
  if (!mod) throw std::runtime_error("That mod is not installed.");
  auto state = read_state(mods);
  const auto root = installed_root(mods);
  auto drop = [&](const Installed &m) {
    remove_tree(root, m.directory);
    if (state["mods"].is_object()) state["mods"].erase(m.manifest.id);
    log::info("mods", "Removed the mod {}", m.manifest.id);
  };
  drop(*mod);
  std::erase_if(all, [&](const Installed &m) { return m.manifest.id == id; });
  // Libraries that came as dependencies go once nothing needs them.
  for (bool again = true; again;) {
    again = false;
    for (const auto &m : all) {
      if (!m.manifest.library || !m.dependency) continue;
      const bool needed = std::ranges::any_of(all, [&](const Installed &other) {
        return std::ranges::any_of(other.manifest.dependencies,
                                   [&](const Dependency &d) { return d.id == m.manifest.id; });
      });
      if (needed) continue;
      drop(m);
      const auto gone = m.manifest.id;
      std::erase_if(all, [&](const Installed &x) { return x.manifest.id == gone; });
      again = true;
      break;
    }
  }
  write_state(mods, state);
}

std::vector<Dependency> missing_dependencies(const std::vector<Installed> &installed, const Manifest &mod) {
  std::vector<Dependency> out;
  for (const auto &dep : mod.dependencies) {
    const auto *have = find(installed, dep.id);
    if (have ? !satisfies(have->manifest.version, dep.range) : dep.required) out.push_back(dep);
  }
  return out;
}

Report install_with_dependencies(const fs::path &mods, std::string_view package, const fs::path &qmod,
                                 const Fetch &fetch, std::stop_token stop) {
  Report report;
  report.installed.push_back(install(mods, package, qmod));
  const auto root = report.installed.front().manifest.id;
  // Each download link is fetched once and kept until the end, so a link
  // several mods name is not fetched again.
  struct Download {
    fs::path file;
    std::optional<Manifest> manifest;
  };
  std::map<std::string, Download> downloads;
  struct Cleanup {
    std::map<std::string, Download> &downloads;
    ~Cleanup() {
      std::error_code ec;
      for (auto &entry : downloads) fs::remove(entry.second.file, ec);
    }
  } cleanup{downloads};
  auto get = [&](const std::string &url) -> const std::optional<Manifest> & {
    auto [it, fresh] = downloads.try_emplace(url);
    if (!fresh) return it->second.manifest;
    if (stop.stop_requested()) throw std::runtime_error("Stopped.");
    it->second.file = installed_root(mods) / paths::from_utf8(".download-" + random_suffix() + ".qmod");
    try {
      fetch(url, it->second.file, stop);
      it->second.manifest = read_package(it->second.file);
    } catch (const std::exception &e) {
      if (stop.stop_requested()) throw;
      log::warn("mods", "Could not get the mod package at {}: {}", url, e.what());
    }
    return it->second.manifest;
  };
  // What the enabled mods need of each dependency of the new mod (and of
  // theirs): one version must satisfy every range at once, so a mod whose
  // manifest links an older version than another mod requires does not decide
  // which one is installed.
  struct Need {
    std::vector<std::string> ranges, links;
    bool required = false;
  };
  std::set<std::string> unresolved;
  for (bool changed = true; changed;) {
    changed = false;
    const auto all = list(mods);
    std::set<std::string> closure{root};
    for (std::vector<std::string> todo{root}; !todo.empty();) {
      const auto *m = find(all, todo.back());
      todo.pop_back();
      if (m)
        for (const auto &dep : m->manifest.dependencies)
          if (closure.insert(dep.id).second) todo.push_back(dep.id);
    }
    std::map<std::string, Need> needs;
    for (const auto &m : all) {
      if (!m.enabled && m.manifest.id != root) continue;
      for (const auto &dep : m.manifest.dependencies) {
        if (dep.id == root || dep.id == m.manifest.id || !closure.contains(dep.id)) continue;
        auto &n = needs[dep.id];
        if (std::ranges::find(n.ranges, dep.range) == n.ranges.end()) n.ranges.push_back(dep.range);
        if (!dep.download.empty() && std::ranges::find(n.links, dep.download) == n.links.end())
          n.links.push_back(dep.download);
        n.required = n.required || dep.required;
      }
    }
    for (const auto &[id, n] : needs) {
      if (unresolved.contains(id)) continue;
      auto fits = [&](const std::string &version) {
        return std::ranges::all_of(n.ranges, [&](const std::string &r) { return satisfies(version, r); });
      };
      const auto *have = find(all, id);
      if (have ? fits(have->manifest.version) : !n.required) continue;
      // The highest version a link offers that satisfies them all.
      const std::string *best_url = nullptr;
      std::optional<Version> best;
      if (fetch)
        for (const auto &url : n.links) {
          const auto &got = get(url);
          if (got && got->id != id) log::warn("mods", "The package at {} is {}, not {}", url, got->id, id);
          if (!got || got->id != id || !fits(got->version)) continue;
          const auto version = parse_version(got->version);
          if (version && (!best || compare(*version, *best) > 0)) {
            best = version;
            best_url = &url;
          }
        }
      std::string ranges;
      for (const auto &r : n.ranges) ranges += (ranges.empty() ? "" : " ") + r;
      if (!best_url) {
        unresolved.insert(id);
        report.missing.push_back(id + " " + ranges);
        if (fetch && !n.links.empty()) log::warn("mods", "No download of {} satisfies {}", id, ranges);
        continue;
      }
      auto installed = install(mods, package, downloads[*best_url].file, have ? have->dependency : true);
      log::info("mods", "Installed {} {} for {}", id, installed.manifest.version, ranges);
      report.installed.push_back(std::move(installed));
      changed = true;
    }
    if (!changed)
      // Installed dependencies that fit are turned on with the mod.
      for (const auto &[id, n] : needs)
        if (const auto *have = find(all, id); have && !have->enabled &&
            std::ranges::all_of(n.ranges, [&](const std::string &r) { return satisfies(have->manifest.version, r); }))
          set_enabled(mods, id, true);
  }
  return report;
}

// --- Items -------------------------------------------------------------------
std::vector<Item> items(const fs::path &mods) {
  std::vector<Item> out;
  std::error_code ec;
  const auto root = items_root(mods);
  if (!fs::is_directory(root, ec) || is_link(root)) return out;
  for (const auto &type : fs::directory_iterator(root, ec)) {
    if (!type.is_directory(ec) || is_link(type.path())) continue;
    for (const auto &file : fs::directory_iterator(type.path(), ec))
      if (file.is_regular_file(ec) && !is_link(file.path()) && !file.path().filename().string().starts_with("."))
        out.push_back({paths::utf8(type.path().filename()), paths::utf8(file.path().filename()), file.path()});
  }
  std::ranges::sort(out, [](const Item &a, const Item &b) { return lower(a.name) < lower(b.name); });
  return out;
}
bool takes_item(const fs::path &mods, const fs::path &file) {
  const auto ext = extension_of(file);
  if (ext.empty()) return false;
  return std::ranges::any_of(list(mods), [&](const Installed &m) {
    return m.enabled && std::ranges::any_of(m.manifest.extensions, [&](const CopyExtension &e) { return e.extension == ext; });
  });
}
Item add_item(const fs::path &mods, const fs::path &file) {
  const auto ext = extension_of(file), name = paths::utf8(file.filename());
  if (!takes_item(mods, file)) throw std::runtime_error("No installed mod takes ." + ext + " files.");
  if (!paths::safe_name(name) || name.starts_with(".") || !safe_id(ext))
    throw std::runtime_error(name + " has a name Venture cannot store.");
  const auto folder = items_root(mods) / paths::from_utf8(ext);
  fs::create_directories(folder);
  const auto target = folder / paths::from_utf8(name);
  const auto part = folder / paths::from_utf8("." + name + "-" + random_suffix());
  struct Remove {
    fs::path file;
    ~Remove() { std::error_code ec; fs::remove(file, ec); }
  } remove_part{part};
  fs::copy_file(file, part);
  paths::replace_file(part, target);
  log::info("mods", "Added {} for the mods that take .{} files", name, ext);
  return {ext, name, target};
}
void remove_item(const fs::path &mods, std::string_view extension, std::string_view name) {
  if (!safe_id(extension) || !paths::safe_name(name)) throw std::runtime_error("Invalid mod file.");
  const auto file = items_root(mods) / paths::from_utf8(extension) / paths::from_utf8(name);
  std::error_code ec;
  if (!is_link(file) && fs::is_regular_file(file, ec)) fs::remove(file);
}

// --- Patched APKs and the loader ---------------------------------------------
std::optional<Loader> apk_loader(const fs::path &apk) {
  android::Archive archive(apk);
  if (const auto *tag = archive.find("modded.json")) {
    const auto bytes = archive.read(*tag, 1 << 20);
    const auto j = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    const auto name = lower(string_field(j, "modloaderName"));
    if (name == "scotland2") return Loader::scotland2;
    if (name == "questloader") return Loader::questloader;
  }
  // QuestLoader's older patchers (BMBF, early QuestPatcher) left a bare tag.
  if (archive.find("BMBF.modded") || archive.find("modded")) return Loader::questloader;
  return std::nullopt;
}
fs::path loader_file(const fs::path &mods) {
  const auto file = mods / "loader" / "libsl2.so";
  std::error_code ec;
  return fs::is_regular_file(file, ec) && !is_link(file) ? file : fs::path{};
}
void set_loader_file(const fs::path &mods, const fs::path &file) {
  {
    std::ifstream in(file, std::ios::binary);
    char magic[4]{};
    if (!in.read(magic, 4) || std::string_view(magic, 4) != "\177ELF")
      throw std::runtime_error(paths::utf8(file.filename()) + " is not a mod loader library.");
  }
  const auto folder = mods / "loader";
  fs::create_directories(folder);
  const auto part = folder / paths::from_utf8(".libsl2.so-" + random_suffix());
  struct Remove {
    fs::path file;
    ~Remove() { std::error_code ec; fs::remove(file, ec); }
  } remove_part{part};
  fs::copy_file(file, part);
  paths::replace_file(part, folder / "libsl2.so");
}

// --- Patched APKs ------------------------------------------------------------
namespace {
struct InstalledApk {
  meta::Plan plan;
  meta::File *file = nullptr;
  fs::path path;
};
InstalledApk installed_apk(const fs::path &directory) {
  InstalledApk out;
  out.plan = meta::parse_plan(paths::read_json(directory / "plan.json"));
  auto it = std::ranges::find(out.plan.files, std::string("apk"), &meta::File::kind);
  if (it == out.plan.files.end()) throw std::runtime_error("This installation has no APK.");
  out.file = &*it;
  out.path = directory / paths::from_utf8(it->name);
  if (!paths::within(directory, out.path) || it->name.find_first_of("/\\") != std::string::npos)
    throw std::runtime_error("Invalid APK name in the installation.");
  return out;
}
std::string file_sha256(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw std::runtime_error("Could not read " + paths::utf8(file.filename()) + ".");
  return digest::sha256(in);
}
}  // namespace
bool replace_apk(const fs::path &directory, const fs::path &apk) {
  auto current = installed_apk(directory);
  const auto manifest = android::read_apk(apk);
  const auto code = std::int64_t(number(Json(current.plan.version_code)));
  if (manifest.package != current.plan.package)
    throw std::runtime_error("This APK is " + manifest.package + ", not " + current.plan.package + ".");
  if (manifest.version_code != code)
    throw std::runtime_error("This APK is version " + manifest.version_name + " (" + std::to_string(manifest.version_code) +
                             "); the installed game is version " + current.plan.version + " (" +
                             current.plan.version_code + "). Install or import that version first.");
  const auto size = fs::file_size(apk);
  const auto hash = file_sha256(apk);
  std::error_code ec;
  if (fs::is_regular_file(current.path, ec) && fs::file_size(current.path, ec) == size &&
      (current.file->sha256.empty() ? file_sha256(current.path) : current.file->sha256) == hash)
    return false;
  if (fs::space(directory).available < size + (64ull << 20))
    throw std::runtime_error("Not enough free space to replace the APK.");
  const auto part = paths::from_utf8(paths::utf8(current.path) + ".part");
  fs::copy_file(apk, part, fs::copy_options::overwrite_existing);
  const auto original = paths::from_utf8(paths::utf8(current.path) + ".original");
  if (!fs::exists(original, ec) && fs::is_regular_file(current.path, ec)) fs::rename(current.path, original);
  paths::replace_file(part, current.path);
  current.file->size = size;
  current.file->sha256 = hash;
  paths::atomic_write(directory / "plan.json", current.plan.json().dump(2));
  log::info("mods", "Replaced the APK of {} {} with {} ({} bytes)", current.plan.package, current.plan.version_code,
            paths::utf8(apk.filename()), size);
  return true;
}
std::vector<std::string> obb_names(const fs::path &directory) {
  std::vector<std::string> names;
  for (const auto &f : meta::parse_plan(paths::read_json(directory / "plan.json")).files)
    if (f.kind == "obb" && paths::safe_name(f.name)) names.push_back(f.name);
  return names;
}
void replace_obb(const fs::path &directory, std::string_view name, const fs::path &file) {
  const auto names = obb_names(directory);
  if (std::ranges::find(names, name) == names.end())
    throw std::runtime_error("This game has no expansion file " + std::string(name) + ".");
  std::error_code ec;
  if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) == 0)
    throw std::runtime_error(paths::utf8(file.filename()) + " is not an expansion file.");
  const auto target = directory / paths::from_utf8(name);
  if (fs::exists(target, ec) && fs::equivalent(file, target, ec)) return;
  const auto size = fs::file_size(file);
  const auto current = fs::is_regular_file(target, ec) ? fs::file_size(target, ec) : 0;
  if (fs::space(directory).available + current < size + (64ull << 20))
    throw std::runtime_error("Not enough free space to replace " + std::string(name) + ".");
  const auto part = paths::from_utf8(paths::utf8(target) + ".replacing");
  struct Remove {
    fs::path file;
    ~Remove() { std::error_code ignored; fs::remove(file, ignored); }
  } remove_part{part};
  fs::copy_file(file, part, fs::copy_options::overwrite_existing);
  paths::replace_file(part, target);
  log::info("mods", "Replaced the expansion file {} with {} ({} bytes)", name, paths::utf8(file.filename()), size);
}
std::vector<std::string> adopt_legacy_obbs(const fs::path &install, const std::vector<std::string> &names,
                                           const fs::path &mods) {
  std::vector<std::string> adopted;
  std::error_code ec;
  if (!fs::is_directory(mods, ec) || is_link(mods)) return adopted;
  for (const auto &name : names) {
    const auto legacy = mods / paths::from_utf8(name), target = install / paths::from_utf8(name);
    if (!paths::safe_name(name) || is_link(legacy) || !fs::is_regular_file(legacy, ec) || !fs::is_regular_file(target, ec))
      continue;
    std::error_code moved;
    fs::rename(legacy, target, moved);  // the same drive: a move
    if (moved) {
      const auto part = paths::from_utf8(paths::utf8(target) + ".replacing");
      fs::copy_file(legacy, part, fs::copy_options::overwrite_existing);
      paths::replace_file(part, target);
      fs::remove(legacy);
    }
    log::info("mods", "The expansion file {} from the mods folder now replaces the installed one", name);
    adopted.push_back(name);
  }
  return adopted;
}
fs::path original_apk(const fs::path &directory) {
  try {
    const auto current = installed_apk(directory);
    const auto original = paths::from_utf8(paths::utf8(current.path) + ".original");
    std::error_code ec;
    return fs::is_regular_file(original, ec) ? original : fs::path{};
  } catch (const std::exception &) {
    return {};
  }
}
void restore_apk(const fs::path &directory) {
  auto current = installed_apk(directory);
  const auto original = paths::from_utf8(paths::utf8(current.path) + ".original");
  std::error_code ec;
  if (!fs::is_regular_file(original, ec)) throw std::runtime_error("This game has no original APK kept.");
  const auto size = fs::file_size(original);
  const auto hash = file_sha256(original);
  paths::replace_file(original, current.path);
  current.file->size = size;
  current.file->sha256 = hash;
  paths::atomic_write(directory / "plan.json", current.plan.json().dump(2));
  log::info("mods", "Restored the original APK of {} {}", current.plan.package, current.plan.version_code);
}

// --- Publishing at launch ----------------------------------------------------
namespace {
Json identity_json(const FileIdentity &id) {
  return Json::array({id.volume, id.file, id.size, id.write_seconds, id.write_nanoseconds});
}
}  // namespace
SyncResult sync(const fs::path &mods, std::string_view package, const fs::path &data_dir) {
  SyncResult result;
  struct Source {
    fs::path file;
    bool link;  // a library the game never writes: a hard link where possible
  };
  std::map<std::string, Source> plan;
  auto add = [&](const fs::path &relative, const fs::path &file, bool link) {
    if (relative.empty()) return;
    plan.try_emplace(paths::generic_utf8(relative), Source{file, link});
  };
  const auto record_file = data_dir.parent_path() / (paths::utf8(data_dir.filename()) + ".mods.json");
  fs::create_directories(data_dir);
  FileLock record_lock(data_dir.parent_path() / (paths::utf8(data_dir.filename()) + ".mods.lock"));
  std::error_code ec;
  std::optional<StoreLock> store;
  if (fs::is_directory(mods, ec) && !is_link(mods)) store.emplace(mods);
  const auto all = store ? list_locked(mods) : std::vector<Installed>{};
  // Libraries first, so a library mod's file wins over a copy another mod carries.
  std::vector<const Installed *> enabled;
  for (const auto &m : all)
    if (m.enabled) enabled.push_back(&m);
  std::ranges::stable_partition(enabled, [](const Installed *m) { return m->manifest.library; });
  const fs::path modloader = fs::path("external") / "ModData" / paths::from_utf8(paths::host_name(package)) / "Modloader";
  for (const auto *m : enabled) {
    auto place = [&](const std::vector<std::string> &names, const fs::path &folder) {
      for (const auto &name : names)
        if (const auto member = member_path(name); !member.empty())
          add(folder / paths::from_utf8(paths::host_name(basename(name))), m->directory / member, true);
    };
    if (m->manifest.loader == Loader::scotland2) {
      place(m->manifest.mod_files, modloader / "early_mods");
      place(m->manifest.late_mod_files, modloader / "mods");
      place(m->manifest.library_files, modloader / "libs");
    } else {
      place(m->manifest.mod_files, fs::path("external-data") / "files" / "mods");
      place(m->manifest.late_mod_files, fs::path("external-data") / "files" / "mods");
      place(m->manifest.library_files, fs::path("external-data") / "files" / "libs");
    }
    for (const auto &c : m->manifest.copies)
      if (const auto member = member_path(c.name); !member.empty())
        add(destination_file(package, c.destination, basename(c.name)), m->directory / member, false);
  }
  for (const auto &item : store ? items(mods) : std::vector<Item>{})
    for (const auto *m : enabled)
      for (const auto &e : m->manifest.extensions)
        if (e.extension == item.extension) add(destination_file(package, e.destination + "/", item.name), item.file, false);
  if (const auto loader = store ? loader_file(mods) : fs::path{}; !loader.empty()) add(modloader / "libsl2.so", loader, true);

  Json record = Json::object();
  if (fs::is_regular_file(record_file, ec) && !is_link(record_file)) {
    record = Json::parse(paths::read(record_file), nullptr, false);
    if (!record.is_object()) record = Json::object();
  }
  Json files = field(record, "files").is_object() ? record["files"] : Json::object();
  Json next = Json::object();
  // A file the game rewrote stays its own, also when its mod comes back.
  auto keep = [&](const std::string &relative, Json previous) {
    if (field(previous, "kept") != true) ++result.kept;
    previous["kept"] = true;
    next[relative] = std::move(previous);
  };
  // What the last sync published and this one does not: removed while it is
  // still the file published.
  for (auto it = files.begin(); it != files.end(); ++it) {
    if (plan.contains(it.key())) continue;
    const auto target = data_dir / paths::from_utf8(it.key());
    if (!paths::within(data_dir, target)) continue;
    const auto id = file_identity(target);
    if (id && field(it.value(), "copy") == identity_json(*id)) {
      fs::remove(paths::io_path(target), ec);
      ++result.removed;
    } else if (id) {
      keep(it.key(), it.value());
    }
  }
  for (const auto &[relative, source] : plan) {
    const auto target = data_dir / paths::from_utf8(relative);
    const auto source_id = file_identity(source.file);
    if (!source_id) continue;
    if (!paths::within(data_dir, target)) {
      log::warn("mods", "Skipped {}: its folder links outside the game's storage", relative);
      continue;
    }
    // Account storage paths nest deep (ModData/<package>/Modloader/...): the
    // standard library calls take the long-path form on Windows.
    fs::create_directories(paths::io_path(target.parent_path()));
    const auto target_id = file_identity(target);
    const auto &previous = field(files, relative);
    const bool recorded = previous.is_object();
    if (target_id && recorded && field(previous, "copy") == identity_json(*target_id)) {
      if (field(previous, "source") == identity_json(*source_id)) {
        next[relative] = previous;
        continue;
      }
    } else if (target_id && recorded && !source.link) {
      keep(relative, previous);  // the game (or a mod) rewrote a copied file
      continue;
    }
    const auto temporary = target.parent_path() / paths::from_utf8(".venture-mod-" + random_suffix());
    struct Remove {
      fs::path file;
      ~Remove() { std::error_code ignored; fs::remove(paths::io_path(file), ignored); }
    } remove_temporary{temporary};
    std::error_code link_error;
    if (source.link) fs::create_hard_link(paths::io_path(source.file), paths::io_path(temporary), link_error);
    if (!source.link || link_error) fs::copy_file(paths::io_path(source.file), paths::io_path(temporary));
    paths::replace_file(temporary, target);
    if (const auto copy = file_identity(target))
      next[relative] = Json{{"source", identity_json(*source_id)}, {"copy", identity_json(*copy)}};
    ++result.published;
  }
  if (next != files || !record.contains("version"))
    paths::atomic_write(record_file, Json{{"version", 1}, {"files", next}}.dump(1));
  if (result.published || result.removed || result.kept || !plan.empty())
    log::info("mods", "Mods for {}: {} files in place, {} published, {} removed, {} kept as the game changed them",
              package, next.size(), result.published, result.removed, result.kept);
  return result;
}
}  // namespace venture::mods
