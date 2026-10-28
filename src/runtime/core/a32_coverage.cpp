#include "common/environment.h"
#include "core/a32_coverage.h"
#include "core/a32_abi.h"
#include "core/hle_runtime.h"
#include "core/process.h"
#include "core/linker_runtime.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>

namespace venture::hle::a32 {
void install_import_coverage_hook() {
  std::map<std::pair<std::string, std::string>, Coverage> index;
  for (auto& row : coverage()) index[{row.library, row.symbol}] = row;
  linker_set_import_coverage_hook([index = std::move(index)](const elf::Image&, const elf::Import& import) {
    if (import.resolution == "guest") return std::string("guest-provided");
    if (import.type == 1) return std::string("data layout requires adapter"); // ELF STT_OBJECT
    if (!descriptor(import.provider, import.name)) return std::string("descriptor missing");
    auto it = index.find({import.provider, import.name});
    if (it == index.end() && import.provider == "libGLESv3.so") it = index.find({"libGLESv2.so", import.name});
    if (it == index.end()) return std::string("descriptor found; not registered");
    return it->second.compatible ? std::string("compatible") : "adapter required: " + it->second.reason;
  });
}
nlohmann::json inspection_report() {
  using json = nlohmann::json;
  bool actual = process_config().isa() == GuestIsa::a32;
  if (!actual) for (auto function : registered_functions()) prepare(function);
  auto rows = coverage();
  json result{{"binding_mode", actual ? "selected A32 handlers" : "projected from registered A64 signatures"},
              {"registered", json::array()}, {"first_contact", json::array()}};
  std::map<std::pair<std::string, std::string>, Coverage> index;
  for (auto& row : rows) {
    index[{row.library, row.symbol}] = row;
    result["registered"].push_back({{"library", row.library}, {"symbol", row.symbol}, {"descriptor_found", row.descriptor_found},
        {"compatible", row.compatible}, {"adapter_required", !row.compatible}, {"reason", row.reason}});
  }
  struct Import { const char* importer; const char* provider; const char* symbol; };
  static constexpr Import imports[]{
#include "core/generated/a32_first_contact.inc"
  };
  for (auto& item : imports) {
    std::string provider(item.provider); auto at = provider.find(" [");
    bool guest = provider.find("[guest]") != provider.npos;
    if (at != provider.npos) provider.resize(at);
    std::string symbol(item.symbol), version;
    if (auto pos = symbol.find('@'); pos != symbol.npos) { version = symbol.substr(pos + 1); symbol.resize(pos); }
    auto* d = descriptor(provider, symbol);
    auto binding = index.find({provider, symbol});
    // register_graphics_libraries exposes GLESv3 as a GLESv2 soname alias.
    if (binding == index.end() && provider == "libGLESv3.so") binding = index.find({"libGLESv2.so", symbol});
    bool compatible = !guest && binding != index.end() && binding->second.compatible;
    std::string status = guest ? "guest-provided" : compatible ? "compatible" : binding == index.end() ? "not registered" : binding->second.reason;
    auto& summary = result["summary"][item.importer];
    if (summary.is_null()) summary = json::object();
    auto increment = [&](const char* key) { summary[key] = summary.value(key, 0) + 1; };
    increment("imports");
    if (guest) increment("guest_provided");
    else { if (d) increment("descriptor_found"); if (compatible) increment("compatible"); else increment("adapter_or_implementation_required"); }
    result["first_contact"].push_back({{"importer", item.importer}, {"library", provider}, {"symbol", symbol}, {"version", version},
      {"descriptor_found", d != nullptr}, {"compatible", compatible}, {"adapter_required", !guest && !compatible}, {"status", status}});
  }
  result["descriptor_counts"] = {{"ndk", ndk_descriptors().size()}, {"libc_supplement", libc_descriptors().size()},
    {"graphics", graphics_descriptors().size()}, {"vendor", vendor_descriptors().size()}};
  // The optional census is read only at inspection. Accept the dispatch-census
  // array format; reject unfamiliar input instead of silently inventing ranks.
  if (auto* path = venture::environment::getenv("VENTURE_A32_BOOT_CENSUS")) {
    std::ifstream stream(path); json census; stream >> census;
    const auto& functions = census.is_array() ? census : census.at("functions");
    std::map<std::pair<std::string, std::string>, std::uint64_t> hits;
    for (const auto& f : functions)
      hits[{f.at("library"), f.contains("symbol") ? f.at("symbol") : f.at("name")}] +=
          (f.contains("hits") ? f.at("hits") : f.at("calls")).get<std::uint64_t>();
    auto ranked = json::array();
    for (auto& row : result["registered"]) if (!row["compatible"].get<bool>()) {
      auto copy = row; copy["boot_calls"] = hits[{row["library"], row["symbol"]}]; ranked.push_back(std::move(copy));
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const json& a, const json& b) { return a["boot_calls"] > b["boot_calls"]; });
    result["adapter_boot_rank"] = std::move(ranked);
  }
  return result;
}
}
