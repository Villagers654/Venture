#include "common/environment.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
extern char** environ;
#endif

namespace venture::environment {
namespace {
constexpr unsigned char fold(unsigned char c) {
#ifdef _WIN32
  return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c;
#else
  return c;
#endif
}
struct Hash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept {
    std::size_t h = 14695981039346656037ull;
    for (unsigned char c : s) h = (h ^ fold(c)) * 1099511628211ull;
    return h;
  }
};
struct Equal {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i=0; i<a.size(); ++i) if (fold(a[i]) != fold(b[i])) return false;
    return true;
  }
};
using Snapshot = std::unordered_map<std::string,std::string,Hash,Equal>;
struct State {
  std::atomic<const Snapshot*> current{nullptr};
  std::mutex mutex;
  // Returned pointers remain valid even across refresh and static destruction.
  std::vector<std::unique_ptr<const Snapshot>> retained;
  std::atomic_flag warned = ATOMIC_FLAG_INIT;
};
State& state() { static auto* value = new State; return *value; }
const Snapshot* capture(State& state) {
  auto next = std::make_unique<Snapshot>();
  bool legacy = false;
  auto add = [&](std::string_view entry) {
    auto split = entry.find('=');
    if (split == entry.npos || split == 0) return; // Windows drive entries are not variables.
    auto name = entry.substr(0,split);
    legacy |= name.size() >= 3 && fold(name[0]) == fold('Q') &&
              fold(name[1]) == fold('P') && name[2] == '_';
    next->try_emplace(std::string(name), std::string(entry.substr(split+1)));
  };
#ifdef _WIN32
  auto* block = GetEnvironmentStringsW();
  if (block) {
    for (auto* p=block; *p; ) {
      const std::wstring_view entry(p);
      const int length = WideCharToMultiByte(CP_ACP,0,p,static_cast<int>(entry.size()),nullptr,0,nullptr,nullptr);
      std::string bytes(length, '\0');
      if (length && WideCharToMultiByte(CP_ACP,0,p,static_cast<int>(entry.size()),bytes.data(),length,nullptr,nullptr)) add(bytes);
      p+=entry.size()+1;
    }
    FreeEnvironmentStringsW(block);
  }
#else
  for (auto** p=environ; p && *p; ++p) add(*p);
#endif
  auto* published = next.get();
  state.retained.push_back(std::move(next));
  state.current.store(published,std::memory_order_release);
  // No values or individual names are logged. Avoid logger initialization recursion.
  if (legacy && !state.warned.test_and_set())
    std::fputs("[environment] Legacy QP_* settings are ignored; use VENTURE_* settings.\n", stderr);
  return published;
}
}
const char* getenv(const char* name) {
  if (!name) return nullptr;
  auto& shared = state();
  auto* snapshot = shared.current.load(std::memory_order_acquire);
  if (!snapshot) {
    std::lock_guard lock(shared.mutex);
    snapshot = shared.current.load(std::memory_order_acquire);
    if (!snapshot) snapshot = capture(shared);
  }
  auto entry = snapshot->find(std::string_view(name));
  return entry == snapshot->end() ? nullptr : entry->second.c_str();
}
void refresh() {
  auto& shared = state();
  std::lock_guard lock(shared.mutex);
  capture(shared);
}
} // namespace venture::environment
