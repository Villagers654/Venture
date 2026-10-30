#include "common/environment.h"
#include "core/process_runtime.h"
#include "core/java_script_loader.h"
#include "core/a32_coverage.h"
#include "core/linker_runtime.h"
#include "core/linker.h"
#include "core/elf.h"
#include "core/low_address_space.h"
#include "cpu/cpu.h"
#include "platform/environment.h"
#include "platform/low_memory.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/file_ops.h"
#include "common/saves_migration.h"
#include <nlohmann/json.hpp>
#include <miniz.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>

namespace venture {
std::uint64_t guest_physical_memory() {
  const auto model = process_config().device_identity->manifest_name;
  const unsigned gib = model == "quest" ? 4 : model == "quest2" ? 6 : model == "questpro" ? 12 : 8;
  return std::uint64_t(gib) << 30;
}
std::uint64_t guest_usable_memory() {
  auto nominal = guest_physical_memory();
  // Linux sysinfo.totalram is usable RAM (si_meminfo/totalram_pages), not
  // the installed DIMM capacity. A32's identity-addressed backing excludes
  // the null guard and foreign host ranges; those bytes cannot be guest RAM.
  // In particular, advertising exactly 4 GiB as usable ARMv7 memory wraps
  // size_t consumers to zero even though the nominal Quest profile is 4 GiB.
  return process_config().isa() == GuestIsa::a32 && low32::active()
      ? std::min(nominal, low32::layout().total) : nominal;
}
ProcessConfig& process_config() { static ProcessConfig config; return config; }
namespace {
std::optional<GuestIsa> frozen_isa;
bool below(std::string_view path, std::string_view prefix) {
  return path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() && path[prefix.size()] == '/');
}
std::uint32_t file_crc(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::array<unsigned char, 65536> data{};
  mz_ulong crc = MZ_CRC32_INIT;
  while (in) { in.read(reinterpret_cast<char*>(data.data()), data.size()); crc = mz_crc32(crc, data.data(), static_cast<std::size_t>(in.gcount())); }
  return static_cast<std::uint32_t>(crc);
}
}  // namespace
void select_process_isa(GuestIsa isa) {
  if (frozen_isa) throw std::runtime_error("process ISA is immutable after HLE registration");
  // APK native-ABI selection precedes ANGLE, driver and HotSpot startup. A64
  // must restore the host's ordinary allocation choices at this boundary.
  if (isa == GuestIsa::a64 && !low32::active()) platform::release_pre_reserved_low_memory();
  auto& c = process_config();
  c.abi = isa == GuestIsa::a32 ? "armeabi-v7a" : "arm64-v8a";
  c.native_lib_suffix = isa == GuestIsa::a32 ? "/lib/arm" : "/lib/arm64";
  c.system_lib_path = isa == GuestIsa::a32 ? "/system/lib" : "/system/lib64";
  c.properties["ro.product.cpu.abi"] = c.abi;
  c.properties["ro.product.cpu.abilist"] = c.abi;
}
void freeze_process_isa() {
  auto isa = process_config().isa();
  if (frozen_isa && *frozen_isa != isa) throw std::runtime_error("process ISA changed after selection");
  frozen_isa = isa;
}
void configure_game(const std::filesystem::path& install, std::string package_override) {
  configure_game(install, std::move(package_override), "local");
}
void configure_game(const std::filesystem::path& install,
                    std::string package_override, std::string account_key) {
  if (frozen_isa) throw std::runtime_error("cannot configure another game after process ISA is sealed");
  std::ifstream stream(install / "plan.json");
  if (!stream) throw std::runtime_error("install directory has no plan.json");
  auto plan = nlohmann::json::parse(stream);
  auto& config = process_config();
  config = {};
  config.account_key = std::move(account_key);
  config.source = plan.value("source", "meta");
  config.package = package_override.empty() ? plan.value("package", "") : std::move(package_override);
  if (config.package.empty() || config.package.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._") != std::string::npos)
    throw std::runtime_error("invalid or absent game package");
  config.version_name = plan.value("version", "");
  config.version_code = plan.value("versionCode", std::int64_t{});
  for (auto& file : plan.at("files")) if (file.value("kind", "") == "apk") {
    auto filename = paths::from_utf8(file.at("name").get<std::string>());
    if (filename.has_parent_path()) throw std::runtime_error("APK filename in plan must be a basename");
    config.apk = std::filesystem::absolute(install / filename); break;
  }
  if (config.apk.empty() || !std::filesystem::is_regular_file(config.apk)) throw std::runtime_error("APK from plan is absent");
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, paths::utf8(paths::io_path(config.apk)).c_str(), 0)) throw std::runtime_error("cannot open APK ZIP");
  struct End { mz_zip_archive* zip; ~End() { mz_zip_reader_end(zip); } } end{&zip};
  bool a64 = false, a32 = false;
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
    mz_zip_archive_file_stat entry{};
    if (!mz_zip_reader_file_stat(&zip, i, &entry)) throw std::runtime_error("invalid APK ZIP entry");
    std::string_view member(entry.m_filename);
    if (!member.ends_with(".so")) continue;
    a64 |= member.starts_with("lib/arm64-v8a/");
    a32 |= member.starts_with("lib/armeabi-v7a/");
  }
  if (!a64 && !a32) throw std::runtime_error("APK has no supported ARM native library slice");
  select_process_isa(a64 ? GuestIsa::a64 : GuestIsa::a32);
  log::info("launch", "Selected APK native slice {}", config.abi);
  config.has_code = mz_zip_reader_locate_file(&zip, "classes.dex", nullptr, 0) >= 0;
  const auto index = mz_zip_reader_locate_file(&zip, "AndroidManifest.xml", nullptr, 0);
  mz_zip_archive_file_stat stat{};
  if (index < 0 || !mz_zip_reader_file_stat(&zip, index, &stat) || stat.m_uncomp_size > 16*1024*1024)
    throw std::runtime_error("APK manifest absent or oversized");
  std::vector<std::byte> manifest(stat.m_uncomp_size);
  if (!mz_zip_reader_extract_to_mem(&zip, index, manifest.data(), manifest.size(), 0))
    throw std::runtime_error("cannot read APK manifest");
  config.device_identity = &select_device_identity(manifest_supported_devices(manifest));
  log::info("launch", "Guest device identity: {} ({}){}", config.device_identity->model,
            config.device_identity->device,
            config.device_identity == &native_device_identity() ? "" : " via Horizon compatibility mode");
  auto base = paths::data_dir();
  config.data_dir = account_data_dir(base, config.account_key, config.package);
  config.guest_cache_dir = config.data_dir / "cache";
  config.cache_dir = base / "cache" / config.package / std::to_string(config.version_code);
  config.native_lib_dir = config.cache_dir / "lib" / config.abi;
  // Extracted libraries and converted Java of earlier versions are only disk
  // use once the game updated; one still in use stays until a later launch.
  if (std::error_code ec; std::filesystem::is_directory(config.cache_dir.parent_path(), ec))
    for (const auto& entry : std::filesystem::directory_iterator(config.cache_dir.parent_path(), ec)) {
      const auto name = paths::utf8(entry.path().filename());
      if (name == std::to_string(config.version_code) || name.empty() ||
          name.find_first_not_of("0123456789") != std::string::npos)
        continue;
      try {
        remove_tree(base / "cache", entry.path());
        log::info("launch", "Removed the cache of version {}", name);
      } catch (const std::exception&) {
      }
    }
  std::filesystem::create_directories(config.data_dir);
  // NativeActivity receives these paths before Java Context getters can create
  // them. Android supplies existing files directories even on the first launch.
  std::filesystem::create_directories(config.data_dir / "files");
  std::filesystem::create_directories(config.data_dir / "external-data" / "files");
  std::filesystem::create_directories(config.guest_cache_dir);
  std::filesystem::create_directories(config.native_lib_dir);
  // JDK File/ZipFile I/O needs a real host directory. Use one dedicated view
  // for Context, Environment and native VFS, with cross-volume copy fallback.
  // Retire all legacy versioned views so updates cannot retain multi-GB files.
  remove_cached_obb_aliases(base, config.package);
  config.obb_dir = obb_view_for(std::filesystem::absolute(install),
                                config.data_dir / "external/Android/obb" / config.package, config.account_key);
  std::vector<std::string> obb_names;
  for (const auto& file : plan.at("files")) {
    const auto kind = file.value("kind", "");
    if (kind != "obb" && kind != "asset") continue;
    const auto name = paths::from_utf8(file.at("name").get<std::string>());
    if (name.empty() || name.has_parent_path() || name == "." || name == ".." ||
        (kind == "obb" && name.extension() != ".obb"))
      throw std::runtime_error("Expansion/asset filename in plan must be a valid basename");
    const auto source = std::filesystem::absolute(install / name);
    if (!std::filesystem::is_regular_file(source)) {
      if (kind == "obb") throw std::runtime_error("OBB from plan is absent");
    }
    obb_names.push_back(paths::utf8(name));
  }
  sync_obb_view(std::filesystem::absolute(install), config.obb_dir, obb_names, mods_dir(base, config.package));
  config.properties = {{"ro.build.version.sdk", std::to_string(config.sdk_level)}, {"ro.product.cpu.abi", config.abi},
      {"ro.product.cpu.abilist", config.abi}};
}
std::vector<std::filesystem::path> extract_native_libraries() {
  auto& config = process_config();
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip, paths::utf8(paths::io_path(config.apk)).c_str(), 0)) throw std::runtime_error("cannot open APK ZIP");
  struct End { mz_zip_archive* zip; ~End() { mz_zip_reader_end(zip); } } end{&zip};
  std::vector<std::filesystem::path> files;
  const std::string prefix = "lib/" + config.abi + "/";
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) throw std::runtime_error("invalid APK ZIP entry");
    std::string_view member(stat.m_filename);
    if (!member.starts_with(prefix) || !member.ends_with(".so")) continue;
    auto basename = member.substr(prefix.size());
    if (basename.empty() || basename.find_first_of("/\\:") != std::string_view::npos || basename == "..") throw std::runtime_error("unsafe APK library path");
    auto path = paths::io_path(config.native_lib_dir / paths::from_utf8(basename));
    files.push_back(path);
    // Android's installer copies every lib/<abi>/*.so; a member that is not an
    // ELF (a loader's config file, e.g. libfrda.config.so) is data for the
    // libraries that read it. Only ELF members must match the slice.
    auto wrong_slice = [&](const std::filesystem::path& file) {
      std::ifstream in(file, std::ios::binary);
      char magic[4]{};
      return in.read(magic, 4) && !std::memcmp(magic, "\177ELF", 4) && elf::detect_isa(file) != config.isa();
    };
    if (std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) == stat.m_uncomp_size && file_crc(path) == stat.m_crc32) {
      if (wrong_slice(path)) throw std::runtime_error("APK slice disagrees with ELF e_machine: " + std::string(member));
      continue;
    }
    auto temporary = path; temporary += ".extracting";
    log::info("apk", "extracting {} ({} bytes)", basename, stat.m_uncomp_size);
    if (!mz_zip_reader_extract_to_file(&zip, i, paths::utf8(temporary).c_str(), 0)) {
      std::filesystem::remove(temporary); throw std::runtime_error("APK extraction or CRC validation failed: " + std::string(member));
    }
    if (wrong_slice(temporary)) {
      std::filesystem::remove(temporary);
      throw std::runtime_error("APK slice disagrees with ELF e_machine: " + std::string(member));
    }
    std::filesystem::remove(path);
    std::filesystem::rename(temporary, path);
  }
  if (files.empty()) throw std::runtime_error("APK has no lib/" + config.abi + " shared objects");
  std::sort(files.begin(), files.end());
  config.embedded_java_scripts = prepare_java_script_loaders(files, resolve_guest_path) > 0;
  return files;
}
std::filesystem::path resolve_guest_path(std::string_view path) {
  const auto& config = process_config();
  if (path == config.guest_apk_path()) return config.apk;
  const std::array<std::pair<std::string, std::filesystem::path>, 13> mappings{{
      {config.guest_native_lib_dir(), config.native_lib_dir},
      {config.guest_data_dir() + "/cache", config.guest_cache_dir},
      {"/data/data/" + config.package + "/cache", config.guest_cache_dir},
      {config.guest_data_dir(), config.data_dir},
      {"/data/data/" + config.package, config.data_dir},
      {config.guest_obb_dir(), config.obb_dir},
      {"/sdcard/Android/data/" + config.package, config.data_dir / "external-data"},
      {"/sdcard/Android/obb/" + config.package, config.obb_dir},
      {"/storage/emulated/0/Android/data/" + config.package, config.data_dir / "external-data"},
      {"/storage/emulated/0", config.data_dir / "external"},
      {"/data", config.data_dir / "volumes/data"},
      {"/system", config.data_dir / "volumes/system"},
      {"/sdcard", config.data_dir / "external"}}};
  for (const auto& [prefix, root] : mappings) if (!root.empty() && below(path, prefix)) {
    auto suffix = path.substr(prefix.size());
    if (!suffix.empty()) suffix.remove_prefix(1);
    std::string escaped;
    for (std::size_t start = 0; start <= suffix.size();) {
      const auto end = std::min(suffix.find('/', start), suffix.size());
      escaped += (start ? "/" : "") + paths::host_name(suffix.substr(start, end - start));
      start = end + 1;
    }
    auto relative = paths::from_utf8(escaped).lexically_normal();
    if (relative.has_root_path()) return {};
    for (const auto& component : relative) if (component == "..") return {};
    return root / relative;
  }
  // Explicit host paths are accepted by the runtime CLI and test runner.
  auto host = paths::from_utf8(path);
  if (std::filesystem::is_regular_file(host)) return host;
  return {};
}
void write_inspection_report(const std::filesystem::path& output) {
  nlohmann::json json = {{"package", process_config().package}, {"versionCode", process_config().version_code},
      {"abi", process_config().abi},
      {"backend", cpu_backend().info().name}, {"hwcap", cpu_backend().info().hwcap}, {"hwcap2", cpu_backend().info().hwcap2},
      {"initializers_run", false}, {"libraries", nlohmann::json::array()}};
  std::uint64_t bytes = 0, relocation_count = 0, aps2 = 0, relr = 0, unresolved = 0, deferred = 0;
  std::map<std::string, std::set<std::string>> by_provider;
  for (const auto& report : linker_reports()) {
    nlohmann::json missing = nlohmann::json::object();
    for (const auto& symbol : report.unresolved) {
      missing[symbol.provider].push_back({{"name", symbol.name}, {"version", symbol.version}, {"weak", symbol.weak}, {"type", symbol.type}});
      by_provider[symbol.provider].insert(symbol.name);
    }
    json["libraries"].push_back({{"soname", report.library}, {"path", report.path}, {"mapped_bytes", report.size},
        {"needed", report.needed}, {"relocations", report.relocations}, {"aps2_count", report.aps2_count},
        {"relr_count", report.relr_count}, {"deferred_resolvers", report.deferred_resolvers}, {"unresolved", missing}});
    if (!report.imports.empty()) {
      auto& imports = json["libraries"].back()["imports"] = nlohmann::json::array();
      for (const auto& symbol : report.imports)
        imports.push_back({{"name", symbol.name}, {"version", symbol.version}, {"provider", symbol.provider},
                          {"weak", symbol.weak}, {"type", symbol.type}, {"symbol_index", symbol.symbol_index},
                          {"address", symbol.address}, {"resolution", symbol.resolution}, {"coverage", symbol.coverage}});
    }
    std::uint64_t count = 0;
    for (auto& [type, n] : report.relocations) count += n;
    std::cout << report.library << ": " << report.size << " bytes, " << count << " relocations, " << report.unresolved.size() << " unresolved\n";
    std::cout << "  needed:"; for (const auto& name : report.needed) std::cout << ' ' << name; std::cout << '\n';
    std::cout << "  relocations:"; for (const auto& [type, n] : report.relocations) std::cout << ' ' << type << '=' << n; std::cout << '\n';
    for (const auto& [provider, symbols] : missing.items()) std::cout << "  " << provider << ": " << symbols.size() << " unresolved\n";
    if (process_config().isa() == GuestIsa::a32) for (const auto& symbol : report.unresolved)
      std::cout << "    " << symbol.provider << '!' << symbol.name << (symbol.version.empty() ? "" : "@" + symbol.version)
                << (symbol.weak ? " (weak, zero)" : "") << " type=" << unsigned(symbol.type) << '\n';
    bytes += report.size; relocation_count += count; aps2 += report.aps2_count; relr += report.relr_count;
    unresolved += report.unresolved.size(); deferred += report.deferred_resolvers;
  }
  json["summary"] = {{"images", json["libraries"].size()}, {"mapped_bytes", bytes}, {"relocations", relocation_count},
      {"aps2_count", aps2}, {"relr_count", relr}, {"unresolved_imports", unresolved}, {"deferred_resolvers", deferred}};
  for (const auto& [provider, names] : by_provider) json["providers"][provider] = {{"unique_symbols", names.size()}, {"symbols", names}};
  json["provider_attribution"] =
      "ELF VERNEED when present; otherwise symbol-family inference; ambiguous "
      "imports are unknown";
  if (process_config().isa() == GuestIsa::a32 || venture::environment::getenv("VENTURE_INSPECT_A32_ABI"))
    json["a32_coverage"] = hle::a32::inspection_report();
  std::ofstream stream(output);
  if (!stream || !(stream << json.dump(2) << '\n')) throw std::runtime_error("cannot write inspection report");
  std::cout << "TOTAL: " << json["libraries"].size() << " images, " << bytes << " bytes, " << relocation_count
            << " relocations (APS2 " << aps2 << ", RELR " << relr << "), " << unresolved << " unresolved imports\n";
  std::cout << "JSON: " << output.string() << '\n';
}
}  // namespace venture
