// Host folders the person picked for the app through the document picker
// (com.venture.runtime.HostDocuments), each a removable storage volume the
// guest sees at /storage/<id>, as Android mounts an SD card. Only picked
// folders are mounted; the Java framework adds them as it grants or reloads
// them. Reads are lock-free snapshots: every guest path resolution checks them.
#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace venture::document_roots {
// (host folder, guest mount point) pairs.
using Roots = std::vector<std::pair<std::filesystem::path, std::string>>;

inline std::shared_ptr<const Roots>& current_roots() {
  static std::shared_ptr<const Roots> roots = std::make_shared<Roots>();
  return roots;
}
inline std::mutex& roots_mutex() {
  static std::mutex mutex;
  return mutex;
}

inline std::shared_ptr<const Roots> snapshot() {
  return std::atomic_load(&current_roots());
}

// A volume ID as Android names one ("1A2B-3C4D"): hex digits and one dash.
inline bool valid_id(std::string_view id) {
  if (id.size() != 9 || id[4] != '-') return false;
  for (std::size_t i = 0; i < id.size(); ++i)
    if (i != 4 && !((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'F'))) return false;
  return true;
}

// Mounts `host` at /storage/<id>, replacing an earlier mount of the same ID.
inline bool mount(std::string_view id, const std::filesystem::path& host) {
  if (!valid_id(id) || !host.is_absolute()) return false;
  std::lock_guard lock(roots_mutex());
  auto next = std::make_shared<Roots>(*snapshot());
  const std::string guest = "/storage/" + std::string(id);
  std::erase_if(*next, [&](const auto& root) { return root.second == guest; });
  next->emplace_back(host.lexically_normal(), guest);
  std::atomic_store(&current_roots(), std::shared_ptr<const Roots>(std::move(next)));
  return true;
}
}  // namespace venture::document_roots
