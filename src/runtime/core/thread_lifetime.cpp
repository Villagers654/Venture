#include "core/thread_lifetime.h"
#include <vector>

namespace venture::thread_lifetime {
namespace {
struct Entry {
  void** slot;
  void* object;
  void (*destroy)(void*);
};
// Set once this thread's owner is destroyed. Trivially destructible, so readable until the
// thread is gone.
thread_local bool owner_destroyed = false;
struct Owner {
  std::vector<Entry> entries;
  ~Owner() {
    // An object's destructor may create another object here: it is destroyed next. Its own
    // slot keeps pointing at it until its destructor returns, as a thread_local's would, so a
    // destructor that reaches its own object through local() does not create a fresh one
    // (which would then be destroyed the same way, without end).
    while (!entries.empty()) {
      const auto entry = entries.back();
      entries.pop_back();
      entry.destroy(entry.object);
      *entry.slot = nullptr;
    }
    owner_destroyed = true;
  }
};
Owner& owner() {
  thread_local Owner value;
  return value;
}
}  // namespace
void prepare() {
  if (!owner_destroyed) (void)owner();
}
namespace detail {
void own(void** slot, void* object, void (*destroy)(void*)) {
  // An object first used after this thread's owner is gone (from a thread_local destructor
  // that runs later still) is left to the process: nothing on this thread outlives it.
  if (owner_destroyed) return;
  owner().entries.push_back({slot, object, destroy});
}
}  // namespace detail
}  // namespace venture::thread_lifetime
