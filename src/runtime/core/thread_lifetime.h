#pragma once
#include <memory>

// Per-thread objects that guest code can reach. An adopted host thread runs guest code (the
// game's thread-exit handlers and TLS destructors) from its adoption record's thread_local
// destructor while the thread exits. The C++ runtime destroys thread_local objects in reverse
// order of construction, so a plain thread_local first used after the adoption is already gone
// by then, and guest code reaching it through an HLE call uses freed memory. A thread_local
// with a destructor that guest code, or an HLE call it makes, can reach belongs here instead:
// these objects live until after the adoption record's destructor has returned.
namespace venture::thread_lifetime {
// Creates this thread's owner of the objects. GuestThread::adopt_current calls it before it
// creates its record, so the owner is destroyed after the record.
void prepare();
namespace detail {
// Takes ownership of `object` (destroyed by `destroy`, in reverse order of adoption), and
// clears `*slot` when it does.
void own(void** slot, void* object, void (*destroy)(void*));
}  // namespace detail
// This thread's T for Tag, value-initialized on first use. Tag tells apart objects of one
// type: an incomplete local struct named in the call, such as local<std::string, struct Error>().
template <class T, class Tag>
T& local() {
  // Trivially destructible: the C++ runtime never destroys it; the owner does.
  static thread_local T* object = nullptr;
  if (!object) [[unlikely]] {
    auto created = std::make_unique<T>();
    detail::own(reinterpret_cast<void**>(&object), created.get(), [](void* p) { delete static_cast<T*>(p); });
    object = created.release();
  }
  return *object;
}
}  // namespace venture::thread_lifetime
